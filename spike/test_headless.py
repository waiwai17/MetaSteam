# -*- coding: utf-8 -*-
"""headless_drive 单元测试（mock 执行器）：循环语义 / 停止哨兵 / 异常韧性 /
索引刷新（flag + 空闲周期）/ 空闲心跳。

不碰真实素材：executor 用桩对象（run_next 返回预置结果）。
"""

import os
import shutil
import sys
import tempfile
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
PKG_ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(PKG_ROOT, "deploy", "Python"))

# mock unreal（仅 import 期）
_unreal = types.ModuleType("unreal")
for _attr in ("log", "log_warning", "log_error"):
    setattr(_unreal, _attr, lambda *a, **k: None)


class _Paths(object):
    ROOT = ""

    @staticmethod
    def project_saved_dir():
        return _Paths.ROOT

    @staticmethod
    def convert_relative_path_to_full(p):
        return p


_unreal.Paths = _Paths
def _unreal_getattr(name):          # PEP 562：任意类属性（pipeline 顶层注解求值用）
    return type(name, (), {})
_unreal.__getattr__ = _unreal_getattr
sys.modules.setdefault("unreal", _unreal)

import MetaHumanSolverEngine.progress as mprog   # noqa: E402
for _fn in ("log", "warn", "error", "display", "result",
            "monitor_begin_task", "monitor_end", "monitor_batch_context"):
    setattr(mprog, _fn, (lambda *a, **k: None))

from MetaHumanSolverEngine import headless_drive   # noqa: E402

OK = [True]
LINES = []


def check(name, cond, detail=""):
    line = "[{}] {}{}".format("PASS" if cond else "FAIL", name,
                              ("  -> " + str(detail)) if (detail and not cond) else "")
    print(line)
    LINES.append(line)
    if not cond:
        OK[0] = False


class _StubState(object):
    def __init__(self):
        self.writes = 0

    def write(self):
        self.writes += 1


class _StubExecutor(object):
    """run_next 按脚本返回 (ran, remaining)；第 n 次可抛异常。"""
    def __init__(self, script, raise_at=None):
        self.script = list(script)
        self.raise_at = raise_at
        self.calls = 0
        self.state = _StubState()

    def run_next(self):
        self.calls += 1
        if self.raise_at and self.calls == self.raise_at:
            raise RuntimeError("模拟单条任务异常")
        if not self.script:
            return (False, 0)
        return self.script.pop(0)


def main():
    tmp = tempfile.mkdtemp(prefix="mhs_drive_")
    try:
        saved = os.path.join(tmp, "Saved")
        cfg = os.path.join(saved, "Config", "MetaHumanSolver")
        os.makedirs(cfg)
        _Paths.ROOT = saved

        logs = []
        log = logs.append

        # ── 1) 停止哨兵：存在即退出（一次都不跑） ──
        stop = os.path.join(cfg, "stop.flag")
        open(stop, "w").close()
        ex1 = _StubExecutor([(True, 1)])
        headless_drive.stream_executor.get_executor = lambda p: ex1
        r1 = headless_drive.run_loop(stop_flag=stop, log=log)
        check("1a 停止哨兵 → 立即退出（iterations=0）", r1["iterations"] == 0, r1)
        check("1b stopped 标记为真", r1["stopped"] is True, r1)
        os.remove(stop)

        # ── 2) 语义：有积压连跑（remaining>0），空转继续等 ──
        ex2 = _StubExecutor([(True, 2), (True, 0), (False, 0)])
        headless_drive.stream_executor.get_executor = lambda p: ex2
        t0 = time.time()
        r2 = headless_drive.run_loop(stop_flag=stop, max_iterations=3,
                                     idle_sleep=0.01, busy_sleep=0.01, log=log)
        check("2a 按 max_iterations 跑满 3 轮", r2["iterations"] == 3, r2)
        check("2b 任务计数 = 成功的 2 条", r2["tasks_run"] == 2, r2)
        check("2c 有积压用短间隔（3 轮 <0.5s）", (time.time() - t0) < 0.5, time.time() - t0)

        # ── 3) 异常韧性：单条异常不终止循环 ──
        ex3 = _StubExecutor([(True, 0)], raise_at=1)
        headless_drive.stream_executor.get_executor = lambda p: ex3
        r3 = headless_drive.run_loop(stop_flag=stop, max_iterations=3,
                                     idle_sleep=0.01, busy_sleep=0.01, log=log)
        check("3a 本轮异常后仍继续（3 轮跑满）", r3["iterations"] == 3, r3)
        # 脚本 [(True,0)]：第1轮抛异常（不计数）→ 第2轮成功（计数1）→ 第3轮空闲
        check("3b 异常轮不计数、后续成功轮正常计数（tasks_run=1）",
              r3["tasks_run"] == 1 and ex3.calls == 3, r3)
        check("3c 异常有日志（不静默）",
              any("本轮执行异常" in m for m in logs), logs[-3:])

        # ── 4) 空闲心跳：state.write 被调用（watchdog 存活判据） ──
        ex4 = _StubExecutor([])            # 一直空闲
        headless_drive.stream_executor.get_executor = lambda p: ex4
        r4 = headless_drive.run_loop(stop_flag=stop, max_iterations=3,
                                     idle_sleep=0.01, busy_sleep=0.01, log=log)
        check("4a 空闲轮写心跳（3 轮 → 3 次）", ex4.state.writes == 3, ex4.state.writes)
        check("4b 空闲不退出（继续等素材）", r4["iterations"] == 3 and not r4["stopped"], r4)

        # ── 5) 模块3：refresh_assets.flag 触发索引刷新 ──
        calls = []
        _real_write = None
        try:
            from MetaHumanSolverEngine import asset_index
            _real_write = asset_index.write_index
            asset_index.write_index = lambda out_path=None: calls.append(1) or os.path.join(cfg, "identity_assets.json")
        except Exception:
            asset_index = None
        if asset_index is not None:
            refresh = os.path.join(cfg, "refresh_assets.flag")
            open(refresh, "w").close()
            ex5 = _StubExecutor([])
            headless_drive.stream_executor.get_executor = lambda p: ex5
            headless_drive.run_loop(stop_flag=stop, refresh_flag=refresh,
                                    max_iterations=1, idle_sleep=0.01, log=log)
            check("5a flag 触发刷新索引", len(calls) == 1, calls)
            check("5b 刷新后删除 flag（不重复触发）", not os.path.exists(refresh))

            # ── 6) 模块3：空闲周期自动刷新 ──
            calls.clear()
            ex6 = _StubExecutor([])
            headless_drive.stream_executor.get_executor = lambda p: ex6
            r6 = headless_drive.run_loop(stop_flag=stop, refresh_flag=refresh,
                                         max_iterations=3, idle_sleep=0.01, log=log)
            check("6a 空闲周期刷新只发生一次（非每轮）", len(calls) == 1, calls)
            asset_index.write_index = _real_write
        else:
            check("5/6 asset_index 可导入", False, "import failed")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    print("=== headless_drive 单测: {} ===".format("全部通过" if OK[0] else "存在失败"))
    with open(os.path.join(HERE, "headless_result.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
