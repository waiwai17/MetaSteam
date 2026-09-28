# -*- coding: utf-8 -*-
"""流式观测层（S1）——批次4

产出 stream_state.json（数据契约 = spike/stream_dashboard.html，Web/手机/面板共用）：
  state / delivered / synced / failed / queued / stage_counts(四步递减链) /
  identity_groups(含分配矩阵) / events(时间线) / eta_seconds(追平预估)

三个增量（2026-09-18 确认）：
  · 局域网手机可见 —— StreamWebServer（标准库 http.server，白名单路由，默认 0.0.0.0）
  · 身份分配矩阵   —— identity_groups 每组含 total/done/failed/pending
  · 追平 ETA       —— eta_seconds = 已完任务平均耗时 × 队列剩余

纪律：
  · 只读聚合：本模块不写队列，队列是唯一事实源，stream_state 是派生快照
  · 原子写（tmp + os.replace）：读方（Web 1s 轮询）永不读到半截 json
  · 事件环形缓冲（最近 200 条）随文件持久化，进程重启不丢时间线
"""

import json
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .stream_queue import STATUS_PENDING, STATUS_IMPORTING, STATUS_SOLVING, \
    STATUS_EXPORTING, STATUS_DONE, STATUS_FAILED

EVENTS_MAX = 200
ACTIVE_STATUSES = (STATUS_IMPORTING, STATUS_SOLVING, STATUS_EXPORTING)


def parse_group_from_name(name):
    """从素材名解析分组（am/pm）——用于身份自动绑定。

    规则：am/pm 必须是"独立词"（前后为分隔符 `_ . -` 或后接数字），
    不能是子串包含——否则 `context_answer__highamp__ang__e013` 会被误判为 am
    （highamp 含 am 子串）。

    例：
      0818_face_rom_pm_002_11      -> "pm"
      0818_face_rom_am_002_11      -> "am"
      context_answer__highamp__... -> "" （未命中，回落根组）
    """
    if not name:
        return ""
    m = re.search(r"(?:^|[_.\-])(am|pm)(?:[_.\-]|\d|$)", name, re.IGNORECASE)
    return m.group(1).lower() if m else ""


def _now_hm():
    return time.strftime("%H:%M")


def _now_ts():
    return time.strftime("%Y-%m-%d %H:%M:%S")


