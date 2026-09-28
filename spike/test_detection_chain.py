# -*- coding: utf-8 -*-
"""全局检测逻辑测试：一条龙覆盖发现层 → 身份解析 → 执行链路 → 状态聚合 → job 指纹

与批次测试的差异：批次测试按模块分片；本测试以"一天的生产流"为叙事，
按真实时序串起全链路，专抓**跨模块接缝**的回归（历史上 8 个真缺陷中 6 个在接缝上）。

环境：mock unreal + mock 重 IO（导入/解算/导出），磁盘文件全部真实创建
（就绪规则/分组解析/复活判据这些"检测逻辑"用真实文件验，只有 UE 重操作 mock）。
"""

import json
import os
import shutil
import sys
import tempfile
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
PKG_ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(PKG_ROOT, "deploy", "Python"))

# mock unreal（仅 import 期）
_unreal = types.ModuleType("unreal")
for _attr in ("log", "log_warning", "log_error"):
    setattr(_unreal, _attr, lambda *a, **k: None)
_unreal.Paths = types.SimpleNamespace(project_saved_dir=lambda: "")
_unreal.get_editor_subsystem = lambda *a, **k: None
_unreal.EditorAssetLibrary = types.SimpleNamespace(
    does_asset_exist=lambda p: False, list_assets=lambda p, recursive=True: [])
def _unreal_getattr(name):
    return type(name, (), {})
_unreal.__getattr__ = _unreal_getattr
sys.modules.setdefault("unreal", _unreal)

from MetaHumanSolverEngine import stream_executor
from MetaHumanSolverEngine.stream_watcher import Scanner, StreamEnqueuer, DefaultRule
from MetaHumanSolverEngine.stream_queue import StreamQueue
import MetaHumanSolverEngine.progress as mprog

for _fn in ("log", "warn", "error", "display", "result",
            "monitor_begin_task", "monitor_end", "monitor_batch_context"):
    setattr(mprog, _fn, (lambda *a, **k: None))

OK = [True]
N_PASS = [0]
N_FAIL = [0]
LINES = []


def check(name, cond, detail=""):
    if cond:
        N_PASS[0] += 1
    else:
        N_FAIL[0] += 1
        OK[0] = False
    line = "[{}] {}{}".format("PASS" if cond else "FAIL", name,
                              ("  -> " + str(detail)) if (detail and not cond) else "")
    print(line)
    LINES.append(line)


def make_take(parent, name, with_mov=True):
    """创建一个真实 take 目录（文件齐全或缺 mov）。"""
    d = os.path.join(parent, name)
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "take.json"), "w").write("{}")
    open(os.path.join(d, "depth_data.bin"), "wb").write(b"\0" * 1000)
    open(os.path.join(d, "depth_metadata.mhaical"), "w").write("{}")
    if with_mov:
        open(os.path.join(d, name + "_5_face.mov"), "wb").write(b"\0" * 500)
    return d


def settle(rule, dirs):
    """记录首个大小快照，再等稳定窗口过去（随后 scan_once 的二次比较即判就绪）。"""
    for d in dirs:
        rule.is_ready(d)          # 首次观测：记快照
    time.sleep(0.12)              # 过稳定窗口（stable_seconds=0.05）


