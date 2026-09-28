"""引擎版本适配层 —— 5.7.4 -> 5.8+ 升级时的唯一修改入口。

所有"随引擎版本变化"的东西集中在本文件：
  1. 官方脚本路径（MetaHumanAnimator Content/Python 目录）
  2. 枚举名映射（实测 5.7 官方脚本里 HAPPY/SAD 是错的，真实枚举是 HAPPINESS/SADNESS）
  3. 官方 Python API 函数名（export_performance / create_capture_data 的入口函数）
  4. Python 属性名约定（5.7 剥 b 前缀/剥 E 前缀，升级后需重新核对）

升级引擎的适配流程：
  1. 改 SUPPORTED_ENGINE_VERSIONS 加入新版本
  2. 逐项核对下方 OFFICIAL_SCRIPTS / ENUMS / API 三张表
  3. C++ 插件按新引擎重编译（BuildId 必须与目标机器引擎一致）
"""

import sys

import unreal

# ── 已适配的引擎版本（用于启动时自检告警） ──
SUPPORTED_ENGINE_VERSIONS = ("5.7",)


def engine_version() -> str:
    """当前引擎版本字符串，如 '5.7'。"""
    return unreal.SystemLibrary.get_engine_version().split("-")[0].rsplit(".", 1)[0]


def check_supported() -> bool:
    """启动自检：当前引擎是否在已适配列表。不在则打警告（不阻断，方便提前验证）。"""
    from . import progress
    ver = engine_version()
    if ver not in SUPPORTED_ENGINE_VERSIONS:
        progress.warn("引擎 {} 未在已适配列表 {} 中，API 可能有差异".format(ver, SUPPORTED_ENGINE_VERSIONS))
        return False
    return True


# ── 官方脚本 ──
# UE 5.7: {Engine}/Plugins/MetaHuman/MetaHumanAnimator/Content/Python/
# 内含 create_capture_data.py（导入）/ export_performance.py（LS 导出）/ process_audio_performance.py
OFFICIAL_SCRIPTS_SUBDIR = "Plugins/MetaHuman/MetaHumanAnimator/Content/Python/"


def official_scripts_dir() -> str:
    """官方 MetaHuman Python 脚本目录（随引擎安装位置自适应）。"""
    return unreal.Paths.engine_dir() + OFFICIAL_SCRIPTS_SUBDIR


def add_official_scripts_to_path() -> str:
    """把官方脚本目录加进 sys.path，返回目录路径。"""
    script_dir = official_scripts_dir()
    if script_dir not in sys.path:
        sys.path.append(script_dir)
    return script_dir


# ── 官方 API 入口函数名（升级引擎时逐一核对官方脚本是否改名/改签名） ──
API = {
    # create_capture_data.py：iPhone 原始数据 -> FootageCaptureData
    "import_take_data": "import_take_data_for_specified_device",
    # export_performance.py：Performance + 目标角色 -> Level Sequence（显式 target，避开身份推断崩溃）
    "export_ls_with_target": "run_meta_human_level_sequence_export",
}


def import_take_data_fn():
    """官方导入函数（带版本核对：函数不存在时给出明确报错而非 AttributeError）。"""
    add_official_scripts_to_path()
    import create_capture_data
    fn = getattr(create_capture_data, API["import_take_data"], None)
    if fn is None:
        raise RuntimeError(
            "官方脚本 create_capture_data.{} 不存在——引擎版本可能不兼容，请核对 versions.py".format(
                API["import_take_data"]))
    return fn


# ── 素材导入链路（official = 官方脚本 / cpp = 项目 C++ MHSTakeImporter 插件）──

def resolve_import_concurrency(cfg=None) -> int:
    """导入转换并发数：auto/未配置 → 0（C++ 按物理核自动）；正整数 → 固定值。

    与 import_footage_fn 共用，供批次导入段/预检报告统一解析。
    """
    if not cfg:
        return 0
    v = cfg.get("imports_concurrency", "auto")
    if isinstance(v, int) and v > 0:
        return v
    return 0


def import_take_list(take_dirs, media_output_root, package_root, concurrency=0):
    """显式 take 目录列表导入（并发转换）。

    供面板混合段/纯导入段合并后的批次导入段使用：只导入传入的 take，
    C++ 侧 ImportTakes 复用并发调度（Python 侧自行做幂等过滤）。
    """
    if not take_dirs:
        return
    unreal.MHSTakeImporter.import_takes(
        list(take_dirs), media_output_root, package_root, True, int(concurrency))


