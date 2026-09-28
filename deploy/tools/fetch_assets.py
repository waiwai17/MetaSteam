# -*- coding: utf-8 -*-
"""补齐 MetaHuman 公共资产包（deploy/assets/MetaHumans）。

为什么需要：Level Sequence 重建要求工程内存在
    /Game/MetaHumans/Common/Face/Face_Archetype_Skeleton   （面部骨架/CtrlRig）
    /Game/MetaHumans/<角色>/BP_<角色>                        （目标角色蓝图）
模板工程刻意"干净"，不带这些（它们体积大且属于 UE/MetaHuman 资产），
所以换到一台新机器时，需要从本机某个已有工程复制一份过来（一次性）。

用法：
    python fetch_assets.py --from F:\\CFH\\CFH\\20260709\\MH_H
    python fetch_assets.py --from <参考工程> --character Flint
    python fetch_assets.py --check          # 只检查本机是否已有

说明：本仓库**不分发**这些资产（体积 2GB 且受 MetaHuman 许可约束）。
"""

import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEPLOY = os.path.dirname(HERE)
ASSETS = os.path.join(DEPLOY, "assets", "MetaHumans")
NEED_COMMON = ["Common"]
DEFAULT_CHARACTER = "Flint"


def is_ready():
    return (os.path.isfile(os.path.join(ASSETS, "Common", "Face", "Face_Archetype_Skeleton.uasset"))
            and any_root_bp())


def any_root_bp():
    for root, _dirs, files in os.walk(ASSETS):
        if any(f.startswith("BP_") and f.endswith(".uasset") for f in files):
            return True
    return False


SKIP_DIRS = {"Windows", "Program Files", "Program Files (x86)", "$Recycle.Bin",
             "System Volume Information", "AppData", "node_modules", ".git",
             "Intermediate", "DerivedDataCache", "Binaries", "Saved"}


def scan_projects(roots, max_depth=6):
    # 深度 6：实测工程常埋在 <盘>\<客户>\<日期>\<工程>\Content\MetaHumans（第 5 层），
    # 默认 4 会漏掉（曾导致"明明有资产却扫不到"）。
    """在本机找含 MetaHuman 资产的工程（返回 [<工程目录>, ...]）。

    用途：新机器上只要曾经在**任意**工程里通过 MetaHuman Creator / Quixel Bridge
    下载过 MetaHuman，就能被自动找到并收录，不必记得它在哪。
    """
    found = []
    for root in roots:
        root = os.path.abspath(root)
        if not os.path.isdir(root):
            continue
        base_depth = root.rstrip("\\/").count(os.sep)
        for cur, dirs, files in os.walk(root):
            depth = cur.count(os.sep) - base_depth
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith(".")]
            if depth > max_depth:
                dirs[:] = []
                continue
            if os.path.basename(cur) == "MetaHumans" and \
                    os.path.isfile(os.path.join(cur, "Common", "Face", "Face_Archetype_Skeleton.uasset")):
                proj = os.path.dirname(os.path.dirname(cur))   # <工程>/Content/MetaHumans
                if proj not in found:
                    found.append(proj)
                dirs[:] = []
    return found


def copy_dir(src, dst):
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst)
    mb = sum(os.path.getsize(os.path.join(r, f)) for r, _d, fs in os.walk(dst) for f in fs) / 1048576
    print("  复制 {}  {:.0f} MB".format(os.path.basename(dst), mb))


def main(argv=None):
    ap = argparse.ArgumentParser(description="从参考工程补齐 MetaHuman 公共资产")
    ap.add_argument("--from", dest="src", help="参考工程目录（含 Content/MetaHumans）")
    ap.add_argument("--character", default=DEFAULT_CHARACTER, help="要带的角色目录名")
    ap.add_argument("--check", action="store_true", help="只检查本机资产包是否已齐")
    ap.add_argument("--scan", nargs="*", default=None,
                    help="自动在本机找含 MetaHuman 资产的工程（不给路径则扫各盘根目录，深度 4）")
    args = ap.parse_args(argv)

    if args.check or (not args.src and args.scan is None):
        print("资产包:", ASSETS)
        print("已就绪:", is_ready())
        return 0 if is_ready() else 1

    # --scan：自动找参考工程（MetaHuman 只能由 UE/Bridge 生成，不能凭空创建 → 只能收录）
    if args.scan is not None:
        roots = args.scan or [d + "\\" for d in
                              ("C:", "D:", "E:", "F:", "G:") if os.path.isdir(d + "\\")]
        print("扫描本机含 MetaHuman 资产的工程…（根目录: {}）".format(" ".join(roots)), flush=True)
        hits = scan_projects(roots)
        if not hits:
            print("[未找到] 本机还没有含 MetaHuman 资产的工程。")
            print("  请在 UE 里用 MetaHuman Creator / Quixel Bridge 下载任一 MetaHuman 到某个工程，")
            print("  然后重跑本命令（它会把资产收录到 {} 供后续所有新工程复用）".format(ASSETS))
            return 1
        for i, p in enumerate(hits):
            print("  [{}] {}".format(i, p))
        src_mh = os.path.join(hits[0], "Content", "MetaHumans")
        args.src = hits[0]
        print("采用: {}".format(hits[0]), flush=True)

    src_mh = os.path.join(args.src, "Content", "MetaHumans")
    if not os.path.isdir(src_mh):
        print("[ERROR] 参考工程里没有 Content/MetaHumans: {}".format(src_mh), file=sys.stderr)
        return 2

    os.makedirs(ASSETS, exist_ok=True)
    for name in NEED_COMMON:
        s = os.path.join(src_mh, name)
        if os.path.isdir(s):
            copy_dir(s, os.path.join(ASSETS, name))
        else:
            print("[WARN] 参考工程缺少 {}".format(name), file=sys.stderr)

    # 角色：优先指定角色；没有就取第一个含 BP_ 的角色目录
    char = args.character
    s = os.path.join(src_mh, char)
    if not os.path.isdir(s):
        picked = None
        for name in sorted(os.listdir(src_mh)):
            d = os.path.join(src_mh, name)
            if os.path.isdir(d) and any(f.startswith("BP_") for f in os.listdir(d)):
                picked = name
                break
        if not picked:
            print("[ERROR] 参考工程里找不到任何含 BP_ 的角色目录", file=sys.stderr)
            return 2
        char = picked
        s = os.path.join(src_mh, char)
    copy_dir(s, os.path.join(ASSETS, char))

    print("[OK] 资产包已就绪：{}（角色 {}）".format(ASSETS, char))
    print("[NEXT] 之后新建的工程会自动带上它；已有工程可把 assets\\MetaHumans 复制到 <工程>\\Content\\MetaHumans")
    return 0


if __name__ == "__main__":
    sys.exit(main())
