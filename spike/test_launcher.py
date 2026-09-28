# -*- coding: utf-8 -*-
"""launcher 单元测试（严格）：配置/路径/预检/job/心跳/队列/存活判定/决策/
watchdog 状态机/原子写/CLI。全部用临时目录与注入函数，不拉起真实编辑器。

核心是 evaluate() 的判定矩阵与 decide() 的动作矩阵——watchdog 的正确性全在这里。
"""

import json
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "deploy", "tools")
sys.path.insert(0, TOOLS)

import launcher   # noqa: E402

OK = [True]
LINES = []


def check(name, cond, detail=""):
    line = "[{}] {}{}".format("PASS" if cond else "FAIL", name,
                              ("  -> " + str(detail)) if (detail and not cond) else "")
    print(line)
    LINES.append(line)
    if not cond:
        OK[0] = False


def write_json(path, data):
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(data, fh)


def build_env(tmp, with_binds=True, bind_map=None):
    """搭一个假工程：project 文件 + Saved/Config/MetaHumanSolver + inbox/out。"""
    proj = os.path.join(tmp, "Proj", "Proj.uproject")
    write_json(proj, {})
    # 预检要求工程自带 MetaHuman 资产（LS 重建必需）→ 假工程也造最小占位
    mh_face = os.path.join(tmp, "Proj", "Content", "MetaHumans", "Common", "Face")
    mh_flint = os.path.join(tmp, "Proj", "Content", "MetaHumans", "Flint")
    os.makedirs(mh_face, exist_ok=True)
    os.makedirs(mh_flint, exist_ok=True)
    open(os.path.join(mh_face, "Face_Archetype_Skeleton.uasset"), "w").close()
    open(os.path.join(mh_flint, "BP_Flint.uasset"), "w").close()
    inbox = os.path.join(tmp, "inbox")
    os.makedirs(os.path.join(inbox, "am"), exist_ok=True)
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    editor = os.path.join(tmp, "UnrealEditor.exe")
    with open(editor, "w") as fh:
        fh.write("fake")
    bootstrap = os.path.join(TOOLS, "headless_bootstrap.py")
    cfg = {"instance": "line01", "ue_editor": editor, "project": proj,
           "inbox": inbox, "fbx_output": out,
           "identity_import_root": "/Game/ID", "import_root": "/Game/Auto",
           "web_port": 8902, "bootstrap": bootstrap}
    paths = launcher.derive_paths(cfg)
    if with_binds:
        write_json(paths["bindings"], {"bindings": bind_map if bind_map else {"am": "/Game/ID/ID_am"}})
    return cfg, paths


