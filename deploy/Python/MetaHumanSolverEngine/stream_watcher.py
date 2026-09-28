# -*- coding: utf-8 -*-
"""流式发现层（W）——批次1：W1 就绪判定器（可插拔接口 + 默认实现）

设计要点：
  · ReadinessRule 为策略接口，实现可换（上环节协议确定后即插即用，其他模块零改动）
  · DefaultRule 为占位默认实现：必需文件齐 + 目录大小短窗口稳定
  · 判定原则：错误代价不对称——宁可晚判（下轮补扫捞回），不可早判（半拷贝进流水线 = 34 分钟白跑）

待实现（批次2）：W2 扫描器（轮询/diff/ID识别/分组）、W3 入队器（绑定/pending池）
"""

import os
import time

# take 必需文件（与 batch_runner.precheck_import 的约定保持一致；音频可选）
REQUIRED_FILES = ("take.json", "depth_data.bin", "depth_metadata.mhaical")
# 额外必需：至少一个该后缀文件存在（precheck_import 的 _check_take_files 同样要求 *.mov 存在）
REQUIRED_SUFFIXES = (".mov",)


class ReadinessRule(object):
    """就绪判定策略接口。

    is_ready(take_dir) -> bool  True = 可入队；False = 拷贝中 / 损坏
    describe()        -> str    策略名（供日志与 Web 观测当前生效策略）
    """

    def is_ready(self, take_dir):
        raise NotImplementedError

    def describe(self):
        return self.__class__.__name__


class DefaultRule(ReadinessRule):
    """默认就绪判定（占位实现）：必需文件齐 + 目录总大小在稳定窗口内不变。

    比"整个目录静默"更快：只需本 take 文件齐 + 大小不再增长。
    调用方式：扫描器每轮调用；首次记录快照返回 False，间隔 >= stable_seconds
    后二次比较，大小未变即判就绪。
    """

    def __init__(self, stable_seconds=3.0, required_files=REQUIRED_FILES,
                 required_suffixes=REQUIRED_SUFFIXES):
        self.stable_seconds = float(stable_seconds)
        self.required_files = tuple(required_files)
        self.required_suffixes = tuple(required_suffixes)
        self._snapshots = {}   # take_dir -> (size_sum, timestamp)

    def _has_required_suffixes(self, take_dir):
        """至少一个必需后缀文件存在（如 *.mov）——缺则视为未拷完。"""
        if not self.required_suffixes:
            return True
        try:
            for name in os.listdir(take_dir):
                lowered = name.lower()
                if any(lowered.endswith(s) for s in self.required_suffixes):
                    return True
        except OSError:
            return False
        return False

    # ── 目录总大小快照（仅统计文件，不递归子目录）──
    def _size_snapshot(self, take_dir):
        total = 0
        try:
            for name in os.listdir(take_dir):
                path = os.path.join(take_dir, name)
                if os.path.isfile(path):
                    total += os.path.getsize(path)
        except OSError:
            return -1
        return total

    def is_ready(self, take_dir):
        if not take_dir or not os.path.isdir(take_dir):
            return False

        # 1) 必需文件是否齐全（缺任一项 -> 判定未就绪并清除快照）
        for name in self.required_files:
            if not os.path.isfile(os.path.join(take_dir, name)):
                self._snapshots.pop(take_dir, None)
                return False

        # 1b) 必需后缀文件是否存在（如 *.mov 未拷完 -> 未就绪）
        if not self._has_required_suffixes(take_dir):
            self._snapshots.pop(take_dir, None)
            return False

        now = time.time()
        total = self._size_snapshot(take_dir)
        if total < 0:
            return False

        prev = self._snapshots.get(take_dir)
        if prev is None:
            # 首次观测：记录快照，本轮不判就绪（等待稳定窗口）
            self._snapshots[take_dir] = (total, now)
            return False

        prev_total, prev_time = prev
        if now - prev_time < self.stable_seconds:
            return False                      # 稳定窗口未到
        if total != prev_total:
            self._snapshots[take_dir] = (total, now)   # 仍在增长 -> 重置窗口
            return False
        return True                            # 文件齐 + 大小稳定 -> 就绪

    def describe(self):
        return "DefaultRule(files={} + {:.0f}s size stable)".format(
            "+".join(self.required_files), self.stable_seconds)


