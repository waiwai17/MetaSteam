# -*- coding: utf-8 -*-
"""流式执行层（Q2 执行器 + Q3 绑定表）——批次3

驱动模型：队列消费 + 幂等补扫（C++ 定时器每次提交 stream_next()）：
  1. 补扫热文件夹（Scanner 幂等，新素材经 Enqueuer 入队，身份取自绑定表快照）
  2. 取队头 pending 任务执行（阻塞）：
       perf 任务：导入（幂等）→ 定位 CD → 解算（run_depth_shot 阻塞）→ 导出（export_shot_outputs）
       id  任务：仅导入（供 Semi 身份制作），标记 done 等待绑定确认
  3. 更新队列状态 → 返回 (ran, remaining)——C++ 端据此决定是否继续提交

上下午身份归属：入队时快照（Enqueuer），积压不影响分配正确性。
本模块跑在 UE 内（导入/解算/导出为 UE Editor API）。
"""

import json
import os
import time

from . import batch_runner
from . import pipeline
from . import progress
from . import versions
from .stream_queue import StreamQueue, STATUS_PENDING, STATUS_IMPORTING, \
    STATUS_SOLVING, STATUS_EXPORTING, STATUS_DONE, STATUS_FAILED
from .stream_watcher import Scanner, StreamEnqueuer, DefaultRule
from .stream_state import StreamStateWriter, StreamWebServer
from .machine_k import MachineCalibration


# ══════════════════════════════════════════════════════════════════
# Q3 · 分组→身份绑定表（持久化）
# ══════════════════════════════════════════════════════════════════

