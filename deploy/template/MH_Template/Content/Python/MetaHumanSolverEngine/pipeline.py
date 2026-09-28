"""三种模式单条流水线封装（音频 / 视频 / 深度）。

音频（轻负载，非阻塞）：load SoundWave -> create_audio_performance(AUDIO) -> 非阻塞解算 -> AnimSequence -> 烘焙 FBX

深度（重负载，阻塞）：iPhone 8 文件 --(官方 create_capture_data 导入)--> FootageCaptureData（7 文件资产夹）
  -> ensure_identity_ready -> create_capture_performance(DEPTH_FOOTAGE) -> 阻塞解算 -> AS + LS + ControlRig FACs FBX

视频（中负载，非阻塞）：FootageCaptureData -> create_capture_performance(MONO_FOOTAGE) -> 非阻塞解算 -> AS + LS + ControlRig FBX

版本相关逻辑一律走 versions.py（5.7.4 -> 5.8 适配只改那个文件）。
"""

import sys

import unreal
from functools import partial

from . import progress, versions

# ── 路径约定 ──
# UE 资产按素材分子目录：
#   /Game/MH_Results/{asset}/Performance|AS|LS_Identity
# FBX 交付平铺（2026-08-26）：所有 take 的 .fbx 直接放 fbx_output_dir 根下，
#   不再套素材子目录（{fbx_output_dir}/{take名}.fbx，原始 take 名无前缀）。
# storage_path / fbx_output_dir 在 job 里是公共根目录。

def _project_root() -> str:
    """项目根目录（如 F:/CFH）。由项目 Content 目录反推，自适应任意盘符。"""
    content = unreal.Paths.project_content_dir().rstrip("/")
    idx = content.rfind("/Content")
    return content[:idx] if idx > 0 else content


def resolve_path(path: str) -> str:
    """解析路径模板变量 {project} -> 项目根目录，分发到任意机器自动适配。"""
    if not path:
        return path
    return path.replace("{project}", _project_root())


def _asset_dir(cfg: dict, asset_name: str) -> str:
    """素材子目录（UE 资产路径）。"""
    return "{}/{}".format(cfg["storage_path"].rstrip("/"), asset_name)


def perf_asset_path(cfg: dict, asset_name: str) -> str:
    return "{}/Performance".format(_asset_dir(cfg, asset_name))


def ls_asset_path(cfg: dict, asset_name: str) -> str:
    return "{}/LS_Identity".format(_asset_dir(cfg, asset_name))


def fbx_asset_dir(cfg: dict, asset_name: str = "") -> str:
    """FBX 交付输出目录（磁盘路径，已解析 {project}）。

    平铺交付（2026-08-26 用户需求）：所有 take 的 FBX 直接放在 fbx_output_dir
    根目录下，不再按素材名套子目录（如 D:\\OUT\\Anims_Flint_Ul_Enter_005.fbx）。
    asset_name 仅兼容既有调用，不再参与目录拼装。
    """
    return resolve_path(cfg.get("fbx_output_dir", ""))


# ── 音频链路（轻负载，非阻塞） ──

def create_audio_performance(cfg: dict) -> unreal.MetaHumanPerformance:
    """按官方脚本创建/复用音频驱动 Performance 资产（幂等：已存在则 load 并刷新属性）。"""
    from . import naming

    sound_wave = unreal.load_asset(cfg["audio_path"])
    if sound_wave is None:
        raise RuntimeError("无法加载 SoundWave: {}".format(cfg["audio_path"]))

    asset_path = perf_asset_path(cfg, sound_wave.get_name())
    perf = naming.get_or_create_asset(
        asset_path,
        unreal.MetaHumanPerformance,
        unreal.MetaHumanPerformanceFactoryNew(),
    )
    if perf is None:
        raise RuntimeError("无法创建/加载 Performance: {}".format(asset_path))

    perf.set_editor_property("input_type", versions.data_input_type_audio())
    perf.set_editor_property("audio", sound_wave)

    audio_cfg = cfg["audio"]
    overrides = unreal.AudioDrivenAnimationSolveOverrides()
    overrides.set_editor_property("mood", versions.audio_mood_value(audio_cfg["mood"]))
    overrides.set_editor_property("mood_intensity", float(audio_cfg["mood_intensity"]))
    perf.set_editor_property("audio_driven_animation_solve_overrides", overrides)
    perf.set_editor_property(
        "audio_driven_animation_output_controls", versions.audio_mask_value(audio_cfg["process_mask"]))
    perf.set_editor_property("head_movement_mode", versions.head_mode_value(audio_cfg["head_movement_mode"]))
    return perf


