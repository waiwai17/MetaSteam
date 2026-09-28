"""命名规则（plan.md 2.6）+ 幂等建资产。

- 默认 pattern {take_name}_{timecode}，可配置前缀（AS_ / PERF_ / CD_）
- get_or_create_asset：重复跑不堆积（官方 get_or_create 的等价实现）
"""

import unreal


def build_name(take_name: str, timecode: str = "", pattern: str = "{take_name}_{timecode}", prefix: str = "") -> str:
    """按 pattern 生成资产名。timecode 缺失时跳过占位符。"""
    name = pattern.format(take_name=take_name, timecode=timecode)
    # 去掉空 timecode 产生的尾部分隔符（_ 或 -）
    name = name.rstrip("_-").strip()
    if not name:
        name = take_name
    return "{}{}".format(prefix, name)


def _split_asset_path(asset_path: str):
    """/Game/A/B/C -> ('/Game/A/B', 'C')。"""
    clean = asset_path.rstrip("/")
    slash = clean.rfind("/")
    return clean[:slash], clean[slash + 1:]


def get_or_create_asset(asset_path: str, asset_class, factory=None):
    """资产已存在则直接加载，否则创建。返回资产对象或 None。"""
    existing = unreal.load_asset(asset_path)
    if existing is not None:
        return existing

    package_path, asset_name = _split_asset_path(asset_path)
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    return tools.create_asset(
        asset_name=asset_name,
        package_path=package_path,
        asset_class=asset_class,
        factory=factory,
    )