def main():
    tmp = tempfile.mkdtemp(prefix="mhs_launcher_")
    try:
        # ── 1) 配置 ──
        cfg, _ = build_env(tmp)
        c2, problems = launcher.load_config(os.path.join(tmp, "nope.json"))
        check("1a 配置缺失 → 不抛，返回问题", isinstance(problems, list) and problems, problems)
        check("1b 缺失时用默认值（mode 相关字段存在）", "ue_editor" in c2 and "web_port" in c2)
        bad = os.path.join(tmp, "bad.json")
        with open(bad, "w", encoding="utf-8") as fh:
            fh.write("{broken")
        c3, p3 = launcher.load_config(bad)
        check("1c 配置损坏 → 不抛，回落默认", p3 and c3.get("web_port") == 8902, p3)
        good = os.path.join(tmp, "good.json")
        write_json(good, {"instance": "line01", "web_port": 8903})
        c4, p4 = launcher.load_config(good)
        check("1d 正常配置被读取（且覆盖默认）", c4["instance"] == "line01" and c4["web_port"] == 8903, c4)

        # ── 2) 路径推导 ──
        cfg, paths = build_env(tmp)
        check("2a config_dir 在 Saved/Config/MetaHumanSolver 下",
              paths["config_dir"].endswith(os.path.join("Saved", "Config", "MetaHumanSolver")),
              paths["config_dir"])
        check("2b job/queue/state/stop/status 六路径齐",
              all(paths.get(k) for k in ("job", "queue", "state", "progress", "stop_flag", "status")), paths)

        # ── 3) 预检 ──
        errors, warnings = launcher.precheck(cfg, paths)
        check("3a 合法环境 → 无 error", errors == [], errors)
        broken = dict(cfg); broken["inbox"] = os.path.join(tmp, "no_such_inbox")
        e2, _ = launcher.precheck(broken, paths)
        check("3b 热文件夹缺失 → error", any("热文件夹" in e for e in e2), e2)
        broken2 = dict(cfg); broken2["ue_editor"] = os.path.join(tmp, "no_editor.exe")
        e3, _ = launcher.precheck(broken2, paths)
        check("3c 编辑器缺失 → error", any("编辑器不存在" in e for e in e3), e3)
        cfg_nb = dict(cfg)
        cfg_nb["bindings_file"] = os.path.join(tmp, "no_such_bindings.json")
        paths_nb = launcher.derive_paths(cfg_nb)
        _, w2 = launcher.precheck(cfg_nb, paths_nb)
        check("3d 绑定文件缺失 → warning（非 error）",
              any("绑定文件不存在" in w for w in w2), w2)
        cfg2, paths_empty_binds = build_env(tmp)
        write_json(paths_empty_binds["bindings"], {"bindings": {}})
        _, w3 = launcher.precheck(cfg2, paths_empty_binds)
        check("3e 绑定表为空 → warning（提示待绑定池）",
              any("绑定表为空" in w for w in w3), w3)

        # ── 4) job 生成（字段对齐已验证格式）──
        job = launcher.build_job(cfg, paths)
        for key in ("mode", "stream_mode", "inbox", "capture_root", "fbx_output_dir",
                    "stream_queue_path", "stream_state_path", "stream_bindings_path",
                    "stream_dashboard_port", "import_then_solve", "export_fbx", "depth"):
            check("4 job 字段存在: " + key, key in job, list(job))
        check("4b inbox 用正斜杠（Exec 传参安全）", "\\" not in job["inbox"], job["inbox"])

        # ── 5) 心跳与队列 ──
        now = time.time()
        check("5a 无文件时心跳 = None", launcher.heartbeat_age(paths, now) is None)
        write_json(paths["state"], {"state": "idle"})
        os.utime(paths["state"], (now - 3, now - 3))
        age = launcher.heartbeat_age(paths, now)
        check("5b 单文件心跳 = 其年龄（~3s）", age is not None and 2.5 <= age <= 4.0, age)
        write_json(paths["progress"], {"state": "solving"})
        os.utime(paths["progress"], (now - 1, now - 1))
        check("5c 取较新者（progress 1s）",
              abs(launcher.heartbeat_age(paths, now) - 1) < 0.6, launcher.heartbeat_age(paths, now))
        check("5d 队列文件缺失 → 活跃数 0", launcher.queue_active_count(paths["queue"]) == 0)
        write_json(paths["queue"], {"tasks": [
            {"key": "a", "status": "pending"}, {"key": "b", "status": "solving"},
            {"key": "c", "status": "done"}, {"key": "d", "status": "failed"}]})
        check("5e 活跃数 = pending+solving（2）", launcher.queue_active_count(paths["queue"]) == 2,
              launcher.queue_active_count(paths["queue"]))
        write_json(paths["queue"], {"tasks": [{"key": "a", "status": "done"}]})
        check("5f 全终态 → 活跃数 0", launcher.queue_active_count(paths["queue"]) == 0)

        # ── 6) evaluate 判定矩阵（watchdog 核心）──
        def ev(alive, queue_tasks, state_age, progress_age=None):
            q = os.path.join(tmp, "q_eval.json")
            write_json(q, {"tasks": [{"key": "t%d" % i, "status": s} for i, s in enumerate(queue_tasks)]})
            p = dict(paths); p["queue"] = q
            write_json(p["state"], {"state": "idle"})
            os.utime(p["state"], (now - state_age, now - state_age))
            if progress_age is not None:
                write_json(p["progress"], {"state": "solving"})
                os.utime(p["progress"], (now - progress_age, now - progress_age))
            else:
                if os.path.exists(p["progress"]):
                    os.remove(p["progress"])
            return launcher.evaluate(p, alive, now=now)

        s, _ = ev(True, ["solving"], 2)
        check("6a 活 + 心跳新 + 有活 → healthy", s == "healthy", s)
        s, _ = ev(True, [], 2)
        check("6b 活 + 心跳新 + 无活 → healthy", s == "healthy", s)
        s, _ = ev(True, ["solving"], 5000)
        check("6c 活 + 心跳陈腐 + 有活 → stalled", s == "stalled", s)
        s, _ = ev(True, [], 5000)
        check("6d 活 + 心跳陈腐 + 无活 → idle（不误重启★）", s == "idle", s)
        s, _ = ev(False, ["pending"], 2)
        check("6e 死 + 有活 → dead", s == "dead", s)
        s, _ = ev(False, [], 2)
        check("6f 死 + 无活 → stopped", s == "stopped", s)
        # 心跳缺失（两个文件都不存在）场景
        p2 = dict(paths); p2["queue"] = os.path.join(tmp, "q2.json")
        write_json(p2["queue"], {"tasks": [{"key": "x", "status": "pending"}]})
        for k in ("state", "progress"):
            if os.path.exists(p2[k]):
                os.remove(p2[k])
        s2, _ = launcher.evaluate(p2, True, now=now)
        check("6g 活 + 无心跳文件 + 有活 → stalled", s2 == "stalled", s2)

        # ── 7) decide 动作矩阵 ──
        check("7a healthy → continue", launcher.decide("healthy", 0)[0] == "continue")
        check("7b idle → continue（不重启★）", launcher.decide("idle", 0)[0] == "continue")
        check("7c stopped → exit", launcher.decide("stopped", 0)[0] == "exit")
        a, w = launcher.decide("stalled", 0)
        check("7d stalled → restart（退避 30s）", a == "restart" and w == 30, (a, w))
        a, w = launcher.decide("dead", 1)
        check("7e 第2次重启 → 退避 120s", a == "restart" and w == 120, (a, w))
        a, _ = launcher.decide("stalled", 3)
        check("7f 超过上限 → abort（不无限拉起★）", a == "abort", a)

        # ── 8) 原子写 ──
        st_path = launcher.write_json_atomic(paths["status"], {"state": "healthy"})
        check("8a 状态文件写出", os.path.exists(st_path))
        check("8b 无 .tmp 残留", not os.path.exists(st_path + ".tmp"))
        with open(st_path, "r", encoding="utf-8") as fh:
            check("8c 内容正确", json.load(fh)["state"] == "healthy")

        # ── 9) watchdog 状态机（注入，不拉真实进程）──
        seq = [True, True, False]      # 活、活、死
        idx = [0]
        calls = []
        def fake_alive():
            i = min(idx[0], len(seq) - 1)
            idx[0] += 1
            return seq[i]
        def fake_launch():
            calls.append(1)
            return 999
        write_json(paths["queue"], {"tasks": [{"key": "a", "status": "solving"}]})
        write_json(paths["progress"], {"state": "solving"})
        os.utime(paths["progress"], (now - 1, now - 1))
        res = launcher.watchdog(cfg, paths, pid=1, is_alive=fake_alive, launch=fake_launch,
                                sleep_fn=lambda s: None, max_ticks=3, log=lambda m: None)
        check("9a watchdog 按 max_ticks 走完", res["ticks"] == 3, res)
        check("9b 死亡+有活 → 触发重启", res["restarts"] >= 1 and calls, res)
        with open(paths["status"], "r", encoding="utf-8") as fh:
            stt = json.load(fh)
        check("9c 状态文件反映最后判定", stt["state"] in ("dead", "stalled", "healthy", "idle"), stt["state"])
        # 上限后 abort
        res2 = launcher.watchdog(cfg, paths, pid=1, is_alive=lambda: True, launch=fake_launch,
                                 sleep_fn=lambda s: None, max_ticks=10,
                                 stall_seconds=0, log=lambda m: None)
        check("9d 持续 stalled → 到上限退出（abort）", res2["action"] == "abort", res2)

        # ── 9b) 后台窗口：ini 关闭节流 + hide 容错 ──
        ini = launcher.ensure_editor_settings(paths)
        if ini and os.path.exists(ini):
            with open(ini, "r", encoding="utf-8") as fh:
                itext = fh.read()
            check("9b-1 写入节 [/Script/UnrealEd.EditorPerformanceSettings]",
                  "[/Script/UnrealEd.EditorPerformanceSettings]" in itext, itext[:120])
            check("9b-2 写入 bThrottleCPUWhenNotForeground=False",
                  "bThrottleCPUWhenNotForeground=False" in itext, itext[:200])
            check("9b-3 值在节内（顺序正确）",
                  itext.index("[/Script/UnrealEd") < itext.index("bThrottleCPUWhenNotForeground=False"))
            launcher.ensure_editor_settings(paths)      # 幂等
            with open(ini, "r", encoding="utf-8") as fh:
                itext2 = fh.read()
            check("9b-4 重复调用不产生重复 key",
                  itext2.count("bThrottleCPUWhenNotForeground=") == 1, itext2.count("bThrottleCPUWhenNotForeground="))
        else:
            check("9b ini 写入", False, "未生成")
        # 既有内容保留（模拟用户已有 ini）
        saved_dir2 = os.path.join(tmp, "Proj2", "Saved")
        p2 = {"saved": saved_dir2}
        ini2 = os.path.join(saved_dir2, "Config", "WindowsEditor", "EditorSettings.ini")
        os.makedirs(os.path.dirname(ini2), exist_ok=True)
        with open(ini2, "w", encoding="utf-8") as fh:
            fh.write("[SomeOtherSection]\nFoo=Bar\n")
        launcher.ensure_editor_settings(p2)
        with open(ini2, "r", encoding="utf-8") as fh:
            keep = fh.read()
        check("9b-5 保留既有 ini 内容", "Foo=Bar" in keep and "SomeOtherSection" in keep, keep[:120])
        # hide 容错：不存在的 pid → False 且不抛
        try:
            r_hide = launcher.hide_editor_window(999999, timeout=0.0, log=lambda m: None)
            check("9b-6 hide 对不存在 pid 返回 False（不抛）", r_hide is False, r_hide)
        except Exception as exc:
            check("9b-6 hide 不抛异常", False, exc)
        # watchdog 的 hide 已节流（每 12 轮一次 + 放在状态写盘后）：EnumWindows 是阻塞
        # 调用，曾挂死主线程 2 小时——节流是自愈机制的一部分，断言必须与实现一致
        hide_calls = []
        launcher.watchdog(cfg, paths, pid=1, is_alive=lambda: True, launch=lambda: 1,
                          sleep_fn=lambda s: None, max_ticks=3, log=lambda m: None,
                          hide=True, hide_fn=lambda: hide_calls.append(1) or True)
        check("9b-7 watchdog hide 节流（3 轮仅第 1 轮调用）", hide_calls == [1], hide_calls)

        # ── 10) 暂停（stop.flag + stop.intent）──
        flag = launcher.request_stop(paths)
        check("10a stop.flag 写出", os.path.exists(flag))
        check("10b stop.intent 同时写出（关键★）", os.path.exists(paths["stop_intent"]))
        # 队列仍有未完成任务 + 进程已死 + 有 intent → 必须判定 stopped（不重启）
        write_json(paths["queue"], {"tasks": [{"key": "a", "status": "pending"}]})
        s_pause, d_pause = launcher.evaluate(paths, False, now=now)
        check("10c 原始判定为 dead（有未完成队列）", s_pause == "dead", s_pause)
        # 走 watchdog：注入 dead 场景 + intent → 期望 exit 而非 restart
        res_p = launcher.watchdog(cfg, paths, pid=1, is_alive=lambda: False,
                                  launch=lambda: 1, sleep_fn=lambda s: None,
                                  max_ticks=3, log=lambda m: None)
        check("10d 暂停时 watchdog 不重启（action=exit★）",
              res_p["action"] == "exit" and res_p["restarts"] == 0, res_p)
        # 崩溃对照：删掉 intent → 同样 dead 场景应触发 restart
        os.remove(paths["stop_intent"])
        res_c = launcher.watchdog(cfg, paths, pid=1, is_alive=lambda: False,
                                  launch=lambda: 1, sleep_fn=lambda s: None,
                                  max_ticks=3, log=lambda m: None)
        check("10e 崩溃时 watchdog 重启（对照★）",
              res_c["action"] == "restart" and res_c["restarts"] >= 1, res_c)
        # 继续：clear_stop 清掉两个标记
        launcher.clear_stop(paths)
        check("10f 继续前清 stop.flag", not os.path.exists(paths["stop_flag"]))
        check("10g 继续前清 stop.intent（否则新会话被当已暂停★）",
              not os.path.exists(paths["stop_intent"]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # ── 10h) 同工程互斥（防两个编辑器共写队列★）──
    hits = launcher.find_running_editor(os.path.join(tmp, "Proj", "Proj.uproject"))
    check("10h-1 find_running_editor 不抛（返回 list）", isinstance(hits, list), hits)
    hits2 = launcher.find_running_editor(os.path.join(tmp, "NoSuch", "X.uproject"))
    check("10h-2 不存在的工程 → 无命中", isinstance(hits2, list) and len(hits2) == 0, hits2)

    # ── 11) CLI（子进程，真实调用）──
    tmp2 = tempfile.mkdtemp(prefix="mhs_cli_")
    try:
        cfg, paths = build_env(tmp2)
        cfg_path = os.path.join(tmp2, "pipeline.config.json")
        write_json(cfg_path, cfg)
        import subprocess
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "launcher.py"), "doctor",
                            "--config", cfg_path], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
        out = r.stdout.decode("utf-8", "ignore")
        check("11a doctor 退出码 0（预检通过）", r.returncode == 0, out[-300:])
        check("11b doctor 输出含 errors 字段", '"errors"' in out, out[-200:])
        r2 = subprocess.run([sys.executable, os.path.join(TOOLS, "launcher.py"), "start",
                             "--config", cfg_path, "--dry-run"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out2 = r2.stdout.decode("utf-8", "ignore")
        check("11c start --dry-run 退出码 0", r2.returncode == 0, out2[-300:])
        check("11d dry-run 打印 job（不拉起进程）", '"stream_queue_path"' in out2, out2[-200:])
        r3 = subprocess.run([sys.executable, os.path.join(TOOLS, "launcher.py"), "status",
                             "--config", cfg_path], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
        check("11e status 退出码 0", r3.returncode == 0,
              r3.stdout.decode("utf-8", "ignore")[-300:])
    finally:
        shutil.rmtree(tmp2, ignore_errors=True)

    print("")
    print("=== launcher 单测: {} ===".format("全部通过" if OK[0] else "存在失败"))
    with open(os.path.join(HERE, "launcher_result.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
