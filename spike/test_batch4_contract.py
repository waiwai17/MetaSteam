# -*- coding: utf-8 -*-
"""批次4 · 层2 契约测试：snapshot 输出必须与 dashboard.html 引用字段逐一吻合

依据：web/stream_dashboard.html 的 render()/tick() 实际引用（手工提取，新增字段一并覆盖）。
另含真实数据冒烟：用批次3闭环验证的真实队列文件（done 任务带 duration）生成快照。
"""

import json
import os
import re
import sys

PKG_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(PKG_ROOT, "deploy", "Python"))

from MetaHumanSolverEngine.stream_queue import StreamQueue
from MetaHumanSolverEngine.stream_state import StreamStateWriter

DASHBOARD = os.path.join(PKG_ROOT, "deploy", "Python", "MetaHumanSolverEngine",
                         "web", "stream_dashboard.html")
REAL_QUEUE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stream_queue_test.json")

OK = [True]
LINES = []


def check(name, cond, detail=""):
    line = "[{}] {}{}".format("PASS" if cond else "FAIL", name,
                              ("  -> " + str(detail)) if (detail and not cond) else "")
    print(line)
    LINES.append(line)
    if not cond:
        OK[0] = False


def validate_contract(snap, tag):
    """契约清单（= dashboard render() 引用全集）"""
    # 顶栏
    check(tag + " state ∈ {running, idle}", snap.get("state") in ("running", "idle"),
          snap.get("state"))
    # 大数字 + 追平行
    for f in ("delivered", "synced", "failed", "queued"):
        check(tag + " {} 为非负 int".format(f),
              isinstance(snap.get(f), int) and snap.get(f) >= 0, snap.get(f))
    eta = snap.get("eta_seconds")
    check(tag + " eta_seconds 为 int|None（且非负）",
              eta is None or (isinstance(eta, int) and eta >= 0), eta)
    # 四步卡
    sc = snap.get("stage_counts")
    check(tag + " stage_counts 长度 4 且均 int>=0",
          isinstance(sc, list) and len(sc) == 4 and
          all(isinstance(v, int) and v >= 0 for v in sc), sc)
    # 分组矩阵
    igs = snap.get("identity_groups")
    check(tag + " identity_groups 为 list", isinstance(igs, list))
    for i, g in enumerate(igs or []):
        for f in ("dir", "id", "until"):
            check(tag + " grp[{}].{} 为 str".format(i, f), isinstance(g.get(f), str), g.get(f))
        check(tag + " grp[{}].now 为 bool".format(i), isinstance(g.get("now"), bool))
        for f in ("total", "done", "failed", "pending"):
            check(tag + " grp[{}].{} 为 int>=0".format(i, f),
                  isinstance(g.get(f), int) and g.get(f) >= 0, g.get(f))
        # 语义不变量：done+failed+pending ≤ total（差值 = importing/solving/exporting 活跃态）
        non_active = g["done"] + g["failed"] + g["pending"]
        check(tag + " grp[{}] 矩阵自洽（四态和≤total）".format(i), non_active <= g["total"],
              "non_active={} total={}".format(non_active, g["total"]))
    # 事件流
    evs = snap.get("events")
    check(tag + " events 为 list", isinstance(evs, list))
    LV_SET = {"运行", "完成", "失败", "待处理", "绑定"}
    for i, e in enumerate(evs[:50]):
        check(tag + " ev[{}].t/lv/tx/grp 齐".format(i),
              isinstance(e.get("t"), str) and isinstance(e.get("lv"), str) and
              isinstance(e.get("tx"), str) and isinstance(e.get("grp"), str), e)
        check(tag + " ev[{}].lv ∈ 筛选集".format(i), e.get("lv") in LV_SET, e.get("lv"))
    # updated_at
    check(tag + " updated_at 为 str", isinstance(snap.get("updated_at"), str))
    # 当前活跃任务（批次5 新增：C++ Stream Tab 当前行数据源）
    cur = snap.get("current")
    active_state = snap.get("state") == "running"
    if active_state:
        check(tag + " current 为 dict 且含 name/status（str）",
              isinstance(cur, dict) and isinstance(cur.get("name"), str)
              and isinstance(cur.get("status"), str), cur)
    else:
        check(tag + " 无活跃任务时 current=None", cur is None, cur)
    # 待绑定池计数（批次5.1：pending 素材可见性）
    pb = snap.get("pending_binding")
    check(tag + " pending_binding 为非负 int（默认 0）",
          isinstance(pb, int) and pb >= 0, pb)

    # ── P1 控制台数据源：身份带 + 三列明细 ──
    ident = snap.get("identity")
    check(tag + " identity 为 dict 且四字段为 str",
          isinstance(ident, dict) and all(isinstance(ident.get(k), str)
                                          for k in ("group", "asset", "state", "detail")), ident)
    VALID_ID_STATES = {"未就绪", "导入中", "已导入·待制作身份", "已绑定", "失败"}
    check(tag + " identity.state ∈ 合法状态集",
          ident.get("state") in VALID_ID_STATES, ident.get("state"))

    detail = snap.get("tasks_detail")
    check(tag + " tasks_detail 五列齐（loading/active/done/failed/imported 为 list）",
          isinstance(detail, dict) and all(isinstance(detail.get(k), list)
                                           for k in ("loading", "active", "done", "failed",
                                                     "imported")), detail)
    for col in ("loading", "active", "done", "failed", "imported"):
        for i, item in enumerate((detail or {}).get(col, [])[:5]):
            check(tag + " {}[{}] key/group 为 str".format(col, i),
                  isinstance(item.get("key"), str) and isinstance(item.get("group"), str), item)
    for i, item in enumerate((detail or {}).get("failed", [])[:5]):
        check(tag + " failed[{}] error 为 str（失败原因可读）".format(i),
              isinstance(item.get("error"), str), item)
    for i, item in enumerate((detail or {}).get("loading", [])[:5]):
        check(tag + " loading[{}] ready 为 bool 且 note 为 str".format(i),
              isinstance(item.get("ready"), bool) and isinstance(item.get("note"), str), item)
    for i, item in enumerate((detail or {}).get("done", [])[:5]):
        check(tag + " done[{}] duration/frames 可为 None 或数值".format(i),
              item.get("duration") is None or isinstance(item.get("duration"), (int, float)))
        # 不变量 I1 延伸：done 列只含 perf 交付（ID 不冒充交付物）
    check(tag + " done 列不含 id 任务（key 不以 _id/ 开头）",
          not any(str(it.get("key", "")).startswith("_id/")
                  for it in (detail or {}).get("done", [])), detail.get("done"))