# ── 预留实现（上环节协议 / T2 实测确定后启用，替换即可，其他模块零改动）──
#
# class DoneMarkerRule(ReadinessRule):
#     """标记文件：拷贝完成后放置 .done —— 语义最明确（需上环节配合）"""
#     def __init__(self, marker=".done"):
#         self.marker = marker
#     def is_ready(self, take_dir):
#         return os.path.isfile(os.path.join(take_dir, self.marker))
#     def describe(self):
#         return "DoneMarkerRule({})".format(self.marker)
#
# class TempRenameRule(ReadinessRule):
#     """temp→rename 原子性：正式名出现即拷完（零配合，需 T2 验证上环节工具行为）"""
#     def is_ready(self, take_dir):
#         # 目录以正式名存在且不含临时后缀（如 .tmp / .part）
#         ...


# ══════════════════════════════════════════════════════════════════
# W2 · 扫描器（批次2）：轮询热文件夹 + 快照 diff + ID 识别 + 分组解析
# ══════════════════════════════════════════════════════════════════

# ID 素材目录名约定（上环节协议项：ID 素材放 inbox/_id/ 下）
ID_DIR_NAME = "_id"


def is_take_dir(path):
    """轻量 take 目录判定：目录且含 take.json（与现有引擎包 _is_take_dir 语义一致，无 UE 依赖）。"""
    return os.path.isdir(path) and os.path.isfile(os.path.join(path, "take.json"))


def _looks_like_rom(name):
    """命名兜底：名字含 rom 视为 ID（ROM）素材（_id 目录约定的补充）。"""
    return "rom" in name.lower()


class Scanner(object):
    """热文件夹扫描器。

    目录约定（与上环节的协议项）：
        inbox/_id/{take}/    ID（ROM）素材，kind="id"
        inbox/{组名}/{take}/ 分组素材（如 am/ pm/），group=组名
        inbox/{take}/        根目录直接放素材，group=""

    scan_once() 返回本轮"新就绪"事件列表（幂等：已产出的 key 不再重复产出；
    跨进程重启的幂等由 StreamQueue.enqueue 兜底——分层防重）。
    """

    def __init__(self, inbox, rule=None, poll_seconds=5.0):
        self.inbox = inbox
        self.rule = rule if rule is not None else DefaultRule()
        self.poll_seconds = float(poll_seconds)
        self._seen = set()   # 已产出过的 take key（进程内幂等）
        # 已见但未就绪（拷贝中/未稳定）：观测层 loading 列数据源——
        # 让 UI 能显示"素材出现了但还在传"，而不是"什么都没发生"
        self.unready = {}    # key -> {key, dir, group, kind, first_seen}

    def pending_unready(self):
        """当前"已见未就绪"的素材列表（loading 列用）。"""
        return list(self.unready.values())

    # ── 对外 ──
    def scan_once(self):
        """单次扫描：产出新就绪事件 [{key, dir, group, kind}]。"""
        events = []
        if not self.inbox or not os.path.isdir(self.inbox):
            return events
        for take_dir, group, kind in self._iter_candidates():
            key = self._key_of(take_dir)
            if key in self._seen:
                continue
            if not self.rule.is_ready(take_dir):
                # 首次见到但尚未就绪（拷贝中/未稳定）→ 记入 loading 列
                if key not in self.unready:
                    self.unready[key] = {"key": key, "dir": take_dir, "group": group,
                                         "kind": kind, "first_seen": time.time()}
                continue
            self._seen.add(key)
            self.unready.pop(key, None)   # 就绪 → 从 loading 列移除
            events.append({"key": key, "dir": take_dir, "group": group, "kind": kind})
        return events

    # ── 内部 ──
    def _key_of(self, take_dir):
        """key = 相对 inbox 的路径（分组内同名 take 不会冲突）。"""
        try:
            return os.path.relpath(take_dir, self.inbox).replace("\\", "/")
        except ValueError:
            return take_dir.replace("\\", "/")

    def _list_subdirs(self, parent):
        try:
            return [os.path.join(parent, n) for n in sorted(os.listdir(parent))
                    if os.path.isdir(os.path.join(parent, n))]
        except OSError:
            return []

    def _iter_candidates(self):
        """遍历候选 take 目录，产出 (take_dir, group, kind)。"""
        try:
            top_names = sorted(os.listdir(self.inbox))
        except OSError:
            return
        for name in top_names:
            top = os.path.join(self.inbox, name)
            if not os.path.isdir(top):
                continue
            if name == ID_DIR_NAME:
                # ID 目录：子目录均为 ID 素材（不做命名兜底——放进来就是 ID）
                for sub in self._list_subdirs(top):
                    if is_take_dir(sub):
                        yield (sub, "", "id")
            elif is_take_dir(top):
                # 根目录直接放 take（含 rom 命名兜底）
                yield (top, "", "id" if _looks_like_rom(name) else "perf")
            else:
                # 分组目录（任意名字）：子目录为 take（含 rom 命名兜底）
                for sub in self._list_subdirs(top):
                    if is_take_dir(sub):
                        sub_name = os.path.basename(sub)
                        yield (sub, name, "id" if _looks_like_rom(sub_name) else "perf")


