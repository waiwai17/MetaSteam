# -*- coding: utf-8 -*-
"""启动壳（launcher）：拉起无界面 UE + 守护它（watchdog）。

职责边界（第一性原理）：业务循环在 UE 进程内（headless_drive），本模块只做
"拉起 + 存活判定 + 崩溃重启 + 状态外置"，不碰任何业务逻辑。

命令：
    python launcher.py start  [--config X] [--dry-run] [--once]   拉起（含 watchdog 循环）
    python launcher.py stop   [--config X]                          写 stop.flag（段间优雅停）
    python launcher.py status [--config X]                          读状态（含存活判定）
    python launcher.py doctor [--config X]                          预检报告（不启动）

存活判据（关键设计）：
    heartbeat = min(state.json 年龄, progress.json 年龄)
    · 进程活着 + 心跳新鲜            → healthy
    · 进程活着 + 心跳陈腐 + 队列非空  → stalled（应重启：该干活却没动静）
    · 进程活着 + 心跳陈腐 + 队列空    → idle（正常！空闲时无人写文件，绝不重启）
    · 进程没了 + 队列非空            → dead（崩溃 → 重启）
    · 进程没了 + 队列空              → stopped
"""

import json
import os
import shutil
import subprocess
import sys
import threading
import time

POLL_SECONDS = 5.0
STALL_SECONDS = 600.0        # 心跳陈腐阈值（10 分钟；解算中 progress 每 500ms 心跳）
MAX_ATTEMPTS = 3
BACKOFF = (30.0, 120.0, 600.0)

CONFIG_DEFAULT = {
    "instance": "default",
    "ue_editor": "D:/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe",
    "project": "",
    "inbox": "",
    "fbx_output": "",
    "identity_import_root": "/Game/CaptureManager/ID",
    "import_root": "/Game/CaptureManager/Auto",
    "bindings_file": "",
    "web_port": 8902,
    "bootstrap": "",
}

ACTIVE_STATUSES = ("pending", "importing", "solving", "exporting")


# ── 配置与路径 ──

def load_config(path):
    """读配置；缺失/损坏 → 用默认值并返回问题列表（不抛）。"""
    cfg = dict(CONFIG_DEFAULT)
    problems = []
    if not path or not os.path.isfile(path):
        problems.append("配置文件不存在（使用默认值）: {}".format(path))
    else:
        try:
            with open(path, "r", encoding="utf-8") as fh:
                cfg.update(json.load(fh))
        except Exception as exc:
            problems.append("配置解析失败（使用默认值）: {}".format(exc))
    return cfg, problems


def derive_paths(cfg, tools_dir=None):
    """由配置推导全部路径（Saved/Config/MetaHumanSolver 下，与引擎侧一致）。"""
    project = cfg.get("project", "") or ""
    proj_dir = os.path.dirname(project.replace("/", os.sep))
    saved = os.path.join(proj_dir, "Saved") if proj_dir else ""
    config_dir = os.path.join(saved, "Config", "MetaHumanSolver") if saved else ""
    tools_dir = tools_dir or os.path.dirname(os.path.abspath(__file__))
    return {
        "project": project,
        "saved": saved,
        "config_dir": config_dir,
        "job": os.path.join(config_dir, "job_stream.json"),
        "queue": os.path.join(config_dir, "queue_state.json"),
        "state": os.path.join(config_dir, "stream_state.json"),
        "progress": os.path.join(config_dir, "progress.json"),
        "bindings": cfg.get("bindings_file") or os.path.join(config_dir, "stream_bindings.json"),
        "stop_flag": os.path.join(config_dir, "stop.flag"),
        # 暂停意图文件：有它 = 人为暂停（watchdog 不重启）；没有 = 崩溃（watchdog 重启续跑）
        "stop_intent": os.path.join(config_dir, "stop.intent"),
        "refresh_flag": os.path.join(config_dir, "refresh_assets.flag"),
        "status": os.path.join(config_dir, "pipeline.status.json"),
        "bootstrap": cfg.get("bootstrap") or os.path.join(tools_dir, "headless_bootstrap.py"),
    }


