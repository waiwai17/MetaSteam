# -*- coding: utf-8 -*-
"""P1 严格验证 · 移动文件模拟加载（不需要 UE）

用真实素材（Facial_Data）通过 **shutil.move 逐文件移动** 模拟上环节同步：
  ① 移动部分文件（缺 mov/bin）→ loading 列应显示"加载中（等待就绪）"，且不入队
  ② 移动剩余文件 → 过稳定期 → 就绪 → 入队 → loading 列转为"已加载·待解算"
  ③ 模拟执行：importing → active 列
  ④ 完成（带 duration/frames）→ done 列 + k 标定收敛 + 追平预估
  ⑤ ID 素材：移动 → 入队 kind=id → 身份带状态流转（未就绪→导入中→已导入·待制作→已绑定）
  ⑥ 身份/三列/标定 的契约与降级
"""
import json
import os
import shutil
import sys
import tempfile
import time

PKG_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(PKG_ROOT, "deploy", "Python"))

from MetaHumanSolverEngine.stream_queue import StreamQueue, STATUS_IMPORTING, \
    STATUS_SOLVING, STATUS_DONE
from MetaHumanSolverEngine.stream_state import StreamStateWriter
from MetaHumanSolverEngine.stream_watcher import Scanner, StreamEnqueuer, DefaultRule
from MetaHumanSolverEngine.machine_k import MachineCalibration, DEFAULT_K

SRC = r"E:\Work\Mobu_maya\SSV_Test\Facial_Data\PM_002"
PERF = "context_answer__highamp__ang__e010_answer_001"
ID_TAKE = "0818_face_rom_pm_002_11"

OK = [True]
LINES = []


def check(name, cond, detail=""):
    line = "[{}] {}{}".format("PASS" if cond else "FAIL", name,
                              ("  -> " + str(detail)) if (detail and not cond) else "")
    print(line)
    LINES.append(line)
    if not cond:
        OK[0] = False


def move_partial(src_dir, dst_dir, only_small=True):
    """移动：only_small=True 只移小文件（模拟拷贝到一半，mov/bin 未到）"""
    os.makedirs(dst_dir, exist_ok=True)
    moved = []
    for name in sorted(os.listdir(src_dir)):
        p = os.path.join(src_dir, name)
        if not os.path.isfile(p):
            continue
        is_big = name.lower().endswith((".mov", ".bin"))
        if only_small and is_big:
            continue
        shutil.move(p, os.path.join(dst_dir, name))
        moved.append(name)
    return moved


