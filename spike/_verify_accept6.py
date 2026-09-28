# -*- coding: utf-8 -*-
"""accept6 环境预检：对素材源做一次真实扫描，确认 2 个 ROM（id）+ 6 条表演
（am×3 / pm×3）的分组与类型判定符合预期；只读检查，不改动任何状态文件。"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "deploy", "Python"))
from MetaHumanSolverEngine.stream_watcher import Scanner, DefaultRule
from MetaHumanSolverEngine.stream_state import parse_group_from_name

INBOX = r"E:\Work\Mobu_maya\MetaSol\spike\accept6_source"

sc = Scanner(INBOX, rule=DefaultRule(stable_seconds=3.0))
sc.scan_once()                      # 首轮记快照
time.sleep(3.2)
events = sc.scan_once()             # 稳定后应全部发现

print("扫描根: {}".format(INBOX))
print("发现 {} 条：".format(len(events)))
for e in sorted(events, key=lambda x: x["key"]):
    print("  {:<62} group={:<4} kind={}".format(e["key"], repr(e["group"]), e["kind"]))

ids = [e for e in events if e["kind"] == "id"]
perf = [e for e in events if e["kind"] == "perf"]
am = [e for e in perf if e["group"] == "am"]
pm = [e for e in perf if e["group"] == "pm"]
print("")
print("ID 素材 {} 条（识别分组: {}）".format(
    len(ids), [parse_group_from_name(os.path.basename(e["dir"].rstrip("\\/"))) for e in ids]))
print("表演素材 {} 条 = am {} + pm {}".format(len(perf), len(am), len(pm)))
print("幂等复扫（应为 0）-> {}".format(len(sc.scan_once())))

ok = (len(ids) == 2 and len(am) == 3 and len(pm) == 3)
print("")
print("预检结论: " + ("PASS" if ok else "FAIL"))