def precheck(cfg, paths):
    """启动前预检 → (errors, warnings)。errors 非空则拒绝启动。"""
    errors, warnings = [], []
    if not os.path.isfile(cfg.get("ue_editor", "")):
        # 换机器后写死的 D:/UE_5.7 会失效 → 顺便探测本机 UE，给出可直接用的路径
        found = find_ue_editor()
        errors.append("编辑器不存在: {}{}".format(
            cfg.get("ue_editor"),
            "（检测到本机 UE：{} —— 请把它填进配置的 ue_editor）".format(found) if found
            else "（未在本机探测到 UE，请安装 UE 5.x + MetaHuman 插件，或设置 MHS_UE_EDITOR）"))
    if not os.path.isfile(cfg.get("project", "")):
        errors.append("工程不存在: {}".format(cfg.get("project")))
    if not cfg.get("inbox") or not os.path.isdir(cfg.get("inbox", "")):
        errors.append("热文件夹不存在: {}".format(cfg.get("inbox")))
    if not os.path.isfile(paths.get("bootstrap", "")):
        errors.append("引导脚本不存在: {}".format(paths.get("bootstrap")))
    out = cfg.get("fbx_output", "")
    if not out:
        warnings.append("未配置输出目录")
    elif not os.path.isdir(out):
        warnings.append("输出目录不存在（将创建）: {}".format(out))
    # 工程内必须有 MetaHuman 目标角色 + Face 骨架（LS 重建必需）——缺了会在解算 25 分钟
    # 之后才倒在 "Identity Level Sequence 导出失败"，代价极高 → 预检就拦住
    proj_dir = os.path.dirname(str(cfg.get("project", "")).replace("/", os.sep))
    mh_dir = os.path.join(proj_dir, "Content", "MetaHumans")
    skeleton = os.path.join(mh_dir, "Common", "Face", "Face_Archetype_Skeleton.uasset")
    bp_found = False
    for root, _dirs, files in os.walk(mh_dir) if os.path.isdir(mh_dir) else ():
        if any(f.startswith("BP_") and f.endswith(".uasset") for f in files):
            bp_found = True
            break
    if not os.path.isfile(skeleton) or not bp_found:
        errors.append("工程缺少 MetaHuman 资产（LS 重建必需）：{}"
                      "（需含 Common/Face/Face_Archetype_Skeleton.uasset 与任一 BP_* 角色；"
                      "新建工程请用 [新建]，或从 deploy/assets/MetaHumans 复制）".format(mh_dir))

    bpath = paths.get("bindings", "")
    if not os.path.isfile(bpath):
        warnings.append("绑定文件不存在（视为无绑定，素材将等待身份）: {}".format(bpath))
    else:
        try:
            with open(bpath, "r", encoding="utf-8") as fh:
                binds = (json.load(fh) or {}).get("bindings", {})
            if not binds:
                warnings.append("绑定表为空 → 素材将进入待绑定池等待")
        except Exception as exc:
            warnings.append("绑定表解析失败: {}".format(exc))
    return errors, warnings


def _editor_procs(project):
    """返回 [(pid, cmdline)]：正在运行且加载了本工程的编辑器。"""
    try:
        ps = ("Get-CimInstance Win32_Process -Filter \"Name='UnrealEditor.exe'\" | "
              "ForEach-Object { $_.ProcessId.ToString() + '|' + $_.CommandLine }")
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        text = out.stdout.decode("utf-8", "ignore")
        key = os.path.basename(str(project)).lower()
        procs = []
        for line in text.splitlines():
            if "|" not in line:
                continue
            pid_txt, cmd = line.split("|", 1)
            if key and key in cmd.lower():
                try:
                    procs.append((int(pid_txt.strip()), cmd.strip()))
                except ValueError:
                    pass
        return procs
    except Exception:
        return []


def find_running_editor(project):
    """找出正在运行、且加载了本工程的编辑器命令行摘要（没有则返回 []）。

    必须互斥：两个编辑器共用一个工程 = 共写同一份队列/状态 → 互相覆盖
    （实测：后台会话 + 前台 --once 各起一个，两个进程争抢同一队列）。
    """
    return [cmd[:60] for _pid, cmd in _editor_procs(project)]


def build_job(cfg, paths):
    """生成 job_stream.json 内容（字段对齐面板 BuildStreamJobJson，已验证格式）。"""
    inbox = cfg.get("inbox", "").replace("\\", "/")
    return {
        "mode": "depth",
        "stream_mode": "semi",
        "stage_automation": {"identity": True, "ingest": True, "solve": True, "export": True},
        # 身份资产目录（做好的身份）独立于"身份导入"（ROM 原料导入位置）：
        # 两者语义不同，未配置时回落 identity_import_root 以保持旧配置可用
        "identity_asset_dir": (cfg.get("identity_asset_dir")
                               or cfg.get("identity_import_root") or "/Game/CaptureManager/ID"),
        "identity_path": "",
        "inbox": inbox,
        "capture_root": cfg.get("import_root", "/Game/CaptureManager/Auto"),
        "storage_path": "/Game/MH_Results",
        "import_mode": "cpp",
        "import_footage": True,
        "import_then_solve": True,
        "footage_source_dir": inbox,
        "media_output_root": os.path.join(paths.get("saved", ""), "MetaHumanSolver", "MediaOut").replace("\\", "/"),
        "fbx_output_dir": cfg.get("fbx_output", "").replace("\\", "/"),
        "skeleton_path": "/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton",
        "auto_prepare_identity": False,
        "export_anim_sequence": True,
        "export_fbx": True,
        "export_level_sequence": True,
        "export_audio_track": False,
        "depth": {"using_livelinkface_data": True, "blocking": True, "skip_tongue_solve": True},
        "stream_dashboard_port": int(cfg.get("web_port", 8902)),
        "stream_queue_path": paths.get("queue", "").replace("\\", "/"),
        "stream_state_path": paths.get("state", "").replace("\\", "/"),
        "stream_bindings_path": paths.get("bindings", "").replace("\\", "/"),
        "stream_stable_seconds": 3.0,
        "exclude": [],
    }