def main():
    if not os.path.isdir(SRC):
        print("[SKIP] 源素材不存在: " + SRC)
        return 0
    work = tempfile.mkdtemp(prefix="mhs_p1_")
    src_copy = os.path.join(work, "src")     # 源副本（移动会消耗原文件，必须副本！）
    inbox = os.path.join(work, "inbox")
    os.makedirs(os.path.join(inbox, "pm"))
    os.makedirs(os.path.join(inbox, "_id"))
    shutil.copytree(os.path.join(SRC, PERF), os.path.join(src_copy, PERF))
    shutil.copytree(os.path.join(SRC, ID_TAKE), os.path.join(src_copy, ID_TAKE))

    q_path = os.path.join(work, "queue.json")
    s_path = os.path.join(work, "state.json")
    k_path = os.path.join(work, "machine_k.json")
    queue = StreamQueue(q_path)
    writer = StreamStateWriter(s_path, queue)
    calib = MachineCalibration(k_path)
    scanner = Scanner(inbox, rule=DefaultRule(stable_seconds=0.5))
    enq = StreamEnqueuer(queue)

    def pump():
        """一轮：扫描 → 入队 → 同步观测层（等价于 executor.scan_and_enqueue）"""
        events = scanner.scan_once()
        if events:
            enq.process(events)
        writer.extra["unready"] = scanner.pending_unready()
        writer.extra["pending_binding"] = len(enq.pending)
        writer.extra["pending_detail"] = [{"key": e.get("key", ""), "group": e.get("group", "")}
                                          for e, _ts in enq.pending]
        writer.extra["k_seconds_per_frame"] = calib.k
        writer.write()
        return events

    def pump_until_ready(max_rounds=6, interval=0.6):
        """轮询直到有素材就绪入队（就绪判定需两轮快照：补全后首轮更新快照、次轮判稳）"""
        got = []
        for _ in range(max_rounds):
            events = pump()
            if events:
                got.extend(events)
                return got
            time.sleep(interval)
        return got

    try:
        # ── ① 部分移动（拷贝中）──
        dst = os.path.join(inbox, "pm", PERF)
        moved = move_partial(os.path.join(src_copy, PERF), dst, only_small=True)
        check("①a 已移动部分文件（缺 mov/bin）", len(moved) > 0, moved)
        pump()
        with open(s_path, "r", encoding="utf-8") as fh:
            snap = json.load(fh)
        loading = snap["tasks_detail"]["loading"]
        check("①b loading 列出现该素材（拷贝中可见）",
              any(PERF in it["key"] for it in loading), loading)
        hit = next((it for it in loading if PERF in it["key"]), {})
        check("①c 状态=加载中（等待就绪）", hit.get("ready") is False, hit)
        check("①d 未入队（队列仍空）", queue.counts().get("total") == 0, queue.counts())

        # ── ② 移动剩余 → 稳定 → 就绪入队 ──
        move_partial(os.path.join(src_copy, PERF), dst, only_small=False)
        time.sleep(0.8)          # 稳定窗口 0.5s
        events = pump_until_ready()
        check("②a 就绪后产出事件并入队", len(events) == 1, events)
        with open(s_path, "r", encoding="utf-8") as fh:
            snap = json.load(fh)
        loading = snap["tasks_detail"]["loading"]
        hit = next((it for it in loading if PERF in it["key"]), {})
        check("②b loading 可见（已加载·等待身份绑定——尚无绑定）",
              hit.get("ready") is True and "等待身份绑定" in (hit.get("note") or ""), hit)
        check("②c 未绑定时队列为空、待绑定计数=1",
              snap["queued"] == 0 and snap["pending_binding"] == 1,
              (snap["queued"], snap["pending_binding"]))
        check("②d unready 已清空（就绪不再显示加载中）",
              not any(it.get("ready") is False for it in loading), loading)

        # ── ②e 绑定身份 → pending 池冲刷入队 ──
        enq.set_group_binding("pm", "ID_pm")
        writer.extra["pending_detail"] = []
        writer.extra["pending_binding"] = len(enq.pending)
        writer.write()
        with open(s_path, "r", encoding="utf-8") as fh:
            snap = json.load(fh)
        check("②e 绑定后冲刷入队（queued=1）", snap["queued"] == 1, snap["queued"])
        check("②f 入队快照=绑定身份 ID_pm",
              queue.next_pending() and queue.next_pending().get("identity") == "ID_pm")

        # ── ③/④ 执行与完成（含帧数标定）──
        task_key = queue.next_pending()["key"]
        queue.set_status(task_key, STATUS_IMPORTING)
        writer.extra["unready"] = scanner.pending_unready()
        writer.write()
        with open(s_path, "r", encoding="utf-8") as fh:
            snap = json.load(fh)
        check("③a active 列含该任务",
              any(task_key == it["key"] for it in snap["tasks_detail"]["active"]),
              snap["tasks_detail"]["active"])
        check("③b 处理中不再出现在 loading 列",
              not any(task_key == it["key"] for it in snap["tasks_detail"]["loading"]))

        queue.set_status(task_key, STATUS_SOLVING)
        queue.set_status(task_key, STATUS_DONE, "", duration=1301.0, frames=2351)
        calib.record(2351, 1301.0)
        writer.extra["k_seconds_per_frame"] = calib.k
        writer.write()
        with open(s_path, "r", encoding="utf-8") as fh:
            snap = json.load(fh)
        done = snap["tasks_detail"]["done"]
        check("④a done 列含该交付", any(task_key == it["key"] for it in done), done)
        check("④b done 含 duration/frames",
              done and done[0].get("duration") == 1301.0 and done[0].get("frames") == 2351, done)
        check("④c k 标定收敛到 0.553", abs(calib.k - 1301.0 / 2351) < 0.001, calib.k)
        check("④d 标定持久化", os.path.exists(k_path))
        with open(k_path, "r", encoding="utf-8") as fh:
            kdata = json.load(fh)
        check("④e 标定文件含样本", len(kdata.get("samples", [])) == 1, kdata)

        # ── ⑤ ID 素材与身份带 ──
        with open(s_path, "r", encoding="utf-8") as fh:
            snap = json.load(fh)
        check("⑤a 身份带初始=未就绪", snap["identity"]["state"] == "未就绪", snap["identity"])
        id_dst = os.path.join(inbox, "_id", ID_TAKE)
        move_partial(os.path.join(src_copy, ID_TAKE), id_dst, only_small=False)
        time.sleep(0.8)
        pump_until_ready()
        id_key = "_id/" + ID_TAKE
        id_task = queue.get(id_key)
        check("⑤b ID 素材入队（kind=id）", id_task is not None and id_task.get("kind") == "id",
              id_task)
        queue.set_status(id_key, STATUS_IMPORTING)
        writer.write()
        with open(s_path, "r", encoding="utf-8") as fh:
            snap = json.load(fh)
        check("⑤c 身份带=导入中", snap["identity"]["state"] == "导入中", snap["identity"])
        queue.set_status(id_key, STATUS_DONE)
        writer.write()
        with open(s_path, "r", encoding="utf-8") as fh:
            snap = json.load(fh)
        check("⑤d 身份带=已导入·待制作身份",
              snap["identity"]["state"] == "已导入·待制作身份", snap["identity"])
        check("⑤e ID 不进 done 列（不是交付物）",
              not any(it["key"] == id_key for it in snap["tasks_detail"]["done"]),
              snap["tasks_detail"]["done"])
        # 绑定后（writer 读取同一份绑定——模拟 executor.reload_bindings 后的状态）
        class _BindingsView(object):
            def as_dict(self):
                return dict(enq.bindings)

        writer.bindings = _BindingsView()
        enq.set_group_binding("pm", "ID_pm")
        writer.extra["k_seconds_per_frame"] = calib.k
        writer.write()
        with open(s_path, "r", encoding="utf-8") as fh:
            snap = json.load(fh)
        check("⑤f 绑定后身份带=已绑定（ID_pm）",
              snap["identity"]["state"] == "已绑定"
              and snap["identity"]["asset"] == "ID_pm", snap["identity"])

        # ── ⑥ 契约与降级 ──
        check("⑥a identity 字段齐（group/asset/state/detail）",
              set(("group", "asset", "state", "detail")) <= set(snap["identity"].keys()))
        check("⑥b tasks_detail 三列齐",
              set(("loading", "active", "done")) <= set(snap["tasks_detail"].keys()))
        check("⑥c 空标定回落 DEFAULT_K",
              MachineCalibration(os.path.join(work, "none.json")).k == DEFAULT_K)
        bad = os.path.join(work, "bad_k.json")
        with open(bad, "w", encoding="utf-8") as fh:
            fh.write("{not json")
        check("⑥d 标定文件损坏不崩（回落默认）",
              MachineCalibration(bad).k == DEFAULT_K)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("")
    print("=== P1 移动文件模拟验证: " + ("全部通过" if OK[0] else "存在失败") + " ===")
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "p1_loading_result.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
