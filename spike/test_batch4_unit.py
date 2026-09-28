# -*- coding: utf-8 -*-
"""批次4 · 层1 单元测试：StreamStateWriter 聚合 / ETA / 事件 / 原子写并发

严格断言：每个数值按手工推导的期望值逐字段比对（不是"跑通即可"）。
"""

import json
import os
import shutil
import sys
import tempfile
import threading
import time

PKG_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(PKG_ROOT, "deploy", "Python"))

from MetaHumanSolverEngine.stream_queue import StreamQueue
from MetaHumanSolverEngine.stream_state import StreamStateWriter

OK = [True]
LINES = []


def check(name, cond, detail=""):
    line = "[{}] {}{}".format("PASS" if cond else "FAIL", name,
                              ("  -> " + str(detail)) if (detail and not cond) else "")
    print(line)
    LINES.append(line)
    if not cond:
        OK[0] = False


def build_queue(path, tasks_spec):
    """构造已知队列。tasks_spec: [(key, kind, group, status, duration)]"""
    q = StreamQueue(path)
    for key, kind, group, status, duration in tasks_spec:
        q.enqueue(key, "D:/" + key, kind=kind, identity="", group=group)
        q.tasks[key]["status"] = status
        if duration is not None:
            q.tasks[key]["duration"] = duration
    q._save()
    return q