def write_json_atomic(path, payload):
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, ensure_ascii=False, indent=1)
    os.replace(tmp, path)
    return path


# ── 存活判定（watchdog 核心，全部可测）──

def _mtime_age(path, now):
    try:
        return now - os.path.getmtime(path)
    except OSError:
        return None


def heartbeat_age(paths, now=None):
    """取 state/progress 中较新的年龄（秒）；都读不到 → None。"""
    now = time.time() if now is None else now
    ages = [a for a in (_mtime_age(paths.get("state", ""), now),
                        _mtime_age(paths.get("progress", ""), now)) if a is not None]
    return min(ages) if ages else None


def queue_active_count(queue_path):
    """队列中未完成（pending/importing/solving/exporting）的任务数。"""
    try:
        with open(queue_path, "r", encoding="utf-8") as fh:
            tasks = (json.load(fh) or {}).get("tasks", [])
    except Exception:
        return 0
    return len([t for t in tasks if str(t.get("status", "")) in ACTIVE_STATUSES])


def evaluate(paths, alive, now=None, stall_seconds=STALL_SECONDS):
    """综合判定 → (state, detail)。state ∈ healthy/idle/stalled/dead/stopped。"""
    now = time.time() if now is None else now
    active = queue_active_count(paths.get("queue", ""))
    age = heartbeat_age(paths, now)
    detail = {"alive": alive, "active_tasks": active,
              "heartbeat_age": None if age is None else round(age, 1)}
    if not alive:
        return ("dead" if active else "stopped"), detail
    if age is not None and age <= stall_seconds:
        return "healthy", detail
    if active:
        return "stalled", detail                 # 有活却没动静
    return "idle", detail                        # 空闲：没人写文件是正常


def decide(state, attempts, max_attempts=MAX_ATTEMPTS, backoff=BACKOFF):
    """状态 → 动作。返回 (action, sleep_seconds)。"""
    if state in ("healthy", "idle"):
        return "continue", POLL_SECONDS
    if state == "stopped":
        return "exit", 0
    if attempts >= max_attempts:
        return "abort", 0
    return "restart", backoff[min(attempts, len(backoff) - 1)]


# ── 进程操作 ──

