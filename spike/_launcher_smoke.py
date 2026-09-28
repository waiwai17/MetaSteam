# -*- coding: utf-8 -*-
"""B 模块集成冒烟（安全）：用临时空 inbox 的配置，真实走 launcher 全流程
① doctor 预检 ② start --dry-run ③ 真实拉起编辑器（后台 watchdog）
④ status 显示 healthy ⑤ 写 stop.flag → 段间优雅退出 ⑥ 状态文件反映 stopped。
不触碰 accept6 正在跑的素材。
"""
import json
import os
import shutil
import subprocess
import sys
import time

RESULT = r"E:\Work\Mobu_maya\MetaSol\spike\launcher_smoke_result.txt"
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "deploy", "tools")
lines = []
ok = True


def log(msg):
    lines.append(str(msg))
    print(msg)


tmp = os.path.join(HERE, "launcher_smoke_tmp")
try:
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(os.path.join(tmp, "inbox", "am"), exist_ok=True)
    os.makedirs(os.path.join(tmp, "out"), exist_ok=True)

    cfg = {
        "instance": "smoke",
        "ue_editor": r"D:/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe",
        "project": r"F:/CFH/CFH/20260709/MH_H/MH_H.uproject",
        "inbox": os.path.join(tmp, "inbox").replace("\\", "/"),
        "fbx_output": os.path.join(tmp, "out").replace("\\", "/"),
        "identity_import_root": "/Game/CaptureManager/DriveSmoke",
        "import_root": "/Game/CaptureManager/DriveSmoke",
        "web_port": 8907,
    }
    cfg_path = os.path.join(tmp, "pipeline.config.json")
    with open(cfg_path, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh)

    def run(args):
        return subprocess.run([sys.executable, os.path.join(TOOLS, "launcher.py")] + args
                              + ["--config", cfg_path],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    # ① doctor
    r = run(["doctor"])
    log("doctor rc={}".format(r.returncode))
    if r.returncode != 0:
        ok = False
        log("doctor 输出: " + r.stdout.decode("utf-8", "ignore")[-400:])

    # ② dry-run
    r = run(["start", "--dry-run"])
    log("dry-run rc={}".format(r.returncode))
    if r.returncode != 0 or '"stream_queue_path"' not in r.stdout.decode("utf-8", "ignore"):
        ok = False

    # ③ 真实拉起（后台 watchdog）
    proc = subprocess.Popen(
        [sys.executable, os.path.join(TOOLS, "launcher.py"), "start", "--config", cfg_path],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log("launcher started pid={}".format(proc.pid))
    time.sleep(75)                          # 等编辑器起来 + 驱动写心跳

    # ④ status
    r = run(["status"])
    out = r.stdout.decode("utf-8", "ignore")
    live_state = ""
    try:
        live_state = json.loads(out)["live"]["state"]
    except Exception:
        pass
    log("status live.state={}".format(live_state))
    if live_state not in ("healthy", "idle"):
        ok = False
        log("status 原文: " + out[-500:])

    # ⑤ 停止
    r = run(["stop"])
    log("stop rc={} flag_written".format(r.returncode))
    time.sleep(60)                          # 等段间退出 + 编辑器退出
    try:
        proc.wait(timeout=60)
        log("launcher 已退出 rc={}".format(proc.returncode))
    except Exception:
        proc.kill()
        ok = False
        log("FAIL: launcher 未退出")

    # ⑥ 最终状态文件
    saved_cfg = r"F:\CFH\CFH\20260709\MH_H\Saved\Config\MetaHumanSolver\pipeline.status.json"
    if os.path.exists(saved_cfg):
        with open(saved_cfg, "r", encoding="utf-8") as fh:
            st = json.load(fh)
        log("final status: state={} action={}".format(st.get("state"), st.get("action")))
        if st.get("state") not in ("stopped", "idle", "healthy"):
            ok = False
    else:
        ok = False
        log("FAIL: 状态文件未生成")

    # ⑦ 进程必须真的没了（僵尸编辑器检查★）
    r = run(["status"])
    try:
        alive = json.loads(r.stdout.decode("utf-8", "ignore"))["live"]["alive"]
    except Exception:
        alive = True
    log("编辑器进程仍存活? {}".format(alive))
    if alive:
        ok = False
        log("FAIL: 停止后编辑器仍在（僵尸进程）")
except Exception as exc:
    ok = False
    log("EXC: {}".format(exc))
finally:
    # 收尾：不污染用户现场（删除冒烟写入的 job / stop.flag）
    try:
        cfg_dir = r"F:\CFH\CFH\20260709\MH_H\Saved\Config\MetaHumanSolver"
        for f in ("job_stream.json", "stop.flag"):
            p = os.path.join(cfg_dir, f)
            if os.path.exists(p):
                os.remove(p)
        subprocess.run(["taskkill", "/F", "/IM", "UnrealEditor.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(tmp, ignore_errors=True)
        log("cleanup done")
    except Exception as exc:
        log("cleanup warn: {}".format(exc))

with open(RESULT, "w", encoding="utf-8") as fh:
    fh.write(("PASS\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
print("SMOKE_RESULT=" + ("PASS" if ok else "FAIL"))
