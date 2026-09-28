"""C++ 唯一入口（plan.md 2.8）。

C++ Slate 端：
  开始解算 -> 写 job.json -> GEngine->Exec("py import MetaHumanSolverEngine.bridge; bridge.run('<json_path>')")
  取消     -> GEngine->Exec("py import MetaHumanSolverEngine.bridge; bridge.cancel()")
"""

from . import batch_runner


def run(json_path: str, skip_done: bool = True):
    batch_runner.run_batch(json_path, skip_done)


def run_next(index: int, json_path: str):
    """单段执行：只解算并导出第 index 段，完成后返回。

    混合段序列（import_then_solve）时索引落在导入段则执行该 take 导入（幂等跳过），
    落在解算段则执行该 take 解算——C++ 定时器统一驱动，段间 UI 刷新。
    """
    batch_runner.run_next(index, json_path)


def count_shots(json_path: str) -> int:
    """返回待处理总段数（用于 UI 预估剩余时间），失败返回 -1。"""
    return batch_runner.count_shots(json_path)


def dry_run(json_path: str):
    """预检：扫描素材 + 校验依赖 + 输出待跑清单，不实际解算。"""
    batch_runner.dry_run(json_path)


def precheck_import(json_path: str):
    """阶段 A 导入预检：磁盘数据完整性 + 数量盘点（不碰 UE，秒级）。"""
    batch_runner.precheck_import(json_path)


def cancel():
    batch_runner.cancel()


# ── 流式（批次3+）：C++ 定时器逐次提交 stream_next，驱动"队列消费 + 幂等补扫" ──

def stream_next(job_stream_path: str) -> int:
    """流式执行一条：补扫热文件夹 → 取队头任务执行（阻塞）。

    返回剩余待处理数（-1=job 读取失败）。返回 0 = 队列空，C++ 可停止提交/降频。
    结尾输出 [MHS_STREAM] REMAINING <n> 协议行（C++ Stream 定时器据此续跑/降频）。
    """
    try:
        from . import stream_executor
        executor = stream_executor.get_executor(job_stream_path)
        _ran, remaining = executor.run_next()
        try:
            import unreal
            unreal.log("[MHS_STREAM] REMAINING {}".format(int(remaining)))
        except Exception:
            pass
        return int(remaining)
    except Exception as exc:
        from . import progress
        progress.error("[MHS_STREAM] stream_next 失败: {}".format(exc))
        return -1


def stream_set_binding(job_stream_path: str, group: str, identity: str) -> bool:
    """面板 [确认完成]：写入 分组→身份 绑定（并冲刷该分组 pending）。

    identity 传空 = 清除绑定。返回是否成功。
    走 set_binding_direct（文件直写，不构造 executor——避免 [开始] 之前
    用旧 job_stream.json 初始化出坏单例）。
    """
    try:
        from . import stream_executor
        ok = stream_executor.set_binding_direct(job_stream_path, group, identity)
        from . import progress
        progress.log("[MHS_STREAM] 绑定确认: 分组 '{}' -> {}".format(group, identity or "(清除)"))
        return ok
    except Exception as exc:
        from . import progress
        progress.error("[MHS_STREAM] 绑定写入失败: {}".format(exc))
        return False


def stream_status(job_stream_path: str) -> str:
    """队列状态快照（JSON 字符串，面板/Web 监控用）。"""
    try:
        import json
        from . import stream_executor
        executor = stream_executor.get_executor(job_stream_path)
        counts = executor.queue.counts()
        return json.dumps({"counts": counts,
                           "bindings": executor.bindings.as_dict()},
                          ensure_ascii=False)
    except Exception as exc:
        return '{"error": "%s"}' % exc


def stream_stop():
    """丢弃执行器单例（面板 [停止]：当前段自然结束后调用，队列状态已持久化）。"""
    from . import stream_executor
    stream_executor.reset_executor()
    return True


def refresh_asset_index() -> str:
    """刷新身份资产索引（identity_assets.json）→ 返回写入路径（空=失败）。

    面板 [▾ 选择] / 外部 UI（写 refresh_assets.flag 后由驱动循环调用）共用。
    """
    try:
        from . import asset_index
        path = asset_index.write_index()
        from . import progress
        if path:
            progress.log("[MHS_LOG] 身份资产索引已刷新: {}（{} 个身份）".format(
                path, len(_index_assets_count(path))))
        return path
    except Exception as exc:
        from . import progress
        progress.error("[MHS_STREAM] 身份资产索引刷新失败: {}".format(exc))
        return ""


def _index_assets_count(path):
    """读回索引里资产数（供日志）。"""
    import json as _json
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return _json.load(fh).get("assets", [])
    except Exception:
        return []
