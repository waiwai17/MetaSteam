# -*- coding: utf-8 -*-
"""批次1 验证：W1 就绪判定器 + Q1 队列管理器（纯 Python，不依赖 UE）

验证项：
  W1-1 缺必需文件 -> 判未就绪
  W1-2 文件齐 -> 首轮快照(False) -> 稳定窗口后 -> 就绪(True)
  W1-3 拷贝中（大小增长）-> 持续未就绪
  Q1-1 入队 + 持久化 + 新实例恢复（模拟崩溃重启）
  Q1-2 幂等（重复入队跳过）
  Q1-3 状态推进 + 计数 + FIFO 取队头
"""

import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from MetaHumanSolverEngine.stream_watcher import DefaultRule, REQUIRED_FILES
from MetaHumanSolverEngine.stream_queue import StreamQueue, STATUS_PENDING, STATUS_SOLVING, STATUS_DONE

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


def make_take(root, name, with_files=True, payload_size=1024, with_mov=True):
    d = os.path.join(root, name)
    if not os.path.isdir(d):
        os.makedirs(d)
    if with_files:
        for f in REQUIRED_FILES:
            with open(os.path.join(d, f), "wb") as fh:
                fh.write(b"x" * payload_size)
        if with_mov:   # precheck_import 同样要求至少一个 *.mov
            with open(os.path.join(d, "face.mov"), "wb") as fh:
                fh.write(b"m" * payload_size)
    return d


def main():
    tmp = tempfile.mkdtemp(prefix="mhs_batch1_")
    try:
        # ── W1 就绪判定 ──
        emit("=== W1 就绪判定器（DefaultRule, stable=1.0s 加速测试）===")
        rule = DefaultRule(stable_seconds=1.0)

        # W1-1 缺文件
        empty = make_take(tmp, "take_missing", with_files=False)
        check("W1-1 缺必需文件 -> 未就绪", rule.is_ready(empty) is False)

        # W1-2 文件齐 -> 首轮快照 -> 稳定后就绪
        full = make_take(tmp, "take_ok")
        check("W1-2a 首轮（仅快照）-> 未就绪", rule.is_ready(full) is False)
        time.sleep(1.2)
        check("W1-2b 稳定窗口后 -> 就绪", rule.is_ready(full) is True)

        # W1-3 拷贝中：大小持续增长 -> 持续未就绪
        growing = make_take(tmp, "take_growing")
        rule2 = DefaultRule(stable_seconds=0.5)
        rule2.is_ready(growing)                       # 首轮快照
        time.sleep(0.6)
        with open(os.path.join(growing, "depth_data.bin"), "ab") as fh:
            fh.write(b"y" * 2048)                     # 模拟继续写入
        check("W1-3 大小增长 -> 仍未就绪", rule2.is_ready(growing) is False)

        # W1-4 缺 mov（三文件齐但未含视频）-> 未就绪
        no_mov = make_take(tmp, "take_no_mov", with_mov=False)
        check("W1-4 缺 *.mov -> 未就绪", rule.is_ready(no_mov) is False)

        # 策略名可观测
        emit("      策略描述: " + rule.describe())

        # ── Q1 队列 ──
        emit("")
        emit("=== Q1 队列管理器 ===")
        state_path = os.path.join(tmp, "queue_state.json")
        q1 = StreamQueue(state_path)

        ok1, msg1 = q1.enqueue("take_a", "/data/take_a", identity="ID_am", group="am/")
        ok2, msg2 = q1.enqueue("take_b", "/data/take_b", identity="ID_am", group="am/")
        check("Q1-1a 入队两条", ok1 and ok2)
        check("Q1-1b 文件已落盘", os.path.exists(state_path))

        # 模拟崩溃重启：新实例从文件恢复
        q2 = StreamQueue(state_path)
        check("Q1-1c 重启后队列恢复（2 条）", len(q2.tasks) == 2)
        check("Q1-1d 身份快照保留", q2.get("take_a").get("identity") == "ID_am")

        # Q1-2 幂等
        ok3, msg3 = q2.enqueue("take_a", "/data/take_a")
        check("Q1-2 重复入队被跳过", ok3 is False)
        emit("      说明: " + msg3)

        # Q1-3 状态推进 + 计数 + FIFO
        q2.set_status("take_a", STATUS_SOLVING)
        q2.set_status("take_a", STATUS_DONE)
        counts = q2.counts()
        check("Q1-3a 完成计数", counts.get("done") == 1 and counts.get("pending") == 1)
        head = q2.next_pending()
        check("Q1-3b FIFO 取队头为 take_b", head is not None and head.get("key") == "take_b")

        emit("")
        emit("队列状态: " + str(counts))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    emit("")
    emit("=== 批次1 总结: " + ("全部通过" if OK[0] else "存在失败项") + " ===")
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "batch1_result.txt")
    with open(out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
