# -*- coding: utf-8 -*-
"""身份资产索引（asset_index）：导出工程内全部 MetaHumanIdentity 的精确清单。

为什么需要：身份资产选择曾是"手填路径 / [检测]取目录内最新"——多身份场景会猜错
（曾差点把空身份绑给分组）。本模块把 AssetRegistry 的精确查询结果落盘为 json，
供所有外部消费者（控制台 / Web / 自绘 UI / headless）使用，保持"UI 只读文件"纪律。

关键信号 size_kb：~66MB = 已 conform 成型身份；~0.2MB = 空/未 conform。
消费方应在列表中标出大小（选错前就看得见）。

数据流：引擎进程内查询（准确）→ identity_assets.json（原子写）→ 外部只读。
"""

import json
import os
import time

ASSET_CLASS = "MetaHumanIdentity"


# ── 内部工具 ──

def _content_dir():
    """工程 Content 绝对路径（UE 相对路径 → 全路径）。"""
    try:
        import unreal
        return unreal.Paths.convert_relative_path_to_full(
            unreal.Paths.project_content_dir())
    except Exception:
        return ""


def _saved_config_dir():
    """Saved/Config/MetaHumanSolver 绝对路径（索引默认落点，与队列/状态同目录）。"""
    try:
        import unreal
        saved = unreal.Paths.convert_relative_path_to_full(
            unreal.Paths.project_saved_dir())
        return os.path.join(saved, "Config", "MetaHumanSolver")
    except Exception:
        return ""


def _package_to_file(pkg):
    """/Game/ID/X → <Content>/ID/X.uasset（找不到返回 ""，调用方按无 stat 处理）。"""
    if not pkg.startswith("/Game/"):
        return ""
    rel = pkg[len("/Game/"):]
    base = _content_dir()
    if not base:
        return ""
    return os.path.join(base, rel.replace("/", "\\")) + ".uasset"


def _stat(path):
    """返回 (epoch, 修改时间文案, 大小KB)；文件缺失返回 (0, "", 0)。"""
    try:
        st = os.stat(path)
        return (st.st_mtime,
                time.strftime("%m-%d %H:%M", time.localtime(st.st_mtime)),
                int(st.st_size // 1024))
    except OSError:
        return (0, "", 0)


# ── 对外 API ──

def collect_identity_assets(roots=None):
    """扫描工程内全部 MetaHumanIdentity 资产（注册表内存查询，非磁盘扫描）。

    roots: 包路径列表，默认 ["/Game"]。
    返回 [{name, path, dir, modified, size_kb}]，按修改时间倒序（最新在前）。
    引擎外调用 / 绑定异常 → 返回 []（不抛——调用方按空态展示，绝不崩消费方）。
    """
    try:
        import unreal
        registry = unreal.AssetRegistryHelpers.get_asset_registry()
        out = []
        for root in (roots or ["/Game"]):
            for data in registry.get_assets_by_path(root, True):
                # 类判定（UE5 asset_class_path；旧版属性名做兜底）
                cls = ""
                try:
                    cls = str(data.asset_class_path.asset_name)
                except Exception:
                    try:
                        cls = str(data.asset_class_name)
                    except Exception:
                        cls = ""
                if cls != ASSET_CLASS:
                    continue
                pkg = str(data.package_name)
                modified_ts, modified, size_kb = _stat(_package_to_file(pkg))
                out.append({
                    "name": pkg.rsplit("/", 1)[-1],
                    "path": pkg,
                    "dir": pkg.rsplit("/", 1)[0],
                    "modified_ts": modified_ts,     # epoch（排序用，UI 可自行格式化）
                    "modified": modified,           # 显示文案（MM-DD HH:MM）
                    "size_kb": size_kb,
                })
        out.sort(key=lambda a: a["modified_ts"], reverse=True)
        return out
    except Exception:
        return []


def write_index(out_path=None):
    """导出 identity_assets.json（原子写）。返回写入路径。

    无资产也写（空列表 = 明确空态，消费方显示"未找到身份——先制作并保存"）。
    """
    if not out_path:
        base = _saved_config_dir()
        if not base:
            return ""
        out_path = os.path.join(base, "identity_assets.json")
    directory = os.path.dirname(out_path)
    if directory and not os.path.isdir(directory):
        os.makedirs(directory)
    assets = collect_identity_assets()
    payload = {
        "generated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "assets": assets,
        "dirs": sorted(set(a["dir"] for a in assets)),
    }
    tmp = out_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, ensure_ascii=False, indent=1)
    os.replace(tmp, out_path)
    return out_path