def run_audio_shot(cfg: dict, item: dict, on_done):
    """非阻塞发起音频解算；完成时回调 on_done(cfg, perf, item)（游戏线程派发）。"""
    perf = create_audio_performance(cfg)
    unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
    perf.set_blocking_processing(False)
    perf.on_processing_finished_dynamic.add_callable(partial(on_done, cfg, perf, item))
    start_error = perf.start_pipeline()
    if start_error != unreal.StartPipelineErrorType.NONE:
        raise RuntimeError("start_pipeline 失败: {}".format(start_error))
    progress.log("已非阻塞启动解算: {}".format(perf.get_name()))


# ── 素材导入（iPhone 原始数据 -> FootageCaptureData） ──

def import_footage(cfg: dict) -> list:
    """把 take 目录导入为 FootageCaptureData（按 cfg['import_mode'] 选择链路）。

    - official（旧行为）：官方 create_capture_data，footage_source_dir 指单个 take 目录
      （含 _Face.mov / depth_data.bin / take.json），返回资产列表。
    - cpp（三阶段拆分·阶段 A）：C++ MHSTakeImporter，footage_source_dir 指数据根，
      其下一级子目录 = 批次（如 face_am01/face_pm01），各批次导入到 capture_root/{批次名}，
      磁盘结构镜像到 UE 资产树；返回批次名列表（C++ 无资产列表返回，数量见其日志）。
    """
    source = cfg.get("footage_source_dir")
    if not source:
        raise RuntimeError("import_footage=true 但未配置 footage_source_dir")

    import_mode = cfg.get("import_mode", "official")
    if import_mode == "cpp":
        media_root = cfg.get("media_output_root")
        if not media_root:
            raise RuntimeError("import_mode=cpp 但未配置 media_output_root")
        progress.log("开始导入素材（C++ 批次模式）: {} -> {}/{{批次}}".format(source, cfg["capture_root"]))
        batches = versions.import_footage_fn("cpp", cfg)(source, False, cfg["capture_root"], media_root)
        progress.log("导入完成: {} 个批次（资产数量见 LogMHSTakeImporter 日志）".format(len(batches)))
        return []

    import_fn = versions.import_footage_fn("official")
    using_llf = bool(cfg.get("depth", {}).get("using_livelinkface_data", False))
    progress.log("开始导入素材: {} (LLF={})".format(source, using_llf))
    assets = import_fn(source, using_llf, cfg["capture_root"], media_root="")
    progress.log("导入完成: {} 个 FootageCaptureData".format(len(assets)))
    return assets


# ── 身份（Identity） ──

def resolve_identity(cfg: dict, package_path: str) -> str:
    """按批次解析素材所属 identity（三阶段拆分·阶段 C 的 AM/PM 映射）。

    package_path 剥掉 capture_root 前缀后的第一段即批次目录名（cpp 导入的镜像结构：
    {capture_root}/{批次}/{take}/CD_...）。命中 identity_map 用映射路径，否则回落
    全局 identity_path（兜底：漏配批次不崩，用全局 identity 跑）。
    素材直接在 capture_root 下（无批次层，官方导入/旧数据）时同样走全局回落。
    """
    identity_map = cfg.get("identity_map") or {}
    root = cfg.get("capture_root", "").rstrip("/")
    pkg = (package_path or "").rstrip("/")
    if identity_map and root and pkg.startswith(root + "/"):
        batch = pkg[len(root) + 1:].split("/", 1)[0]
        if batch in identity_map:
            return identity_map[batch]
        progress.warn("批次 {} 未配置 identity_map 映射，回落全局 identity_path".format(batch))
    return cfg.get("identity_path", "")


