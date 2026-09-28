# -*- coding: utf-8 -*-
"""批次2 验证：W2 扫描器 + W3 入队器（纯 Python，不依赖 UE）

验证项：
  W2-1 目录约定解析：_id/ → ID 事件；am/ pm/ → 分组；根目录 → 无分组
  W2-2 拷贝中（缺 mov）→ 不发现
  W2-3 幂等：同轮重复扫描不重复产出
  W3-1 无绑定 → pending 池
  W3-2 身份就绪（set_group_binding）→ 自动冲刷该分组 pending，身份快照正确
  W3-3 ID 素材产出 id_events（不直接入队）
  W3-4 超时降级：fallback 身份入队 + degraded 标记
  W3-5 积压不影响分配：入队时快照，后续切绑定不改已入队任务
"""

import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from MetaHumanSolverEngine.stream_watcher import Scanner, StreamEnqueuer, ID_DIR_NAME
from MetaHumanSolverEngine.stream_queue import StreamQueue, STATUS_PENDING

LINES = []
OK = [True]


def emit(msg):
    print(msg)
    LINES.append(msg)


def check(name, cond):
    if cond:
        emit("[PASS] " + name)
    else:
        emit("[FAIL] " + name)
        OK[0] = False


def make_take(parent, name, payload=1024, with_mov=True):
    d = os.path.join(parent, name)
    if not os.path.isdir(d):
        os.makedirs(d)
    for f in ("take.json", "depth_data.bin", "depth_metadata.mhaical"):
        with open(os.path.join(d, f), "wb") as fh:
            fh.write(b"x" * payload)
    if with_mov:
        with open(os.path.join(d, "face.mov"), "wb") as fh:
            fh.write(b"m" * payload)
    return d


