"""配置读取 / 校验：job.json -> dict（C++ 表单与 Python 引擎之间的唯一桥梁）。

三种模式：depth（深度，生产主链路）/ video（视频）/ audio（音频）。
job.json 关键字段见 README.md「job.json 字段说明」。
"""

import json

from . import progress

VALID_MODES = ("depth", "video", "audio")

DEFAULTS = {
    "mode": "depth",               # depth / video / audio
    "identity_path": "",           # ROM 身份资产（/Game/...），深度/视频必需，需已 prepare
    # 批次级 identity 映射（三阶段拆分模式）：{批次目录名: identity资产路径}。
    # 素材 package_path 在 capture_root 下的第一级子目录名即批次名（如 face_am01/face_pm01），
    # 命中则用映射的 identity 解算，未命中回落全局 identity_path。AM/PM 双批工作流：
    #   "identity_map": {"face_am01": "/Game/MetaHumans/Flint/Models/0710_SF",
    #                    "face_pm01": "/Game/MetaHumans/Ares/Models/xxxx"}
    "identity_map": {},
    "capture_root": "",            # FootageCaptureData 素材根目录（深度/视频）
    "meta_human_class": "",        # 目标角色蓝图（LS 导出 target，深度/视频）
    "audio_path": "",              # SoundWave 资产路径（音频模式）
    "skeleton_path": "/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton",
    "storage_path": "/Game/MH_Results",
    "export_anim_sequence": True,
    "export_level_sequence": True,
    # export_fbx = ControlRig FACs FBX（MH_FaceAnimation_*.fbx，深度/视频交付标准，默认导）
    "export_fbx": True,
    # export_baked_fbx = 烘焙骨骼 FBX（音频模式唯一 FBX 产物；深度/视频默认不导）
    "export_baked_fbx": False,
    # {project} 模板变量：分发到任意机器自动跟随项目位置
    "fbx_output_dir": "{project}/../face_ok",
    "auto_prepare_identity": False,
    # 导入链路选择：cpp = 项目 C++ MHSTakeImporter（批次目录镜像导入，推荐）
    #              official = 官方 create_capture_data 脚本（单目录）
    "import_mode": "cpp",
    # 全自动模式（面板任务列表）：导入完成后续接解算，混合段序列（导入段×N + 解算段×M）
    "import_then_solve": False,
    "import_footage": False,
    # take 数据根目录。cpp 模式：其下一级子目录 = 批次（如 face_am01/face_pm01），
    # 各批次导入到 capture_root/{批次名}，磁盘结构镜像到 UE 资产树；
    # official 模式：指单个含 _Face.mov/depth_data.bin/take.json 的目录。
    "footage_source_dir": "",
    # cpp 模式媒体转换产物输出根目录（磁盘路径，每 take 建独立子目录）
    "media_output_root": "",
    # 导入转换并发数：auto = 按本机物理核自动计算（物理核/3，24核≈8）/ 正整数 = 固定并发
    "imports_concurrency": "auto",
    "import_only": False,
    # LS 是否导出音频轨（默认导；深度模式纯面部交付可关——C++ 面板"不含舌头与音频"勾选）
    "export_audio_track": True,
    "limit": 0,
    "exclude": [],
    "naming": {"pattern": "{take_name}_{timecode}", "prefix": "AS_"},
    # depth 解算参数：
    #   using_livelinkface_data: iPhone LiveLinkFace 数据源开关
    #   blocking: 阻塞解算（保数据，UI 段间刷新）
    #   solve_type: 解算细节等级（Preview 最快 / Standard / AdditionalTweakers 默认最细，仅深度）
    #   skip_tongue_solve: 跳过舌头解算（素材带音频轨时默认会额外启用舌头追踪，延长时间；不需要舌头动画可设 true）
    "depth": {
        "using_livelinkface_data": True,
        "blocking": True,
        "solve_type": "AdditionalTweakers",
        "skip_tongue_solve": False,
    },
    "audio": {
        "mood": "Auto",
        "mood_intensity": 1.0,
        "process_mask": "FullFace",
        "head_movement_mode": "ControlRig",
    },
}

# 各模式必填项（缺失直接报错，避免跑到一半才发现）
# 说明：meta_human_class（目标角色蓝图）不做必填。FBX 交付物从"已存在的 Level Sequence"
# 的 ControlRig Section 导出（工作流中 LS 为前置资产，直接在 Sequence 里右键导出），
# 仅当 LS 尚不存在且需要重建时才需要 meta_human_class（见 pipeline.export_level_sequence）。
REQUIRED = {
    "depth": ("identity_path", "capture_root", "storage_path"),
    "video": ("identity_path", "capture_root", "storage_path"),
    "audio": ("audio_path", "storage_path"),
}


def _deep_merge(base: dict, override: dict) -> dict:
    """递归合并，override 只覆盖提供的键。"""
    out = dict(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = _deep_merge(out[key], value)
        else:
            out[key] = value
    return out


def load(json_path: str) -> dict:
    """读取 job.json，合并默认值并做基础校验；失败抛 ValueError。"""
    with open(json_path, "r", encoding="utf-8-sig") as fh:
        raw = json.load(fh)

    if not isinstance(raw, dict):
        raise ValueError("job.json 顶层必须是 JSON 对象")

    cfg = _deep_merge(DEFAULTS, raw)

    mode = cfg.get("mode")
    if mode not in VALID_MODES:
        raise ValueError("mode 必须是 depth / video / audio，当前: {}".format(mode))

    if cfg.get("import_mode") not in ("official", "cpp"):
        raise ValueError("import_mode 必须是 official / cpp，当前: {}".format(cfg.get("import_mode")))
    if cfg.get("import_mode") == "cpp" and cfg.get("import_footage") and not cfg.get("media_output_root"):
        raise ValueError("import_mode=cpp 且 import_footage=true 时必须配置 media_output_root")

    # import_only（阶段 A 导入）先于阶段 B（identity 制作）：导入不需要身份资产，
    # 放宽 identity_path 必填，避免"先有蛋还是先有鸡"的依赖死锁。
    # 流式（stream_mode）同理：身份由分组绑定表在入队时快照（Semi [确认完成]），
    # 全局 identity_path 可为空（快照是主要来源，resolve 时再回落全局）。
    required_keys = REQUIRED[mode]
    if cfg.get("import_only") or cfg.get("stream_mode"):
        required_keys = tuple(k for k in required_keys if k != "identity_path")

    missing = [key for key in required_keys if not cfg.get(key)]
    if missing:
        raise ValueError("mode={} 缺少必填字段: {}".format(mode, ", ".join(missing)))

    return cfg
