"""进度/日志协议：统一打 [MHS_*] 前缀，供 C++ Slate 端 FOutputDeviceRedirector 解析分发。

协议：
  [MHS_PROGRESS] 3/12 Shot_003  -> C++ 更新 SProgressBar
  [MHS_LOG]      开始处理 ...    -> C++ 追加日志框
  [MHS_WARN]     ...             -> C++ 追加黄色日志
  [MHS_ERROR]    ...             -> C++ 追加红色日志
  [MHS_DONE]     ok=10 failed=[] -> C++ 恢复按钮状态
"""

import unreal


def log(message: str):
    unreal.log("[MHS_LOG] {}".format(message))


def warn(message: str):
    unreal.log_warning("[MHS_WARN] {}".format(message))


def error(message: str):
    unreal.log_error("[MHS_ERROR] {}".format(message))


def progress(current: int, total: int, shot_name: str):
    unreal.log("[MHS_PROGRESS] {}/{} {}".format(current, total, shot_name))


def display(current: int, total: int, shot_name: str):
    """表演级显示进度（[MHS_DISPLAY] 仅驱动面板进度条，不影响 C++ 段推进——
    推进依赖 [MHS_PROGRESS] 的段级 total，两协议解耦保证正确性）。
    混合模式：j/m = 组内表演序号/总数；导入段不发（进度保持不动）。"""
    unreal.log("[MHS_DISPLAY] {}/{} {}".format(current, total, shot_name))


def done(ok_count: int, failed: list):
    unreal.log("[MHS_DONE] ok={} failed={}".format(ok_count, failed if failed else "[]"))


def result(shot_name: str, success: bool, elapsed: float):
    """每段解算结果：段名|成功(1/0)|耗时秒。C++ 端追加到结果清单。"""
    unreal.log("[MHS_RESULT] {}|{}|{:.1f}".format(shot_name, 1 if success else 0, elapsed))


def status(state: str):
    """状态灯：running / idle / error。C++ 端更新指示灯颜色。"""
    unreal.log("[MHS_STATUS] {}".format(state))


# ── 原生进度监控（C++ MHSProgressMonitor：心跳线程 + 原生小窗 + progress.json） ──
# 设计文档《解算进度监控》：深度解算同步阻塞导致 Slate 冻结，监控走游戏线程旁路。
# 阶段值：0=解算 1=导出AS 2=导出LS 3=导出FBX。
# 纪律：三处全部 try/except 包裹——进度监控失败绝不影响解算主流程（26 分钟级负载）。

STAGE_SOLVE = 0
STAGE_EXPORT_AS = 1
STAGE_EXPORT_LS = 2
STAGE_EXPORT_FBX = 3
STAGE_IMPORT = 4


def monitor_begin(perf, asset_name: str, expected_passes: int = 1):
    """解算开始前调用：注册逐帧回调 + 启动心跳线程 + 弹原生小窗。

    expected_passes：解算 Pass 数（引擎 MetaHumanPerformance.cpp ProcessComplete
    阶段推进核实）——深度 Standard/AdditionalTweakers=3、深度 Preview=2（跳过后处理）、
    单目视频=1。C++ 按此选 Pass 权重带做多 Pass 加权进度（Pass 边界由帧号回绕检测）。
    """
    try:
        unreal.MetaHumanSolverProgressLibrary.begin_progress(perf, asset_name, int(expected_passes))
    except Exception as exc:
        warn("进度监控启动失败（不影响解算）: {}".format(exc))


def monitor_begin_task(task_name: str, stage: int = STAGE_IMPORT):
    """无帧级数据的任务监控（素材导入段）：弹小窗显示 阶段名+计时+批次 [i/N]。

    C++ BeginTaskProgress（无 perf 版）；引擎未编译该 API 时降级跳过
    （try/except 兜底，绝不影响导入主流程）。
    """
    try:
        unreal.MetaHumanSolverProgressLibrary.begin_task_progress(task_name, int(stage))
    except Exception as exc:
        warn("任务进度监控启动失败（不影响导入）: {}".format(exc))


def monitor_set_task_context(total: int, concurrency: int):
    """任务进度上下文（批次导入段）：total=本组待导入 take 数，concurrency=并发转换数。

    设置后小窗显示"x/y take · 并发n · 剩余"；total<=0 清除。C++ 并发调度逐 take 更新进度。
    """
    try:
        unreal.MetaHumanSolverProgressLibrary.set_task_context(int(total), int(concurrency))
    except Exception:
        pass


def monitor_stage(stage: int):
    """阶段切换（0=解算 1=导出AS 2=导出LS 3=导出FBX）。"""
    try:
        unreal.MetaHumanSolverProgressLibrary.set_stage(stage)
    except Exception:
        pass


def monitor_batch_context(index: int, total: int, eta_seconds: float = -1.0):
    """批处理上下文（跨段持久，不随 begin/end 重置）。

    控制台/progress.json 显示 [index/total] 批次位置与批次剩余预估。
    index 从 1 计；index/total <= 0 表示清空上下文（批末/单段模式收尾）。
    eta_seconds：基于已完段平均耗时外推的批次剩余秒数，-1 = 未知。
    """
    try:
        unreal.MetaHumanSolverProgressLibrary.set_batch_context(int(index), int(total), float(eta_seconds))
    except Exception:
        pass


def monitor_end(success: bool):
    """单段结束：停心跳 + 终态展示（成功强制 100%）+ 关窗。未激活时幂等无操作。"""
    try:
        unreal.MetaHumanSolverProgressLibrary.end_progress(success)
    except Exception:
        pass
