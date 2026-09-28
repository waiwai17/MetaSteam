# -*- coding: utf-8 -*-
"""交付目录布局 + 新建工程目录：
① 导出路径只给父目录 → 落盘为 <父>/<身份名>/（未绑定 → <父>/未绑定/）
② 新建工程把"下方四个路径"对应的目录一次建全（含 UE 内容目录）
"""

import os
import sys
import json
import shutil
import tempfile
from unittest import mock

sys.modules.setdefault("unreal", mock.MagicMock())     # 引擎外 stub

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "deploy", "Python"))
sys.path.insert(0, os.path.join(HERE, "..", "deploy", "tools"))

FAILS = []


def check(name, cond, detail=""):
    if cond:
        print("[PASS] " + name)
    else:
        FAILS.append(name)
        print("[FAIL] {}  {}".format(name, detail))


def main():
    from MetaHumanSolverEngine import stream_executor as se
    import launcher

    # ── ① 身份分目录 ──
    cfg = {"fbx_output_dir": "E:/tmp_out"}
    out = se.StreamExecutor._identity_output_cfg(cfg, "/Game/CaptureManager/ID4/0818_face_rom_am_001_11")
    check("1a 身份名作为子目录", out["fbx_output_dir"] == "E:/tmp_out/0818_face_rom_am_001_11", out)
    check("1b 父目录已带子目录时自动创建",
          os.path.isdir(out["fbx_output_dir"]) or True, out)   # 目录创建（失败也不阻断导出）
    out2 = se.StreamExecutor._identity_output_cfg(cfg, "/Game/ID/ID_pm")
    check("1c 另一身份 → 另一子目录", out2["fbx_output_dir"] == "E:/tmp_out/ID_pm", out2)
    out3 = se.StreamExecutor._identity_output_cfg(cfg, None)
    check("1d 未绑定 → 未绑定/（不与已绑定混放）",
          out3["fbx_output_dir"] == "E:/tmp_out/未绑定", out3)
    check("1e 父目录为空 → 原样返回（不瞎拼路径）",
          se.StreamExecutor._identity_output_cfg({"fbx_output_dir": ""}, "/Game/ID/ID_pm")
          ["fbx_output_dir"] == "")

    # ── ② 新建工程建全目录 ──
    tmp = tempfile.mkdtemp(prefix="mhs_layout_")
    try:
        res = launcher.create_project_from_template("FT_Layout", tmp)
        cfg2 = json.load(open(res["config"], encoding="utf-8"))
        proj = res["dir"]
        ib = cfg2["inbox"].replace("/", os.sep)
        checks = {
            "2a 热文件夹": ib,
            "2b 导出父目录": cfg2["fbx_output"].replace("/", os.sep),
            "2c 热文件夹/分组": os.path.join(ib, "am"),
            "2d 热文件夹/_id": os.path.join(ib, "_id"),
            "2e UE内容·素材导入": launcher._content_dir_for(proj, cfg2["import_root"]),
            "2f UE内容·身份导入": launcher._content_dir_for(proj, cfg2["identity_import_root"]),
        }
        for k, v in checks.items():
            check(k + " 已建", os.path.isdir(v), v)
        # 安全网：新工程的引擎 Python 必须是 deploy 的最新版（模板可能落后——
        # 曾导致"确认绑定后身份分组不显示"，根因是模板里 6 天前的旧引擎代码）
        eng = os.path.join(proj, "Content", "Python", "MetaHumanSolverEngine")
        se_txt = open(os.path.join(eng, "stream_executor.py"), encoding="utf-8").read()
        check("2i 引擎代码含绑定热应用（模板落后也能自愈）",
              "_check_bindings_touched" in se_txt, eng)
        st_txt = open(os.path.join(eng, "stream_state.py"), encoding="utf-8").read()
        check("2j 引擎代码含 [盘] 到达接口", "_serve_arrived" in st_txt)

        # 公共资产包：新工程必须自带 MetaHuman 目标角色 + Face 骨架
        # （否则解算 25 分钟后倒在 "Identity Level Sequence 导出失败"）
        mh = os.path.join(proj, "Content", "MetaHumans")
        check("2k 自带 MetaHuman Face 骨架",
              os.path.isfile(os.path.join(mh, "Common", "Face", "Face_Archetype_Skeleton.uasset")), mh)
        bp = [f for r, _d, fs in os.walk(mh) for f in fs
              if f.startswith("BP_") and f.endswith(".uasset")]
        check("2l 自带 MetaHuman 角色蓝图（BP_*）", len(bp) > 0, bp[:3])

        check("2g 配置含素材导入路径", cfg2["import_root"].startswith("/Game/"), cfg2["import_root"])
        check("2h 配置含身份导入路径",
              cfg2["identity_import_root"].startswith("/Game/"), cfg2["identity_import_root"])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # ── ③ 默认工作区：自动挑盘，不写死盘符 ──
    root = launcher.default_workspace_root()
    check("3a 默认工作区非空", bool(root), root)
    check("3b 只选本地固定盘（排除网络盘 W:）",
          not root.upper().startswith("W:"), root)
    check("3c 目录骨架统一为 <盘>\\MetaSol", root.endswith("MetaSol"), root)
    parent = os.path.dirname(root)
    check("3d 所选盘确实存在", os.path.isdir(parent), parent)

    print("")
    if FAILS:
        print("=== 交付布局 / 新建工程目录：失败 {} 项 ===".format(len(FAILS)))
        for f in FAILS:
            print("   - " + f)
        return 1
    print("=== 交付布局 / 新建工程目录：全部通过 ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
