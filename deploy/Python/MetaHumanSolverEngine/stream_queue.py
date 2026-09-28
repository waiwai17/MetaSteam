# -*- coding: utf-8 -*-
"""流式调度层（Q）——批次1：Q1 队列管理器

设计要点：
  · 任务结构：{key, dir, kind, identity, status, ...}——identity 为"入队时快照绑定"
    （批次3 的上下午身份归属依赖它：积压不影响分配正确性）
  · 持久化：queue_state.json 原子写（tmp + os.replace），读方永远看到完整文件
  · 崩溃恢复：重启后从文件恢复队列，已入队/已完成任务不重复入队（幂等）
  · 纯 Python 无 UE 依赖（路径由调用方传入），便于脱离 UE 独立验证

待实现（批次3）：Q2 流水线执行器（取队头 + 复用现有解算/导出链路）、Q3 身份/分组管理
"""

import json
import os
import time
from collections import OrderedDict

# 任务状态机
STATUS_PENDING = "pending"
STATUS_IMPORTING = "importing"
STATUS_SOLVING = "solving"
STATUS_EXPORTING = "exporting"
STATUS_DONE = "done"
STATUS_FAILED = "failed"

ALL_STATUSES = (STATUS_PENDING, STATUS_IMPORTING, STATUS_SOLVING,
                STATUS_EXPORTING, STATUS_DONE, STATUS_FAILED)

# 素材类型
KIND_ID = "id"        # 身份（ROM/ID）素材
KIND_PERF = "perf"    # 表演素材


class StreamQueue(object):
    """流式任务队列（单一实例由 stream 流程持有）。"""

    def __init__(self, state_path):
        self.state_path = state_path
        self.tasks = OrderedDict()   # key -> task dict（保持入队顺序，FIFO 消费）
        self._load()

    # ── 持久化 ──
    def _load(self):
        if not self.state_path or not os.path.exists(self.state_path):
            return
        try:
            with open(self.state_path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
        except Exception:
            return
        tasks = data.get("tasks") or []
        for task in tasks:
            key = task.get("key")
            if key:
                self.tasks[key] = task

    def _save(self):
        """原子写：临时文件 + 替换（读方不会读到半截 json）。"""
        if not self.state_path:
            return
        directory = os.path.dirname(self.state_path)
        if directory and not os.path.isdir(directory):
            try:
                os.makedirs(directory)
            except Exception:
                return
        payload = {
            "updated": time.strftime("%Y-%m-%d %H:%M:%S"),
            "tasks": list(self.tasks.values()),
        }
        tmp_path = self.state_path + ".tmp"
        try:
            with open(tmp_path, "w", encoding="utf-8") as fh:
                json.dump(payload, fh, indent=2, ensure_ascii=False)
            os.replace(tmp_path, self.state_path)
        except Exception:
            pass

    # ── 入队（幂等：已存在则跳过）──
    def enqueue(self, key, take_dir, kind=KIND_PERF, identity="", group=""):
        """入队一条素材。返回 (是否新入队, 说明)。

        identity 为入队时刻的身份快照（批次3 由分组绑定表提供）；此处仅记录，不校验。
        """
        if not key:
            return False, "缺少 key"
        if key in self.tasks:
            return False, "已在队列中（幂等跳过）"
        now = time.time()
        self.tasks[key] = {
            "key": key,
            "dir": take_dir,
            "kind": kind,
            "group": group,
            "identity": identity,          # 入队时快照绑定——解算时不再查全局指针
            "status": STATUS_PENDING,
            "enqueued_at": now,
            "updated_at": now,
            "error": "",
        }
        self._save()
        return True, "已入队"

    # ── 状态推进 ──
    def set_status(self, key, status, error="", duration=None, frames=None):
        task = self.tasks.get(key)
        if task is None:
            return False
        if status not in ALL_STATUSES:
            return False
        task["status"] = status
        task["updated_at"] = time.time()
        task["error"] = error or ""
        if duration is not None:
            task["duration"] = round(float(duration), 1)   # 总耗时（观测层 ETA 用）
        if frames is not None:
            task["frames"] = int(frames)                   # 帧数（帧数标定 k 用）
        self._save()
        return True

    def bind_identity(self, key, identity):
        """补绑定身份（pending 池超时降级 / 人工改绑时用）。"""
        task = self.tasks.get(key)
        if task is None:
            return False
        task["identity"] = identity
        task["updated_at"] = time.time()
        self._save()
        return True

    def remove(self, key):
        if key in self.tasks:
            self.tasks.pop(key)
            self._save()
            return True
        return False

    # ── 查询 ──
    def get(self, key):
        return self.tasks.get(key)

    def by_status(self, status):
        return [t for t in self.tasks.values() if t.get("status") == status]

    def next_pending(self):
        """取队头待处理任务（FIFO）——批次3 执行器消费入口。"""
        for task in self.tasks.values():
            if task.get("status") == STATUS_PENDING:
                return task
        return None

    def counts(self):
        out = dict((s, 0) for s in ALL_STATUSES)
        for task in self.tasks.values():
            status = task.get("status")
            if status in out:
                out[status] += 1
        out["total"] = len(self.tasks)
        return out


def default_queue_path():
    """默认队列文件路径：<Saved>/Config/MetaHumanSolver/queue_state.json"""
    try:
        import unreal
        return os.path.join(unreal.Paths.project_saved_dir(),
                            "Config", "MetaHumanSolver", "queue_state.json")
    except Exception:
        return ""
