"""ControlRig FACs FBX 导出（交付标准产物）。

从 Level Sequence 的 ControlRig Section 导出 FACs 驱动曲线（进 Maya/MotionBuilder 的正解）：
  - unreal.LevelSequenceEditorBlueprintLibrary.open_level_sequence
  - unreal.ControlRigSequencerLibrary.get_control_rigs / export_fbx_from_control_rig_section
  - unreal.MovieSceneUserExportFBXControlRigSettings
    + load_control_mappings_from_preset(True)  ← 等价于手动导出时选「MetaHuman控制映射」

另保留 AnimSequence -> 烘焙 FBX（export_anim_sequence_to_fbx）作为通用备选导出。
"""

import os

import unreal

from . import progress


def _ensure_out_dir(fbx_path: str):
    """确保 FBX 输出目录存在（run_asset_export_task 不会自动创建目录，
    目录不存在时导出会静默失败——2026-08-14 实测：新素材子目录缺失导致 FBX 未落盘）。"""
    out_dir = os.path.dirname(fbx_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)


def export_anim_sequence_to_fbx(anim_sequence, fbx_path: str, options=None) -> bool:
    """把 AnimSequence 导出为磁盘 FBX。成功返回 True，失败打 [MHS_ERROR] 并返回 False。"""
    if anim_sequence is None:
        progress.error("export_fbx: anim_sequence 为 None")
        return False

    _ensure_out_dir(fbx_path)
    task = unreal.AssetExportTask()
    task.set_editor_property("object", anim_sequence)
    task.set_editor_property("exporter", unreal.AnimSequenceExporterFBX())
    task.set_editor_property("filename", fbx_path)
    task.set_editor_property("selected", False)
    task.set_editor_property("replace_identical", True)
    task.set_editor_property("prompt", False)
    task.set_editor_property("automated", True)
    if options is not None:
        task.set_editor_property("options", options)

    ok = unreal.Exporter.run_asset_export_task(task)
    if not ok:
        progress.error("export_fbx: FBX 导出失败 -> {}".format(fbx_path))
    return ok


def _open_sequence_verified(level_sequence) -> bool:
    """段间隔离地打开目标 LS：先关当前序列（清掉上一段的 Sequencer 状态），
    再 open 目标并用完整路径校验（所有 LS 短名固定 LS_Identity，短名校验形同虚设——
    批处理段间 open 未就绪时 get_current 会返回上一段的 LS，导错段 FBX 小一个量级）。
    校验不符重试一次（Sequencer 切换的异步窗口），仍不符才报错。"""
    try:
        unreal.LevelSequenceEditorBlueprintLibrary.close_level_sequence()
    except Exception:
        pass  # 无已打开序列时忽略
    target_path = level_sequence.get_path_name()
    for attempt in (1, 2):
        unreal.LevelSequenceEditorBlueprintLibrary.open_level_sequence(level_sequence)
        current = unreal.LevelSequenceEditorBlueprintLibrary.get_current_level_sequence()
        if current and current.get_path_name() == target_path:
            return True
        progress.warn("Sequencer 序列切换未就绪（第 {} 次，当前: {}），重试".format(
            attempt, current.get_path_name() if current else "<无>"))
    progress.error("export_fbx: Sequencer 未能打开目标 Level Sequence: {}".format(target_path))
    return False


def export_level_sequence_fbx_control_rig(level_sequence, fbx_path: str) -> bool:
    """从 Level Sequence 的 ControlRig Section 导出 FBX（FACs 驱动曲线，供 Maya/MotionBuilder）。

    参考同事 run_metahuman.py 的 export_level_sequence_to_fbx（已在本项目验证）。
    """
    if level_sequence is None:
        progress.error("export_fbx: level_sequence 为 None")
        return False

    try:
        _ensure_out_dir(fbx_path)
        if not _open_sequence_verified(level_sequence):
            return False
        current_seq = unreal.LevelSequenceEditorBlueprintLibrary.get_current_level_sequence()

        control_rigs = unreal.ControlRigSequencerLibrary.get_control_rigs(current_seq)
        if not control_rigs:
            progress.error("export_fbx: Level Sequence 中没有 Control Rig")
            return False

        # 按 ControlRig 名优先取 Face 面部板（多 CR 序列时 control_rigs[0] 顺序不定）
        chosen = next((cr for cr in control_rigs
                       if "Face" in cr.control_rig.get_name()), control_rigs[0])
        section = chosen.track.get_section_to_key()
        if not section:
            progress.error("export_fbx: Control Rig 没有可导出的 Section")
            return False

        settings = unreal.MovieSceneUserExportFBXControlRigSettings()
        settings.set_editor_property("export_file_name", fbx_path)
        settings.load_control_mappings_from_preset(True)

        result = unreal.ControlRigSequencerLibrary.export_fbx_from_control_rig_section(
            current_seq, section, settings)
        # 检查导出结果：UE 5.7 该 API 返回 bool（见引擎 ExportFBXFromControlRigSection 声明）。
        # 必须检查，否则导出静默失败仍会误报成功（此前缺陷1：丢交付物）。
        if not result:
            progress.error("export_fbx: ControlRig FBX 导出未成功（返回 False）")
            return False
        unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
        return True
    except Exception as exc:
        progress.error("export_fbx: ControlRig FBX 导出失败: {}".format(exc))
        return False
