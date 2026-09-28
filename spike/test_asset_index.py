# -*- coding: utf-8 -*-
"""asset_index 单元测试（mock unreal）：收集/过滤/排序/落盘/空态/绑定异常容错。

mock 方式：真实临时目录 + 假 AssetRegistry（返回指向真实临时文件的假资产数据）
——文件 stat（时间/大小）走真实文件系统，绑定层走假对象。
"""

import json
import os
import shutil
import sys
import tempfile
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
PKG_ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(PKG_ROOT, "deploy", "Python"))

CONTENT = None   # main() 里赋值：假 Content 根


class _FakeAsset(object):
    def __init__(self, pkg, cls):
        self.package_name = pkg
        self.asset_class_path = types.SimpleNamespace(asset_name=cls)


class _FakeRegistry(object):
    def __init__(self, assets):
        self._assets = assets

    def get_assets_by_path(self, root, recursive):
        return [a for a in self._assets]


def _make_identity(content_dir, rel_path, size_kb, mtime_offset):
    """在假 Content 下创建一个真实 .uasset 文件并返回假资产数据。"""
    full = os.path.join(content_dir, rel_path.replace("/", "\\")) + ".uasset"
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as fh:
        fh.write(b"\0" * (size_kb * 1024))
    mtime = time.time() - mtime_offset
    os.utime(full, (mtime, mtime))
    return _FakeAsset("/Game/" + rel_path, "MetaHumanIdentity")


def install_mock(assets):
    """安装假 unreal（仅本测试进程内）。"""
    m = types.ModuleType("unreal")

    class _Paths(object):
        @staticmethod
        def project_content_dir():
            return CONTENT + "/"
        @staticmethod
        def project_saved_dir():
            return os.path.join(os.path.dirname(CONTENT), "Saved") + "/"
        @staticmethod
        def convert_relative_path_to_full(p):
            return os.path.abspath(p)

    m.Paths = _Paths
    m.AssetRegistryHelpers = types.SimpleNamespace(
        get_asset_registry=lambda: _FakeRegistry(assets))
    sys.modules["unreal"] = m


OK = [True]
LINES = []


def check(name, cond, detail=""):
    line = "[{}] {}{}".format("PASS" if cond else "FAIL", name,
                              ("  -> " + str(detail)) if (detail and not cond) else "")
    print(line)
    LINES.append(line)
    if not cond:
        OK[0] = False


def main():
    global CONTENT
    tmp = tempfile.mkdtemp(prefix="mhs_idx_")
    try:
        CONTENT = os.path.join(tmp, "Content")
        os.makedirs(CONTENT)

        # ── 1) 收集：过滤非身份资产 + 按修改时间倒序 ──
        a_pm = _make_identity(CONTENT, "ID/20260907_PM_ROM02_13", 66269, 100)   # 旧
        a_am = _make_identity(CONTENT, "CaptureManager/ID4/0818_face_rom_am_001_11", 66210, 10)  # 新
        non_identity = _FakeAsset("/Game/Cap/CD_some_take", "MetaHumanCaptureData")
        install_mock([a_pm, a_am, non_identity])

        from MetaHumanSolverEngine import asset_index
        items = asset_index.collect_identity_assets()
        check("1a 只收集 MetaHumanIdentity（非身份被过滤）", len(items) == 2, [i["name"] for i in items])
        check("1b 按修改时间倒序（最新在前）",
              items[0]["name"] == "0818_face_rom_am_001_11", [i["name"] for i in items])
        check("1c 字段齐（path/dir/modified/size_kb）",
              all(("path" in i and "dir" in i and "modified" in i and "size_kb" in i) for i in items),
              items[:1])
        check("1d size_kb 为真实文件大小（66210）",
              items[0]["size_kb"] == 66210, items[0].get("size_kb"))
        check("1e dir 为父包路径", items[1]["dir"] == "/Game/ID", items[1])

        # ── 2) 落盘：结构 + 原子写 ──
        out = asset_index.write_index()
        check("2a 返回默认路径（Saved/Config/MetaHumanSolver）",
              out.endswith("identity_assets.json") and "MetaHumanSolver" in out, out)
        with open(out, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        check("2b 结构齐（generated_at/assets/dirs）",
              "generated_at" in data and "assets" in data and "dirs" in data, list(data))
        check("2c assets 内容一致", len(data["assets"]) == 2)
        check("2d dirs 去重排序", data["dirs"] == ["/Game/CaptureManager/ID4", "/Game/ID"], data["dirs"])
        check("2e 无 .tmp 残留", not os.path.exists(out + ".tmp"))

        # ── 3) 空态：无身份资产也落盘（明确空，而非不写） ──
        install_mock([])
        items = asset_index.collect_identity_assets()
        check("3a 无资产 → 空列表（不抛）", items == [])
        out2 = asset_index.write_index()
        with open(out2, "r", encoding="utf-8") as fh:
            data2 = json.load(fh)
        check("3b 空态仍落盘（assets=[] dirs=[]）",
              data2["assets"] == [] and data2["dirs"] == [], data2)

        # ── 4) 绑定异常容错：引擎外调用返回空不崩 ──
        bad = types.ModuleType("unreal")
        bad.AssetRegistryHelpers = types.SimpleNamespace(
            get_asset_registry=lambda: (_ for _ in ()).throw(RuntimeError("no engine")))
        sys.modules["unreal"] = bad
        import importlib
        importlib.reload(asset_index)
        items = asset_index.collect_identity_assets()
        check("4a 引擎异常 → []（消费方不崩）", items == [])
        check("4b 引擎异常 → write_index 返回空路径（不落盘）", asset_index.write_index() == "")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    print("=== asset_index 单测: {} ===".format("全部通过" if OK[0] else "存在失败"))
    with open(os.path.join(HERE, "asset_index_result.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
