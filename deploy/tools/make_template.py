# -*- coding: utf-8 -*-
"""构建"模板工程" MH_Template（GUI 的 [新建] 就是复制它）。

模板内容（刻意保持干净、小巧、免编译）：
    MH_Template.uproject        插件清单（从参考工程复制，无 Modules = 纯内容工程）
    Plugins/MetaHumanSolver/    uplugin + Source + Binaries（**不带 .pdb**，58MB 调试符号不分发）
    Content/Python/MetaHumanSolverEngine/   引擎 Python 包（流式管线全部逻辑）
    Content/MH_Results/         导出存储目录（占位）
    Content/ID/                身份资产目录（占位）

为什么复制而不是"新建+编译"：编译需要 VS 工具链、耗时且可能失败；
复制模板 = 免安装免编译，且工程干净（不含历史素材/身份/队列）。

用法：python make_template.py [--out <dir>]（默认 deploy/template）
"""

import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEPLOY = os.path.dirname(HERE)

PLUGIN_DIR = os.path.join(DEPLOY, "MetaHumanSolver")
PY_DIR = os.path.join(DEPLOY, "Python", "MetaHumanSolverEngine")
REF_UPROJECT = r"F:\CFH\CFH\20260709\MH_H\MH_H.uproject"

TEMPLATE_NAME = "MH_Template"

# 排除项：调试符号/中间产物/缓存
EXCLUDE_DIRS = {"Intermediate", "__pycache__", ".git"}
EXCLUDE_SUFFIX = {".pdb", ".pyc", ".exp", ".lib", ".obj"}


def _copy_tree(src, dst, label=""):
    if not os.path.isdir(src):
        raise RuntimeError("源目录不存在: {} ({})".format(src, label))
    os.makedirs(dst, exist_ok=True)
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        rel = os.path.relpath(root, src)
        target_dir = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(target_dir, exist_ok=True)
        for f in files:
            if os.path.splitext(f)[1].lower() in EXCLUDE_SUFFIX:
                continue
            shutil.copy2(os.path.join(root, f), os.path.join(target_dir, f))


def verify_plugin_modules(plugin_dir):
    """自检：.uplugin 声明的每个模块都必须有对应 DLL，否则插件加载失败 → 编辑器直接退出。

    实例教训：曾漏拷 UnrealEditor-MetaHumanSolverTakeIngest.dll →
    "无法找到模块 MetaHumanSolverTakeIngest，插件加载失败" → 新工程起不来。
    """
    up = os.path.join(plugin_dir, "MetaHumanSolver.uplugin")
    if not os.path.isfile(up):
        raise RuntimeError("插件缺少 .uplugin: {}".format(up))
    with open(up, "r", encoding="utf-8") as fh:
        modules = [m.get("Name") for m in (json.load(fh) or {}).get("Modules", [])]
    bin_dir = os.path.join(plugin_dir, "Binaries", "Win64")
    missing = [m for m in modules
               if not os.path.isfile(os.path.join(bin_dir, "UnrealEditor-{}.dll".format(m)))]
    if missing:
        raise RuntimeError("插件模块缺少 DLL（会导致工程无法启动）: {} —— 请先同步 Binaries".format(missing))
    return modules


def build_uproject(path):
    """生成模板 .uproject：沿用参考工程的插件清单（保证 MetaHuman 等依赖齐全）。"""
    plugins = []
    engine = "{7AC9F0D1-4586-8E10-215C-459FB0661066}"
    if os.path.isfile(REF_UPROJECT):
        try:
            with open(REF_UPROJECT, "r", encoding="utf-8") as fh:
                ref = json.load(fh)
            plugins = ref.get("Plugins", [])
            engine = ref.get("EngineAssociation", engine)
        except Exception:
            plugins = []
    payload = {
        "FileVersion": 3,
        "EngineAssociation": engine,
        "Category": "",
        "Description": "MetaHuman 流式解算管线模板工程",
        "Plugins": plugins,
    }
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, ensure_ascii=False, indent=1)
    return payload


def build_template(out_dir):
    """构建模板工程，返回模板目录路径。"""
    tmpl = os.path.join(out_dir, TEMPLATE_NAME)
    if os.path.isdir(tmpl):
        shutil.rmtree(tmpl)
    os.makedirs(tmpl, exist_ok=True)

    build_uproject(os.path.join(tmpl, TEMPLATE_NAME + ".uproject"))

    # ① 插件（含预编译 DLL）——先自检模块 DLL 齐全，避免做出"起不来"的模板
    modules = verify_plugin_modules(PLUGIN_DIR)
    _copy_tree(PLUGIN_DIR, os.path.join(tmpl, "Plugins", "MetaHumanSolver"), "plugin")
    verify_plugin_modules(os.path.join(tmpl, "Plugins", "MetaHumanSolver"))

    # ② 引擎 Python 包
    _copy_tree(PY_DIR, os.path.join(tmpl, "Content", "Python", "MetaHumanSolverEngine"), "python")

    # ③ 目录约定占位
    for sub in ("MH_Results", "ID"):
        os.makedirs(os.path.join(tmpl, "Content", sub), exist_ok=True)

    return tmpl


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    out = DEPLOY
    if "--out" in argv:
        out = argv[argv.index("--out") + 1]
    out = os.path.join(out, "template")
    tmpl = build_template(out)
    total = sum(os.path.getsize(os.path.join(r, f)) for r, _d, fs in os.walk(tmpl) for f in fs)
    print("[OK] 模板已构建: {}".format(tmpl))
    print("     体积: {:.1f} MB".format(total / 1024.0 / 1024.0))
    for rel in (TEMPLATE_NAME + ".uproject",
                os.path.join("Plugins", "MetaHumanSolver", "MetaHumanSolver.uplugin"),
                os.path.join("Content", "Python", "MetaHumanSolverEngine", "bridge.py")):
        print("     {} -> {}".format(rel, os.path.exists(os.path.join(tmpl, rel))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
