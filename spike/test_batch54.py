# -*- coding: utf-8 -*-
"""批次5.4 · e011 事故回归：失败任务复活 + 素材目录预检

完整复现 2026-09-21 e011 事故链：
  10:53 拷入 → 发现 → 导入中编辑器关闭 → 恢复机制重排队 →
  素材已被移走（目录不存在）→ 导入失败"未找到已导入素材"（误导）→
  11:50 素材放回 → 永远无人再处理（失败三不管）。

四项修复的验证：
  ① _revive_failed：素材放回（目录 mtime > 失败时刻）→ 自动重新排队
  ② mtime 判据防循环：持续失败的素材不无限重试
  ③ 目录预检：素材被移走时给出准确错误（而非"未找到已导入素材"）
  ④ tasks_detail.failed：失败明细进状态文件（控制台"已失败"区块数据源）

环境：mock unreal + stub 就绪规则（解耦 DefaultRule 文件清单，聚焦复活逻辑）。
"""

import json
import os
import shutil
import sys
import tempfile
import time
import types

PKG_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(PKG_ROOT, "deploy", "Python"))

# mock unreal（仅 import 期）
_unreal = types.ModuleType("unreal")
for _attr in ("log", "log_warning", "log_error"):
    setattr(_unreal, _attr, lambda *a, **k: None)
_unreal.Paths = types.SimpleNamespace(project_saved_dir=lambda: "")
_unreal.get_editor_subsystem = lambda *a, **k: None
_unreal.EditorAssetLibrary = types.SimpleNamespace(
    does_asset_exist=lambda p: False, list_assets=lambda p, recursive=True: [])
def _unreal_getattr(name):
    return type(name, (), {})
_unreal.__getattr__ = _unreal_getattr
sys.modules.setdefault("unreal", _unreal)

from MetaHumanSolverEngine import stream_executor
import MetaHumanSolverEngine.progress as mprog

# progress 在 mock 环境下安全化（executor 运行路径会调用）
for _fn in ("log", "warn", "error", "display", "result",
            "monitor_begin_task", "monitor_end", "monitor_batch_context"):
    setattr(mprog, _fn, (lambda *a, **k: None))

OK = [True]
LINES = []


def check(name, cond, detail=""):
    line = "[{}] {}{}".format("PASS" if cond else "FAIL", name,
                              ("  -> " + str(detail)) if (detail and not cond) else "")
    print(line)
    LINES.append(line)
    if not cond:
        OK[0] = False