def main():
    tmp = tempfile.mkdtemp(prefix="mhs_chain_")
    # ── mock 重 IO（仅 UE 操作；磁盘检测逻辑全部真实）──
    imported = []
    restore = []
    def patch(mod, name, fn):
        restore.append((mod, name, getattr(mod, name)))
        setattr(mod, name, fn)
    try:
        br = stream_executor.batch_runner
        pl = stream_executor.pipeline
        vs = stream_executor.versions
        patch(br, "_import_already_done", lambda cfg, item: item["name"] in imported)
        patch(vs, "import_single_take", lambda src, dst, base: imported.append(os.path.basename(src.rstrip("\\/"))))
        patch(br, "_find_footage_capture_data", lambda cfg, mode: "FOUND")
        patch(br, "_match_footage_item", lambda found, name: {"package_path": "/Game/Cap/CD_" + name})
        patch(pl, "resolve_identity", lambda cfg, pkg: "ID_resolve_fallback")
        patch(pl, "ensure_identity_ready", lambda cfg, ident: None)
        def _fake_solve(cfg, item, on_done):
            on_done(cfg, "PERF_FAKE", item)      # 模拟解算完成回调
        patch(pl, "run_depth_shot", _fake_solve)
        patch(br, "export_shot_outputs", lambda cfg, perf, item: (True, ""))

        inbox = os.path.join(tmp, "inbox")
        os.makedirs(inbox)

        # ════════ Part A · Scanner 全局发现 ════════
        print("── Part A 扫描器 ──")
        rule = DefaultRule(stable_seconds=0.05)
        sc = Scanner(inbox, rule=rule)
        check("A1 空目录 → 0 事件", sc.scan_once() == [])

        pm = os.path.join(inbox, "pm")
        os.makedirs(pm)
        t1 = make_take(pm, "take_incomplete", with_mov=False)   # 拷贝中（缺 mov）
        check("A2 缺 mov（拷贝中）→ 不发现",
              all("take_incomplete" not in e["key"] for e in sc.scan_once()))

        open(os.path.join(t1, "take_incomplete_5_face.mov"), "wb").write(b"\0" * 500)
        check("A3a 文件补齐但稳定窗口内 → 不发现",
              all("take_incomplete" not in e["key"] for e in sc.scan_once()))
        settle(rule, [t1])
        evs = sc.scan_once()
        keys = [e["key"] for e in evs]
        check("A3b 稳定后 → 发现", keys == ["pm/take_incomplete"], keys)
        ev = evs[0]
        check("A3c 事件字段齐（dir/group/kind）",
              ev["group"] == "pm" and ev["kind"] == "perf" and os.path.isdir(ev["dir"]), ev)
        check("A4 幂等：重复扫描不重复产出", sc.scan_once() == [])

        root_take = make_take(inbox, "take_root")
        settle(rule, [root_take])
        evs = {e["key"]: e for e in sc.scan_once()}
        check("A5 根目录直放 → group=''", evs.get("take_root", {}).get("group") == "",
              evs.get("take_root"))

        am = os.path.join(inbox, "am")
        os.makedirs(am)
        rom_am = make_take(am, "face_rom_am_001")               # 分组内 rom 命名
        rom_pm = make_take(pm, "anyname_rom_x")                 # pm 组 rom 命名
        plain_pm = make_take(pm, "plain_take_pm")               # pm 组普通命名
        os.makedirs(os.path.join(inbox, "_id"))
        rom_id = make_take(os.path.join(inbox, "_id"), "face_rom_direct")
        nottake = os.path.join(inbox, "pm", "not_a_take")       # 无 take.json
        os.makedirs(nottake, exist_ok=True)
        settle(rule, [rom_am, rom_pm, plain_pm, rom_id])
        evs = {e["key"]: e for e in sc.scan_once()}
        check("A6a 分组内 rom 命名兜底 → kind=id",
              evs.get("am/face_rom_am_001", {}).get("kind") == "id", evs.get("am/face_rom_am_001"))
        check("A6b _id 目录 → kind=id（任意名）",
              evs.get("_id/face_rom_direct", {}).get("kind") == "id", evs.get("_id/face_rom_direct"))
        check("A6c pm 组名字含 rom → kind=id",
              evs.get("pm/anyname_rom_x", {}).get("kind") == "id", evs.get("pm/anyname_rom_x"))
        check("A6d 普通命名 → kind=perf",
              evs.get("pm/plain_take_pm", {}).get("kind") == "perf", evs.get("pm/plain_take_pm"))
        check("A7 非 take 目录（无 take.json）→ 忽略", "pm/not_a_take" not in evs, list(evs))

        # ════════ Part B · Enqueuer 身份解析链 ════════
        print("── Part B 身份解析 ──")
        q = StreamQueue(os.path.join(tmp, "q.json"))
        enq = StreamEnqueuer(q, pending_timeout_seconds=3600.0)
        enq.process([{"key": "am/rom", "dir": "D:/r", "group": "am", "kind": "id"}])
        check("B1 ID 素材入队 kind=id", q.get("am/rom").get("kind") == "id")

        ev_perf = [{"key": "am/take_new", "dir": "D:/n", "group": "am", "kind": "perf"}]
        r = enq.process(ev_perf)
        check("B2a 无绑定 → 进 pending 池", len(r["pending"]) == 1 and q.get("am/take_new") is None)

        enq.set_group_binding("", "ID_root_global")             # 绑根组 = 全局兜底
        t = q.get("am/take_new")
        check("B2b 根组绑定冲刷全部 pending", t is not None)
        check("B2c 身份 = 根组兜底", t.get("identity") == "ID_root_global", t.get("identity"))

        enq.set_group_binding("am", "ID_am_specific")           # 组绑定优先
        enq.process([{"key": "am/take_2nd", "dir": "D:/2", "group": "am", "kind": "perf"}])
        check("B3 组绑定优先于根组兜底", q.get("am/take_2nd").get("identity") == "ID_am_specific")
        check("B4 已入队快照不变（am/take_new 仍是根组身份）",
              q.get("am/take_new").get("identity") == "ID_root_global")

        enq2 = StreamEnqueuer(StreamQueue(os.path.join(tmp, "q2.json")), pending_timeout_seconds=0.0)
        enq2.last_identity = "ID_recent"                        # 模拟曾有身份就绪
        enq2.process([{"key": "zz/take_wait", "dir": "D:/w", "group": "zz", "kind": "perf"}])
        time.sleep(0.02)
        deg = enq2.check_pending_timeout()
        check("B5 超时降级：无绑定 + 有最近身份 → 降级入队", len(deg) == 1, deg)

        # ════════ Part C · Executor 执行链路（mock IO）════════
        print("── Part C 执行链路 ──")
        job = os.path.join(tmp, "job_stream.json")
        q_path = os.path.join(tmp, "eq.json")
        b_path = os.path.join(tmp, "eb.json")
        s_path = os.path.join(tmp, "es.json")
        with open(job, "w", encoding="utf-8") as fh:
            json.dump({"mode": "depth", "stream_mode": "semi", "import_only": True,
                       "identity_path": "", "inbox": inbox,
                       "capture_root": "/Game/Cap", "identity_import_root": "/Game/ID4",
                       "storage_path": "/Game/MH_Results",
                       "import_mode": "cpp", "import_footage": True,
                       "media_output_root": os.path.join(tmp, "media"),
                       "fbx_output_dir": os.path.join(tmp, "fbx"),
                       "stream_queue_path": q_path,
                       "stream_bindings_path": b_path,
                       "stream_state_path": s_path,
                       "stream_stable_seconds": 0.05}, fh)
        # 预置绑定：am 组专用 + 根组兜底（双绑定 = 生产真实形态）
        with open(b_path, "w", encoding="utf-8") as fh:
            json.dump({"bindings": {"am": "/Game/ID4/ID_am", "": "ID_root_exec"}}, fh)

        stream_executor.reset_executor()
        ex = stream_executor.get_executor(job)
        check("C1a 绑定表加载并同步到入队器", ex.enqueuer.bindings.get("am") == "/Game/ID4/ID_am")

        # 预热就绪快照（执行器自己的 rule），首轮 run_next 即完成全量发现
        all_takes = []
        for grp in ("am", "pm", "_id", ""):
            base_dir = os.path.join(inbox, grp) if grp else inbox
            if not os.path.isdir(base_dir):
                continue
            for n in os.listdir(base_dir):
                p = os.path.join(base_dir, n)
                if os.path.isdir(p) and os.path.isfile(os.path.join(p, "take.json")):
                    all_takes.append(p)
        settle(ex.scanner.rule, all_takes)

        ran, _remaining = ex.run_next()
        id_tasks = [t for t in ex.queue.tasks.values() if t["kind"] == "id"]
        check("C1b ID 素材全部入队（rom 兜底 ×2 + _id ×1 = 3）",
              len(id_tasks) == 3, [t["key"] for t in id_tasks])

        # 持续推进直到队列空（mock 解算秒级）
        guard = 0
        while guard < 40:
            ran, _remaining = ex.run_next()
            if not ran:
                break
            guard += 1
        statuses = {t["key"]: t["status"] for t in ex.queue.tasks.values()}
        pendings = [k for k, s in statuses.items() if s == "pending"]
        check("C2a 全部任务到达终态（无 pending 滞留）", not pendings, pendings)
        done_perf = [t for t in ex.queue.tasks.values()
                     if t["kind"] == "perf" and t["status"] == "done"]
        check("C2b 表演任务含 duration（ETA 数据源）",
              all(t.get("duration") is not None for t in done_perf),
              [(t["key"], t.get("duration")) for t in done_perf])
        check("C2c am 组任务用组绑定身份（组优先于根兜底）",
              all(t.get("identity") == "/Game/ID4/ID_am" for t in done_perf
                  if t.get("group") == "am"),
              [(t["key"], t.get("identity")) for t in done_perf if t.get("group") == "am"])
        check("C2d 根目录/未绑组任务用根组兜底身份",
              all(t.get("identity") == "ID_root_exec" for t in done_perf
                  if t.get("group") != "am"),
              [(t["key"], t.get("identity")) for t in done_perf if t.get("group") != "am"])

        # C3 · 目录预检 + 失败复活
        mv_dir = make_take(am, "take_vanish")
        settle(ex.scanner.rule, [mv_dir])
        ex.run_next()                                            # 发现入队（am 组有绑定 → 直接入队）
        tk = ex.queue.get("am/take_vanish")
        check("C3a am 组有绑定 → 素材直接入队", tk is not None)
        shutil.move(mv_dir, os.path.join(tmp, "stash_vanish"))   # 排队后被移走
        if tk:
            ex._run_perf_task(tk)
            t = ex.queue.get("am/take_vanish")
            check("C3b 目录预检：failed 且原因准确",
                  t["status"] == "failed" and "素材目录不存在" in (t.get("error") or ""),
                  t.get("error"))
            shutil.move(os.path.join(tmp, "stash_vanish"), mv_dir)
            os.utime(mv_dir, None)                               # 模拟重新拷贝（mtime 更新）
            n = ex._revive_failed()
            check("C3c 素材放回 → 复活重排队",
                  n == 1 and ex.queue.get("am/take_vanish")["status"] == "pending", n)
            ex._revive_failed()
            check("C3d 防循环：目录未再变 → 不重复复活",
                  ex.queue.get("am/take_vanish")["status"] == "pending")

        # ════════ Part D · State 聚合契约 ════════
        print("── Part D 状态聚合 ──")
        ex.state.extra["pending_binding"] = len(ex.enqueuer.pending)
        snap = ex.state.snapshot()
        for field in ("state", "delivered", "synced", "failed", "queued", "stage_counts",
                      "identity_groups", "events", "eta_seconds", "current",
                      "pending_binding", "tasks_detail", "identity"):
            check("D1 字段存在: " + field, field in snap)
        detail = snap["tasks_detail"]
        for col in ("loading", "active", "done", "failed", "imported"):
            check("D2 tasks_detail 列存在: " + col, isinstance(detail.get(col), list))
        idk = [g for g in snap["identity_groups"] if g["dir"] == "am/"]
        check("D3 am 组矩阵（id=组绑定身份）", idk and idk[0]["id"] == "/Game/ID4/ID_am", idk)
        check("D4 identity 带组优先于根组（group=am）", snap["identity"].get("group") == "am",
              snap["identity"])
        done_keys = [d["key"] for d in detail["done"]]
        check("D5 done 列仅含 perf 交付", all("rom" not in k for k in done_keys), done_keys)
        id_done = [i["key"] for i in detail["imported"]]
        check("D6 imported 列含已导入 ID 素材", len(id_done) >= 1, id_done)

        # ════════ Part C5 · 绑定文件外部写入的热感知（GUI 直写 bindings.json）════════
        # 场景：引擎运行中，GUI 直接改写绑定文件 → 下一轮 run_next 必须热应用（冲刷 pending）
        with open(b_path, "w", encoding="utf-8") as fh:
            json.dump({"bindings": {"zz": "/Game/ID/ID_zz_new",
                                    "am": "/Game/ID4/ID_am"}}, fh)
        os.utime(b_path, None)                       # mtime 更新（外部写入的标志）
        ex.run_next()                                # 任意一轮（队列空也行）
        check("C5a 外部写入的组绑定被热应用",
              ex.enqueuer.bindings.get("zz") == "/Game/ID/ID_zz_new",
              ex.enqueuer.bindings)
        check("C5b mtime 已缓存（下轮不重复重载）",
              getattr(ex, "_bindings_mtime", None) == os.path.getmtime(b_path))
        # 再跑一轮不重载也不报错
        ex.run_next()
        check("C5c 重复轮次稳定", ex.enqueuer.bindings.get("zz") == "/Game/ID/ID_zz_new")

        # ════════ Part E · job 指纹重建 ════════
        print("── Part E job 指纹 ──")
        ex1 = stream_executor.get_executor(job)
        with open(job, "w", encoding="utf-8") as fh:            # 重写 job（[开始] 语义）
            json.dump({"mode": "depth", "stream_mode": "semi", "import_only": True,
                       "identity_path": "", "inbox": os.path.join(tmp, "inbox2"),
                       "capture_root": "/Game/Cap2",
                       "storage_path": "/Game/MH_Results",
                       "import_mode": "cpp", "import_footage": True,
                       "media_output_root": os.path.join(tmp, "media"),
                       "fbx_output_dir": os.path.join(tmp, "fbx"),
                       "stream_queue_path": q_path, "stream_bindings_path": b_path,
                       "stream_state_path": s_path,
                       "stream_stable_seconds": 0.05}, fh)
        ex2 = stream_executor.get_executor(job)
        check("E1 job 变更 → 单例重建", ex2 is not ex1)
        check("E2 新单例使用新配置", ex2.scanner.inbox == os.path.join(tmp, "inbox2"),
              ex2.scanner.inbox)
    finally:
        for mod, name, fn in restore:                           # 还原 mock（进程内整洁）
            try:
                setattr(mod, name, fn)
            except Exception:
                pass
        stream_executor.reset_executor()
        shutil.rmtree(tmp, ignore_errors=True)

    total = N_PASS[0] + N_FAIL[0]
    print("")
    print("=== 全局检测逻辑测试: {}/{} 项 {} ===".format(
        N_PASS[0], total, "全部通过" if OK[0] else "存在失败"))
    with open(os.path.join(HERE, "detection_chain_result.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
