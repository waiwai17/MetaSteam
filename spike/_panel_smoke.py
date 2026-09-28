# -*- coding: utf-8 -*-
"""面板布局冒烟：open_tool_window 打开面板（触发身份区新布局的 Slate 构造），
存活 + 零崩溃 = PASS。绑定校验（IsIdentityAssetAt）走 Python 侧同路径验证。"""
import time
import unreal

RESULT = r"E:\Work\Mobu_maya\MetaSol\spike\panel_smoke_result.txt"
lines = []


def log(msg):
    lines.append(str(msg))
    unreal.log("[SMOKE] {}".format(msg))


ok = True
try:
    # 1) 打开面板（新布局 Slate 构造——若布局代码有错此处即崩/抛异常）
    unreal.MetaHumanSolverCommands.open_tool_window()
    time.sleep(8)
    log("panel_opened_and_alive_8s")
except Exception as exc:
    ok = False
    log("panel_failed: {}".format(exc))

# 2) 绑定链路（Python 侧）：真实身份资产路径直写——面板按钮最终调用的同一入口
try:
    from MetaHumanSolverEngine import bridge
    # am 真实身份（磁盘已确认存在）
    r = bridge.stream_set_binding(
        r"F:/CFH/CFH/20260709/MH_H/Saved/Config/MetaHumanSolver/job_stream.json",
        "am", "/Game/CaptureManager/ID4/0818_face_rom_am_001_11")
    log("set_binding_am={}".format(r))
    if not r:
        ok = False
    r2 = bridge.stream_set_binding(
        r"F:/CFH/CFH/20260709/MH_H/Saved/Config/MetaHumanSolver/job_stream.json",
        "am", "")   # 立即还原（不改变用户现场——绑定留给用户自己点）
    log("clear_binding_am={}".format(r2))
except Exception as exc:
    ok = False
    log("binding_failed: {}".format(exc))

with open(RESULT, "w", encoding="utf-8") as fh:
    fh.write(("PASS\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
unreal.SystemLibrary.quit_editor()