# ══════════════════════════════════════════════════════════════════
# W3 · 入队器（批次2）：分组→身份绑定 + pending 池 + 超时降级
# ══════════════════════════════════════════════════════════════════

class StreamEnqueuer(object):
    """扫描事件 → 队列（入队时快照绑定身份）。

    上下午归属的核心解法：
      · 入队时从绑定表查该分组的身份并"快照"到任务上——之后无论何时解算，
        积压不影响分配正确性（积压只影响何时算完，不影响用哪个身份算）。
      · 分组暂无绑定（如新 ID 制作中）→ 任务进 pending 池；
        身份就绪（set_group_binding）自动冲刷该分组的 pending。
      · pending 超时（默认 30 分钟）→ 用最近就绪的身份降级入队 + 警告标记
        （防止 ID 制作失败导致素材永久滞留）。
    """

    def __init__(self, queue, pending_timeout_seconds=1800.0):
        self.queue = queue
        self.pending_timeout = float(pending_timeout_seconds)
        self.bindings = {}        # group -> identity（身份就绪时由上层写入）
        self.last_identity = ""   # 最近就绪身份（超时降级的 fallback）
        self.pending = []         # [(event, 入池时间戳)]

    # ── 绑定（身份就绪信号：Q3 / 面板确认完成后调用）──
    def set_group_binding(self, group, identity):
        """写入分组绑定；identity 非空时自动冲刷 pending。

        根组（''）绑定 = 全局兜底身份 → 冲刷全部 pending（单演员场景绑定一次全部生效）。
        """
        self.bindings[group] = identity
        if identity:
            self.last_identity = identity
            self.flush_pending(None if group == "" else group)

    def flush_pending(self, group=None):
        """冲刷 pending（group=None 冲全部）。返回本次入队的事件列表。

        身份解析同 process：组绑定 → 根组兜底（绑定根组后待绑定池即刻全量冲刷）。
        """
        flushed = []
        rest = []
        for ev, ts in self.pending:
            if group is None or ev.get("group") == group:
                identity = self._identity_for(ev.get("group", ""))
                ok, _msg = self.queue.enqueue(ev["key"], ev["dir"],
                                              ev.get("kind", "perf"),
                                              identity=identity,
                                              group=ev.get("group", ""))
                if ok:
                    flushed.append(ev)
            else:
                rest.append((ev, ts))
        self.pending = rest
        return flushed

    # ── 主入口 ──
    def process(self, events):
        """处理扫描事件。返回 {"enqueued","pending","id_events"}。

        ID（ROM）素材也入队（kind=id，由执行器 _run_id_task 导入后置 done，
        身份制作由 Semi 人工 + [确认完成] 绑定）；id_events 供上层即时提示。
        身份解析顺序：组绑定 → 根组('')绑定兜底（Semi 单演员场景：绑定根组=全局身份）
        → 无任何绑定进 pending 池。
        """
        out = {"enqueued": [], "pending": [], "id_events": []}
        for ev in events or []:
            if ev.get("kind") == "id":
                ok, _msg = self.queue.enqueue(ev["key"], ev["dir"], "id",
                                              identity="", group=ev.get("group", ""))
                if ok:
                    out["id_events"].append(ev)
                continue
            group = ev.get("group", "")
            identity = self._identity_for(group)
            if identity:
                ok, _msg = self.queue.enqueue(ev["key"], ev["dir"], "perf",
                                              identity=identity, group=group)
                if ok:
                    out["enqueued"].append(ev)
                # ok=False = 队列幂等跳过（重启后重复发现），不视为错误
            else:
                self.pending.append((ev, time.time()))
                out["pending"].append(ev)
        return out

    def _identity_for(self, group):
        """身份解析：组绑定优先，根组('')绑定兜底，再无则空（进 pending 池）。"""
        return self.bindings.get(group, "") or self.bindings.get("", "")

    # ── 超时降级 ──
    def check_pending_timeout(self, now=None):
        """超时降级：等待超过 pending_timeout 的任务用 fallback 身份入队。

        无 fallback（从未有身份就绪）时不降级——保持等待，由上层兜底。
        返回降级入队的事件列表（上层据此发警告）。
        """
        now = time.time() if now is None else now
        degraded = []
        rest = []
        for ev, ts in self.pending:
            if (now - ts >= self.pending_timeout) and self.last_identity:
                ok, _msg = self.queue.enqueue(ev["key"], ev["dir"], "perf",
                                              identity=self.last_identity,
                                              group=ev.get("group", ""))
                if ok:
                    degraded.append(ev)
            else:
                rest.append((ev, ts))
        self.pending = rest
        return degraded
