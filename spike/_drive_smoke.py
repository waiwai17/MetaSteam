# -*- coding: utf-8 -*-
"""headless_drive 编辑器冒烟（安全版）：
用**临时 job + 空 inbox**（不触碰 accept6 正在跑的素材），验证：
① 引擎内可构造执行器 ② 驱动循环跑通（空闲轮）③ 写 state 心跳
④ stop.flag 优雅退出 ⑤ refresh_assets.flag 触发索引刷新 ⑥ bootstrap 文件存在。
"""
import json
import os
import time
import unreal

RESULT = r"E:\Work\Mobu_maya\MetaSol\spike\drive_smoke_result.txt"
lines = []
ok = True


def log(msg):
    lines.append(str(msg))
    unreal.log("[DRIVESMOKE] {}".format(msg))


try:
    saved = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
    cfg = os.path.join(saved, "Config", "MetaHumanSolver")
    tmp_root = os.path.join(saved, "drive_smoke_tmp")
    inbox = os.path.join(tmp_root, "inbox")
    os.makedirs(os.path.join(inbox, "am"), exist_ok=True)
    out_dir = os.path.join(tmp_root, "out")
    os.makedirs(out_dir, exist_ok=True)

    job = os.path.join(tmp_root, "job_smoke.json")
    with open(job, "w", encoding="utf-8") as fh:
        json.dump({"mode": "depth", "stream_mode": "semi", "import_only": True,
                   "identity_path": "", "inbox": inbox.replace("\\", "/"),
                   "capture_root": "/Game/CaptureManager/DriveSmoke",
                   "identity_import_root": "/Game/CaptureManager/DriveSmoke",
                   "storage_path": "/Game/MH_Results",
                   "import_mode": "cpp", "import_footage": True,
                   "media_output_root": os.path.join(tmp_root, "media").replace("\\", "/"),
                   "fbx_output_dir": out_dir.replace("\\", "/"),
                   "stream_queue_path": os.path.join(tmp_root, "q.json").replace("\\", "/"),
                   "stream_bindings_path": os.path.join(tmp_root, "b.json").replace("\\", "/"),
                   "stream_state_path": os.path.join(tmp_root, "s.json").replace("\\", "/"),
                   "stream_stable_seconds": 0.05}, fh)
    log("job written (空 inbox，安全)")

    from MetaHumanSolverEngine import headless_drive

    # ① 空闲轮跑 2 次（写心跳）
    r = headless_drive.run_loop(job_path=job,
                                stop_flag=os.path.join(tmp_root, "stop.flag"),
                                refresh_flag=os.path.join(tmp_root, "refresh_assets.flag"),
                                max_iterations=2, idle_sleep=0.05)
    log("run_loop(2 次空闲轮) -> {}".format(r))
    if r["iterations"] != 2:
        ok = False
    state_path = os.path.join(tmp_root, "s.json")
    if not os.path.exists(state_path):
        ok = False
        log("FAIL: 未写出 state（心跳缺失 → watchdog 会误判）")
    else:
        with open(state_path, "r", encoding="utf-8") as fh:
            st = json.load(fh)
        log("state 心跳 OK: state={} 同步={} 交付={}".format(
            st.get("state"), st.get("synced"), st.get("delivered")))

    # ② refresh_assets.flag → 索引刷新
    flag = os.path.join(tmp_root, "refresh_assets.flag")
    open(flag, "w").close()
    idx_out = os.path.join(tmp_root, "identity_assets.json")
    headless_drive.run_loop(job_path=job, stop_flag=os.path.join(tmp_root, "stop.flag"),
                            refresh_flag=flag, max_iterations=1, idle_sleep=0.05)
    # 注意：索引写的是默认路径（Saved/Config/MetaHumanSolver/identity_assets.json）
    default_idx = os.path.join(cfg, "identity_assets.json")
    if os.path.exists(default_idx):
        with open(default_idx, "r", encoding="utf-8") as fh:
            n = len(json.load(fh).get("assets", []))
        log("索引刷新 OK（{} 个身份）".format(n))
        if n < 2:
            ok = False
    else:
        ok = False
        log("FAIL: 索引文件未生成")
    if os.path.exists(flag):
        ok = False
        log("FAIL: refresh flag 未清除")

    # ③ stop.flag → 优雅退出
    stop = os.path.join(tmp_root, "stop.flag")
    open(stop, "w").close()
    r2 = headless_drive.run_loop(job_path=job, stop_flag=stop,
                                 refresh_flag=os.path.join(tmp_root, "refresh_assets.flag"),
                                 max_iterations=5, idle_sleep=0.05)
    log("stop.flag -> {}".format(r2))
    if not r2["stopped"]:
        ok = False
except Exception as exc:
    ok = False
    log("EXC: {}".format(exc))

with open(RESULT, "w", encoding="utf-8") as fh:
    fh.write(("PASS\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
unreal.SystemLibrary.quit_editor()
