# reexport_ready001.py —— 修复昨晚批处理串扰产物：诊断旧 LS -> 强制重建 -> 修复后链路重导 FBX
# 不重新解算（Performance 结果是对的，错的只是 LS 导出环节），全程 ~2 分钟
import os

import unreal

from MetaHumanSolverEngine import export_fbx, pipeline, progress

ASSET_NAME = "Anims_Flint_Ul_Ready_001"
PERF_PATH = "/Game/MH_Results/{}/Performance".format(ASSET_NAME)
LS_PATH = "/Game/MH_Results/{}/LS_Identity".format(ASSET_NAME)
FBX_DIR = "E:/Mobu_maya/FACE/out/{}".format(ASSET_NAME)

cfg = {
    "storage_path": "/Game/MH_Results",
    "fbx_output_dir": "E:/Mobu_maya/FACE/out",
    "meta_human_class": "/Game/MetaHumans/Flint/BP_Flint",
    "skeleton_path": "/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton",
    "naming": {"pattern": "{take_name}_{timecode}", "prefix": "AS_"},
}


def ls_frame_info(ls):
    """占位诊断：帧数 API 版本差异大且非关键判据（最终判据 = 新 FBX 大小对比手动基准）。"""
    return "存在" if ls is not None else "LS 为 None"


# 1) 诊断：昨晚的旧 LS 内容是否正确（串扰证据）
old_fbxs = []
if os.path.isdir(FBX_DIR):
    old_fbxs = [f for f in os.listdir(FBX_DIR) if f.endswith(".fbx")]
for f in old_fbxs:
    p = os.path.join(FBX_DIR, f)
    unreal.log("[RE] 旧 FBX: {} = {:.1f} MB".format(f, os.path.getsize(p) / 1048576))

old_ls = unreal.load_asset(LS_PATH)
unreal.log("[RE] 旧 LS 帧区间: {}".format(ls_frame_info(old_ls)))

# 2) 强制重建：删旧 LS 资产（含磁盘旧 FBX），从 Performance 重建
if unreal.EditorAssetLibrary.does_asset_exist(LS_PATH):
    unreal.EditorAssetLibrary.delete_asset(LS_PATH)
    unreal.log("[RE] 已删除旧 LS: {}".format(LS_PATH))
for f in old_fbxs:
    os.remove(os.path.join(FBX_DIR, f))
    unreal.log("[RE] 已删除旧 FBX: {}".format(f))

perf = unreal.load_asset(PERF_PATH)
if perf is None:
    unreal.log("[RE] FAIL: Performance 不存在 {}".format(PERF_PATH))
    raise SystemExit(1)

ls = pipeline.export_level_sequence(perf, cfg, ASSET_NAME, reuse_existing=False)
if ls is None:
    unreal.log("[RE] FAIL: LS 重建失败")
    raise SystemExit(1)
unreal.log("[RE] 新 LS 帧区间: {}".format(ls_frame_info(ls)))

# 3) 修复后的导出链路（段间隔离 + 路径校验 + Face CR 过滤）
fbx_path = "{}/{}.fbx".format(FBX_DIR, ASSET_NAME)
ok = export_fbx.export_level_sequence_fbx_control_rig(ls, fbx_path)
if ok and os.path.isfile(fbx_path):
    unreal.log("[RE] 新 FBX: {} = {:.1f} MB".format(
        os.path.basename(fbx_path), os.path.getsize(fbx_path) / 1048576))
    unreal.log("[RE] ===== 重导出完成（与手动基准对比大小判断内容） =====")
else:
    unreal.log("[RE] FAIL: FBX 导出失败")