def ensure_identity_ready(cfg: dict, identity_path: str = ""):
    """解算前置：确保 identity 已 prepare（Prepare for Performance）。

    identity_path：item 级解析结果（batch_runner 注入），空则用全局 cfg['identity_path']。
    auto_prepare_identity=True 时自动跑 run_predictive_solver_training（阻塞，可能数分钟）。
    默认 False：要求先在编辑器里手动 prepare 过（更可控，训练不重复跑）。
    """
    if not cfg.get("auto_prepare_identity"):
        progress.log("identity 需已 prepare（Prepare for Performance）。如需自动准备请设 auto_prepare_identity=true")
        return

    identity = unreal.load_asset(identity_path or cfg["identity_path"])
    if identity is None:
        raise RuntimeError("无法加载 Identity: {}".format(identity_path or cfg["identity_path"]))
    progress.log("正在 Prepare Identity for Performance（可能数分钟）...")
    face = identity.get_or_create_part_of_class(unreal.MetaHumanIdentityFace)
    face.run_predictive_solver_training()
    progress.log("Identity 已 prepare 完成")


# ── 深度 / 视频解算 ──

def create_capture_performance(cfg: dict, capture_data_path: str, input_type, asset_name: str,
                               identity_path: str = "") -> unreal.MetaHumanPerformance:
    """创建/复用深度/视频 Performance（幂等）：绑定 identity + FootageCaptureData。

    input_type: versions.data_input_type_depth() / versions.data_input_type_mono()
    identity_path: item 级解析结果（batch_runner 注入），空则用全局 cfg['identity_path']。
    """
    from . import naming

    capture_data = unreal.load_asset(capture_data_path)
    if capture_data is None:
        raise RuntimeError("无法加载 FootageCaptureData: {}".format(capture_data_path))
    resolved_identity = identity_path or cfg["identity_path"]
    identity = unreal.load_asset(resolved_identity)
    if identity is None:
        raise RuntimeError("无法加载 Identity: {}".format(resolved_identity))

    asset_path = perf_asset_path(cfg, asset_name)
    perf = naming.get_or_create_asset(
        asset_path,
        unreal.MetaHumanPerformance,
        unreal.MetaHumanPerformanceFactoryNew(),
    )
    if perf is None:
        raise RuntimeError("无法创建/加载 Performance: {}".format(asset_path))

    # set_editor_property 才会触发 PostEditChangeProperty 初始化（官方注释）
    perf.set_editor_property("input_type", input_type)
    perf.set_editor_property("identity", identity)
    perf.set_editor_property("footage_capture_data", capture_data)

    # 深度解算细节等级 / 跳过舌头（5.7：SolveType / bSkipTongueSolve，Python 属性名剥前缀）。
    # 仅深度有 SolveType；skip_tongue_solve 对深度/视频都有效（带音频轨时默认启用舌头追踪）。
    # 防打断设计：set_editor_property 若因属性名/枚举不匹配抛异常，仅降级为引擎默认值，
    # 绝不中断 24 分钟级的解算主流程。
    if input_type == versions.data_input_type_depth():
        depth_cfg = cfg.get("depth", {})
        try:
            perf.set_editor_property(
                "solve_type", versions.solve_type_value(depth_cfg.get("solve_type", "AdditionalTweakers")))
            perf.set_editor_property("skip_tongue_solve", bool(depth_cfg.get("skip_tongue_solve", False)))
        except Exception as exc:
            progress.warn("解算参数下发失败（使用引擎默认，不影响解算）: {}".format(exc))
    return perf