def import_footage_fn(import_mode: str, cfg=None):
    """统一导入闭包：签名 (source_dir, using_llf, capture_root, media_output_root) -> list。

    - official：官方 import_take_data_for_specified_device(source, using_llf, capture_root)，
      返回导入的资产对象列表。
    - cpp：UMHSTakeImporter.import_takes_from_root，按 source 一级子目录（批次）分次调用，
      每批次导入到 capture_root/{批次名}（磁盘结构镜像到 UE 资产树，AM/PM 批次天然隔离）；
      source 本身直接是 take 目录（无批次层）时整根导入到 capture_root。
      C++ 侧无返回值，返回本次处理的批次目录名列表（数量见 LogMHSTakeImporter 日志）。
      cfg["imports_concurrency"]（auto/正整数）控制 C++ 转换并发数（0=auto 由 C++ 按物理核算）。
    """
    if import_mode != "cpp":
        official_fn = import_take_data_fn()

        def _official(source_dir, using_llf, capture_root, media_output_root):
            return official_fn(source_dir, using_llf, capture_root)
        return _official

    import os

    def _cpp(source_dir, using_llf, capture_root, media_output_root):
        if not media_output_root:
            raise RuntimeError("import_mode=cpp 需要 media_output_root（媒体转换输出根目录）")
        concurrency = resolve_import_concurrency(cfg)

        def _has_take_marker(directory):
            if os.path.isfile(os.path.join(directory, "take.json")):
                return True
            try:
                return any(f.endswith(".cptake") for f in os.listdir(directory))
            except OSError:
                return False

        if _has_take_marker(source_dir):
            unreal.MHSTakeImporter.import_takes_from_root(
                source_dir, media_output_root, capture_root, True, concurrency)
            return [""]

        batches = sorted(
            d for d in os.listdir(source_dir)
            if os.path.isdir(os.path.join(source_dir, d)))
        if not batches:
            raise RuntimeError("cpp 导入源目录下没有任何子目录: {}".format(source_dir))
        for batch in batches:
            unreal.MHSTakeImporter.import_takes_from_root(
                os.path.join(source_dir, batch), media_output_root,
                "{}/{}".format(capture_root.rstrip("/"), batch), True, concurrency)
        return batches
    return _cpp


def import_single_take(source_dir: str, media_output_root: str, package_root: str) -> None:
    """单 take 目录导入（ROM/表演通用，混合段序列的导入段实现）。

    package_root：本 take 资产的父目录（如 /Game/CaptureManager/Imports/{批次名}）。
    调 C++ import_take_directory（含 take.json 校验，非法目录由 C++ 报错）。
    """
    unreal.MHSTakeImporter.import_take_directory(source_dir, media_output_root, package_root, True)


def export_ls_with_target_fn():
    """官方 LS 导出函数（显式 target_meta_human 版本）。"""
    add_official_scripts_to_path()
    import export_performance
    fn = getattr(export_performance, API["export_ls_with_target"], None)
    if fn is None:
        raise RuntimeError(
            "官方脚本 export_performance.{} 不存在——引擎版本可能不兼容，请核对 versions.py".format(
                API["export_ls_with_target"]))
    return fn


# ── Python 属性/枚举命名约定（5.7 实测，升级后核对） ──
# - bool 属性剥 b 前缀：bShowExportDialog -> show_export_dialog
# - 枚举类型剥 E 前缀：EPerformanceExportRange -> PerformanceExportRange
# - 枚举值以引擎实际为准（官方脚本注释可能过期，如 HAPPY -> HAPPINESS）
def performance_export_range_whole():
    return unreal.PerformanceExportRange.WHOLE_SEQUENCE


def data_input_type_depth():
    return unreal.DataInputType.DEPTH_FOOTAGE


def data_input_type_mono():
    return unreal.DataInputType.MONO_FOOTAGE


def data_input_type_audio():
    return unreal.DataInputType.AUDIO


# ── 深度解算细节等级（ESolveType：Preview / Standard / AdditionalTweakers，默认最细） ──
def solve_type_value(name: str):
    if name == "Preview":
        return unreal.SolveType.PREVIEW
    if name == "Standard":
        return unreal.SolveType.STANDARD
    if name == "AdditionalTweakers":
        return unreal.SolveType.ADDITIONAL_TWEAKERS
    raise ValueError("未知 solve_type: {}".format(name))


# ── 音频解算枚举映射（5.7 实测：官方脚本注释的 HAPPY/SAD 是错的，真实枚举是 HAPPINESS/SADNESS） ──

def audio_mood_value(name: str):
    if name == "Auto":
        return unreal.AudioDrivenAnimationMood.AUTO_DETECT
    if name == "Neutral":
        return unreal.AudioDrivenAnimationMood.NEUTRAL
    if name == "Happy":
        return unreal.AudioDrivenAnimationMood.HAPPINESS
    if name == "Sad":
        return unreal.AudioDrivenAnimationMood.SADNESS
    if name == "Disgust":
        return unreal.AudioDrivenAnimationMood.DISGUST
    if name == "Anger":
        return unreal.AudioDrivenAnimationMood.ANGER
    if name == "Surprise":
        return unreal.AudioDrivenAnimationMood.SURPRISE
    if name == "Fear":
        return unreal.AudioDrivenAnimationMood.FEAR
    raise ValueError("未知 mood: {}".format(name))


def audio_mask_value(name: str):
    if name == "FullFace":
        return unreal.AudioDrivenAnimationOutputControls.FULL_FACE
    if name == "MouthOnly":
        return unreal.AudioDrivenAnimationOutputControls.MOUTH_ONLY
    raise ValueError("未知 process_mask: {}".format(name))


def head_mode_value(name: str):
    if name == "ControlRig":
        return unreal.PerformanceHeadMovementMode.CONTROL_RIG
    if name == "TransformTrack":
        return unreal.PerformanceHeadMovementMode.TRANSFORM_TRACK
    if name == "Disabled":
        return unreal.PerformanceHeadMovementMode.DISABLED
    raise ValueError("未知 head_movement_mode: {}".format(name))
