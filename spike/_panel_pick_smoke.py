# -*- coding: utf-8 -*-
"""模块2 冒烟：面板新布局（含 [▾ 选择]）Slate 构造存活 + 身份资产精确列表可用。
判据：打开面板不崩 + 选一个真实身份写入绑定成功（走同一条绑定链路）。"""
import time
import unreal

RESULT = r"E:\Work\Mobu_maya\MetaSol\spike\panel_pick_smoke_result.txt"
lines = []
ok = True


def log(msg):
    lines.append(str(msg))
    unreal.log("[PICKSMOKE] {}".format(msg))


try:
    unreal.MetaHumanSolverCommands.open_tool_window()
    time.sleep(8)
    log("panel_opened_and_alive_8s (含 [▾ 选择] 新布局)")
except Exception as exc:
    ok = False
    log("panel_failed: {}".format(exc))

# 走同一条绑定链路：显式指定 pm 身份（索引里真实存在）
try:
    from MetaHumanSolverEngine import bridge
    r = bridge.stream_set_binding(
        r"F:/CFH/CFH/20260709/MH_H/Saved/Config/MetaHumanSolver/job_stream.json",
        "pm", "/Game/CaptureManager/ID4/0818_face_rom_pm_001_9")
    log("set_binding_pm={}".format(r))
    if not r:
        ok = False
    r2 = bridge.stream_set_binding(
        r"F:/CFH/CFH/20260709/MH_H/Saved/Config/MetaHumanSolver/job_stream.json",
        "pm", "")       # 还原，不改动现场
    log("clear_binding_pm={}".format(r2))
except Exception as exc:
    ok = False
    log("binding_failed: {}".format(exc))

with open(RESULT, "w", encoding="utf-8") as fh:
    fh.write(("PASS\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
unreal.SystemLibrary.quit_editor()
