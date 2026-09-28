# MetaHumanSolverEngine
#
# MetaHuman 解算批量引擎（音频 / 视频 / 深度三种模式）。
# 运行环境：UE 5.7+ 编辑器 Python（3.11），全部使用 unreal 模块，无第三方依赖。
#
# 模块职责：
#   versions.py     引擎版本适配层（5.7.4 -> 5.8 升级唯一修改口）
#   config.py       job.json 读取与校验
#   naming.py       命名规则 + 幂等建资产
#   pipeline.py     三种模式单条流水线（音频/视频/深度）
#   batch_runner.py 批处理调度器（断点续跑/幂等/预检/耗时预估）
#   export_fbx.py   ControlRig FACs FBX 导出（含 MetaHuman 控制映射预设）
#   progress.py     进度/日志协议 [MHS_*]
#   bridge.py       C++ 唯一入口 run(json_path) / dry_run() / cancel()

__version__ = "1.0.0"
