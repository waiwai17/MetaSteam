# -*- coding: utf-8 -*-
"""批次5.3 · 根因回归测试：job 指纹重建 + 绑定直写（不构造坏单例）

复现用户场景：
  确认身份（旧 job 存在）→ [开始] 重写 job（新 inbox）→ stream_next 必须用新配置。
批次5.2 前的缺陷：确认时用旧 job 构造单例 → inbox 为空 → 发现层失明（队列永远 0）。

环境：mock unreal（stream_executor 传递依赖 batch_runner→unreal，stub 掉即可 import）。
"""

import json
import os
import shutil
import sys
import tempfile
import types

PKG_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(PKG_ROOT, "deploy", "Python"))

# mock unreal（仅 import 期——stub 不被实际调用）
_unreal = types.ModuleType("unreal")
for _attr in ("log", "log_warning", "log_error"):
    setattr(_unreal, _attr, lambda *a, **k: None)
_unreal.Paths = types.SimpleNamespace(project_saved_dir=lambda: "")
_unreal.get_editor_subsystem = lambda *a, **k: None
_unreal.EditorAssetLibrary = types.SimpleNamespace(
    does_asset_exist=lambda p: False, list_assets=lambda p, recursive=True: [])
def _unreal_getattr(name):          # PEP 562：任意类属性（pipeline 顶层注解求值用）
    return type(name, (), {})
_unreal.__getattr__ = _unreal_getattr
sys.modules.setdefault("unreal", _unreal)

from MetaHumanSolverEngine import stream_executor

OK = [True]
LINES = []


def check(name, cond, detail=""):
    line = "[{}] {}{}".format("PASS" if cond else "FAIL", name,
                              ("  -> " + str(detail)) if (detail and not cond) else "")
    print(line)
    LINES.append(line)
    if not cond:
        OK[0] = False


def write_job(path, inbox):
    with open(path, "w", encoding="utf-8") as fh:
        json.dump({"mode": "depth", "stream_mode": "semi", "import_only": True,
                   "identity_path": "", "inbox": inbox,
                   "capture_root": "/Game/CaptureManager/T",
                   "storage_path": "/Game/MH_Results",
                   "import_mode": "cpp", "import_footage": True,
                   "media_output_root": "X:/m", "fbx_output_dir": "X:/o",
                   "stream_queue_path": q_path,
                   "stream_bindings_path": b_path,
                   "stream_state_path": s_path}, fh)


def main():
    global q_path, b_path, s_path
    tmp = tempfile.mkdtemp(prefix="mhs_b53_")
    try:
        job = os.path.join(tmp, "job_stream.json")
        q_path = os.path.join(tmp, "queue.json")
        b_path = os.path.join(tmp, "bindings.json")
        s_path = os.path.join(tmp, "state.json")

        # ── 场景复现：旧 job（坏 inbox）→ 构造单例 → [开始] 写新 job（好 inbox）──
        write_job(job, inbox="")                    # 旧格式：inbox 空（坏）
        stream_executor.reset_executor()
        ex1 = stream_executor.get_executor(job)     # 模拟"确认身份"触发构造
        check("1a 旧 job 构造单例（inbox 空）", ex1.scanner.inbox == "")

        write_job(job, inbox="D:/new_inbox")        # [开始] 重写 job（指纹变化）
        ex2 = stream_executor.get_executor(job)
        check("1b job 变更后单例重建（不再是旧实例）", ex2 is not ex1)
        check("1c 新单例使用新 inbox", ex2.scanner.inbox == "D:/new_inbox")

        # job 未变 → 复用单例
        ex3 = stream_executor.get_executor(job)
        check("1d job 未变复用单例", ex3 is ex2)

        # ── 绑定直写（不构造单例）──
        stream_executor.reset_executor()
        ok = stream_executor.set_binding_direct(job, "pm", "ID_pm")
        check("2a 绑定直写成功", ok is True)
        with open(b_path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        check("2b 绑定落盘（job 配置路径）", data.get("bindings", {}).get("pm") == "ID_pm")
        check("2c 绑定直写不构造单例", stream_executor._executor is None)

        # 绑定文件被下一单例读取（[开始] 后 executor 同源生效）
        ex4 = stream_executor.get_executor(job)
        check("2d 新单例加载既有绑定", ex4.bindings.get("pm") == "ID_pm"
              and ex4.enqueuer.bindings.get("pm") == "ID_pm")

        # job 不存在时的绑定直写（回落默认路径不崩）
        ok2 = stream_executor.set_binding_direct(os.path.join(tmp, "nope.json"), "am", "ID_am")
        check("2e job 缺失时绑定回落默认路径", ok2 is True)
    finally:
        stream_executor.reset_executor()
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    print("=== 批次5.3 根因回归: " + ("全部通过" if OK[0] else "存在失败") + " ===")
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "batch53_result.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