def _pid_alive(pid):
    """跨平台进程存活检查（无第三方依赖）。"""
    if not pid or pid <= 0:
        return False
    if os.name == "nt":
        try:
            out = subprocess.run(["tasklist", "/FI", "PID eq {}".format(int(pid))],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            return str(int(pid)) in out.stdout.decode("gbk", "ignore")
        except Exception:
            return False
    try:
        os.kill(int(pid), 0)
        return True
    except OSError:
        return False


def launch_editor(cfg, paths):
    """拉起无界面编辑器，返回 PID。"""
    cmd = [cfg["ue_editor"], cfg["project"], "-unattended", "-nosplash",
           "-ExecCmds=py {}".format(paths["bootstrap"])]
    proc = subprocess.Popen(cmd)
    return proc.pid


def ensure_editor_settings(paths):
    """关闭"后台降频"（headless 形态必须）。

    面板路径会在 [启动监听] 时关掉它（C++ 侧），但无头模式没有面板 →
    必须在这里写 ini：否则编辑器在后台/离屏时被大幅降频，驱动心跳被拖慢。
    """
    saved = paths.get("saved", "")
    if not saved:
        return ""
    ini = os.path.join(saved, "Config", "WindowsEditor", "EditorSettings.ini")
    section = "[/Script/UnrealEd.EditorPerformanceSettings]"
    key = "bThrottleCPUWhenNotForeground=False"
    try:
        os.makedirs(os.path.dirname(ini), exist_ok=True)
        text = ""
        if os.path.isfile(ini):
            with open(ini, "r", encoding="utf-8", errors="ignore") as fh:
                text = fh.read()
        if section in text and "bThrottleCPUWhenNotForeground" in text:
            text = _replace_ini_value(text, section, "bThrottleCPUWhenNotForeground", "False")
        else:
            text = text.rstrip() + "\n\n" + section + "\n" + key + "\n"
        with open(ini, "w", encoding="utf-8") as fh:
            fh.write(text)
        return ini
    except Exception:
        return ""


def _replace_ini_value(text, section, key, value):
    """在指定 ini 段里替换/插入 key=value（极简实现，够用即可）。"""
    lines = text.splitlines()
    out = []
    in_section = False
    replaced = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("["):
            if in_section and not replaced:
                out.append("{}={}".format(key, value))
                replaced = True
            in_section = (stripped == section)
        elif in_section and stripped.startswith(key + "="):
            out.append("{}={}".format(key, value))
            replaced = True
            continue
        out.append(line)
    if in_section and not replaced:
        out.append("{}={}".format(key, value))
    return "\n".join(out) + "\n"


def hide_editor_window(pid, timeout=0.0, log=None, hard_timeout=None):
    """线程隔离 + 硬超时：Win32 EnumWindows 偶发卡住（实测挂死 watchdog 主线程 2 小时）。

    窗口移屏是"锦上添花"，绝不能阻塞守护循环 → 放进守护线程，超时即放弃本次。
    """
    log = log or (lambda m: None)
    hard_timeout = float(hard_timeout) if hard_timeout is not None else max(8.0, float(timeout) + 5.0)
    box = {}

    def runner():
        try:
            box["ok"] = _hide_impl(pid, timeout, log)
        except Exception:
            box["ok"] = False

    t = threading.Thread(target=runner, daemon=True)
    t.start()
    t.join(hard_timeout)
    if t.is_alive():
        log("[launcher] 窗口移屏超时（{}s）→ 放弃本次，watchdog 不受影响".format(hard_timeout))
        return False
    return bool(box.get("ok"))


def _hide_impl(pid, timeout=0.0, log=None):
    """把编辑器主窗口移出屏幕（不是最小化/隐藏 → 不会触发"窗口最小化"节流）。

    -unattended -nosplash 只压对话框与启动图，**主窗口仍会弹出**（用户实测）。
    这里用 Win32 把窗口挪到 (-32000,-32000)：视觉上消失，进程照常渲染与解算。
    非 Windows / 找不到窗口 → 返回 False（不影响流程）。
    timeout>0 时重试等待窗口出现（编辑器加载需要时间）。
    """
    log = log or (lambda m: None)
    if os.name != "nt":
        return False
    try:
        import ctypes
        from ctypes import wintypes
    except Exception:
        return False

    found = []

    def _enum(hwnd, _):
        try:
            pid_out = wintypes.DWORD()
            ctypes.windll.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid_out))
            if int(pid_out.value) == int(pid):
                if ctypes.windll.user32.IsWindowVisible(hwnd):
                    found.append(hwnd)

        except Exception:
            pass
        return True

    deadline = time.time() + max(0.0, timeout)
    while True:
        found.clear()
        try:
            EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
            ctypes.windll.user32.EnumWindows(EnumWindowsProc(_enum), 0)
        except Exception:
            return False
        if found:
            moved = 0
            for hwnd in found:
                try:
                    SWP_NOSIZE = 0x0001
                    SWP_NOZORDER = 0x0004
                    SWP_NOACTIVATE = 0x0010
                    # 关键：SWP_ASYNCWINDOWPOS（0x4000）= 把请求**投递**给窗口所属线程，
                    # 而不是同步等待它处理。解算期 UE 的 UI 线程冻结（20 分钟级阻塞），
                    # 同步 SetWindowPos/SetWindowPos 会一直等待 → 卡死并超时放弃（窗口留在屏幕上）。
                    SWP_ASYNCWINDOWPOS = 0x4000
                    ctypes.windll.user32.SetWindowPos(
                        hwnd, 0, -32000, -32000, 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)
                    moved += 1
                except Exception:
                    pass
            if moved:
                log("[launcher] 编辑器窗口已移出屏幕 pid={}（{} 个窗口，异步投递）".format(pid, moved))
                return True
            return False
        if time.time() >= deadline:
            return False
        time.sleep(1.0)


def request_stop(paths):
    """暂停：写 stop.flag（驱动段间退出）+ stop.intent（告知 watchdog 这是人为，不要重启）。

    两个文件缺一不可：只写 stop.flag 时，若队列还有未完成任务，watchdog 会把
    "进程退出"误判为崩溃并重新拉起 —— 那就变成"暂停不了"。
    """
    d = os.path.dirname(paths["stop_flag"])
    if d and not os.path.isdir(d):
        os.makedirs(d)
    stamp = str(time.time())
    with open(paths["stop_flag"], "w", encoding="utf-8") as fh:
        fh.write(stamp)
    with open(paths["stop_intent"], "w", encoding="utf-8") as fh:
        fh.write(stamp)
    return paths["stop_flag"]


def clear_stop(paths):
    """启动前清理上一次的停止/暂停标记（否则新会话一启动就被当成已暂停）。"""
    clear_flag(paths.get("stop_flag", ""))
    clear_flag(paths.get("stop_intent", ""))


def clear_flag(path):
    try:
        os.remove(path)
    except OSError:
        pass


# ── watchdog ──