def main():
    tmp = tempfile.mkdtemp(prefix="mhs_b4_")
    try:
        # ═══ 案例1：多组多状态聚合（手工推导期望值）═══
        spec = [
            ("pm/a", "perf", "pm", "done", 100.0),
            ("pm/b", "perf", "pm", "failed", None),
            ("pm/c", "perf", "pm", "pending", None),
            ("pm/d", "perf", "pm", "solving", None),
            ("am/x", "perf", "am", "done", 300.0),
            ("_id/rom", "id", "", "done", None),
        ]
        q = build_queue(os.path.join(tmp, "q1.json"), spec)

        class FakeBindings(object):
            def as_dict(self):
                return {"am": "ID_am", "pm": "ID_pm"}

        w = StreamStateWriter(os.path.join(tmp, "s1.json"), q, FakeBindings())
        snap = w.snapshot()

        check("1a state=running（solving 存在）", snap["state"] == "running", snap["state"])
        check("1b delivered=2", snap["delivered"] == 2, snap["delivered"])
        check("1c synced=6", snap["synced"] == 6, snap["synced"])
        check("1d failed=1", snap["failed"] == 1, snap["failed"])
        check("1e queued=1（仅 pm/c）", snap["queued"] == 1, snap["queued"])
        check("1f stage_counts=[2,5,3,2]（ID就绪=绑定数2）",
              snap["stage_counts"] == [2, 5, 3, 2], snap["stage_counts"])
        # ID: max(id_done=1, 绑定数2)=2 | 已导入: 非 pending=5 | 解算通过: exporting+done=3 | 导出: done(perf)=2
        check("1g eta=平均(100,300)×pending(1)=200",
              snap["eta_seconds"] == 200, snap["eta_seconds"])
        groups = dict((g["dir"], g) for g in snap["identity_groups"])
        check("1h 两组+根组（_id 任务 group=''）", set(groups.keys()) == {"pm/", "am/", "(根)"},
              groups.keys())
        check("1i pm 矩阵 total=4 done=1 failed=1 pending=1",
              (groups["pm/"]["total"], groups["pm/"]["done"],
               groups["pm/"]["failed"], groups["pm/"]["pending"]) == (4, 1, 1, 1), groups["pm/"])
        check("1j pm 为当前活跃组", groups["pm/"]["now"] is True and groups["am/"]["now"] is False)
        check("1k pm 绑定 ID_pm", groups["pm/"]["id"] == "ID_pm")
        check("1l 根组 id=<未绑定>", groups["(根)"]["id"] == "<未绑定>")

        # ═══ 案例2：idle / 无历史 ETA ═══
        q2 = build_queue(os.path.join(tmp, "q2.json"),
                         [("z/1", "perf", "z", "pending", None)])
        w2 = StreamStateWriter(os.path.join(tmp, "s2.json"), q2, None)
        snap2 = w2.snapshot()
        check("2a 全 pending → idle", snap2["state"] == "idle")
        check("2b 无耗时历史 → eta=None", snap2["eta_seconds"] is None)
        check("2c stage_counts=[0,0,0,0]", snap2["stage_counts"] == [0, 0, 0, 0])

        q3 = build_queue(os.path.join(tmp, "q3.json"),
                         [("z/1", "perf", "z", "done", 500.0)])
        w3 = StreamStateWriter(os.path.join(tmp, "s3.json"), q3, None)
        check("2d pending=0 → eta=None（无积压）", w3.snapshot()["eta_seconds"] is None)

        # ═══ 案例3：事件环形缓冲 + 恢复 ═══
        w3b = StreamStateWriter(os.path.join(tmp, "s3b.json"), q3, None)
        for i in range(250):
            w3b.record("运行", "事件{}".format(i), "z")
        check("3a 环形缓冲裁剪到 200", len(w3b.events) == 200, len(w3b.events))
        check("3b 最新在上", w3b.events[0]["tx"] == "事件249")
        w3c = StreamStateWriter(os.path.join(tmp, "s3b.json"), q3, None)   # 重建=模拟重启
        check("3c 事件重启恢复", len(w3c.events) == 200 and w3c.events[0]["tx"] == "事件249")
        check("3d 事件字段齐全", all(set(("t", "lv", "tx", "grp")) <= set(e.keys())
                                    for e in w3c.events[:10]))

        # ═══ 案例4：文件损坏容错 ═══
        bad = os.path.join(tmp, "bad.json")
        with open(bad, "w", encoding="utf-8") as fh:
            fh.write("{not json at all")
        w4 = StreamStateWriter(bad, q3, None)
        check("4a 损坏文件不崩溃且事件清零", w4.events == [])

        # ═══ 案例5：原子写 · 并发读写无撕裂 ═══
        q5 = build_queue(os.path.join(tmp, "q5.json"),
                         [("k/{}".format(i), "perf", "k", "pending", None) for i in range(20)])
        s5 = os.path.join(tmp, "s5.json")
        w5 = StreamStateWriter(s5, q5, None)
        w5.write()
        stop = threading.Event()
        errors = []

        def writer_thread():
            i = 0
            while not stop.is_set():
                i += 1
                key = "k/{}".format(i % 20)
                q5.set_status(key, "solving" if i % 2 else "done", duration=float(i))
                try:
                    w5.record("运行", "压测{}".format(i), "k")
                except Exception as exc:
                    errors.append("write: " + str(exc))

        t = threading.Thread(target=writer_thread, daemon=True)
        t.start()
        def reader_once(path):
            """读方约定：失败立即重试一次（Windows replace 共享冲突毛刺，非撕裂）。
            返回 ("ok"|"retried_ok"|"torn"|"failed", exception_or_None)。"""
            for attempt in (0, 1):
                try:
                    with open(path, "r", encoding="utf-8") as fh:
                        json.load(fh)
                    return ("ok" if attempt == 0 else "retried_ok"), None
                except json.JSONDecodeError as exc:
                    return "torn", exc          # 撕裂=严重缺陷，重试也不允许
                except PermissionError:
                    if attempt == 0:
                        time.sleep(0.002)
                        continue
                    return "failed", exc_holder[0]
            return "failed", None

        exc_holder = [None]
        stats = {"ok": 0, "retried_ok": 0, "torn": 0, "failed": 0}
        for _ in range(300):
            try:
                with open(s5, "r", encoding="utf-8") as fh:
                    json.load(fh)
                stats["ok"] += 1
                continue
            except json.JSONDecodeError as exc:
                stats["torn"] += 1
                exc_holder[0] = exc
            except PermissionError as exc:
                exc_holder[0] = exc
                time.sleep(0.002)
                result, _ = reader_once(s5)
                stats[result] += 1
            time.sleep(0.001)
        stop.set()
        t.join(timeout=2)
        check("5a 零撕裂（JSONDecodeError=0）", stats["torn"] == 0,
              "torn={} err={}".format(stats["torn"], exc_holder[0]))
        check("5b 读方一次重试内 100% 可读",
              stats["failed"] == 0, stats)
        check("5c 写线程 0 异常", not errors, errors[:3])

        # ═══ 案例6：duration 经 set_status 写入 ═══
        q6 = build_queue(os.path.join(tmp, "q6.json"), [("k/0", "perf", "k", "solving", None)])
        q6.set_status("k/0", "done", "", duration=123.456)
        check("6a duration 落任务（round 1 位）", q6.tasks["k/0"]["duration"] == 123.5)

        # ═══ 案例7：extra 注入（待绑定池可见性，批次5.1）═══
        w7 = StreamStateWriter(os.path.join(tmp, "s7.json"), q6, None)
        check("7a 默认 pending_binding=0", w7.snapshot().get("pending_binding") == 0)
        w7.extra["pending_binding"] = 3
        check("7b extra 注入覆盖默认（3）", w7.snapshot().get("pending_binding") == 3)
        w7.write()
        with open(os.path.join(tmp, "s7.json"), "r", encoding="utf-8") as fh:
            on_disk = json.load(fh)
        check("7c 落盘含注入值", on_disk.get("pending_binding") == 3)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    print("=== 批次4单元测试: " + ("全部通过" if OK[0] else "存在失败") + " ===")
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "batch4_unit_result.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