def _expected_passes(cfg: dict, input_type) -> int:
    """解算 Pass 数（引擎 UMetaHumanPerformance::ProcessComplete 阶段推进核实）：

    深度 Standard/AdditionalTweakers -> 跟踪解算/全局最终解算/滤波共 3 段；
    深度 Preview -> 跳过后处理，2 段；单目视频 -> 单段。
    C++ 侧据此选 Pass 权重带做多 Pass 加权进度。
    """
    if input_type == versions.data_input_type_depth():
        solve_type = str(cfg.get("depth", {}).get("solve_type", "AdditionalTweakers"))
        return 2 if solve_type == "Preview" else 3
    return 1


def run_capture_shot(cfg: dict, item: dict, on_done, input_type):
    """发起深度/视频解算；完成回调 on_done(cfg, perf, item)。

    - 深度：默认阻塞（depth.blocking 默认 true，官方/同事方案）——逐帧神经网络推理
      26 分钟级负载，非阻塞实测收尾崩溃且丢数据，必须阻塞。
    - 视频：非阻塞（46s 级，已验证稳定，UI 不冻结）。
    """
    perf = create_capture_performance(
        cfg, item["package_path"], input_type, item["name"], item.get("identity", ""))
    unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)

    blocking = (input_type == versions.data_input_type_depth()
                and bool(cfg.get("depth", {}).get("blocking", True)))
    if blocking:
        perf.set_blocking_processing(True)
        progress.log("开始阻塞解算: {}".format(perf.get_name()))
        # 原生进度监控：小窗 + 心跳 json，阻塞期间唯一可见的实时信号（设计文档 §8）
        # expected_passes：多 Pass 加权进度（Pass 边界 = 帧号回绕检测，设计文档 11.2）
        progress.monitor_begin(perf, item["name"], _expected_passes(cfg, input_type))
        try:
            start_error = perf.start_pipeline()
            if start_error != unreal.StartPipelineErrorType.NONE:
                raise RuntimeError("start_pipeline 失败: {}".format(start_error))
        except Exception:
            # 解算启动/执行失败：先关监控再上抛（batch_runner 侧统一收尾）
            progress.monitor_end(False)
            raise
        # 阻塞模式同步返回时解算已完成 -> 进入导出阶段（切换小窗为"导出中"）
        progress.monitor_stage(progress.STAGE_EXPORT_AS)
        on_done(cfg, perf, item)
        return

    perf.set_blocking_processing(False)
    perf.on_processing_finished_dynamic.add_callable(partial(on_done, cfg, perf, item))
    start_error = perf.start_pipeline()
    if start_error != unreal.StartPipelineErrorType.NONE:
        raise RuntimeError("start_pipeline 失败: {}".format(start_error))
    progress.log("已非阻塞启动解算: {}".format(perf.get_name()))


def run_depth_shot(cfg: dict, item: dict, on_done):
    run_capture_shot(cfg, item, on_done, versions.data_input_type_depth())


def run_video_shot(cfg: dict, item: dict, on_done):
    run_capture_shot(cfg, item, on_done, versions.data_input_type_mono())


# ── 导出 ──