def watchdog(cfg, paths, pid=0, poll=POLL_SECONDS, max_attempts=MAX_ATTEMPTS,
             stall_seconds=STALL_SECONDS, is_alive=None, launch=None,
             sleep_fn=None, max_ticks=0, log=None, hide=True, hide_fn=None):
    """守护循环。所有外部副作用可注入 → 完整可单测。

    返回 {"ticks":n, "last_state":s, "restarts":k, "action":a}。
    """
    # flush=True：watchdog 由 .bat 拉起时 stdout 可能是管道，缓冲写满会永久阻塞主线程
    # （实测：主线程停摆 2 小时，状态文件不再刷新 → 自愈机制实际离线）
    log = log or (lambda m: print(m, flush=True))
    sleep_fn = sleep_fn or time.sleep
    is_alive = is_alive or (lambda: _pid_alive(pid))
    launch = launch or (lambda: launch_editor(cfg, paths))
    hide_fn = hide_fn or (lambda: hide_editor_window(pid, timeout=0.0, log=log))

    attempts = 0
    ticks = 0
    restarts = 0
    last_state = "unknown"
    last_action = "continue"

    while True:
        # 窗口移屏不再每轮执行：EnumWindows 是阻塞调用，放在主循环里一旦卡住，
        # 整个 watchdog 就停摆（实测主线程 UserRequest 阻塞 2 小时）。
        # 改为每 12 轮（≈60s）一次，且放在状态写盘之后——即使它卡住，状态也已刷新。
        state, detail = evaluate(paths, is_alive(), stall_seconds=stall_seconds)
        # 人为暂停（有 stop.intent）≠ 崩溃（无 intent）：前者不重启，后者要重启续跑
        if state == "dead" and paths.get("stop_intent") and os.path.exists(paths["stop_intent"]):
            state = "stopped"
            detail["intent"] = True
        action, wait = decide(state, attempts, max_attempts)
        last_state, last_action = state, action
        ticks += 1
        write_json_atomic(paths["status"], {
            "instance": cfg.get("instance", "default"),
            "pid": pid, "state": state, "action": action,
            "attempts": attempts, "restarts": restarts,
            "detail": detail, "updated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        })
        log("[launcher] tick={} state={} detail={} action={}".format(ticks, state, detail, action))

        # 每 3 轮（≈15s）尝试一次：解算期 UI 线程冻结时投递会被排队，
        # 一旦出现空隙（任务间/加载完）就生效 —— 60s 间隔会让窗口长期露在屏幕上
        if hide and (ticks % 3 == 1):
            try:
                hide_fn()
            except Exception:
                pass

        if action == "restart":
            attempts += 1
            restarts += 1
            # 退避必须先等再拉起（曾先拉起后睡 → 退避形同虚设，会"立即重启"）
            if sleep_fn:
                sleep_fn(wait)
            pid = launch()
            log("[launcher] 退避 {}s 后重启编辑器 pid={}（第 {} 次）".format(wait, pid, attempts))
        elif action == "abort":
            log("[launcher] 连续失败超过上限（{}）→ 放弃并置 failed".format(max_attempts))
            write_json_atomic(paths["status"], {
                "instance": cfg.get("instance", "default"), "pid": pid,
                "state": "failed", "action": "abort", "attempts": attempts,
                "restarts": restarts, "detail": detail,
                "updated_at": time.strftime("%Y-%m-%d %H:%M:%S")})
            break
        elif action == "exit":
            break

        if max_ticks and ticks >= max_ticks:
            break
        sleep_fn(wait)

    return {"ticks": ticks, "last_state": last_state,
            "restarts": restarts, "action": last_action}


# ── CLI ──

def cmd_start(cfg_path, dry_run=False, once=False):
    cfg, problems = load_config(cfg_path)
    paths = derive_paths(cfg)
    for p in problems:
        print("[WARN] {}".format(p))
    errors, warnings = precheck(cfg, paths)
    for w in warnings:
        print("[WARN] {}".format(w))
    # 互斥：已有编辑器在跑本工程 → 默认拒绝再起一个（除非 --force）
    # 但 --adopt = 接管：只接监督权、不再起第二个进程。
    # （watchdog 进程自身挂掉后，编辑器还活着却永远无法被重新守护 —— 这个口子必须留）
    running_procs = _editor_procs(cfg.get("project", ""))
    if running_procs and "--adopt" in rest_flags():
        adopt_pid = running_procs[0][0]
        print("[INFO] 接管已运行的编辑器 pid={}（不再新建进程）".format(adopt_pid), flush=True)
    elif running_procs and "--force" not in rest_flags():
        running = [cmd[:60] for _p, cmd in running_procs]
        print("[ERROR] 已有编辑器在跑本工程（共写同一队列会互相覆盖）：{}".format(running), flush=True)
        print("[HINT] 先运行 停止管线.bat（或 pause）；"
              "仅想重新守护它请用 --adopt；强行再起请加 --force", flush=True)
        write_json_atomic(paths["status"], {"state": "already_running",
                                            "running": running,
                                            "updated_at": time.strftime("%Y-%m-%d %H:%M:%S")})
        return 3
    else:
        adopt_pid = 0
    if errors:
        for e in errors:
            print("[ERROR] {}".format(e))
        write_json_atomic(paths["status"], {"state": "precheck_failed", "errors": errors,
                                            "updated_at": time.strftime("%Y-%m-%d %H:%M:%S")})
        return 2

    job = build_job(cfg, paths)
    if not dry_run:
        write_json_atomic(paths["job"], job)
        clear_stop(paths)          # 清 stop.flag + stop.intent（继续 = 干净的新会话）
    print("[INFO] job={} dry_run={}".format(paths["job"], dry_run))
    if dry_run:
        print(json.dumps(job, ensure_ascii=False, indent=1))
        return 0

    pid = adopt_pid if adopt_pid else launch_editor(cfg, paths)
    print("[INFO] 编辑器已拉起 pid={}".format(pid), flush=True)
    if "--show-window" not in _REST_FLAGS:
        # 后台形态：主窗口移出屏幕（-unattended 并不隐藏它）；先等它出现（加载需时）。
        # 等待窗口不要久到"饿死" watchdog：超时即放弃，循环里还有节流重试。
        hide_editor_window(pid, timeout=15.0, log=lambda m: print("[INFO] " + m, flush=True))
    result = watchdog(cfg, paths, pid=pid, max_ticks=1 if once else 0)
    print("[INFO] watchdog 结束: {}".format(result))
    return 0


def cmd_stop(cfg_path):
    cfg, _ = load_config(cfg_path)
    paths = derive_paths(cfg)
    print("[INFO] 停止哨兵: {}".format(request_stop(paths)))
    return 0


def default_template_dir():
    """模板工程默认位置（deploy/template/MH_Template）。"""
    return os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "template", "MH_Template")


def default_workspace_root():
    """默认工作区根：固定盘中"剩余空间最多的非系统盘"，没有则回落用户目录。

    与 GUI 的 Workspace.DefaultRoot() 同规则（两边保持一致）。
    为什么不全放 C:：素材与解算中间产物体积大（单条 ~200MB，全量 87 条约 17GB+），
    系统盘常是容量较小的 SSD，且重装系统会丢交付物。
    为什么不写死盘符：各机器盘符不同 → 运行时探测；用户仍可在 GUI 改。
    """
    import string as _string
    import ctypes as _ctypes
    sys_root = (os.environ.get("SystemRoot", "C:\\Windows") or "C:\\Windows")[:2].upper()
    best, best_free, sys_best, sys_free = "", -1, "", -1
    try:                                        # 只认本地固定盘（3=DRIVE_FIXED）
        get_type = _ctypes.windll.kernel32.GetDriveTypeW
        get_type.argtypes = [_ctypes.c_wchar_p]
    except Exception:
        get_type = None
    for letter in _string.ascii_uppercase:
        root = letter + ":\\"
        if not os.path.isdir(root):
            continue
        # 关键：排除网络盘/光驱/U盘（实测曾挑到映射的网络盘 W:，写素材会极慢）
        if get_type is not None:
            try:
                if int(get_type(root)) != 3:
                    continue
            except Exception:
                continue
        try:
            free = shutil.disk_usage(root).free
        except Exception:
            continue
        if root[:2].upper() == sys_root:
            if free > sys_free:
                sys_free, sys_best = free, root
            continue
        if free > best_free:
            best_free, best = free, root
    root = best or sys_best or "C:\\"
    return os.path.join(root, "MetaSol")


def _content_dir_for(project_dir, ue_path):
    """/Game/X/Y → <工程>/Content/X/Y（UE 内容路径对应的磁盘目录）。非法路径返回 ""。"""
    parts = (ue_path or "").replace("\\", "/").strip("/").split("/")
    if not parts or parts[0] != "Game":
        return ""
    return os.path.join(project_dir, "Content", *parts[1:])


def create_project_from_template(name, target_dir=None, template_dir=None,
                                 inbox=None, fbx_output=None):
    """从模板工程复制出一个新工程（免新建向导、免编译）。

    · 复制模板目录 → target/<name>
    · .uproject 改名为 <name>.uproject（内容里的工程名一并改写）
    · 写出该工程自带的 pipeline.config.json（inbox/输出可指定，缺省用工程同级目录）

    返回 {"project": <.uproject 路径>, "config": <config 路径>}；失败抛 RuntimeError。
    """
    template_dir = template_dir or default_template_dir()
    if not os.path.isdir(template_dir):
        raise RuntimeError("模板工程不存在: {}".format(template_dir))
    if not name:
        raise RuntimeError("工程名不能为空")
    target_dir = target_dir or default_workspace_root()
    os.makedirs(target_dir, exist_ok=True)
    dest = os.path.join(target_dir, name)
    if os.path.exists(dest):
        raise RuntimeError("目标已存在: {}（换名或先删除）".format(dest))
    os.makedirs(target_dir, exist_ok=True)
    shutil.copytree(template_dir, dest)

    # .uproject 改名 + 内容改写
    src_up = os.path.join(dest, os.path.basename(template_dir) + ".uproject")
    dst_up = os.path.join(dest, name + ".uproject")
    if os.path.isfile(src_up):
        try:
            with open(src_up, "r", encoding="utf-8") as fh:
                data = json.load(fh)
            with open(dst_up, "w", encoding="utf-8") as fh:
                json.dump(data, fh, ensure_ascii=False, indent=1)
            os.remove(src_up)
        except Exception:
            os.rename(src_up, dst_up)
    if not os.path.isfile(dst_up):
        raise RuntimeError("模板缺少 .uproject：{}".format(template_dir))

    # 公共资产包：LS 重建需要工程内存在 MetaHuman 目标角色（/Game/MetaHumans/BP_*）
    # + Face 骨架/CtrlRig。模板刻意"干净"不含它 → 新工程解算 25 分钟后倒在 LS 导出
    # （实测 20260928_Anims_… · Identity Level Sequence 导出失败）。这里从
    # deploy/assets 复制最小必需集（Common + 一个角色，~2.1GB）。
    assets_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                              "assets", "MetaHumans")
    if os.path.isdir(assets_dir):
        dst_mh = os.path.join(dest, "Content", "MetaHumans")
        if not os.path.isdir(dst_mh):
            shutil.copytree(assets_dir, dst_mh)

    # 安全网：引擎 Python 以 deploy/Python 为唯一事实源，复制后覆盖一次。
    # 为什么必需：模板可能落后（实测模板 9/22、deploy 9/28）→ 新工程缺"绑定热应用"
    # 等修复，表现为"确认绑定后身份分组不显示 / 已绑定仍进待绑定池"，且极难排查。
    deploy_py = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                             "Python", "MetaHumanSolverEngine")
    dest_py = os.path.join(dest, "Content", "Python", "MetaHumanSolverEngine")
    if os.path.isdir(deploy_py):
        shutil.rmtree(dest_py, ignore_errors=True)
        shutil.copytree(deploy_py, dest_py,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))

    # 该工程自带的配置（inbox/输出默认放在工程同级）
    inbox = inbox or os.path.join(target_dir, name + "_inbox")
    fbx_output = fbx_output or os.path.join(target_dir, name + "_out")

    # 路径一次建全：热文件夹（含分组子目录）+ 输出 + UE 内容目录（素材导入/身份导入）
    # ——面板下方四个路径不再需要用户手动建目录（新建即用）
    for d in (inbox, fbx_output):
        os.makedirs(d, exist_ok=True)
    for g in ("am", "pm", "_id"):
        os.makedirs(os.path.join(inbox, g), exist_ok=True)

    import_root = CONFIG_DEFAULT["import_root"] + name          # /Game/CaptureManager/Auto<name>
    identity_root = CONFIG_DEFAULT["identity_import_root"]      # /Game/CaptureManager/ID
    for ue_path in (import_root, identity_root):
        fs = _content_dir_for(dest, ue_path)
        if fs:
            os.makedirs(fs, exist_ok=True)
    cfg = dict(CONFIG_DEFAULT)
    cfg.update({"instance": name, "project": dst_up.replace("\\", "/"),
                "inbox": inbox.replace("\\", "/"),
                "fbx_output": fbx_output.replace("\\", "/"),
                "import_root": import_root,                  # 素材导入（UE 内容路径，已建目录）
                "identity_import_root": identity_root})      # 身份导入（已建目录）
    cfg_path = os.path.join(dest, "pipeline.config.json")
    write_json_atomic(cfg_path, cfg)
    return {"project": dst_up, "config": cfg_path, "dir": dest}