class StreamStateWriter(object):
    """队列 → stream_state.json 派生快照（原子写）+ 事件时间线。"""

    def __init__(self, path, queue, bindings=None):
        self.path = path
        self.queue = queue
        self.bindings = bindings       # StreamBindings 或 None
        self.events = []               # 最新在上：[{t, lv, tx, grp}]
        self.extra = {}                # 执行层注入的附加字段（如 pending_binding：待绑定池计数）
        self._load_events()

    # ── 事件 ──
    def record(self, lv, tx, group=""):
        """记录一条事件并立即落盘（状态变化触发写）。

        lv ∈ {运行, 完成, 失败, 待处理, 绑定}（Web 筛选按钮按 "失败"/"待处理" 过滤）。
        """
        self.events.insert(0, {"t": _now_hm(), "lv": lv, "tx": tx, "grp": group})
        if len(self.events) > EVENTS_MAX:
            del self.events[EVENTS_MAX:]
        self.write()

    def _load_events(self):
        """从现有文件恢复事件流（进程重启不丢时间线；损坏则从空开始）。"""
        if not self.path or not os.path.exists(self.path):
            return
        try:
            with open(self.path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
            events = data.get("events")
            if isinstance(events, list):
                self.events = [e for e in events
                               if isinstance(e, dict) and "t" in e and "lv" in e][:EVENTS_MAX]
        except Exception:
            self.events = []

    # ── 快照聚合 ──
    def _tasks_detail(self, tasks, active):
        """控制台明细列：loading（加载中）/ active（处理中）/ done（已完成）/ failed（已失败）。

        loading = 未就绪（拷贝中，来自 extra.unready） + 队列待处理（已就绪待解算）
                  + 待绑定池（已发现、等身份绑定）
        active  = importing/solving/exporting（百分比由 UI 侧合并 progress.json）
        done    = 仅 perf 交付（ID 不是交付物，不进此列）
        failed  = 失败任务（含原因）——此前不在任何展示名单，导致控制台 [盘]
                  实时视图把它误标为"已到达 · 未监听"（e011 事故）
        """
        loading = []
        unready = (self.extra or {}).get("unready") or []
        for item in unready:
            loading.append({"key": item.get("key", ""), "group": item.get("group", ""),
                            "ready": False, "note": "加载中（等待就绪）"})
        for t in self.queue.by_status(STATUS_PENDING):
            loading.append({"key": t.get("key", ""), "group": t.get("group", ""),
                            "ready": True, "note": "已加载 · 待解算"})
        # 待绑定池（已发现、等身份绑定）——不入队列也必须可见
        for item in ((self.extra or {}).get("pending_detail") or []):
            loading.append({"key": item.get("key", ""), "group": item.get("group", ""),
                            "ready": True, "note": "已加载 · 等待身份绑定"})
        act = [{"key": t.get("key", ""), "group": t.get("group", ""),
                "kind": t.get("kind", "perf"),
                "status": t.get("status", "")} for t in active]
        done = [{"key": t.get("key", ""), "group": t.get("group", ""),
                 "duration": t.get("duration"), "frames": t.get("frames")}
                for t in tasks if t.get("kind") != "id" and t.get("status") == STATUS_DONE]
        failed = [{"key": t.get("key", ""), "group": t.get("group", ""),
                   "error": (t.get("error") or "")[:80]}
                  for t in tasks if t.get("status") == STATUS_FAILED]
        # 已导入的 ID 素材（ROM）：ID 任务此前不在任何展示名单 → 控制台 [盘]
        # 实时视图把它的目录永远标成"已到达 · 等待接管"（e011 之后第二类误报）
        # note 按"该 ROM 对应组（或根组）是否已有绑定"给出准确状态——
        # 曾一律显示"等待制作/绑定身份"，身份绑好了还提示等待（自相矛盾）
        bmap = self.bindings.as_dict() if self.bindings else {}
        imported = []
        for t in tasks:
            if t.get("kind") == "id" and t.get("status") == STATUS_DONE:
                name = os.path.basename(str(t.get("dir", "")).rstrip("\\/"))
                grp = parse_group_from_name(name)
                bound = bool(bmap.get(grp) or bmap.get(""))
                imported.append({"key": t.get("key", ""), "group": t.get("group", ""),
                                 "note": "已导入 · 身份已绑定" if bound
                                         else "已导入 · 等待制作/绑定身份"})
        return {"loading": loading, "active": act, "done": done,
                "failed": failed, "imported": imported}

    def _identity(self, tasks, bindings):
        """身份带：分组 → 身份 + ID 素材状态（未就绪/导入中/已导入·待制作/已绑定）。"""
        id_tasks = [t for t in tasks if t.get("kind") == "id"]
        group = ""
        state = "未就绪"
        detail = ""
        if id_tasks:
            latest = max(id_tasks, key=lambda t: t.get("updated_at", 0))
            name = os.path.basename(str(latest.get("dir", "")).rstrip("\\/"))
            if latest.get("status") == STATUS_IMPORTING:
                state, detail = "导入中", name
            elif latest.get("status") == STATUS_FAILED:
                state, detail = "失败", (latest.get("error") or name)
            else:
                state, detail = "已导入·待制作身份", name
        # 已绑定 → 以绑定为准（身份就绪是最终态）
        # 注意：组名为空字符串是合法的"根组"（全局兜底绑定），不能用 `if group` 判空，
        # 否则根组绑定会被误显示为 <未绑定>（曾导致控制台身份带自相矛盾）
        # 展示优先级：具体组在前、根组在后——身份带跟随"最具体的绑定"；
        # 根组仅在它是唯一绑定时显示（解算取身份的实际规则始终是 组绑定 → 根组兜底，
        # 与此处展示无关）。曾按根组优先排序，用户已绑 pm 仍显示 (根) 造成困惑。
        bound_groups = [g for g, _i in (bindings or {}).items() if bindings.get(g)]
        # 优先：**当前正在处理的任务所属组**的绑定（解算 pm 时不能显示 am 的身份——实测误导）
        active = [t for t in tasks if t.get("status") in (STATUS_IMPORTING, STATUS_SOLVING, STATUS_EXPORTING)]
        active_group = active[0].get("group", "") if active else None
        if active_group is not None and bindings.get(active_group):
            group = active_group
        elif bound_groups:
            group = sorted(bound_groups, key=lambda g: (g == "", g))[0]
        # 选定了组 → 状态一律反映"该组已绑定"（曾因缩进把这段圈进 elif 分支：
        # 走"当前任务组"分支时状态不更新为已绑定）
        bound_asset = bindings.get(group, "")
        if bound_asset:
            state = "已绑定"
            detail = bound_asset
        # 来源：**只在能与"显示的身份"对应时才给** —— 曾直接取"最近一条 ID（ROM）素材"，
        # 多演员/多身份场景会出现"身份 am/…AM_ROM_6，来源 …PM_ROM_9"的自相矛盾
        # （实测）。匹配规则：来源素材名 == 绑定身份资产名（同演员同批命名）；
        # 没有绑定且工程里只有一条 ID 素材时，才退回"最近一条"。
        asset_name = os.path.basename(str(bindings.get(group, "")).rstrip("/\\"))
        source = ""
        if asset_name:
            for t in id_tasks:
                n = os.path.basename(str(t.get("dir", "")).rstrip("\\/"))
                if n == asset_name:
                    source = n
                    break
        elif len(id_tasks) == 1:
            source = os.path.basename(str(id_tasks[0].get("dir", "")).rstrip("\\/"))
        # 已绑定分组一览（多组时 UI 用它显示"已绑定 N 组"，而不是只报一个）
        bound = dict((g, a) for g, a in (bindings or {}).items() if a)
        # 若已有绑定 → 推断组 = 该绑定所在组；否则从 ROM 命名解析（am/pm）
        suggested = group if (group and bindings.get(group)) else parse_group_from_name(source)

        return {"group": group, "asset": bindings.get(group, ""),
                "state": state, "detail": detail,
                "source": source, "suggested_group": suggested,
                "bound": bound, "bound_count": len(bound)}

    def snapshot(self):
        """从队列聚合出契约快照（纯读，不落盘）。"""
        counts = self.queue.counts()
        tasks = list(self.queue.tasks.values())
        bindings = self.bindings.as_dict() if self.bindings else {}

        perf = [t for t in tasks if t.get("kind") != "id"]
        id_tasks = [t for t in tasks if t.get("kind") == "id"]
        delivered = [t for t in perf if t.get("status") == STATUS_DONE]
        failed = [t for t in tasks if t.get("status") == STATUS_FAILED]
        active = [t for t in tasks if t.get("status") in ACTIVE_STATUSES]

        # 四步卡（递减链条：累计通过数）
        # ID 就绪数 = max(已导入 ID 素材数, 已绑定身份数)——Semi 流程身份经由
        # [确认完成] 绑定（无 ID 素材入队），绑定数才是"身份就绪"的真实口径
        id_ready = len([t for t in id_tasks if t.get("status") == STATUS_DONE])
        if bindings:
            id_ready = max(id_ready, len(bindings))
        stage_counts = [
            id_ready,                                                                # ID 就绪
            len([t for t in tasks if t.get("status") != STATUS_PENDING]),      # 已导入
            len([t for t in tasks if t.get("status") in (STATUS_EXPORTING, STATUS_DONE)]),  # 解算通过
            len(delivered),                                                    # 导出通过
        ]

        # 追平 ETA：已完任务平均耗时 × 剩余 pending（无历史返回 None）
        durations = [t.get("duration") for t in delivered
                     if isinstance(t.get("duration"), (int, float)) and t["duration"] > 0]
        eta_seconds = None
        if durations and counts.get(STATUS_PENDING, 0) > 0:
            eta_seconds = round(sum(durations) / len(durations) * counts[STATUS_PENDING])

        # 当前活跃任务（段间快照语义：34min 阻塞期 UI 不更新，"截至 updated_at"）
        current = None
        if active:
            t = active[0]
            current = {"name": os.path.basename(str(t.get("dir", "")).rstrip("\\/")),
                       "status": t.get("status", "")}

        snap = {
            "state": "running" if active else "idle",
            "delivered": len(delivered),
            "synced": len(tasks),                       # 今日已同步（发现入队）数
            "failed": len(failed),
            "queued": counts.get(STATUS_PENDING, 0),
            "pending_binding": 0,                       # 待绑定池（executor 注入覆盖；0=无等待）
            "stage_counts": stage_counts,
            "identity_groups": self._groups(tasks, bindings, active),
            "identity": self._identity(tasks, bindings),          # 控制台身份带
            "tasks_detail": self._tasks_detail(tasks, active),    # 控制台三列
            "events": list(self.events),
            "eta_seconds": eta_seconds,
            "current": current,
            "updated_at": _now_ts(),
        }
        # 执行层注入覆盖（pending_binding 等——W3 待绑定池计数，UI 提示"等待身份绑定"）
        snap.update(self.extra or {})
        return snap

    def _groups(self, tasks, bindings, active):
        """身份分配矩阵：组 → 绑定身份 + total/done/failed/pending + 活跃标记。"""
        names = set(t.get("group", "") for t in tasks)
        names.update(bindings.keys())
        active_groups = set(t.get("group", "") for t in active)
        groups = []
        for name in sorted(names):
            gt = [t for t in tasks if t.get("group") == name]
            last_update = max([t.get("updated_at", 0) for t in gt] or [0])
            until = (time.strftime("%H:%M", time.localtime(last_update)) + " 止"
                     if last_update else "")
            groups.append({
                "dir": (name + "/") if name else "(根)",
                "id": bindings.get(name, "") or "<未绑定>",
                "until": until,
                "now": name in active_groups,
                "total": len(gt),
                "done": len([t for t in gt if t.get("status") == STATUS_DONE]),
                "failed": len([t for t in gt if t.get("status") == STATUS_FAILED]),
                "pending": len([t for t in gt if t.get("status") == STATUS_PENDING]),
            })
        return groups

    # ── 落盘（原子写）──
    def write(self):
        if not self.path:
            return
        directory = os.path.dirname(self.path)
        if directory and not os.path.isdir(directory):
            try:
                os.makedirs(directory)
            except Exception:
                return
        tmp_path = self.path + ".tmp"
        try:
            with open(tmp_path, "w", encoding="utf-8") as fh:
                json.dump(self.snapshot(), fh, ensure_ascii=False)
            # Windows：目标正被读方打开时 os.replace 瞬发共享冲突——短重试消除
            # （Web 端 1s 轮询读、record 状态变化写，冲突窗口 <1ms）
            for _attempt in range(6):
                try:
                    os.replace(tmp_path, self.path)
                    return
                except OSError:
                    time.sleep(0.002)
        except Exception:
            pass


# ══════════════════════════════════════════════════════════════════
# 增量 · 局域网 Web 服务（手机/任意设备浏览器可见）
# ══════════════════════════════════════════════════════════════════

class StreamWebServer(object):
    """极简白名单路由（标准库实现，无第三方依赖）：
        GET /                  -> 包内 web/stream_dashboard.html
        GET /stream_state.json -> json_dir/stream_state.json
        GET /progress.json     -> json_dir/progress.json
    其余一律 404；非 GET 一律 405（只读服务，无目录遍历面）。
    """

    _server = None
    _port = None
    _lock = threading.Lock()

    @classmethod
    def start(cls, port, json_dir):
        """启动（幂等：同端口已运行则跳过）。返回是否可用。"""
        with cls._lock:
            if cls._server is not None:
                return cls._port == port
            if not port:
                return False
            dashboard = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "web", "stream_dashboard.html")
            if not os.path.isfile(dashboard):
                return False
            # 注意：类体内 `json_dir = json_dir` 同名赋值会因 LOAD_NAME 不走闭包而 NameError，
            # 先起别名再进类体（dashboard 行为不同：dashboard 未在类体赋值，走 LOAD_CLASSDEREF）
            data_dir = json_dir

            class _Handler(BaseHTTPRequestHandler):
                dashboard_path = dashboard
                json_dir = data_dir

                def log_message(self, fmt, *args):      # 静默（不刷 UE 日志）
                    pass

                def _serve_file(self, path, ctype):
                    try:
                        with open(path, "rb") as fh:
                            body = fh.read()
                        self.send_response(200)
                        self.send_header("Content-Type", ctype)
                        self.send_header("Cache-Control", "no-store")
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)
                    except Exception:
                        self.send_error(404)

                def do_GET(self):
                    route = self.path.split("?", 1)[0]
                    if route in ("/", "/index.html"):
                        self._serve_file(self.dashboard_path, "text/html; charset=utf-8")
                    elif route == "/stream_state.json":
                        self._serve_file(os.path.join(self.json_dir, "stream_state.json"),
                                         "application/json; charset=utf-8")
                    elif route == "/progress.json":
                        self._serve_file(os.path.join(self.json_dir, "progress.json"),
                                         "application/json; charset=utf-8")
                    elif route == "/arrived.json":
                        self._serve_arrived()
                    else:
                        self.send_error(404)

                def _serve_arrived(self):
                    """盘面扫描：解算阻塞期 executor 不扫盘，state.json 看不到新拷入的
                    素材 → web 线程（不在阻塞路径上）代扫 inbox，网页显示 [盘] 行
                    （与 UE 控制台 / GUI 的 [盘] 行同语义）。"""
                    inbox, known = "", set()
                    try:
                        with open(os.path.join(self.json_dir, "job_stream.json"), "r", encoding="utf-8") as fh:
                            inbox = (json.load(fh) or {}).get("inbox", "")
                        with open(os.path.join(self.json_dir, "stream_state.json"), "r", encoding="utf-8") as fh:
                            td = (json.load(fh) or {}).get("tasks_detail", {}) or {}
                        for col in ("loading", "active", "done", "failed", "imported"):
                            for row in (td.get(col) or []):
                                if row.get("key"):
                                    known.add(row["key"])
                    except Exception:
                        pass
                    arrived = []
                    try:
                        if inbox and os.path.isdir(inbox):
                            for g in sorted(os.listdir(inbox)):
                                gpath = os.path.join(inbox, g)
                                if not os.path.isdir(gpath):
                                    continue
                                if os.path.isfile(os.path.join(gpath, "take.json")):
                                    if g not in known:
                                        arrived.append(g)
                                    continue
                                for name in sorted(os.listdir(gpath)):
                                    if not os.path.isfile(os.path.join(gpath, name, "take.json")):
                                        continue
                                    key = g + "/" + name
                                    if key not in known:
                                        arrived.append(key)
                    except Exception:
                        pass
                    body = json.dumps({"arrived": arrived}, ensure_ascii=False).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json; charset=utf-8")
                    self.send_header("Cache-Control", "no-store")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)

                def do_POST(self):
                    self.send_error(405)

                do_PUT = do_POST
                do_DELETE = do_POST

            try:
                cls._server = ThreadingHTTPServer(("0.0.0.0", int(port)), _Handler)
                cls._server.daemon_threads = True
                cls._port = int(port)
                threading.Thread(target=cls._server.serve_forever, daemon=True).start()
                return True
            except Exception:
                cls._server = None
                cls._port = None
                return False

    @classmethod
    def stop(cls):
        with cls._lock:
            if cls._server is not None:
                try:
                    cls._server.shutdown()
                except Exception:
                    pass
                cls._server = None
                cls._port = None
