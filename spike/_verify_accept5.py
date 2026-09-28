# -*- coding: utf-8 -*-
"""accept5 环境预检：对新素材源做一次真实扫描，确认分组/类型/就绪判定符合预期
（不改动任何状态文件，只读检查）。"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "deploy", "Python"))
from MetaHumanSolverEngine.stream_watcher import Scanner, DefaultRule

INBOX = r"E:\Work\Mobu_maya\MetaSol\spike\accept5_source"

rule = DefaultRule(stable_seconds=3.0)
sc = Scanner(INBOX, rule=rule)

# 首轮：记录快照（不判就绪）；等稳定窗口后二轮：应有发现
sc.scan_once()
time.sleep(3.2)
events = sc.scan_once()

print("扫描根: {}".format(INBOX))
print("发现 {} 条：".format(len(events)))
for e in sorted(events, key=lambda x: x["key"]):
    print("  {:<70} group={:<4} kind={}".format(e["key"], repr(e["group"]), e["kind"]))
print("")
print("第二次扫描（幂等）应为 0 条 -> {}".format(len(sc.scan_once())))
