# -*- coding: utf-8 -*-
"""模板工程测试：① 从真实 deploy 源构建模板 ② 从模板创建新工程（改名/配置/目录约定）
③ 错误路径（目标已存在 / 模板缺失 / 空工程名）④ CLI new。
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "deploy", "tools")
sys.path.insert(0, TOOLS)

import launcher       # noqa: E402
import make_template  # noqa: E402

OK = [True]
LINES = []


def check(name, cond, detail=""):
    line = "[{}] {}{}".format("PASS" if cond else "FAIL", name,
                              ("  -> " + str(detail)) if (detail and not cond) else "")
    print(line)
    LINES.append(line)
    if not cond:
        OK[0] = False


def write_file(path, text="x"):
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def main():
    tmp = tempfile.mkdtemp(prefix="mhs_tmpl_")
    try:
        # ── 1) 从真实源构建模板（集成） ──
        tmpl = make_template.build_template(tmp)
        up = os.path.join(tmpl, "MH_Template.uproject")
        check("1a 模板构建成功（.uproject 存在）", os.path.isfile(up), os.listdir(tmpl))
        check("1b 插件 uplugin 就位",
              os.path.isfile(os.path.join(tmpl, "Plugins", "MetaHumanSolver",
                                          "MetaHumanSolver.uplugin")))
        check("1c 预编译 DLL 就位（免编译★）",
              os.path.isfile(os.path.join(tmpl, "Plugins", "MetaHumanSolver", "Binaries",
                                          "Win64", "UnrealEditor-MetaHumanSolver.dll")))
        check("1c-2 两个模块 DLL 齐全（缺一个工程就起不来★）",
              os.path.isfile(os.path.join(tmpl, "Plugins", "MetaHumanSolver", "Binaries",
                                          "Win64", "UnrealEditor-MetaHumanSolver.dll"))
              and os.path.isfile(os.path.join(tmpl, "Plugins", "MetaHumanSolver", "Binaries",
                                              "Win64", "UnrealEditor-MetaHumanSolverTakeIngest.dll")),
              os.listdir(os.path.join(tmpl, "Plugins", "MetaHumanSolver", "Binaries", "Win64")))
        check("1d 不分发 .pdb（58MB 调试符号★）",
              len([f for _r, _d, fs in os.walk(tmpl) for f in fs if f.endswith(".pdb")]) == 0)
        check("1e 引擎 Python 包就位",
              os.path.isfile(os.path.join(tmpl, "Content", "Python",
                                          "MetaHumanSolverEngine", "bridge.py")))
        check("1f 目录约定占位（MH_Results / ID）",
              os.path.isdir(os.path.join(tmpl, "Content", "MH_Results"))
              and os.path.isdir(os.path.join(tmpl, "Content", "ID")))
        with open(up, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        check("1g .uproject 含插件清单（MetaHuman 依赖齐）",
              any(p.get("Name") == "MetaHuman" for p in data.get("Plugins", [])),
              [p.get("Name") for p in data.get("Plugins", [])][:6])

        # ── 2) 从模板创建新工程 ──
        parent = os.path.join(tmp, "projects")
        res = launcher.create_project_from_template("MH_Line01", parent, template_dir=tmpl)
        dest = res["dir"]
        check("2a 新工程目录创建", os.path.isdir(dest))
        check("2b .uproject 改名为工程名",
              os.path.isfile(os.path.join(dest, "MH_Line01.uproject")))
        check("2c 旧模板名 .uproject 已移除",
              not os.path.exists(os.path.join(dest, "MH_Template.uproject")))
        check("2d 插件被复制", os.path.isdir(os.path.join(dest, "Plugins", "MetaHumanSolver")))
        check("2e Python 包被复制",
              os.path.isfile(os.path.join(dest, "Content", "Python",
                                          "MetaHumanSolverEngine", "bridge.py")))
        cfg_path = res["config"]
        with open(cfg_path, "r", encoding="utf-8") as fh:
            cfg = json.load(fh)
        check("2f 工程自带 pipeline.config.json", os.path.isfile(cfg_path))
        check("2g 配置指向新工程", cfg["project"].endswith("MH_Line01.uproject"), cfg["project"])
        check("2h instance = 工程名", cfg["instance"] == "MH_Line01", cfg.get("instance"))
        inbox = cfg["inbox"]
        check("2i 热文件夹含目录约定（am/pm/_id★）",
              all(os.path.isdir(os.path.join(inbox, g)) for g in ("am", "pm", "_id")), inbox)
        check("2j 输出目录已建", os.path.isdir(cfg["fbx_output"]))

        # 新工程可用：doctor 预检（编辑器/工程/热文件夹/引导脚本）
        errs, warns = launcher.precheck(cfg, launcher.derive_paths(cfg))
        check("2k 新工程预检无 error（可直接启动★）", errs == [], errs)

        # ── 3) 错误路径 ──
        try:
            launcher.create_project_from_template("MH_Line01", parent, template_dir=tmpl)
            check("3a 目标已存在 → 抛错", False, "未抛")
        except Exception as exc:
            check("3a 目标已存在 → 抛错（不覆盖★）", "已存在" in str(exc), str(exc))
        try:
            launcher.create_project_from_template("", parent, template_dir=tmpl)
            check("3b 空工程名 → 抛错", False, "未抛")
        except Exception as exc:
            check("3b 空工程名 → 抛错", True)
        try:
            launcher.create_project_from_template("X", parent,
                                                  template_dir=os.path.join(tmp, "no_tmpl"))
            check("3c 模板缺失 → 抛错", False, "未抛")
        except Exception as exc:
            check("3c 模板缺失 → 抛错", "模板工程不存在" in str(exc), str(exc))

        # ── 3d) 模块自检：缺 DLL 的插件源 → 构建模板应失败（防做出坏模板★） ──
        bad_src = os.path.join(tmp, "bad_plugin")
        shutil.copytree(make_template.PLUGIN_DIR, bad_src)
        os.remove(os.path.join(bad_src, "Binaries", "Win64",
                               "UnrealEditor-MetaHumanSolverTakeIngest.dll"))
        try:
            make_template.verify_plugin_modules(bad_src)
            check("3d 缺模块 DLL → 自检报错", False, "未报错")
        except Exception as exc:
            check("3d 缺模块 DLL → 自检报错★", "MetaHumanSolverTakeIngest" in str(exc), str(exc))

        # ── 4) CLI new（子进程） ──
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "launcher.py"), "new",
                            "--name", "MH_CLI", "--dir", parent, "--template", tmpl],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = r.stdout.decode("utf-8", "ignore")
        check("4a CLI new 退出码 0", r.returncode == 0, out[-300:])
        check("4b 新工程文件存在",
              os.path.isfile(os.path.join(parent, "MH_CLI", "MH_CLI.uproject")), out[-200:])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    print("=== 模板工程测试: {} ===".format("全部通过" if OK[0] else "存在失败"))
    with open(os.path.join(HERE, "template_result.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