class StreamBindings(object):
    """group -> identity 持久化绑定表（json 原子写）。

    写入方：面板 [确认完成]（Semi 身份制作完成后人工确认）→ bridge.stream_set_binding。
    读取方：StreamExecutor（构造/重载时同步到 Enqueuer 的内存绑定）。
    """

    def __init__(self, path):
        self.path = path
        self._data = {}
        self.reload()

    def reload(self):
        self._data = {}
        if not self.path or not os.path.exists(self.path):
            return
        try:
            with open(self.path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
            if isinstance(data, dict):
                self._data = dict((str(k), str(v))
                                  for k, v in (data.get("bindings") or {}).items())
        except Exception:
            self._data = {}

    def _save(self):
        if not self.path:
            return
        directory = os.path.dirname(self.path)
        if directory and not os.path.isdir(directory):
            try:
                os.makedirs(directory)
            except Exception:
                return
        payload = {"updated": time.strftime("%Y-%m-%d %H:%M:%S"),
                   "bindings": self._data}
        tmp_path = self.path + ".tmp"
        try:
            with open(tmp_path, "w", encoding="utf-8") as fh:
                json.dump(payload, fh, indent=2, ensure_ascii=False)
            os.replace(tmp_path, self.path)
        except Exception:
            pass

    def set(self, group, identity):
        """写入绑定（identity 为空 = 清除该分组绑定）。"""
        if identity:
            self._data[group] = identity
        else:
            self._data.pop(group, None)
        self._save()

    def get(self, group):
        return self._data.get(group, "")

    def as_dict(self):
        return dict(self._data)


# ══════════════════════════════════════════════════════════════════
# Q2 · 流式执行器（补扫 + 取队头 + 导入/解算/导出）
# ══════════════════════════════════════════════════════════════════

def _read_total_frames(state_dir):
    """读 progress.json 的 total_frames（C++ 心跳线程写，与目标文件同目录）。

    读不到返回 None——标定与预估均可降级，绝不阻断主流程。
    """
    path = os.path.join(state_dir or "", "progress.json")
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        frames = data.get("total_frames")
        return int(frames) if frames else None
    except Exception:
        return None


def _default_stream_paths():
    """默认流式文件路径：<Saved>/Config/MetaHumanSolver/{queue_state,stream_bindings}.json"""
    base = ""
    try:
        import unreal
        base = os.path.join(unreal.Paths.project_saved_dir(),
                            "Config", "MetaHumanSolver")
    except Exception:
        base = os.path.join(os.path.expanduser("~"), ".meta_human_solver")
    return (os.path.join(base, "queue_state.json"),
            os.path.join(base, "stream_bindings.json"))


class StreamExecutor(object):
    """流式协调器：W（发现）→ Q1（队列）→ Q2（执行）一体化。

    生命周期：UE 进程内单例（bridge.stream_next 首次调用创建）；
    C++ 定时器逐次提交 run_next()，阻塞段间返回让 UI 刷新。
    """

    def __init__(self, cfg):
        self.cfg = cfg
        q_path, b_path = _default_stream_paths()
        self.queue = StreamQueue(cfg.get("stream_queue_path") or q_path)
        self.bindings = StreamBindings(cfg.get("stream_bindings_path") or b_path)
        # W 层（进程内长活：Scanner._seen 与 Enqueuer 内存状态跨调用保持）
        self.scanner = Scanner(
            cfg.get("inbox", ""),
            rule=DefaultRule(stable_seconds=float(cfg.get("stream_stable_seconds", 3.0))))
        self.enqueuer = StreamEnqueuer(
            self.queue,
            pending_timeout_seconds=float(cfg.get("stream_pending_timeout", 1800.0)))
        # S1 观测层：状态快照（与队列同目录）+ Web 服务（手机/局域网可见，0=禁用）
        state_path = cfg.get("stream_state_path") or os.path.join(
            os.path.dirname(self.queue.state_path), "stream_state.json")
        # S-B 帧数标定（追平预估的换算系数，换机器自动收敛）
        self.calib = MachineCalibration(os.path.join(
            os.path.dirname(self.queue.state_path), "machine_k.json"))
        self.state = StreamStateWriter(state_path, self.queue, self.bindings)
        web_port = int(cfg.get("stream_dashboard_port", 0) or 0)
        if web_port:
            StreamWebServer.start(web_port, os.path.dirname(state_path) or ".")
        self._sync_bindings()
        self._clear_stale_active()
        self.state.write()

    def _clear_stale_active(self):
        """启动清理：上次会话中断遗留的"进行中"状态回退为待处理。

        为什么必需：进程退出/监听停止时，正在导入/解算的任务状态会残留为
        importing/solving/exporting，控制台会永久显示"处理中"（僵尸态）。
        回退为 pending 后由幂等保护重新执行（已完成的不会重跑）。
        """
        stale = 0
        for key, task in list(self.queue.tasks.items()):
            if task.get("status") in (STATUS_IMPORTING, STATUS_SOLVING, STATUS_EXPORTING):
                self.queue.set_status(key, STATUS_PENDING)
                stale += 1
        if stale:
            progress.log("[MHS_STREAM] 清理上次中断遗留的进行中任务 {} 条 → 重新排队".format(stale))

    # ── 绑定 ──
    def _sync_bindings(self):
        """绑定文件 → Enqueuer 内存（set_group_binding 同时冲刷 pending——身份就绪信号）。"""
        for group, identity in self.bindings.as_dict().items():
            self.enqueuer.set_group_binding(group, identity)

    def reload_bindings(self):
        self.bindings.reload()
        self._sync_bindings()
        try:
            self._bindings_mtime = os.path.getmtime(self.bindings.path)
        except OSError:
            self._bindings_mtime = None

    def _check_bindings_touched(self):
        """外部（GUI/面板）直接改写绑定文件的热感知：mtime 变了即重载。

        没有这个检查，GUI 写 bindings.json 后运行中的引擎永远不冲刷 pending——
        用户必须重启才生效（不可接受）。放在 run_next 每轮开头（一次 stat，开销可忽略）。
        """
        try:
            mtime = os.path.getmtime(self.bindings.path)
        except OSError:
            mtime = None
        if mtime != getattr(self, "_bindings_mtime", None):
            self.reload_bindings()
            progress.log("[MHS_STREAM] 绑定文件已更新（外部写入）→ 热应用并冲刷 pending")

    # ── 补扫（幂等）──
    def scan_and_enqueue(self):
        """补扫热文件夹 → 新素材入队（身份=绑定快照）。返回 (入队数, id事件列表)。"""
        events = self.scanner.scan_once()
        if not events:
            return 0, []
        result = self.enqueuer.process(events)
        degraded = self.enqueuer.check_pending_timeout()
        # 观测层注入（record 前——事件落盘时即为最新值）
        self.state.extra["pending_binding"] = len(self.enqueuer.pending)
        pending_groups = {}
        pending_detail = []
        for _ev, _ts in self.enqueuer.pending:
            g = _ev.get("group", "")
            pending_groups[g] = pending_groups.get(g, 0) + 1
            pending_detail.append({"key": _ev.get("key", ""), "group": g})
        self.state.extra["pending_groups"] = pending_groups
        # 待绑定池明细（控制台 loading 列："已加载·等待身份绑定"）
        self.state.extra["pending_detail"] = pending_detail
        # loading 列：已见未就绪（拷贝中）素材
        self.state.extra["unready"] = self.scanner.pending_unready()
        self.state.extra["k_seconds_per_frame"] = self.calib.k
        if degraded:
            progress.warn("[MHS_STREAM] pending 超时降级 {} 条（用最近就绪身份入队，请核查身份制作）".format(
                len(degraded)))
            for ev in degraded:
                self.state.record("待处理", "{} 超时降级入队（旧身份）".format(ev["key"]), ev.get("group", ""))
        for ev in result.get("id_events", []):
            progress.log("[MHS_STREAM] ID 素材就绪: {}（等待身份制作与绑定确认）".format(ev["key"]))
            self.state.record("待处理", "ID 素材就绪: {}".format(ev["key"]), ev.get("group", ""))
        for ev in result.get("enqueued", []):
            self.state.record("待处理", "{} 排队中（identity: {}）".format(
                ev["key"], ev.get("identity") or "<待绑定>"), ev.get("group", ""))
        for ev in result.get("pending", []):
            self.state.record("待处理", "{} 已发现 · 等待身份绑定（ID制作区点[确认完成]）".format(
                ev["key"]), ev.get("group", ""))
        return len(result.get("enqueued", [])), result.get("id_events", [])

    # ── 主入口（C++ 定时器逐次提交）──
    def run_next(self):
        """补扫 → 取队头 → 执行一条（阻塞）。返回 (ran: bool, remaining: int)。

        ran=False 表示队列空（C++ 停止提交或降低轮询频率）；
        remaining=本次执行后仍待处理数。
        """
        try:
            self._check_bindings_touched()
        except Exception as exc:
            progress.warn("[MHS_STREAM] 绑定热感知异常（跳过本轮）: {}".format(exc))
        try:
            self.scan_and_enqueue()
        except Exception as exc:
            progress.warn("[MHS_STREAM] 补扫异常（跳过本轮，下轮重试）: {}".format(exc))
        try:
            self._revive_failed()
        except Exception as exc:
            progress.warn("[MHS_STREAM] 失败复活检查异常（跳过本轮）: {}".format(exc))

        # 待绑定池计数注入观测层（UI/Web 显示"等待身份绑定"而非"等待素材"）
        self.state.extra["pending_binding"] = len(self.enqueuer.pending)

        task = self.queue.next_pending()
        if task is None:
            return False, 0

        if task.get("kind") == "id":
            self._run_id_task(task)
        else:
            self._run_perf_task(task)

        remaining = len(self.queue.by_status(STATUS_PENDING))
        return True, remaining

    def _revive_failed(self):
        """失败任务复活检查：素材被移走后放回 → 自动重新排队（e011 事故修复）。

        判据：status=failed 且 目录存在 且 就绪 且 **目录 mtime > 失败时刻**。
        mtime 判据防循环：持续失败的素材目录不变 → 只复活一次；重新拷贝才会再触发。
        """
        revived = 0
        for t in self.queue.by_status(STATUS_FAILED):
            d = t.get("dir", "")
            if not d or not os.path.isdir(d):
                continue
            try:
                if os.path.getmtime(d) <= t.get("updated_at", 0):
                    continue
            except OSError:
                continue
            if not self.scanner.rule.is_ready(d):
                continue
            self.queue.set_status(t["key"], STATUS_PENDING)
            self.state.record("待处理", "{} 素材已恢复，重新排队（此前失败: {}）".format(
                t["key"], (t.get("error") or "")[:60]), t.get("group", ""))
            progress.log("[MHS_STREAM] 失败任务复活: {}（素材已放回热文件夹）".format(t["key"]))
            revived += 1
        return revived

    # ── 内部 ──
    def _group_base(self, group):
        root = self.cfg["capture_root"].rstrip("/")
        return "{}/{}".format(root, group) if group else root

    def _run_id_task(self, task):
        """ID 素材：仅导入（身份制作 = Semi 人工 + 面板确认，批次4+ 接线）。"""
        key = task["key"]
        name = os.path.basename(task["dir"].rstrip("\\/"))
        group = task.get("group", "")
        cfg = self.cfg
        # ID 素材落位：identity_import_root（配置项"身份导入"）→ 回落 capture_root
        id_root = (cfg.get("identity_import_root") or cfg["capture_root"]).rstrip("/")
        base = "{}/{}".format(id_root, group) if group else id_root
        try:
            if not os.path.isdir(task["dir"]):
                raise RuntimeError("素材目录不存在（可能在排队后被移走，放回后自动重新排队）: {}".format(
                    task["dir"]))
            # 导入幂等："已导入过"的查找根必须是 ID 导入根目录（identity_import_root）——
            # 曾误用 cfg["capture_root"]（素材导入根）→ 永远查不到 → 已做过身份的 ROM
            # 每次启动都被重新导入（用户实测：am ROM 明明早已导入仍在转码）
            check_cfg = dict(cfg)
            check_cfg["capture_root"] = id_root
            if batch_runner._import_already_done(check_cfg, {"name": name, "batch": group}):
                progress.log("[MHS_STREAM] ID 素材已导入过，跳过: {}".format(name))
                self.state.record("完成", "ID 素材已存在（跳过导入）: {}".format(name), group)
            else:
                self.queue.set_status(key, STATUS_IMPORTING)
                # 关键：导入前写"运行"事件——状态文件否则停留在上一快照
                # （任务还是 pending → 控制台显示"未监听"，与实际导入中矛盾）
                self.state.record("运行", "导入 ID 素材: {}".format(name), group)
                # [ID] 前缀：单任务窗一眼区分"在做身份"还是"跑表演"（P2）
                progress.monitor_begin_task("[ID] " + name)
                progress.log("[MHS_STREAM] 导入 ID 素材: {}".format(name))
                versions.import_single_take(task["dir"], cfg["media_output_root"], base)
                progress.monitor_end(True)
            self.queue.set_status(key, STATUS_DONE)
            self.state.record("完成", "ID 素材就绪: {}".format(name), group)
            progress.log("[MHS_STREAM] ID 素材就绪: {}（请在面板完成身份制作并 [确认完成] 绑定到分组 '{}'）".format(
                name, group or "(根)"))
        except Exception as exc:
            self.queue.set_status(key, STATUS_FAILED, str(exc))
            self.state.record("失败", "ID 素材导入失败 {}: {}".format(name, exc), group)
            progress.error("[MHS_STREAM] ID 素材导入失败 {}: {}".format(name, exc))

    def _run_perf_task(self, task):
        """表演素材：导入（幂等）→ 定位 CD → 解算（阻塞）→ 导出。"""
        key = task["key"]
        name = os.path.basename(task["dir"].rstrip("\\/"))
        group = task.get("group", "")
        cfg = self.cfg
        base = self._group_base(group)
        scan_cfg = dict(cfg)
        scan_cfg["capture_root"] = base

        started = time.time()
        try:
            # 0) 目录预检：素材在排队期间被移走时给出准确原因（否则 C++ 导入器
            #    只在日志报"目录不存在"，Python 继续走定位 → 误报"未找到已导入素材"）
            if not os.path.isdir(task["dir"]):
                raise RuntimeError("素材目录不存在（可能在排队后被移走，放回后自动重新排队）: {}".format(
                    task["dir"]))

            # 1) 导入（幂等：已导入跳过）
            if not batch_runner._import_already_done(cfg, {"name": name, "batch": group}):
                self.queue.set_status(key, STATUS_IMPORTING)
                self.state.record("运行", "导入: {}".format(name), group)
                progress.log("[MHS_STREAM] 导入: {}（分组 {}）".format(name, group or "(根)"))
                # 导入时长估算注入观测层（控制台进度条用）：素材体积 / 实测吞吐
                # ~1.5MB/s（e011 实测 222MB → 141s）。曾用固定 30s 基准 → 大素材
                # 进度条停在 99% 数分钟，与实际流程不符。
                try:
                    size_mb = sum(
                        os.path.getsize(os.path.join(r_, f))
                        for r_, _d, fs in os.walk(task["dir"]) for f in fs
                    ) / (1024.0 * 1024.0)
                    self.state.extra["import_estimate_seconds"] = max(15, int(size_mb / 1.5))
                    self.state.write()
                except Exception:
                    pass
                # 启动导入阶段监控：刷新 progress.json（否则它保持上轮终态——
                # 控制台会显示上一次任务的 asset/耗时，看起来像"卡了很久"）
                progress.monitor_begin_task("[导入] " + name)
                try:
                    versions.import_single_take(task["dir"], cfg["media_output_root"], base)
                finally:
                    self.state.extra.pop("import_estimate_seconds", None)
                    self.state.write()
                    progress.monitor_end(True)

            # 2) 定位已导入 CD（与 run_next 解算段同源逻辑，避免命名规则漂移）
            self.queue.set_status(key, STATUS_SOLVING)
            found = batch_runner._find_footage_capture_data(scan_cfg, cfg["mode"])
            matched = batch_runner._match_footage_item(found, name)
            if matched is None:
                raise RuntimeError("未找到已导入素材（检查导入是否成功）")

            item = {"name": name, "type": cfg["mode"], "dir": task["dir"],
                    "batch": group, "package_path": matched["package_path"]}
            # 身份：入队时快照优先（积压不影响分配）；无快照回落 resolve（再回落全局）
            identity = task.get("identity") or pipeline.resolve_identity(
                scan_cfg, matched["package_path"])
            item["identity"] = identity

            # 3) 解算（阻塞）——与现有单段链路完全同源
            progress.monitor_batch_context(0, 0, -1.0)
            progress.monitor_begin_task("流式: {}".format(name))
            self.state.record("运行", "解算: {}（identity: {}）".format(name, identity or "<全局>"), group)
            progress.log("[MHS_STREAM] 解算: {}（identity: {}）".format(name, identity or "<全局>"))
            pipeline.ensure_identity_ready(cfg, identity)
            holder = {}

            def _collect(_cfg, perf, _item, _h=holder):
                _h["perf"] = perf

            if cfg["mode"] == "video":
                pipeline.run_video_shot(cfg, item, _collect)
            else:
                pipeline.run_depth_shot(cfg, item, _collect)
            perf = holder.get("perf")
            if perf is None:
                raise RuntimeError("解算未返回 Performance（内部异常，见上方日志）")

            # 4) 导出（复用批处理导出核心：AS + LS + ControlRig FBX）
            self.queue.set_status(key, STATUS_EXPORTING)
            self.state.record("运行", "导出: {}".format(name), group)
            # 交付目录按"匹配的身份"分子目录：用户只指定父目录，
            # 落盘为 <父目录>/<身份名>/<take>.fbx —— 多演员/多身份混跑时不混在一起
            export_cfg = self._identity_output_cfg(cfg, identity)
            ok, err = batch_runner.export_shot_outputs(export_cfg, perf, item)
        except Exception as exc:
            ok, err = False, str(exc)
            progress.error("[MHS_STREAM] {} 失败: {}".format(name, exc))

        elapsed = time.time() - started
        progress.monitor_end(ok)
        progress.result(name, ok, elapsed)
        # 帧数（用于 k 标定与逐条预估）：解算期间由 C++ 心跳写入 progress.json
        frames = _read_total_frames(os.path.dirname(self.queue.state_path)) if ok else None
        if ok and frames:
            self.calib.record(frames, elapsed)
        self.queue.set_status(key, STATUS_DONE if ok else STATUS_FAILED, err,
                              duration=elapsed, frames=frames)
        self.state.record("完成" if ok else "失败",
                          ("交付 {}（{:.0f}s）".format(name, elapsed)) if ok else
                          "{}: {}".format(name, err), group)
        if ok:
            progress.log("[MHS_STREAM] 交付: {}（{:.0f}s）".format(name, elapsed))

    @staticmethod
    def _identity_output_cfg(cfg, identity):
        """导出配置：交付目录 = <父目录>/<身份名>/（目录自动创建）。

        身份名取身份资产末段（如 /Game/CaptureManager/ID4/0818_face_rom_am_001_11
        → 0818_face_rom_am_001_11）。无身份（未绑定）→ <父目录>/未绑定/，
        避免与已绑定交付混在一起（可一眼看出"这条还没绑身份"）。
        """
        parent = (cfg.get("fbx_output_dir") or "").replace("\\", "/").rstrip("/")
        if not parent:
            return cfg
        folder = os.path.basename(str(identity or "").replace("\\", "/").rstrip("/")) or "未绑定"
        out = parent + "/" + folder
        try:
            os.makedirs(out, exist_ok=True)
        except Exception:
            pass                      # 创建失败则按原目录交付（不阻断导出）
        out_cfg = dict(cfg)
        out_cfg["fbx_output_dir"] = out
        return out_cfg


# ══════════════════════════════════════════════════════════════════
# UE 进程内单例 + 便捷入口（bridge 调用）
# ══════════════════════════════════════════════════════════════════

_executor = None


def _job_fingerprint(job_stream_path):
    """job 文件指纹（mtime+size）——None=文件不存在。"""
    try:
        st = os.stat(job_stream_path)
        return (st.st_mtime_ns, st.st_size)
    except OSError:
        return None


def get_executor(job_stream_path):
    """获取/创建执行器单例。

    job 变更自动重建（指纹比对）：面板每次 [开始] 重写 job_stream.json——
    防止"早于 [开始] 的调用（如绑定确认）用旧 job 构造的单例"携带过期
    inbox 等配置导致发现层失明。
    """
    global _executor
    fp = _job_fingerprint(job_stream_path)
    if _executor is not None and getattr(_executor, "_job_fp", None) != fp:
        # job 已变化（[开始] 重写了配置）→ 丢弃旧单例重建
        # 注意：pending 池/扫描快照随之重置——幂等补扫保证素材不丢，仅状态回退
        reset_executor()
    if _executor is None:
        from . import config
        cfg = config.load(job_stream_path)
        _executor = StreamExecutor(cfg)
        _executor._job_fp = fp
    return _executor


def set_binding_direct(job_stream_path, group, identity):
    """绑定确认入口（不构造 executor）。

    group 为空 → 自动推断：取 stream_state.json 的 identity.suggested_group
    （由 ID/ROM 素材名解析出的 am/pm；未命中则为空=根组兜底）。
    绑定只依赖绑定文件（路径 = job 配置或默认）——避免在 [开始] 之前
    调用时用旧 job 构造出坏单例。executor 已存在则热应用（reload+冲刷 pending）。
    """
    if not group:
        try:
            from . import config
            cfg = config.load(job_stream_path)
            state_path = cfg.get("stream_state_path") or os.path.join(
                os.path.dirname(cfg.get("stream_queue_path") or ""), "stream_state.json")
            if os.path.exists(state_path):
                with open(state_path, "r", encoding="utf-8") as fh:
                    import json as _json
                    group = (_json.load(fh).get("identity") or {}).get("suggested_group", "") or ""
        except Exception:
            group = ""
    q_path, b_path = _default_stream_paths()
    try:
        from . import config
        cfg = config.load(job_stream_path)
        b_path = cfg.get("stream_bindings_path") or b_path
    except Exception:
        pass   # job 尚不存在/旧格式：回落默认绑定路径（[开始] 后 executor 用同一路径）
    bindings = StreamBindings(b_path)
    bindings.set(group, identity)
    # 热应用：执行器在跑（同 job）则立即生效并冲刷 pending
    if _executor is not None:
        _executor.reload_bindings()
    return True


def reset_executor():
    """丢弃单例（停止/换 job 时用，下轮调用重建）。"""
    global _executor
    _executor = None
