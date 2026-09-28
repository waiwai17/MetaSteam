# run_stage_a.py —— 任务一验证：阶段 A cpp 批次导入（E:\Mobu_maya\FACE -> /Imports/{批次}/{take}）
# 验证点：① versions._cpp 闭包枚举出 face_am01/face_pm01 两批次 ② 镜像结构落盘
#          ③ ROM 与表演 take 全部产出 7 资产
import time

import unreal

from MetaHumanSolverEngine import batch_runner

start = time.time()
unreal.log("[STAGE_A] cpp 批次导入验证启动")
batch_runner.run_batch(r"e:\Mobu_maya\MetaSol\job_stage_a.json")
unreal.log("[STAGE_A] 导入返回，总耗时 {:.1f}s".format(time.time() - start))
