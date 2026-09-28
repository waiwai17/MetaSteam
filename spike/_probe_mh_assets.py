# -*- coding: utf-8 -*-
"""探针：确认工程内"LS 重建所需的 MetaHuman 资产"是否齐全（不跑解算）。

在编辑器里执行（-ExecCmds="py <本文件>"）：
    ① AssetRegistry 查 /Game/MetaHumans 下的角色蓝图（BP_*）
    ② 加载 pipeline 用到的关键资产（Face 骨架 / CtrlRig）
    ③ 直接调 pipeline._find_default_meta_human_class 看能否选中 target
结果写 <Project>/Saved/probe_mh_assets.json（供外部读取）。
"""

import json
import os
import traceback

result = {"ok": False, "blueprints": [], "assets": {}, "chosen": "", "errors": []}

try:
    import unreal

    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    ar = unreal.ARFilter(package_paths=["/Game/MetaHumans"], recursive_paths=True,
                         class_paths=[unreal.TopLevelAssetPath("/Script/Engine", "Blueprint")])
    for a in reg.get_assets(ar):
        result["blueprints"].append(str(a.package_name))

    for p in ("/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton",
              "/Game/MetaHumans/Common/Face/Face_Archetype",
              "/Game/MetaHumans/Common/Face/Face_ControlBoard_CtrlRig"):
        result["assets"][p] = unreal.load_asset(p) is not None

    try:
        import MetaHumanSolverEngine.pipeline as pipeline
        result["chosen"] = pipeline._find_default_meta_human_class("probe") or ""
    except Exception as exc:
        result["errors"].append("pipeline: {}".format(exc))

    result["ok"] = bool(result["chosen"]) and all(result["assets"].values())
except Exception as exc:
    result["errors"].append(traceback.format_exc())
    try:
        import unreal
        unreal.log_error("[PROBE] {}".format(exc))
    except Exception:
        pass

try:
    import unreal
    out = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
    path = os.path.join(out, "probe_mh_assets.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(result, fh, ensure_ascii=False, indent=1)
    unreal.log("[PROBE] 结果: ok={} chosen={} bp数={} -> {}".format(
        result["ok"], result["chosen"], len(result["blueprints"]), path))
    unreal.SystemLibrary.quit_editor()
except Exception:
    pass
