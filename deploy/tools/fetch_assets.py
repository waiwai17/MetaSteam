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
    args = ap.parse_args(argv)

    if args.check or not args.src:
        print("资产包:", ASSETS)
        print("已就绪:", is_ready())
        return 0 if is_ready() else 1

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