def main():
    # ── A. 从 dashboard.html 提取引用字段，确认契约清单未漂移 ──
    with open(DASHBOARD, "r", encoding="utf-8") as fh:
        html = fh.read()
    refs = set(re.findall(r"stream\.([a-zA-Z_]+)", html))
    expected = {"state", "delivered", "synced", "failed", "queued",
                "stage_counts", "identity_groups", "events", "eta_seconds", "current",
                "pending_binding"}
    check("A dashboard 引用字段均在契约清单内（无未覆盖引用）",
          refs <= expected, "extra={}".format(refs - expected))
    grefs = set(re.findall(r"\bg\.([a-zA-Z_]+)", html))
    gexpected = {"dir", "id", "until", "now", "total", "done", "failed", "pending"}
    check("A 分组字段引用均在契约清单内", grefs <= gexpected, "extra={}".format(grefs - gexpected))

    # ── B. 合成快照契约校验 ──
    import tempfile
    import shutil
    tmp = tempfile.mkdtemp(prefix="mhs_c_")
    try:
        q = StreamQueue(os.path.join(tmp, "q.json"))
        q.enqueue("pm/t1", "D:/t1", kind="perf", identity="ID_pm", group="pm")
        q.set_status("pm/t1", "done", "", duration=100.0)
        q.enqueue("pm/t2", "D:/t2", kind="perf", identity="ID_pm", group="pm")
        q.set_status("pm/t2", "solving")           # 活跃任务 → current 正向分支
        q.enqueue("_id/rom", "D:/rom", kind="id", group="")
        w = StreamStateWriter(os.path.join(tmp, "s.json"), q, None)
        for lv, tx in (("待处理", "t2 排队中"), ("运行", "解算 t1"), ("完成", "交付 t1")):
            w.record(lv, tx, "pm")
        validate_contract(w.snapshot(), "B")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # ── C. 真实数据冒烟（批次3闭环的真实队列：1 条 done，duration 1301s 级）──
    if os.path.exists(REAL_QUEUE):
        rq = StreamQueue(REAL_QUEUE)
        rw = StreamStateWriter(os.path.join(os.path.dirname(REAL_QUEUE),
                                            "stream_state_real.json"), rq, None)
        snap = rw.snapshot()
        validate_contract(snap, "C")
        check("C 真实队列 done=1", snap["delivered"] == 1, snap["delivered"])
        with open(REAL_QUEUE, "r", encoding="utf-8") as fh:
            raw = json.load(fh)
        dur = (raw.get("tasks") or [{}])[0].get("duration")
        if isinstance(dur, (int, float)):
            check("C 队列 duration 已记录（>=600s）", dur >= 600, dur)
        else:
            # 批次3旧文件（无 duration 字段）：必须正确降级为 eta=None，不崩不猜
            check("C 旧队列文件兼容（无 duration → eta=None 降级）",
                  snap.get("eta_seconds") is None, snap.get("eta_seconds"))
    else:
        print("[SKIP] C 真实队列文件不存在: " + REAL_QUEUE)

    print("")
    print("=== 批次4契约测试: " + ("全部通过" if OK[0] else "存在失败") + " ===")
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "batch4_contract_result.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