def export_anim_sequence(perf, cfg: dict, asset_name: str):
    """Performance -> AnimSequence（保存到素材子目录，AS 资产短名固定 'AS'）。

    幂等：目标 'AS' 已存在（上次运行残留 / 同一素材重跑）→ 先删除再创建。
    否则导出会因 "The asset 'AS' already exists in package ..." 直接失败——
    实测代价：20 分 50 秒的解算跑完后倒在这一步（重跑同一素材必崩）。
    与导入侧幂等（跳过已完成）同一纪律：重跑必须能安全重入。
    """
    target = _asset_dir(cfg, asset_name).rstrip("/") + "/AS"
    try:
        if unreal.EditorAssetLibrary.does_asset_exist(target):
            progress.log("清理残留 AS 资产（重跑幂等）: {}".format(target))
            unreal.EditorAssetLibrary.delete_asset(target)
    except Exception as exc:
        progress.warn("清理残留 AS 失败（继续尝试导出）: {}".format(exc))

    settings = unreal.MetaHumanPerformanceExportUtils.get_export_animation_sequence_settings(perf)
    settings.set_editor_property("show_export_dialog", False)
    settings.set_editor_property("auto_save_anim_sequence", True)
    settings.set_editor_property("export_range", versions.performance_export_range_whole())
    settings.set_editor_property("asset_name", "AS")
    settings.set_editor_property("package_path", _asset_dir(cfg, asset_name))

    skeleton = unreal.load_asset(cfg["skeleton_path"])
    if skeleton is not None:
        settings.set_editor_property("target_skeleton_or_skeletal_mesh", skeleton)

    return unreal.MetaHumanPerformanceExportUtils.export_animation_sequence(perf, settings)


def _ls_core_name(asset_name: str) -> str:
    """素材核心名：去掉捕获资产常见前缀（CD_/CC_/IS_/LF_/SW_ 等）。"""
    for prefix in ("CD_", "CC_", "IS_", "LF_", "SW_", "AS_", "MH_"):
        if asset_name.startswith(prefix):
            return asset_name[len(prefix):]
    return asset_name


def _ls_has_control_rig(ls_path: str) -> bool:
    """LS 是否含 ControlRig 轨（Face_ControlBoard_CtrlRig）——可导出 ControlRig FBX 的前提。"""
    try:
        ls = unreal.load_asset(ls_path)
        if ls is None:
            return False
        for binding in ls.get_bindings():
            if binding.find_tracks_by_exact_type(unreal.MovieSceneControlRigParameterTrack):
                return True
    except Exception:
        return False
    return False


def _find_existing_level_sequence(cfg: dict, asset_name: str) -> str:
    """按素材名在项目里扫描既有 LevelSequence（含 ControlRig 轨），找到返回资产路径。

    覆盖手动流程 / MetaHuman Animator 在"非固定路径"生成的 LS（如 LS_MetaHumanPerformance、
    LS_xxx_素材名），使批处理第一条解算后即可复用，无需 meta_human_class。
    """
    core = _ls_core_name(asset_name)
    # 1) 5.7 首选：AssetRegistry 按类路径查询。
    #    get_assets_by_class(class_path_name: TopLevelAssetPath, search_sub_classes: bool)
    #    —— 5.7 必须传 TopLevelAssetPath 结构体（不能传字符串）。
    try:
        registry = unreal.AssetRegistryHelpers.get_asset_registry()
        ls_path = unreal.TopLevelAssetPath("/Script/LevelSequence", "LevelSequence")
        assets = registry.get_assets_by_class(ls_path, search_sub_classes=True) or []
        for asset_data in assets:
            name = str(asset_data.asset_name)
            if core and core in name or asset_name in name:
                pkg = str(asset_data.package_name)
                if _ls_has_control_rig(pkg):
                    progress.log("扫描到素材关联 LS: {}".format(pkg))
                    return pkg
    except Exception as exc:
        progress.warn("AssetRegistry 类查询失败，改用 list_assets 过滤: {}".format(exc))

    # 2) 兜底：EditorAssetLibrary 全项目列表按类名过滤（性能次优但 API 稳定）
    try:
        for asset_path in unreal.EditorAssetLibrary.list_assets("/Game", recursive=True):
            pkg = asset_path.rsplit(".", 1)[0]
            name = pkg.rsplit("/", 1)[-1]
            if not (core and core in name or asset_name in name):
                continue
            if _ls_has_control_rig(pkg):
                progress.log("扫描到素材关联 LS: {}".format(pkg))
                return pkg
    except Exception as exc:
        progress.warn("LS 扫描复用失败（将走固定路径/重建）: {}".format(exc))
    return ""


