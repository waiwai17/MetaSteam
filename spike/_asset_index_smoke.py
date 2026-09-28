# -*- coding: utf-8 -*-
"""asset_index 编辑器冒烟：① 验证 AssetRegistry Python 绑定 ② write_index 落盘
③ 真实身份必须出现（am 的 66MB + pm 的 64.7MB）④ bridge 入口可用。"""
import json
import unreal

RESULT = r"E:\Work\Mobu_maya\MetaSol\spike\asset_index_smoke_result.txt"
lines = []
ok = True


def log(msg):
    lines.append(str(msg))
    unreal.log("[IDXSMOKE] {}".format(msg))


# ① 绑定验证
try:
    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    all_assets = reg.get_assets_by_path("/Game", True)
    log("API_OK get_assets_by_path: /Game 下 {} 个资产".format(len(all_assets)))
    cls_sample = ""
    for a in all_assets[:5]:
        try:
            cls_sample = str(a.asset_class_path.asset_name)
            break
        except Exception:
            pass
    log("asset_class_path 可读，样例类名: {}".format(cls_sample or "(空)"))
except Exception as exc:
    ok = False
    log("API_FAIL: {}".format(exc))

# ② write_index + 内容校验
try:
    from MetaHumanSolverEngine import asset_index
    path = asset_index.write_index()
    log("written: {}".format(path))
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    log("assets={} dirs={}".format(len(data["assets"]), data["dirs"]))
    for a in data["assets"]:
        log("  {:<38} {:<34} {:>7} KB  {}".format(
            a["name"], a["dir"], a["size_kb"], a["modified"]))
    names = [a["name"] for a in data["assets"]]
    for need in ("20260907_PM_ROM02_13", "0818_face_rom_am_001_11"):
        if need not in names:
            ok = False
            log("MISSING identity: {}".format(need))
    big = [a for a in data["assets"] if a["size_kb"] > 10000]
    if len(big) < 2:
        ok = False
        log("成型身份数不足（size_kb>10000 应有 2 个）: {}".format(
            [a["size_kb"] for a in data["assets"]]))
    else:
        log("成型身份 2 个（size>10MB）✓")
except Exception as exc:
    ok = False
    log("WRITE_FAIL: {}".format(exc))

# ③ bridge 入口
try:
    from MetaHumanSolverEngine import bridge
    p = bridge.refresh_asset_index()
    log("bridge.refresh_asset_index -> {}".format(p or "(空=失败)"))
    if not p:
        ok = False
except Exception as exc:
    ok = False
    log("BRIDGE_FAIL: {}".format(exc))

with open(RESULT, "w", encoding="utf-8") as fh:
    fh.write(("PASS\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
unreal.SystemLibrary.quit_editor()