def cmd_new(cfg_path, rest):
    """CLI: launcher.py new --name X --dir <parent> [--inbox ..] [--out ..]"""
    def arg(flag, default=None):
        return rest[rest.index(flag) + 1] if flag in rest else default
    name = arg("--name")
    parent = arg("--dir", os.path.dirname(os.path.abspath(cfg_path)))
    if not name:
        print("[ERROR] 需要 --name <工程名>")
        return 2
    try:
        res = create_project_from_template(
            name, parent, template_dir=arg("--template"),
            inbox=arg("--inbox"), fbx_output=arg("--out"))
    except Exception as exc:
        print("[ERROR] 创建失败: {}".format(exc))
        return 1
    print("[OK] 工程已创建: {}".format(res["project"]))
    print("     配置: {}".format(res["config"]))
    print("[HINT] 启动：python launcher.py start --config \"{}\"".format(res["config"]))
    return 0


def cmd_hide(cfg_path):
    """把已在运行的编辑器窗口移出屏幕（对已启动的会话立即生效）。"""
    cfg, _ = load_config(cfg_path)
    paths = derive_paths(cfg)
    print("[INFO] 节流设置: {}".format(ensure_editor_settings(paths) or "(跳过)"))
    try:
        with open(paths["status"], "r", encoding="utf-8") as fh:
            pid = int((json.load(fh) or {}).get("pid", 0) or 0)
    except Exception:
        pid = 0
    ok = hide_editor_window(pid, timeout=0.0, log=lambda m: print("[INFO] " + m)) if pid else False
    print("[INFO] hide pid={} -> {}".format(pid, ok))
    return 0 if ok else 1


