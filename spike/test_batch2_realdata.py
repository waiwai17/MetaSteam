# -*- coding: utf-8 -*-
"""批次2 真实数据验证：用 Facial_Data（87 take，AM/PM_001/PM_002 三分组）当热文件夹

验证：
  1) 真实目录结构下扫描发现数 = 全部 take（含每组 1 个 ROM → id 事件）
  2) 分组解析正确（AM / PM_001 / PM_002）
  3) 三组各绑定身份 → 全部 perf 入队，身份快照正确
"""

import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "deploy", "Python"))

from MetaHumanSolverEngine.stream_watcher import Scanner, StreamEnqueuer, DefaultRule
from MetaHumanSolverEngine.stream_queue import StreamQueue


def count_takes(inbox):
    """直接磁盘统计的对照值（含 take.json 的目录，按顶层分组归档）。"""
    stats = {"total": 0, "groups": {}}
    for name in sorted(os.listdir(inbox)):
        top = os.path.join(inbox, name)
        if not os.path.isdir(top):
            continue
        if os.path.isfile(os.path.join(top, "take.json")):
            stats["total"] += 1
            stats["groups"][""] = stats["groups"].get("", 0) + 1
        else:
            for sub in sorted(os.listdir(top)):
                subpath = os.path.join(top, sub)
                if os.path.isdir(subpath) and os.path.isfile(os.path.join(subpath, "take.json")):
                    stats["total"] += 1
                    stats["groups"][name] = stats["groups"].get(name, 0) + 1
    return stats


def main():
    inbox = sys.argv[1] if len(sys.argv) > 1 else r"E:\Work\Mobu_maya\SSV_Test\Facial_Data"
    if not os.path.isdir(inbox):
        print("目录不存在: " + inbox)
        return 2

    truth = count_takes(inbox)
    print("磁盘对照：共 {} 个 take，分组 {}".format(truth["total"], truth["groups"]))

    # 稳定窗口 1s（87 个静态文件，首轮快照后 1s 即稳定）
    scanner = Scanner(inbox, rule=DefaultRule(stable_seconds=1.0))
    t0 = time.time()
    scanner.scan_once()                 # 首轮：快照
    time.sleep(1.2)
    events = scanner.scan_once()        # 第二轮：就绪
    elapsed = time.time() - t0
    print("扫描耗时（两轮含稳定等待）: {:.1f}s".format(elapsed))

    by_group = {}
    id_events = [e for e in events if e["kind"] == "id"]
    perf_events = [e for e in events if e["kind"] == "perf"]
    for e in perf_events:
        by_group[e["group"]] = by_group.get(e["group"], 0) + 1
    print("发现：perf={} id={} 分组(perf)={}".format(len(perf_events), len(id_events), by_group))
    print("id 事件: " + ", ".join(e["key"] for e in id_events))

    ok_total = len(events) == truth["total"]
    # 对照口径：id 事件（ROM）也归属其所在分组 —— 合并后与磁盘全量分组比对
    for e in id_events:
        by_group[e["group"]] = by_group.get(e["group"], 0) + 1
    ok_groups = by_group == truth["groups"]
    print("")
    print("[{}] 发现数 = 对照数 ({})".format("PASS" if ok_total else "FAIL", truth["total"]))
    print("[{}] 分组解析与磁盘一致".format("PASS" if ok_groups else "FAIL"))

    # 入队验证：三组绑定身份 → 全部 perf 入队
    state_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "queue_real2.json")
    if os.path.exists(state_path):
        os.remove(state_path)               # 每次干净起跑
    queue = StreamQueue(state_path)
    enq = StreamEnqueuer(queue)
    enq.set_group_binding("AM", "ID_am")
    enq.set_group_binding("PM_001", "ID_pm1")
    enq.set_group_binding("PM_002", "ID_pm2")
    r = enq.process(events)
    counts = queue.counts()
    print("[{}] 绑定后入队 {} 条（pending {}）".format(
        "PASS" if len(r["enqueued"]) == len(perf_events) and len(r["pending"]) == 0 else "FAIL",
        len(r["enqueued"]), len(r["pending"])))
    # 身份快照抽查
    sample = next((t for t in queue.tasks.values() if t["group"] == "AM"), None)
    ok_snap = sample is not None and sample["identity"] == "ID_am"
    print("[{}] 身份快照抽查（AM 组 → ID_am）".format("PASS" if ok_snap else "FAIL"))
    # 持久化恢复
    q2 = StreamQueue(state_path)
    ok_recover = len(q2.tasks) == len(queue.tasks)
    print("[{}] 重启恢复 {} 条".format("PASS" if ok_recover else "FAIL", len(q2.tasks)))
    print("")
    print("队列: " + str(counts))
    return 0 if (ok_total and ok_groups and ok_snap and ok_recover) else 1


if __name__ == "__main__":
    sys.exit(main())