def _find_default_meta_human_class(asset_name: str = "") -> str:
    """自动发现可用的 MetaHuman 角色蓝图（作为 LS 重建 target）。

    角色蓝图只是 LS 重建 API 的强制参数，FBX 内容（Face_ControlBoard_CtrlRig FACs 曲线）
    与 target 无关，因此任意可用 BP 均可。优先匹配素材名里的角色 token，否则取第一个。
    """
    try:
        registry = unreal.AssetRegistryHelpers.get_asset_registry()
        # 先扫 /Game/MetaHumans 根目录（角色 BP 通常直接放在这里，如 BP_Flint），
        # 避免递归扫描 Common 子目录下海量资产；根目录没有再递归兜底。
        bp_list = []
        for recursive in (False, True):
            for ad in registry.get_assets_by_path("/Game/MetaHumans", recursive=recursive):
                name = str(ad.asset_name)
                if not name.startswith("BP_"):
                    continue
                # 5.7 AssetData 无 asset_class_name；asset_class_path 是 TopLevelAssetPath 结构体，
                # 类名在 asset_name 字段（如 {package_name:"/Script/Engine", asset_name:"Blueprint"}）
                try:
                    class_name = str(ad.asset_class_path.asset_name)
                except Exception:
                    class_name = ""
                if class_name != "Blueprint":
                    continue
                # 排除非角色 BP（LevelSequence / MeshCollection / Preview 等）
                if any(tag in name for tag in ("LevelSequence", "MeshCollection", "Preview")):
                    continue
                bp_list.append((name, str(ad.package_name)))
            if bp_list:
                break

        # 优先匹配素材名中的角色 token（如 Anims_Flint_Ul_Enter_005_5 -> BP_Flint）
        if asset_name:
            for bp_name, pkg in bp_list:
                role = bp_name[len("BP_"):]
                if role and role in asset_name:
                    progress.log("匹配素材角色蓝图: {} -> {}".format(role, pkg))
                    return pkg
        # 兜底取第一个角色 BP
        return bp_list[0][1] if bp_list else ""
    except Exception as exc:
        progress.warn("默认角色蓝图发现失败: {}".format(exc))
    return ""


def export_level_sequence(perf, cfg: dict, asset_name: str, reuse_existing: bool = True):
    """Performance -> Level Sequence（含目标 MetaHuman 角色 + ControlRig 轨），导出后落盘。

    复用策略（默认 reuse_existing=True，批处理第一条生成后后续快速复用）：
      1) 固定路径 {storage}/{asset}/LS_Identity 已存在 -> 直接复用；
      2) 按素材名在项目里扫描既有 LS（手动流程/Animator 生成）-> 复用；
      3) 都没有才重建：target 角色蓝图 = job.meta_human_class 显式配置，
         否则自动发现 /Game/MetaHumans 下 BP_*（任意角色均可，FBX 内容与 target 无关）。

    重建走 _export_level_sequence_no_key_reduce（直接用 ExportLevelSequence API 并关
    remove_redundant_keys，保留逐帧关键帧），而非官方 run_meta_human_level_sequence_export
    （内部默认删帧，导致 FBX 曲线数 133 vs 手动 222）。

    如需强制重建（Performance 重新解算后 LS 过期），先手动删除旧 LS 资产再跑。
    """
    ls_path = ls_asset_path(cfg, asset_name)

    # 1) 固定路径复用（{storage}/{asset}/LS_Identity——按 take 隔离，批处理安全）
    if reuse_existing and unreal.EditorAssetLibrary.does_asset_exist(ls_path):
        progress.log("LS 已存在，复用: {}（强制重建请先删除旧资产）".format(ls_path))
        return unreal.load_asset(ls_path)

    # 2) 素材关联 LS 扫描复用（手动流程/Animator 生成在别处的 LS）。
    #    批处理（import_then_solve）模式禁用：项目级扫描可能拾取旧手动流程的 LS
    #    （名字含 take 名但内容是旧数据/删帧）——批处理只信固定路径，杜绝跨段串扰。
    if reuse_existing and not cfg.get("import_then_solve"):
        found = _find_existing_level_sequence(cfg, asset_name)
        if found:
            progress.log("复用素材关联 LS: {}".format(found))
            return unreal.load_asset(found)

    # 3) 重建：需要 target 角色蓝图（显式配置或自动发现默认 BP）
    mh_path = cfg.get("meta_human_class", "")
    if not mh_path:
        mh_path = _find_default_meta_human_class(asset_name)
        if mh_path:
            progress.log("自动选择默认角色蓝图: {}（LS 重建 target，不影响 FBX 内容）".format(mh_path))

    if not mh_path:
        # 仍无可用的 target 角色蓝图，给出可操作提示
        progress.error("Identity Level Sequence 不存在且未找到可用角色蓝图。"
                       "请确认该素材已在 MetaHuman Animator 中生成 LS，或在 job 配置 meta_human_class"
                       "（/Game/MetaHumans 下需有 BP_* 角色蓝图）以重建。")
        return None

    result = _export_level_sequence_no_key_reduce(perf, mh_path, asset_name, cfg)
    return result