def main():
    tmp = tempfile.mkdtemp(prefix="mhs_b54_")
    try:
        job = os.path.join(tmp, "job_stream.json")
        q_path = os.path.join(tmp, "queue.json")
        b_path = os.path.join(tmp, "bindings.json")
        s_path = os.path.join(tmp, "state.json")
        inbox = os.path.join(tmp, "inbox", "pm")
        os.makedirs(inbox)
        with open(job, "w", encoding="utf-8") as fh:
            json.dump({"mode": "depth", "stream_mode": "semi", "import_only": True,
                       "identity_path": "", "inbox": os.path.join(tmp, "inbox"),
                       "capture_root": "/Game/CaptureManager/T",
                       "storage_path": "/Game/MH_Results",
                       "import_mode": "cpp", "import_footage": True,
                       "media_output_root": "X:/m", "fbx_output_dir": "X:/o",
                       "stream_queue_path": q_path,
                       "stream_bindings_path": b_path,
                       "stream_state_path": s_path}, fh)

        stream_executor.reset_executor()
        ex = stream_executor.get_executor(job)
        ex.scanner.rule = types.SimpleNamespace(  # 解耦 DefaultRule，聚焦复活逻辑
            is_ready=lambda d: True, describe=lambda: "stub")

        take_dir = os.path.join(inbox, "take_a")
        os.makedirs(take_dir)
        with open(os.path.join(take_dir, "take.json"), "w") as fh:
            fh.write("{}")
        T_OLD = time.time() - 1000
        os.utime(take_dir, (T_OLD, T_OLD))          # 目录 mtime = 久远（拷入时）

        q = ex.queue
        q.enqueue("pm/take_a", take_dir, kind="perf", identity="ID_pm", group="pm")
        q.set_status("pm/take_a", "failed", "未找到已导入素材（检查导入是否成功）")
        time.sleep(0.01)

        # ── ① mtime 判据：素材未动 → 不复活 ──
        n = ex._revive_failed()
        check("1a 目录 mtime 早于失败时刻 → 不复活", n == 0, n)
        check("1b 状态保持 failed", q.get("pm/take_a")["status"] == "failed")

        # ── ② 素材放回（mtime 变新）→ 复活 ──
        time.sleep(0.01)
        os.utime(take_dir, None)                     # mtime = now（重新拷贝）
        n = ex._revive_failed()
        check("2a 素材放回 → 复活 1 条", n == 1, n)
        check("2b 状态推进 pending", q.get("pm/take_a")["status"] == "pending")
        evs = [e["tx"] for e in ex.state.events]
        check("2c 事件含'素材已恢复，重新排队'",
              any("素材已恢复" in t for t in evs), evs[:3])

        # ── ③ 防循环：复活后再失败（目录没再变）→ 不复活 ──
        q.set_status("pm/take_a", "failed", "再失败")
        n = ex._revive_failed()
        check("3a 持续失败（目录未变）→ 不复活（防无限重试）", n == 0, n)

        # ── ④ 再放回 → 再复活（用户重新拷贝才触发）──
        os.utime(take_dir, None)
        n = ex._revive_failed()
        check("4a 重新拷贝后再次复活", n == 1, n)

        # ── ⑤ 素材被移走 → 不复活不崩（e011 @11:38 的真实场景）──
        q.set_status("pm/take_a", "failed", "素材目录不存在")
        away = os.path.join(tmp, "take_a_away")
        shutil.move(take_dir, away)
        n = ex._revive_failed()
        check("5a 目录不存在 → 不复活不崩", n == 0, n)

        # ── ⑥ 目录预检：移走状态下执行 → 准确错误（非"未找到已导入素材"）──
        q.set_status("pm/take_a", "pending")
        task = q.next_pending()
        ex._run_perf_task(task)
        t = q.get("pm/take_a")
        check("6a 预检失败 → status=failed", t["status"] == "failed", t["status"])
        check("6b 错误含'素材目录不存在'（准确归因）",
              "素材目录不存在" in (t.get("error") or ""), t.get("error"))

        # ── ⑦ tasks_detail.failed（控制台'已失败'区块数据源）──
        snap = ex.state.snapshot()
        detail = snap.get("tasks_detail", {})
        check("7a tasks_detail 含 failed 列表", "failed" in detail, list(detail.keys()))
        fkeys = [f["key"] for f in detail.get("failed", [])]
        check("7b failed 列表含失败任务", "pm/take_a" in fkeys, fkeys)
        ferr = [f.get("error", "") for f in detail.get("failed", [])
                if f["key"] == "pm/take_a"]
        check("7c failed 携带错误原因", ferr and "素材目录不存在" in ferr[0], ferr)

        # ── ⑧ 放回素材 → 复活（真实'放回'=重新拷贝：拷贝过程写入文件会刷新目录 mtime，
         #    e011 实测 CreationTime=11:50 证明；move 重命名保留旧 mtime，故补 utime 模拟）──
        shutil.move(away, take_dir)
        os.utime(take_dir, None)
        n = ex._revive_failed()
        check("8a 物理放回目录 → 复活", n == 1, n)
    finally:
        stream_executor.reset_executor()
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    print("=== 批次5.4 e011事故回归: " + ("全部通过" if OK[0] else "存在失败") + " ===")
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "batch54_result.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
