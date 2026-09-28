# -*- coding: utf-8 -*-
"""批次1 真实数据验证：用真实 take 目录跑 DefaultRule + StreamQueue

用法：
  python test_batch1_realdata.py <take目录> [可选队列文件路径]

验证：
  1) 真实 take 的文件结构是否满足必需清单（三文件 + 至少一个 *.mov）
  2) 首轮快照 -> 稳定窗口后 -> 判就绪
  3) 真实路径入队 -> 持久化 -> 新实例恢复
"""

import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "deploy", "Python"))

from MetaHumanSolverEngine.stream_watcher import (DefaultRule, REQUIRED_FILES,
                                                  REQUIRED_SUFFIXES)
from MetaHumanSolverEngine.stream_queue import StreamQueue


def main():
    if len(sys.argv) < 2:
        print("用法: python test_batch1_realdata.py <take目录>")
        return 2
    take_dir = sys.argv[1]
    if not os.path.isdir(take_dir):
        print("目录不存在: " + take_dir)
        return 2

    print("=== 真实 take 文件结构 ===")
    print("目录: " + take_dir)
    total = 0
    for name in sorted(os.listdir(take_dir)):
        path = os.path.join(take_dir, name)
        if os.path.isfile(path):
            size = os.path.getsize(path)
            total += size
            mark = ""
            lowered = name.lower()
            if name in REQUIRED_FILES:
                mark = "  <- 必需"
            elif any(lowered.endswith(s) for s in REQUIRED_SUFFIXES):
                mark = "  <- 必需后缀"
            print("  {:<40} {:>12,} B{}".format(name, size, mark))
    print("  合计: {:,} B".format(total))

    print("")
    print("=== 就绪判定（DefaultRule, stable=1.0s）===")
    rule = DefaultRule(stable_seconds=1.0)
    r1 = rule.is_ready(take_dir)
    print("  首轮（仅快照）      : " + ("就绪" if r1 else "未就绪"))
    time.sleep(1.2)
    r2 = rule.is_ready(take_dir)
    print("  稳定窗口后          : " + ("就绪" if r2 else "未就绪"))
    print("  策略: " + rule.describe())

    print("")
    print("=== 队列入队 + 持久化恢复（真实路径）===")
    state_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "queue_real.json")
    key = os.path.basename(take_dir.rstrip("\\/"))
    q1 = StreamQueue(state_path)
    ok, msg = q1.enqueue(key, take_dir, kind="id" if ("rom" in key.lower() or "id" in key.lower()) else "perf",
                         identity="", group="")
    print("  入队 {} : {} ({})".format(key, ok, msg))
    q2 = StreamQueue(state_path)
    recovered = q2.get(key)
    ok_recover = recovered is not None and recovered.get("dir") == take_dir
    print("  重启恢复            : " + ("通过" if ok_recover else "失败"))
    print("  队列计数            : " + str(q2.counts()))

    passed = (not r1) and r2 and ok_recover
    print("")
    print("=== 真实数据验证: " + ("通过" if passed else "失败") + " ===")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