def _export_level_sequence_no_key_reduce(perf, mh_path: str, asset_name: str, cfg: dict):
    """用引擎 API 直接导出 LS，并显式关闭 remove_redundant_keys（保留逐帧关键帧）。

    实验确证：官方 run_meta_human_level_sequence_export 内部走
    ExportLevelSequence 时 bRemoveRedundantKeys 默认为 true，导致生成的 LS 关键帧被
    稀疏化（曲线数 133 vs 手动 222），从 LS 导出的 FBX 与手动从 Performance 导出的
    逐帧数据不一致。此函数完整复刻官方 meta_human 版字段，仅把 remove_redundant_keys
    设为 False，使 FBX 曲线数/关键帧密度恢复到手动水平。
    """
    settings = unreal.MetaHumanPerformanceExportLevelSequenceSettings()
    settings.set_editor_property("show_export_dialog", False)
    settings.set_editor_property("package_path", _asset_dir(cfg, asset_name))
    settings.set_editor_property("asset_name", "LS_Identity")
    settings.set_editor_property("export_video_track", True)
    settings.set_editor_property("export_depth_track", False)
    # 音频轨可配置（默认导出；深度模式纯面部交付时 job 设 false 关闭）
    settings.set_editor_property("export_audio_track", bool(cfg.get("export_audio_track", True)))
    settings.set_editor_property("export_image_plane", False)
    settings.set_editor_property("export_identity", False)
    settings.set_editor_property("export_camera", True)
    settings.set_editor_property("apply_lens_distortion", True)
    settings.set_editor_property("export_depth_mesh", False)
    settings.set_editor_property("export_control_rig_track", True)
    settings.set_editor_property("export_transform_track", False)
    settings.set_editor_property("keep_frame_range", True)

    bp = unreal.load_asset(mh_path)
    if bp is None:
        raise RuntimeError("无法加载目标角色蓝图: {}".format(mh_path))
    settings.set_editor_property("target_meta_human_class", bp)
    settings.set_editor_property("enable_meta_human_head_movement", True)
    settings.set_editor_property("export_range", versions.performance_export_range_whole())
    settings.set_editor_property("curve_interpolation", unreal.RichCurveInterpMode.RCIM_LINEAR)
    # 关键：关闭冗余关键帧移除，保留逐帧数据（与手动从 Performance 导出一致）
    try:
        settings.set_editor_property("remove_redundant_keys", False)
    except Exception as exc:
        progress.warn("remove_redundant_keys 设置失败（将沿用引擎默认删帧）: {}".format(exc))

    result = unreal.MetaHumanPerformanceExportUtils.export_level_sequence(
        performance=perf, export_settings=settings)
    if result is not None:
        # 导出后立即保存资产，否则内存态重启即丢
        unreal.get_editor_subsystem(unreal.EditorAssetSubsystem).save_asset(
            result.get_outer().get_name(), False)
    return result
