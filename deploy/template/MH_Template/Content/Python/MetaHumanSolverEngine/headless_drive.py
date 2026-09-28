# -*- coding: utf-8 -*-
"""无面板驱动（headless drive）：替代面板 C++ 定时器的循环。

为什么需要：面板 [启动监听] 会启动 C++ 定时器逐次提交 stream_next；无界面形态下
没有面板，循环必须由 Python 承担。语义 1:1 复制已验证的 C++ 定时器：
    有积压（remaining>0）→ 0.05s 后立即下一条；空转 → 5s 补扫等新素材。
退出条件只有停止哨兵（stop.flag）——队列空 ≠ 退出（流式语义：常驻等下一批）。

副产物（模块3 · 身份资产索引刷新协议）：
    · refresh_assets.flag 存在 → 立刻重写 identity_assets.json 并删除 flag（外部 UI 请求）
    · 空闲轮每 IDLE_INDEX_SECONDS 自动刷新一次（省心，UI 不必请求）
    · 空闲轮写 state 作为**驱动心跳**——否则空闲期间状态文件 mtime 不更新，
      watchdog 会误判"卡死"而重启（这是实现 watchdog 时必须知道的坑）。

用法（由 launcher 拉起的 bootstrap 调用）：
    import MetaHumanSolverEngine.headless_drive as hd; hd.main()
"""

import os
import time

from . import progress
from . import stream_executor

IDLE_SLEEP = 5.0            # 空转补扫间隔（与 C++ 定时器一致）
BUSY_SLEEP = 0.05           # 有积压 → 立即下一条
IDLE_INDEX_SECONDS = 30.0   # 空闲时身份索引自动刷新周期
CONFIG_DIR_NAME = "Config/MetaHumanSolver"
STOP_FLAG = "stop.flag"
REFRESH_FLAG = "refresh_assets.flag"


def _config_dir():
    """Saved/Config/MetaHumanSolver 绝对路径。"""
    try:
        import unreal
        saved = unreal.Paths.convert_relative_path_to_full(
            unreal.Paths.project_saved_dir())
        return os.path.join(saved, CONFIG_DIR_NAME.replace("/", os.sep))
    except Exception:
        return ""


def _default_job_path():
    d = _config_dir()
    return os.path.join(d, "job_stream.json") if d else ""


def _stop_flag_path():
    d = _config_dir()
    return os.path.join(d, STOP_FLAG) if d else ""


def _refresh_flag_path():
    d = _config_dir()
    return os.path.join(d, REFRESH_FLAG) if d else ""


def _maybe_refresh_index(refresh_flag, idle, last_index_refresh, now, log):
    """模块3：按 flag / 空闲周期刷新身份资产索引。返回新的 last_index_refresh。"""
    should = False
    if refresh_flag and os.path.exists(refresh_flag):
        should = True
        try:
            os.remove(refresh_flag)
        except OSError:
            pass
    elif idle and (now - last_index_refresh) >= IDLE_INDEX_SECONDS:
        should = True
    if not should:
        return last_index_refresh
    try:
        from . import asset_index
        path = asset_index.write_index()
        if path and log:
            log("[MHS_STREAM] 身份资产索引已刷新: {}".format(path))
    except Exception as exc:
        if log:
            log("[MHS_WARN] 身份资产索引刷新失败: {}".format(exc))
    return time.time()


def run_loop(job_path=None, stop_flag=None, refresh_flag=None,
             max_iterations=0, idle_sleep=IDLE_SLEEP, busy_sleep=BUSY_SLEEP,
             heartbeat=True, log=None):
    """驱动循环。max_iterations>0 时到次数即停（测试用）。

    返回 {"iterations":n, "stopped":bool, "tasks_run":k}。
    单条任务异常不终止循环（一条毒素材不能杀死整条管线）。
    """
    log = log or (lambda m: progress.log(m))
    job_path = job_path or _default_job_path()
    stop_flag = stop_flag or _stop_flag_path()
    refresh_flag = refresh_flag or _refresh_flag_path()

    iterations = 0
    tasks_run = 0
    stopped = False
    last_index_refresh = 0.0

    log("[MHS_STREAM] headless drive 启动 · job={} · 停止哨兵={}".format(job_path, stop_flag))
    try:
        executor = stream_executor.get_executor(job_path)
    except Exception as exc:
        log("[MHS_ERROR] 执行器构造失败（job 是否就绪？）: {}".format(exc))
        return {"iterations": 0, "stopped": False, "tasks_run": 0, "error": str(exc)}

    while True:
        if stop_flag and os.path.exists(stop_flag):
            stopped = True
            log("[MHS_STREAM] 收到停止哨兵 → 当前段已完成，驱动退出")
            break
        if max_iterations and iterations >= max_iterations:
            break

        iterations += 1
        ran = False
        remaining = 0
        try:
            ran, remaining = executor.run_next()
            if ran:
                tasks_run += 1
        except Exception as exc:
            log("[MHS_WARN] 本轮执行异常（跳过继续）: {}".format(exc))

        now = time.time()
        if not ran:
            # 空闲：刷新索引（周期/flag）+ 写 state 心跳（watchdog 存活判据）
            last_index_refresh = _maybe_refresh_index(
                refresh_flag, True, last_index_refresh, now, log)
            if heartbeat:
                try:
                    executor.state.write()
                except Exception:
                    pass

        time.sleep(busy_sleep if remaining > 0 else idle_sleep)

    return {"iterations": iterations, "stopped": stopped, "tasks_run": tasks_run}


def request_editor_exit(reason=""):
    """请求编辑器退出（驱动结束 / 致命错误时）。

    必须做：驱动循环结束 ≠ 进程结束。不请求退出会留下僵尸编辑器——
    占着端口与资产句柄，且 watchdog 会一直判定"alive+idle"而永不退出
    （集成测试实测：驱动已退出但进程仍在，launcher 停不下来）。
    """
    try:
        import unreal
        unreal.log("[MHS_STREAM] 请求编辑器退出: {}".format(reason or "驱动结束"))
        unreal.SystemLibrary.quit_editor()
        return True
    except Exception:
        return False


def main():
    """launcher 拉起的入口（无限循环，直到 stop.flag）。

    循环结束 → 请求编辑器退出（进程生命周期归 watchdog 管）。
    """
    result = run_loop()
    progress.log("[MHS_STREAM] headless drive 退出: {}".format(result))
    request_editor_exit("驱动循环结束 stopped={}".format(result.get("stopped")))
    return result
