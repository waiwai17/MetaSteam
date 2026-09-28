# -*- coding: utf-8 -*-
"""主回归运行器：一键跑全部测试套件，输出汇总表。

用法：
    python run_all_tests.py            # 全部合成套件（~1 分钟）
    python run_all_tests.py --realdata # 追加真实数据套件（87 take，需 Facial_Data 在位）

套件说明见同目录 README_tests.md。
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PKG_ROOT = os.path.join(HERE, "..")
DEPLOY_PY = os.path.join(PKG_ROOT, "deploy", "Python")

SUITES = [
    ("批次1 队列+就绪", "test_batch1.py"),
    ("批次2 扫描+入队", "test_batch2.py"),
    ("批次5.3 指纹重建", "test_batch53.py"),
    ("批次5.4 e011事故回归", "test_batch54.py"),
    ("全局检测链", "test_detection_chain.py"),
    ("批次4 单元(观测层)", "test_batch4_unit.py"),
    ("批次4 契约", "test_batch4_contract.py"),
    ("批次4 Web端到端", "test_batch4_web.py"),
    ("asset_index 身份索引", "test_asset_index.py"),
    ("headless_drive 驱动循环", "test_headless.py"),
    ("launcher 启动壳+watchdog", "test_launcher.py"),
    ("交付布局+新建工程目录", "test_output_layout.py"),
]
REALDATA_SUITES = [
    ("批次1 真实数据", "test_batch1_realdata.py"),
    ("批次2 真实数据(87take)", "test_batch2_realdata.py"),
]


def main():
    run_realdata = "--realdata" in sys.argv
    suites = list(SUITES) + (REALDATA_SUITES if run_realdata else [])
    env = dict(os.environ)
    env["PYTHONPATH"] = DEPLOY_PY + os.pathsep + env.get("PYTHONPATH", "")

    results = []
    for title, script in suites:
        path = os.path.join(HERE, script)
        if not os.path.isfile(path):
            results.append((title, script, "SKIP", "脚本不存在"))
            continue
        proc = subprocess.run(
            [sys.executable, path], cwd=HERE, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        ok = (proc.returncode == 0)
        results.append((title, script, "PASS" if ok else "FAIL",
                        "exit={}".format(proc.returncode)))

    print("")
    print("=" * 62)
    print(" 回归汇总（{} 套件）".format(len(results)))
    print("=" * 62)
    n_fail = 0
    for title, script, status, note in results:
        if status != "PASS":
            n_fail += 1
        print("  [{}] {:<22} {:<28} {}".format(status, title, script, note))
    print("=" * 62)
    print(" 结果: {} / {} 通过".format(len(results) - n_fail, len(results)))
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