def cmd_status(cfg_path):
    cfg, _ = load_config(cfg_path)
    paths = derive_paths(cfg)
    data = {}
    try:
        with open(paths["status"], "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except Exception:
        data = {"state": "unknown", "detail": "状态文件不可用"}
    alive = _pid_alive(int(data.get("pid", 0) or 0))
    state, detail = evaluate(paths, alive)
    print(json.dumps({"file": data, "live": {"alive": alive, "state": state, "detail": detail}},
                     ensure_ascii=False, indent=1))
    return 0


def cmd_doctor(cfg_path):
    cfg, problems = load_config(cfg_path)
    paths = derive_paths(cfg)
    errors, warnings = precheck(cfg, paths)
    print(json.dumps({"config": cfg, "paths": paths, "problems": problems,
                      "errors": errors, "warnings": warnings},
                     ensure_ascii=False, indent=1))
    return 0 if not errors else 2


_REST_FLAGS = []


def rest_flags():
    return list(globals().get("_REST_FLAGS", []))


# ── 干净机器支持：UE 自动探测 + 一键初始化 ──

def find_ue_editor():
    """探测本机的 UnrealEditor.exe（换机器不依赖写死的 D:/UE_5.7）。

    顺序：环境变量 MHS_UE_EDITOR → 常见安装位置（各盘 UE_5.x / Epic Games / UE_5.x）。
    找不到返回 ""（doctor 会给出可操作提示）。
    """
    env = os.environ.get("MHS_UE_EDITOR", "")
    if env and os.path.isfile(env):
        return env
    import string as _string
    cands = []
    for letter in _string.ascii_uppercase:
        root = letter + ":\\"
        if not os.path.isdir(root):
            continue
        for ver in ("UE_5.7", "UE_5.6", "UE_5.5", "UE_5.4", "UE_5.3"):
            cands.append(os.path.join(root, ver, "Engine", "Binaries", "Win64", "UnrealEditor.exe"))
        for base in (os.path.join(root, "Program Files", "Epic Games"),
                     os.path.join(root, "Epic Games")):
            if os.path.isdir(base):
                try:
                    for name in sorted(os.listdir(base)):
                        cands.append(os.path.join(base, name, "Engine", "Binaries",
                                                  "Win64", "UnrealEditor.exe"))
                except OSError:
                    pass
    for c in cands:
        if os.path.isfile(c):
            return c.replace("\\", "/")
    return ""


def cmd_init(cfg_path, rest):
    """一键初始化（干净机器首跑）：探测 UE → 建工程 → 建 inbox/out → 写配置。

    用法：launcher.py init [--name MH_Line01] [--dir D:\\MetaSol] [--config X]
    """
    name = None
    target = None
    if "--name" in rest:
        name = rest[rest.index("--name") + 1]
    if "--dir" in rest:
        target = rest[rest.index("--dir") + 1]
    name = name or "MH_Line01"

    editor = find_ue_editor()
    if not editor:
        print("[ERROR] 未找到 UnrealEditor.exe。请先安装 UE 5.x + MetaHuman 插件，"
              "或设置环境变量 MHS_UE_EDITOR 指向 UnrealEditor.exe", flush=True)
        return 2

    if os.path.isfile(cfg_path):
        cfg = load_config(cfg_path)[0]
        print("[INFO] 配置已存在：{}（如需重来请先删除它）".format(cfg_path), flush=True)
    else:
        res = create_project_from_template(name, target)
        project = res["project"]
        inbox = res.get("inbox") or ""
        out = res.get("fbx_output") or ""
        cfg = dict(CONFIG_DEFAULT)
        cfg.update({
            "instance": name,
            "ue_editor": editor,
            "project": project.replace("\\", "/"),
            "inbox": str(inbox).replace("\\", "/"),
            "fbx_output": str(out).replace("\\", "/"),
            "import_root": "/Game/CaptureManager/Auto" + name,
            "identity_import_root": "/Game/CaptureManager/ID",
        })
        write_json_atomic(cfg_path, cfg)
        print("[OK] 已初始化：{}".format(cfg_path), flush=True)

    print("[INFO] 编辑器: {}".format(cfg.get("ue_editor", "")), flush=True)
    print("[INFO] 工程  : {}".format(cfg.get("project", "")), flush=True)
    print("[INFO] 热文件夹: {}".format(cfg.get("inbox", "")), flush=True)
    print("[INFO] 导出  : {}".format(cfg.get("fbx_output", "")), flush=True)
    print("[NEXT] 1) 拷 ID(ROM) 素材到 <热文件夹>\\_id\\  2) 启动管线  3) 在 UE 里做身份并绑定",
          flush=True)
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    cmd = argv[0] if argv else "start"
    rest = argv[1:]
    globals()["_REST_FLAGS"] = list(rest)
    cfg_path = None
    if "--config" in rest:
        cfg_path = rest[rest.index("--config") + 1]
    if not cfg_path:
        cfg_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "pipeline.config.json")
    if cmd == "start":
        return cmd_start(cfg_path, dry_run="--dry-run" in rest, once="--once" in rest)
    if cmd == "init":                     # 干净机器首跑：探测 UE + 建工程 + 写配置
        return cmd_init(cfg_path, rest)
    if cmd == "new":                      # 从模板工程新建（免手选 .uproject、免编译）
        return cmd_new(cfg_path, rest)
    if cmd in ("stop", "pause"):          # pause = 暂停（跑完当前条退出，队列保留）
        return cmd_stop(cfg_path)
    if cmd == "hide":
        return cmd_hide(cfg_path)
    if cmd == "status":
        return cmd_status(cfg_path)
    if cmd == "doctor":
        return cmd_doctor(cfg_path)
    print("用法: launcher.py start|stop|status|doctor [--config X] [--dry-run] [--once]")
    return 1


if __name__ == "__main__":
    sys.exit(main())
