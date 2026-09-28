# -*- coding: utf-8 -*-
"""组装一个"带上午(am) + 下午(pm)"的流式测试热文件夹。

用法（Windows / Python3，无第三方依赖）：
    python make_testset.py --out D:\\MetaSol\\TestLine_inbox
    # 只有 ID 素材（本仓库自带的 am/pm 两个 ROM，~68MB）：
    python make_testset.py --out <目录>
    # 连同表演素材（从本机素材库拷，默认 E:\\Work\\Mobu_maya\\SSV_Test\\Facial_Data）：
    python make_testset.py --out <目录> --with-perf
    python make_testset.py --out <目录> --with-perf --source D:\\其它素材库 --per-takes 2

生成的目录结构（与执行器的约定一致）：
    <out>\\_id\\{ROM}/        ID（身份原料：上午一个、下午各一个）
    <out>\\am\\{take}/         上午分组的表演素材
    <out>\\pm\\{take}/         下午分组的表演素材

说明：表演素材单条 ~150~220MB（GitHub 单文件 100MB 上限），因此**不入库**，
     由本脚本从本机素材库复制；仓库只自带两个 ROM（am/pm，均为 30MB 级）。
"""

import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROM_AM = os.path.join(HERE, "testset", "_id", "0818_face_rom_am_001_11")
ROM_PM = os.path.join(HERE, "testset", "_id", "0818_face_rom_pm_001_9")
DEFAULT_SOURCE = r"E:\Work\Mobu_maya\SSV_Test\Facial_Data"


def is_take_dir(path):
    return os.path.isdir(path) and os.path.isfile(os.path.join(path, "take.json"))


def copy_take(src, dst_root, group):
    dst_dir = os.path.join(dst_root, group) if group else dst_root
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, os.path.basename(src.rstrip("\\/")))
    if os.path.isdir(dst):
        print("  已存在，跳过: {}".format(dst))
        return False
    shutil.copytree(src, dst)
    mb = sum(os.path.getsize(os.path.join(r, f)) for r, _d, fs in os.walk(dst) for f in fs) / 1048576
    print("  复制 {}/{}  {:.0f} MB".format(group or "(根)", os.path.basename(dst), mb))
    return True


def pick_perf(source, group, count):
    base = os.path.join(source, group)
    if not os.path.isdir(base):
        return []
    takes = [os.path.join(base, n) for n in sorted(os.listdir(base))]
    takes = [t for t in takes if is_take_dir(t)]
    # 取体积最小的前 N 条（跑得快）
    def size(p):
        return sum(os.path.getsize(os.path.join(r, f)) for r, _d, fs in os.walk(p) for f in fs)
    takes.sort(key=size)
    return takes[:count]


def main(argv=None):
    ap = argparse.ArgumentParser(description="组装 am/pm 流式测试热文件夹")
    ap.add_argument("--out", required=True, help="热文件夹（inbox）目录")
    ap.add_argument("--with-perf", action="store_true", help="连同复制表演素材")
    ap.add_argument("--source", default=DEFAULT_SOURCE, help="本机素材库根目录")
    ap.add_argument("--per-takes", type=int, default=2, help="每组表演素材条数")
    args = ap.parse_args(argv)

    out = os.path.abspath(args.out)
    for sub in ("_id", "am", "pm"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    print("热文件夹: {}".format(out))

    # ① ID 素材（上午 + 下午）——仓库自带，必须齐全
    if not is_take_dir(ROM_AM) or not is_take_dir(ROM_PM):
        print("[ERROR] 缺少 ROM 测试素材（tests/fixtures/testset/_id/）", file=sys.stderr)
        return 2
    copy_take(ROM_AM, out, "_id")
    copy_take(ROM_PM, out, "_id")

    # ② 表演素材（可选，从本机素材库）
    if args.with_perf:
        for group in ("AM", "PM_001"):
            takes = pick_perf(args.source, group, args.per_takes)
            if not takes:
                print("  未找到分组 {}: {}".format(group, os.path.join(args.source, group)))
                continue
            for t in takes:
                copy_take(t, out, "am" if group == "AM" else "pm")

    print("\n完成。下一步：")
    print("  1) GUI [新建] 建工程（或 launcher.py new --name X）")
    print("  2) 加载路径指向本目录 -> [预检] -> [启动 / 继续]")
    print("  3) _id 下两个 ROM 会被自动导入 -> 在 UE 里做身份 -> 分别绑定 am/ 与 pm/")
    print("  4) am/ pm/ 下的表演素材随后自动解算，FBX 落到 <输出>/<身份名>/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