def main():
    tmp = tempfile.mkdtemp(prefix="mhs_batch2_")
    try:
        inbox = os.path.join(tmp, "inbox")
        id_dir = os.path.join(inbox, ID_DIR_NAME)
        am_dir = os.path.join(inbox, "am")
        pm_dir = os.path.join(inbox, "pm")
        for d in (id_dir, am_dir, pm_dir):
            os.makedirs(d)

        make_take(id_dir, "rom_am_001")                       # ID 素材（_id 约定）
        make_take(am_dir, "take_a")                            # am 分组
        make_take(pm_dir, "take_b")                            # pm 分组
        make_take(inbox, "take_root")                          # 根目录直放
        make_take(am_dir, "take_partial", with_mov=False)      # 拷贝中（缺 mov）

        state_path = os.path.join(tmp, "queue_state.json")
        queue = StreamQueue(state_path)
        rule_stable = 1.0
        scanner = Scanner(inbox, rule=None, poll_seconds=5.0)
        # 用短稳定窗口的规则（测试加速；默认 3.0s 逻辑相同）
        from MetaHumanSolverEngine.stream_watcher import DefaultRule
        scanner.rule = DefaultRule(stable_seconds=rule_stable)

        # ── W2 扫描 ──
        emit("=== W2 扫描器 ===")
        scanner.scan_once()                     # 首轮：全部只记快照
        time.sleep(rule_stable + 0.2)
        events = scanner.scan_once()            # 第二轮：稳定即就绪

        ev_keys = sorted(e["key"] for e in events)
        check("W2-1a 发现 4 个就绪 take（partial 不算）", len(events) == 4)
        check("W2-1b partial 未出现", all("take_partial" not in k for k in ev_keys))
        by_key = dict((e["key"], e) for e in events)
        check("W2-1c _id 素材 kind=id", by_key.get("_id/rom_am_001", {}).get("kind") == "id")
        check("W2-1d am 分组 group=am", by_key.get("am/take_a", {}).get("group") == "am")
        check("W2-1e 根目录 group='' kind=perf", by_key.get("take_root", {}).get("group") == "" and
              by_key.get("take_root", {}).get("kind") == "perf")
        # 拷贝中的 take：缺 mov，就绪判定 False
        check("W2-2 缺 mov 的 take 未发现（拷贝中保护）", "am/take_partial" not in by_key)
        # 幂等：再扫一次不重复
        again = scanner.scan_once()
        check("W2-3 幂等（重复扫描零产出）", len(again) == 0)

        # ── W3 入队 ──
        emit("")
        emit("=== W3 入队器 ===")
        enq = StreamEnqueuer(queue, pending_timeout_seconds=3600.0)
        r1 = enq.process(events)
        id_task = queue.get("_id/rom_am_001")
        check("W3-3 ID 素材入队（kind=id）+ 产出 id_events",
              len(r1["id_events"]) == 1 and id_task is not None and id_task.get("kind") == "id")
        check("W3-1 无绑定 → 全部 pending", len(r1["pending"]) == 3 and len(r1["enqueued"]) == 0)

        # am 身份就绪 → 冲刷 am 的 pending
        enq.set_group_binding("am", "ID_am")
        a_task = queue.get("am/take_a")
        check("W3-2a am 绑定后 take_a 入队", a_task is not None)
        check("W3-2b 身份快照 = ID_am（入队时绑定）",
              a_task is not None and a_task.get("identity") == "ID_am")
        check("W3-2c 其他分组仍 pending", len(enq.pending) == 2)

        # pm 身份就绪
        enq.set_group_binding("pm", "ID_pm")
        b_task = queue.get("pm/take_b")
        check("W3-2d pm 绑定后 take_b 入队（快照 ID_pm）",
              b_task is not None and b_task.get("identity") == "ID_pm")

        # take_root 仍在 pending（无绑定）→ 超时降级（伪造超时时刻）
        check("W3-4a take_root 仍 pending", len(enq.pending) == 1)
        # 直接把入池时间改到过去（模拟超时）
        if enq.pending:
            ev0, _ts = enq.pending[0]
            enq.pending[0] = (ev0, time.time() - 7200.0)
        degraded = enq.check_pending_timeout()
        root_task = queue.get("take_root")
        check("W3-4b 超时降级入队（fallback=最近身份 ID_pm）",
              len(degraded) == 1 and root_task is not None and
              root_task.get("identity") == "ID_pm")
        check("W3-4c 降级后 pending 清空", len(enq.pending) == 0)

        # W3-5 积压不影响分配：切 am 绑定后，已入队的 take_a 身份不变
        enq.set_group_binding("am", "ID_pm_new")
        check("W3-5 已入队任务的身份快照不被后续绑定切换影响",
              queue.get("am/take_a").get("identity") == "ID_am")

        # ── W3-6 根组绑定兜底（批次5.2：单演员场景绑定根组=全局身份）──
        # 场景：全新队列，根目录 take + pm 组 take 均无绑定 → pending；
        # 绑定根组 '' → 全部 pending 冲刷且身份=根组身份
        q2 = StreamQueue(os.path.join(tmp, "q_root.json"))
        e2 = StreamEnqueuer(q2)
        r2 = e2.process([
            {"key": "take_r", "dir": "D:/r", "group": "", "kind": "perf"},
            {"key": "pm/take_p", "dir": "D:/p", "group": "pm", "kind": "perf"},
        ])
        check("W3-6a 无绑定时根目录+pm 组均 pending", len(r2["pending"]) == 2)
        e2.set_group_binding("", "ID_global")
        tr, tp = q2.get("take_r"), q2.get("pm/take_p")
        check("W3-6b 根组绑定冲刷全部 pending（含其他组）",
              tr is not None and tp is not None)
        check("W3-6c 身份=根组兜底身份（根目录组）", tr.get("identity") == "ID_global")
        check("W3-6d 身份=根组兜底身份（pm 组：组无绑定回落根组）",
              tp.get("identity") == "ID_global")
        # 组绑定优先于根组兜底
        e2.set_group_binding("pm", "ID_pm_only")
        check("W3-6e 已入队快照不变（仍是根组身份）", tp.get("identity") == "ID_global")
        r3 = e2.process([{"key": "pm/take_q", "dir": "D:/q", "group": "pm", "kind": "perf"}])
        tq = q2.get("pm/take_q")
        check("W3-6f 组绑定优先于根组兜底（新任务用组身份）",
              tq is not None and tq.get("identity") == "ID_pm_only")

        counts = queue.counts()
        emit("")
        emit("队列状态: " + str(counts))
        check("总结：队列 4 条（3 perf + 1 id）+ pending 0",
              counts.get("total") == 4 and counts.get("pending") == 4)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    emit("")
    emit("=== 批次2 总结: " + ("全部通过" if OK[0] else "存在失败项") + " ===")
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "batch2_result.txt")
    with open(out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
