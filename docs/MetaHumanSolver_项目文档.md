# MetaHumanSolver 项目文档（综合唯一版）

> **文档基线：2026-08-26**（由原 5 份文档合成：README / 第一性原理审核报告 / 解算进度监控设计 / 三阶段拆分与多进程实验计划 / 插件 CHANGELOG）
> **架构**：C++ 插件（Slate 面板 + TakeIngest 导入器 + 进度监控）+ Python 引擎包（三模式批处理）
> **当前宿主工程**：`E:\Mobu_maya\SSV_Test\SSV`（UE 5.7.4，`D:/UE_5.7`）；开发工作区：`e:\Mobu_maya\MetaSol`
> **兼容**：UE 5.7.4（`versions.py` 为版本适配唯一入口，升级引擎只改它）

---

## 一、项目总览

### 1.1 系统本质（第一性原理）

把一组"捕获数据"（iPhone 深度录像 / 单视频 / 音频）**可靠地**批量转换为下游（Maya/MotionBuilder）**可用的 FBX 动画交付物**（ControlRig FACs 曲线或烘焙骨骼），且整个过程**可规模化、可中断恢复、无静默丢数据**。

### 1.2 必须满足的不变量

| # | 不变量 | 推导依据 |
|---|---|---|
| I1 | **交付物完整性**：只有 FBX 真实落盘才算"该段完成" | 本质目标是交付物，不是"解算跑完" |
| I2 | **幂等性**：同一 job 重复执行，产出不重复、不累积 | 批量工具必然面对重跑/失败重试 |
| I3 | **可恢复性**：中断后继续，不重跑已完成段 | 26 分钟/段的深度解算，一次全跑不现实 |
| I4 | **可取消性**：长任务应能中止 | 深度单段可达 26 分钟 |
| I5 | **反馈可见性**：运行中进度/日志应持续可见 | 用户需要知道"跑没跑、跑到哪" |
| I6 | **失败隔离**：单段失败不拖垮整批 | 素材质量参差是常态 |
| I7 | **配置一致性**：UI 表单 ↔ 引擎语义必须单点对齐 | 曾发生 `export_baked_fbx` 表单与引擎不一致的"死配置" |
| I8 | **可移植性**：跨机器、跨引擎版本可迁移 | 工具要分发给团队 |

### 1.3 架构分层

- **C++ 插件只做 UI/桥接/观测**：Slate 面板、导入管线、原生进度监控（独立线程小窗）；
- **业务全在 Python**：批处理调度、解算/导出流水线、命名、版本适配；
- **通信走运行时协议**：job.json + `[MHS_*]` 日志 + `progress.json` 心跳，零静态绑定。
- 分层正确性：升级引擎只需动 Python 与 `versions.py`；C++ 按新引擎重编译。

---

## 二、工作区目录结构（2026-08-26 整理后）

```
e:\Mobu_maya\MetaSol\                        ← 开发工作区
├── deploy\                                  ← ★ 同步物（每次同步整个文件夹复制）
│   ├── MetaHumanSolver\                     ← C++ 插件（→ {工程}\Plugins\）
│   │   ├── Source\                          ←   插件主模块 + TakeIngest 导入模块
│   │   ├── Binaries\ / Intermediate\        ←   编译产物（同步时随目录一起带上）
│   │   └── MetaHumanSolver.uplugin          ←   插件描述符
│   └── Python\MetaHumanSolverEngine\        ← Python 引擎包（→ {工程}\Content\Python\）
├── docs\
│   └── MetaHumanSolver_项目文档.md           ← 本文件（唯一文档）
├── jobs\
│   ├── job_stage_a.json                     ← 阶段 A：批次导入 job 模板
│   └── job_stage_c.json                     ← 阶段 C：深度解算 job 模板（含 identity_map）
├── tools\
│   ├── run_stage_a.py                       ← 阶段 A 无头命令行生产入口
│   └── reexport_ready001.py                 ← 单素材重导出工具（不重解算，编辑器内跑）
├── experiments\                             ← 历史实验归档（20260820 全量数据）
└── build_log.txt                            ← 编译日志（Build.bat 输出重定向目标）
```

### 2.1 插件内结构（deploy\MetaHumanSolver\Source\）

| 模块 | 职责 |
|---|---|
| `MetaHumanSolver\` | 面板 UI（`SMetaHumanSolverWindow.*`）、进度监控（`MHSProgressMonitor.*`）、模块入口 |
| `MetaHumanSolverTakeIngest\` | take 导入管线（`MHSTakeImporter.*`：①解析→②转换→③cparch→④建资产→⑤组装→⑥保存）、早期 spike/Utils |

### 2.2 Python 引擎包文件用途

| 文件 | 职责 |
|---|---|
| `__init__.py` | 包入口 |
| `bridge.py` | C++ ↔ Python 唯一入口（run / run_next / dry_run / precheck_import / cancel） |
| `config.py` | job.json 读取/校验/默认值（字段字典在此） |
| `batch_runner.py` | 批处理调度（混合段序列 / 状态机 / 幂等 / 预检 / 交付清单） |
| `pipeline.py` | 解算/导出流水线（Performance 创建 / AS / LS / identity 解析 / 段间串扰防护） |
| `export_fbx.py` | ControlRig FACs FBX 导出（Sequencer 隔离）+ 烘焙 FBX |
| `progress.py` | `[MHS_*]` 日志协议 + 原生进度监控 Python 入口 |
| `naming.py` | 资产命名规则 |
| `versions.py` | 引擎版本适配层（5.7→5.8 只改此文件） |

---

## 三、部署与同步

### 3.1 同步到宿主工程（每次改动后）

| 同步物 | 源（本工作区） | 目标（宿主工程） |
|---|---|---|
| C++ 插件 | `deploy\MetaHumanSolver\`（整个文件夹） | `{工程}\Plugins\MetaHumanSolver\` |
| Python 包 | `deploy\Python\MetaHumanSolverEngine\` | `{工程}\Content\Python\MetaHumanSolverEngine\` |

**注意**：
- Python 开发主线在宿主工程（`{工程}\Content\Python\MetaHumanSolverEngine\`，纯 Python 无需编译、改完即生效）；每次改动后用 PowerShell 镜像回 `deploy\Python\` 保持分发源最新：
  ```powershell
  Copy-Item -Force "{工程}\Content\Python\MetaHumanSolverEngine\*.py" "e:\Mobu_maya\MetaSol\deploy\Python\MetaHumanSolverEngine\"
  ```
- C++ 开发主线在 `deploy\MetaHumanSolver\`，改后同步到 `{工程}\Plugins\MetaHumanSolver\` 再编译。

### 3.2 编译（C++ 改动后）

```powershell
& "D:\UE_5.7\Engine\Build\BatchFiles\Build.bat" {工程}Editor Win64 Development -project="{工程}\{工程}.uproject" -WaitMutex *> e:\Mobu_maya\MetaSol\build_log.txt
```
当前宿主：`SSVEditor` + `E:\Mobu_maya\SSV_Test\SSV\SSV.uproject`。退出码 0 为成功；输出重定向到 `build_log.txt` 再读取（PowerShell 管道会吞 Build.bat 输出）。

### 3.3 迁移到新主机/新项目

**迁移物就两个文件夹**：`deploy\MetaHumanSolver\` + `deploy\Python\MetaHumanSolverEngine\`。版本一致（UE 5.7.4）时放进新项目即可用。

**迁移后必做（3 步）**：
1. **编译插件**（见 3.2，换成新项目名与路径）；
2. **启用插件**：编辑器 → Edit → Plugins → 搜 MetaHumanSolver → Enabled → 重启；
3. **前置资产检查**（新项目里必须存在，缺则面板预检会报）：
   - MetaHuman 角色蓝图：`/Game/MetaHumans/{角色}/BP_{角色}`（LS 重建 target）
   - 骨架：`/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton`
   - 身份资产：每批表演对应的 `MetaHumanIdentity`（用面板①区"身份创建"导入 ROM 后手动制作）

**引擎版本不一致时**：唯一改动点 `versions.py`（文件头有升级流程注释）；C++ 需按新引擎重编译。

---

## 四、工作流使用速查

### 4.1 面板两阶段工作流

```
① 身份创建（新演员才需要）：填 ROM take 文件夹 → [浏览]选 → [导入] → 编辑器里做 identity + Prepare
② 解算任务：任务行填 身份资产 + 表演源文件夹（可多行多批）→ 输出路径 → [开始解算]
   自动流程：导入表演 → 深度解算 → 导出 FBX（原始 take 名）
   高级选项：排除素材 / ☑不含舌头与音频（纯面部交付，默认勾选）
```

### 4.2 监视命令

进度窗被关时，`progress.json` 为实时数据源：
```powershell
# {工程}\Saved\Config\MetaHumanSolver\progress.json
Get-Content "{工程}\Saved\Config\MetaHumanSolver\progress.json" -Wait
```
字段含：`asset_name / state / stage / pass / pass_count / current_frame / total_frames / percent / elapsed_seconds / eta_seconds / batch_index / batch_total / batch_eta_seconds / updated_at`。

### 4.3 交付约定（2026-08-26 更新）

- **FBX 平铺交付**：所有 take 的 `.fbx` 直接放 `fbx_output_dir` 根目录下（不再按素材名套子目录），如 `D:\OUT\Anims_Flint_Ul_Enter_005.fbx`；
- **交付清单**：每段完成后覆盖写 `delivery_{时间戳}.json` + `.txt` 到 `fbx_output_dir`（与 FBX 同处），含每段 FBX 路径/大小、AS/LS 路径、状态、耗时、失败原因；
- **解算控制台单行格式**（原生小窗行 1，固定列宽对齐）：
  ```
  [3/12] A数据名             阶段1     1517/3658帧       5:23/12:26
  ```

---

## 五、核心设计

### 5.1 三阶段拆分（A/B/C）

| 阶段 | 内容 | 实现 |
|---|---|---|
| A：自动导入 | ROM take → UE 资产（每 take 7 资产：`CD_`/`IS_Video_`/`IS_Depth_`/`SW_Audio_`/`CC_`/`LF_Video_`/`LF_Depth_`） | C++ `UMHSTakeImporter`，`import_mode="cpp"` 按批次镜像导入 |
| B：手动角色 ID | 编辑器内 Identity 关联 + Prepare | 人工环节，`auto_prepare_identity` 可自动训练 |
| C：批量解算 + FBX | 扫描 capture_root → 阻塞解算 → AS + LS + ControlRig FBX | `batch_runner.run_batch` |

- 批次镜像约定：磁盘 `{source}/{批次}/{take}\` → UE `/Game/CaptureManager/Imports/{批次}/{take}/CD_...`；
- `identity_map: {"face_am01": "...", "face_pm01": "..."}` 按包路径首段（批次名）解析 identity，未命中回落全局 `identity_path`（兜底不崩）；
- ROM take 不参与解算：用 `exclude` 名单排除（identity 命名与 ROM 名无规律关联，排除是唯一可靠手段）。

### 5.2 多进程实验结论（2026-08-20 实测，i7-10700 + RTX 3060）

| 实验 | 结果 | 关键数据 |
|---|---|---|
| 无头解算 | ⚠️ **已被 2026-08-27 实测推翻** | 见 §5.6：commandlet 强制 NullRHI，触碰 identity 即断言崩溃 |
| 单实例基线 | ✅ | 单 take **34.3 分钟**；显存 ~3.3GB；GPU 利用率仅 ~10% |
| 双实例分片 | ✅ 无冲突 | 显存峰值 9.6GB 未 OOM；**加速比 ≈ 1.31x** |
| 产物核验 | ✅ | 6/6 资产可加载，ControlRig 轨完好，FBX 零冲突 |

**结论**：技术可行但本机收益有限（Pass 2 纯 CPU 求解，双实例瓜分 8 核）。本机保留单实例串行；16 核+机器可启用分片（job 文件 `exclude` 分片即可，机制已就绪）。真正优化方向：阶段 A 转换与阶段 C 解算的流水线并行（资源画像互补）。

> ⚠️ **2026-08-27 修正**：上表"无头解算 ✅"与"双实例分片"结论在后续实境验证中未能复现，见 §5.6。
> 另：A/C 流水线的收益需重新评估——按单 take 转换 28.3s vs 解算 2058s 计算，导入仅占整批约 0.5%，
> **流水线并行的理论收益上限极低，不应作为主攻方向**（瓶颈 99.5% 在解算本身）。

### 5.3 导入异步化（2026-08-26）

- **根因**：`RunConversionBlocking` 虽已在后台线程转换，但游戏线程阻塞在 `DoneEvent->Wait()` 上 37 分钟 → 失焦/未响应/需点击才继续。
- **修复**：导入管线拆两段——`ImportSingleTake_Prepare`（①②③解析/转换/cparch，后台 `ThreadPool` 执行）+ `ImportSingleTake_Finalize`（④⑤⑥建资产/组装/保存，回游戏线程）；游戏线程等待期间 `FSlateApplication::PumpMessages()` 泵消息保持窗口响应。
- 另：进度监控心跳线程每 500ms `PostMessage(WM_NULL)` 程序化唤醒 UE 主窗口消息泵，根治"点击面板才推进下一批"（无头模式 Hwnd 为空自动跳过）。

### 5.4 导入并行（多 take 并发转换，按核数自适应）

**决策依据（第一性原理）**：
- 导入耗时大头是 ② 媒体转换（IO/解码密集）；并行度由"瓶颈资源"决定，不是核数本身；
- 用户实测：**24 核机器单 take 转换仅占 ~10%（≈2.4 核）** → 单 take 没吃满多核，多 take 并发转换有明显收益；
- 形态选**单实例多线程（路径 A）**而非多进程（路径 B）：无 UE 冷启动开销、共享引擎/asset registry，本机 24 核直接吃满；
- 本机 8 核的深度解算双实例 1.31x 教训不适用于 24 核导入（资源画像不同）。

**实现**（`MHSTakeImporter.cpp/.h`）：
- `ImportTakesFromRoot(..., bBlocking, InConcurrency)`：`InConcurrency=0/负` = auto，`>0` = 固定并发；
- **auto 并发 = 物理核 / kCoresPerTake**（`FPlatformMisc::NumberOfCores()` 取物理核；`kCoresPerTake=3` 按实测 2.4 核保守取整，留系统/磁盘余量），`Clamp` 到 `[1, 16]` 防磁盘 IO 争抢反噬；
- **并发调度**：替换原逐 take 串行循环 → 并发槽位调度，同时最多 N 个 `Prepare` 在后台 `ThreadPool` 转换；每个完成后回游戏线程**串行** `Finalize`（建资产/保存，asset registry 无并发写冲突），释放槽位拉下一个 take；
- 传递用 `TQueue<FMHSImportWork, EQueueMode::Mpsc>`（多生产者/单消费者）+ `FEvent` 唤醒；等待期间泵 Slate 消息保持窗口响应；
- 幂等/排除逻辑不变，只并行转换阶段。

**核占用探测**（校准 `kCoresPerTake` 用）：②转换前后 `GetProcessTimes` 采样进程累计 CPU 时间，打日志：
```
[MHSTake] ② 转换诊断: 耗时 28.3s · 平均核占用 2.4 核（本机物理核 24 / 逻辑核 48）
```
在目标机器跑 1 个 take 取实测值，更新 `kCoresPerTake`（`平均核占用` 向上取整+1）。

**配置**（`config.py`）：
```json
"imports_concurrency": "auto"   // auto=按物理核自动 / 正整数=固定并发
```
Python 侧 `versions.py` `import_footage_fn("cpp", cfg)` 解析并传 C++（0=auto）。

**预检并发报告**（`precheck_import` 末尾，静态检测不阻断）：C++ `GetRecommendedImportConcurrency` UFUNCTION（与导入共用同一并发计算）返回 [物理核, 逻辑核, 实际并发]，预检输出：
- 并发配置合法性校验（非 auto/非正整数 → 报错）；
- 实际生效并发 + auto/显式值来源；
- `take 数 < 并发数` → 提示并发浪费（建议收敛）；
- 并发达上限 16 + 数据盘 HDD → 提示 IO 争抢风险；
- 首跑无转换基准 → 提示参考 `② 转换诊断` 日志校准。

**面板混合段导入并发（2026-08-26）**：`_discover_mixed_segments` 与纯导入序列把 N 个导入段合并为 **1 个批次导入段**（`batch_import=True`），段内按 take 列表调 C++ `ImportTakes`（显式 take 目录列表入口，复用同一并发调度）并发转换；解算段仍逐段。要点：
- **幂等**：`_filter_done`/`run_next` 按 take 粒度过滤已导入项（`_import_already_done`），重跑只重导未完成 take；全部已导入则整段跳过；
- **exclude 精确**：批次导入段从 takes 列表剔除被排除 take（不整目录扫描，ROM 不会误导入）；
- **段语义**：进度格从 `[导入×N, 解算×N]` 变 `[批次导入, 解算×N]`，perf 显示序号自动适配（导入段不计数）；批次导入段名 = 源目录名。
- **任务进度可视化（2026-08-26）**：C++ 并发调度每完成一个 take 更新 `FMHSProgressMonitor::UpdateTaskProgress(completed, eta, 活跃take名)`，小窗实时显示：
  ```
  行1: [导入批次 face_am01] 3/10 take · 并发8 · 已耗 4:12 · 剩余 11:30
  行2: 转换中: take4, take7, take8
  ```
  Python `monitor_set_task_context(total, concurrency)` 开启任务进度（`SetTaskContext` 新 UFUNCTION）；ETA 由 C++ 按 `已耗/完成×剩余` 外推；活跃名单以目录 basename 匹配（`FMHSImportWork.TakeDirectory`）；批末自动清空。
  - 类已加 `METAHUMANSOLVER_API` 导出（TakeIngest 跨模块链接）；`TaskActiveMutex` 用 `std::mutex` + `std::lock_guard`（FScopeLock 不适用 std::mutex）。

### 5.5 解算进度监控（核心设计）

**假死的本质**：深度解算同步阻塞 → 游戏线程单帧无限延长 → Slate Tick / 渲染 / Windows 消息泵三者全停。
**核心推论**：要"让用户知道在跑" = 在游戏线程不可用的情况下，开辟一条**不依赖游戏线程的实时信号通道**。

**总体架构**：
```
Python（游戏线程）──begin_progress(perf, name)──▶ C++ FMHSProgressMonitor
                                                      ├─ OnFrameProcessed 回调 → atomic store
                                                      ├─ 心跳线程 Worker（500ms 读 atomic → 百分比/ETA → 写 json + 刷小窗）
                                                      └─ 原生小窗（独立线程 + 消息循环 / conhost 控制台模式）
引擎 MetaHumanPerformance ──每帧 Broadcast──▶ 回调写 atomic
```

**线程纪律**：游戏线程只写 atomic（Begin/SetStage/End/逐帧回调）；心跳线程只读 atomic + 写文件/刷窗；窗口线程独占窗口创建/消息循环/销毁。

**多 Pass 加权进度**：深度全量=3 段（跟踪解算/全局最终解算/滤波），Preview=2，单目=1。Pass 边界由帧号回绕检测（收到比水位更小的归一化帧号 = 新 Pass）。权重带实测校准 **19 / 80 / 1**。防卡死爬行：帧号 >8s 未推进 → 向当前 Pass 权重带末端指数趋近（进度条永不静止），显示高水位单调不回退，99% 钳制 / 100% 由 `end_progress` 硬触发。

**控制台渲染**（conhost 控制台模式）：标题 `MetaHuman 解算中 45.3% [3/12]`；行 1 固定列宽对齐（批次/数据名/阶段N/帧/已耗-剩余）；行 2 批次剩余预估 + Pass 摘要。`bOwnedConsole` 标记确保只释放自己 Alloc 的控制台（`-stdout` 无头模式保护）。

---

### 5.6 实测验证记录（2026-08-27）

> **目的**：为"解算(S) / 导出(E) 解耦"与"自适应并行解算"确认 API 可用性与无头可行性。
> **环境**：UE 5.7.4 / i7-10700 / 64GB，宿主 `E:\Work\Mobu_maya\SSV_Test\SSV`。
> **方法**：无头 commandlet + 编辑器模式共 5 轮探针（临时脚本已清理）。

#### 5.6.1 Performance API 可用性（实测）

| C++ API | Python 名 | 结果 |
|---|---|---|
| `CanExportAnimation()` | `can_export_animation()` | ✅ 可用 |
| `GetNumberOfProcessedFrames()` | `get_number_of_processed_frames()` | ✅ 可用 |
| `CanProcess()` | `can_process()` | ✅ 可用 |
| `IsProcessing()` | `is_processing()` | ✅ 可用 |
| `DiagnosticsIndicatesProcessingIssue()` | `diagnostics_indicates_processing_issue()` | ❌ 返回 `None`，Python 绑定不可用，**勿用** |

**源码依据**（`MetaHumanPerformance.cpp`）：

| 机制 | 位置 | 结论 |
|---|---|---|
| `CanExportAnimation()` == `ContainsAnimationData()` | :503 → :2211 | 判断"是否至少一帧有效动画数据" |
| `AnimationData` 自定义 `Serialize` | :478-501 | **`Ar << AnimationData` 无条件写入** |

> ✅ **关键结论**：解算结果随 Performance 资产**持久化落盘**，不依赖 transient 内存
> → **S/E 解耦在原理上成立**（S 保存后，E 可在另一进程/会话消费）。

`GetNumberOfProcessedFrames()`（:2234）返回非空动画帧数，是比布尔更强的判据——
可识别"只解算了部分帧就崩溃"的情况。建议 S 阶段出口采用双重判据：
`can_export_animation() and get_number_of_processed_frames() >= 预期帧数 * 0.95`。

#### 5.6.2 无头路径不可用（推翻 §5.2 记载）

| 验证项 | 结果 |
|---|---|
| commandlet（`UnrealEditor-Cmd -run=pythonscript`）RHI | **`rhiname="Null"`**；`-dx12` / `-d3d12` **均无效** |
| 无头下触碰 identity | **断言崩溃**：`MetaHumanIdentityParts.cpp:1689` `check(DefaultSolver->PredictiveSolver)` |
| 崩溃性质 | **进程级**，Python `try/except` 无法捕获，退出码 3 |

**根因**（`MetaHumanIdentityParts.cpp:1666-1693`）：
`LoadDefaultFaceFittingSolvers()` 在 `IPredictiveSolverInterface` 模块化特性可用时，
强制要求 `DefaultSolver->PredictiveSolver` 非空；该求解器在**无 GPU（NullRHI）下加载失败** → 断言。

> ❌ **结论：无头多实例并行解算方案不成立。**

#### 5.6.3 编辑器模式下仍不可解算（待定位）

| 验证项 | 结果 |
|---|---|
| 编辑器模式 RHI | ✅ `D3D12` / `SM6` |
| identity 加载 | ✅ 不崩溃（对比 5.6.2 → 证实崩溃由 NullRHI 引起，与是否 Prepare 无关） |
| identity 训练 `run_predictive_solver_training` | ✅ 可完成（在副本上执行，未污染生产资产） |
| 延迟至编辑器完全就绪（8s tick）后判定 | ✅ 排除"启动早期资源部分加载"因素 |
| `default_tracker` / `default_solver` / `predictive_solver` | ✅ 均已加载 |
| **`start_pipeline()`** | ❌ **`StartPipelineErrorType.DISABLED`** |

**✅ 已定位（2026-08-27）：根因是素材数据问题，非代码/环境问题。**

工程内现有 CD 引用的媒体序列路径指向**他机**：

```
CD.image_sequences[0].sequence_path = C:/Users/philyuan_M01/Documents/CaptureManager/Media/SSV/.../Video/Video
CD.depth_sequences[0].sequence_path = C:/Users/philyuan_M01/Documents/CaptureManager/Media/SSV/.../Depth/Depth
os.path.exists() == False
```

**因果链**：媒体文件缺失 → 帧数 0 → `ProcessingLimitFrameRange` 上下界相等
→ `CanProcess()==false`（`MetaHumanPerformance.cpp:1699`）→ `start_pipeline` 返回 `DISABLED`。

**已逐一排除**：tracker / solver / predictive solver / RHI（D3D12 + SM6）/ identity Prepare 状态 /
authoring objects（`/MetaHuman/GenericTracker/Chin` 确认存在）/ 加载时机 / 素材种类（3 个素材表现一致）。

**修复验证**（以本机有效 `media_output_root` 重新导入 1 个 take）：

```
IMG   → MediaOut_Test/.../Video/Video   exists=True  (2351 帧)
DEPTH → MediaOut_Test/.../Depth/Depth   exists=True  (1176 帧)
can_process = True        ← 由 False 转为 True，修复确认
```

> ⚠️ **运维结论（易复发，务必注意）**：导入使用的 `media_output_root` 必须是
> **本机有效且长期保留**的路径。否则 CD 资产会变成"孤儿"——资产壳子在项目里、
> 媒体数据不在本机，症状是"能导入、能扫描、列表里看得到，但解算报 `DISABLED`"，
> 极易误判为环境或代码问题。
>
> **建议**：在 `precheck_import` 中增加 **CD 媒体路径存在性校验**（导入后逐 CD 校验
> `image_sequences` / `depth_sequences` 的 `sequence_path` 是否真实存在且非空），
> 把该问题拦截在导入阶段，而不是留到 34 分钟的解算阶段。

#### 5.6.4 顺带发现的生产风险（建议尽快加固）

`pipeline.create_capture_performance` 执行 `unreal.load_asset(identity)`。
在 NullRHI 环境下该调用会**断言崩溃并杀死整个进程**，且 **Python 无法捕获**
→ 批处理全灭、无失败记录、状态文件停留在"运行中"。

现有代码仅 `_check_dependencies` 用 `does_asset_exist`（不加载）规避，**运行时路径无防护**。
建议：解算前增加 identity 可用性守卫，并在 job 配置层面禁止无头跑深度解算。

---

## 六、运行时数据与交付约定

| 数据 | 位置 | 说明 |
|---|---|---|
| 面板生成的 job | `{工程}\Saved\Config\MetaHumanSolver\*.json` | 运行时产物 |
| 媒体转换中间产物 | `{工程}\Saved\MetaHumanSolver\MediaOut\` | 可随时删（重导自动重建） |
| 进度心跳 | `{工程}\Saved\Config\MetaHumanSolver\progress.json` | 监视命令数据源 |
| 导入的 UE 素材 | `/Game/CaptureManager/Imports/{批次}\{take}\` | 生产素材（随项目走） |
| 解算产物（UE） | `/Game/MH_Results\{asset}\Performance|AS|LS_Identity` | 动画资产 |
| FBX 交付（磁盘） | `{fbx_output_dir}\*.fbx`（平铺） | 交付物 |
| 交付清单 | `{fbx_output_dir}\delivery_{时间戳}.{json,txt}` | 每段覆盖写，批末完整 |
| 批量状态 | `{job 同目录}\*_state.json` | 断点续跑（done/failed） |

**命名约定**：FBX 用原始 take 名（`Anims_Flint_Ul_Enter_005.fbx`，剥 `{yyyymmdd}_` 日期前缀、保留 `_vN` 版本后缀）；UE 资产 CD 名 = `CD_{slate}_{take}`。

---

## 七、变更记录

> 自 2026-08 起，以「批量解算 + 交付 ControlRig FACs FBX」为目标。**保数据原则全程坚持**：改动均在 UI / 桥接 / 交互层，未触碰深度阻塞解算核心逻辑。

### 7.1 核心流程修复

1. **插件加载失败修复（RulesError）**：`MetaHumanDepthProcessing` 误放引擎目录改名 `_5.7` 导致 UBT 无法解析 → 移入项目 `Plugins/` 规范目录名，`Installed=false`。
2. **输出路径对接 `fbx_output_dir`**：表单「输出路径」→ 磁盘目录浏览 → job `fbx_output_dir`；`storage_path` 独立固定 `/Game/MH_Results`。
3. **ControlRig FBX 交付可靠性**：FBX 导出失败从「警告继续」改「抛异常标记失败」；`_is_shot_done` 增 FBX 磁盘文件检查（I1 落地）。
4. **移除 `meta_human_class` 强制必填**：FBX 从 Animator 建好的 LS ControlRig Section 导出，不依赖目标角色蓝图。
5. **FBX 关键帧密度修复**：`remove_redundant_keys` 默认删帧导致曲线数 133 vs 手动 222 → 新增 `_export_level_sequence_no_key_reduce`（仅关删帧），曲线数与手动对齐。

### 7.2 UI 与交互

6. 共享进度条 / 共享日志框（修复切 Tab 不更新）；浏览路由枚举化（`EBrowseTarget`）；浏览功能增强（类型筛选/磁盘对话框）；标签文案优化。
7. 新增「分批数量」字段（`limit`）；UI 质感升级（主色蓝、Tab 激活态、品牌区、等宽日志）。
8. 定时器逐段驱动批处理（`run_next`/`count_shots`，段间刷新）；一键预检按钮（`dry_run`）；每段结果清单（`[MHS_RESULT]`）；状态指示灯（`[MHS_STATUS]`）。
9. 菜单项注册（`RegisterStartupCallback` + `ExtendMenu` + 图标）。

### 7.3 进度监控演进

10. **v1 → v2 多 Pass 加权**：修复 Pass 2 期间进度冻结（帧号回绕检测 + 权重带 19/80/1 实测校准）。
11. **防卡死爬行**：帧号静止 >8s 指数趋近，进度条永不静止、单调不回退。
12. **批处理上下文**（`set_batch_context`，跨段持久）+ 控制台两行渲染 + 160 列缓冲。
13. **FreeConsole 修复**：`bOwnedConsole` 只释放自己的控制台（`-stdout` 保护）。
14. **Pass 耗时自动日志 + 回绕诊断**（零成本 atomic 带出累计广播数/回绕触发帧）。

### 7.4 2026-08-21~26 最新变更

15. **表演级进度 `[MHS_DISPLAY]` 协议**：混合模式 `[j/m]` = 组内表演序号/总数，仅驱动面板进度条；`[MHS_PROGRESS]` 保持段级供 C++ 推进（两协议解耦保证正确性）。
16. **舌头/音频勾选框**（默认勾选"不含舌头与音频"= 纯面部交付）。
17. **预检音频降级警告**：缺音频只提示（无 SW_Audio_ 与舌头追踪，不阻断）。
18. **LaunchSegment 诊断时间戳**：日志"提交段 N"带时间戳，可量化批处理推进延迟。
19. **poke 主窗口修复**：心跳线程 `PostMessage(WM_NULL)` 程序化唤醒消息泵，根治"点击面板才推进下一批"（无头 Hwnd 空自动跳过）。
20. **导入异步化**（见 5.3）：媒体转换下放后台线程 + 游戏线程泵消息，根治"导入失焦卡死/需点击"。
21. **解算控制台单行格式**（见 4.3）：固定列宽空格对齐（`数据名 阶段N Cur/Total帧 已耗/剩余`）。
22. **FBX 平铺交付**（见 4.3）：去掉 `{asset}` 子目录层，FBX 直接落 `fbx_output_dir` 根。
23. **交付清单**（见 4.3）：`delivery_{时间戳}.txt + .json` 双份，每段覆盖写、批末完整，含 FBX 实测大小/失败原因。
24. **FBX 交付命名统一**：`_delivery_name`（剥日期前缀）与 `_take_core_name`（剥 CD_+TakeNumber）双链路同源。
25. **批处理串扰防护**：Sequencer 路径校验（`_open_sequence_verified`）+ 禁用项目级 LS 复用（`import_then_solve` 只信固定路径）。
26. **状态机闭环**：`_finish_single_segment` 统一出口，每条 return 路径必经重置（杜绝"按钮死灰"）。
27. **导入并行**（见 5.4）：多 take 并发转换 + 按物理核自适应（24 核 auto=8），`imports_concurrency` 配置；`② 转换诊断` 日志探测核占用校准 `kCoresPerTake`。
28. **预检并发报告**：`precheck_import` 调用 C++ `GetRecommendedImportConcurrency` 输出并发配置校验/实际并发/take数浪费/磁盘提示（见 5.4）。
29. **面板混合段导入并发**（见 5.4）：导入段合并为批次导入段，C++ 新增 `ImportTakes` 显式 take 列表入口（并发调度抽为 `ImportTakesBlocking_Concurrent` 共享），幂等/exclude 按 take 粒度精确生效。
30. **导入任务进度可视化**（见 5.4）：小窗显示 `x/y take · 并发n · 已耗 · 剩余` + 活跃 take 名，根治批次导入黑盒（C++ 调度逐 take 更新，`UpdateTaskProgress` 跨模块导出）。

### 7.5 已修复的审核缺陷（对应原第一性原理审核报告）

| 原缺陷 | 状态 |
|---|---|
| P0-1 `run_next` 无幂等过滤 | ✅ 已修复（run_next 现含 `_is_shot_done` 检查） |
| P0-2 非阻塞 delegate 累积绑定 | ⚠️ 深度/视频走阻塞或单次回调，音频非阻塞路径未做 clear（低风险） |
| P1-3 段内取消无效 | ⚠️ 保持阻塞保数据；进度监控解决"无反馈"，取消仍段间生效 |
| P1-4 `TotalShots` 隐式依赖 + 首段失败提前终止 | ✅ 已修复（失败也发 progress，首段失败仍推进） |
| P1-5 预检不覆盖 LS 存在性 | ✅ 已修复（dry_run 逐段校验 LS 交付路径） |
| P1-6 手工拼 JSON 转义不完整 | ⚠️ `JsonEscape` 覆盖主要字符；结构化生成列为待办 |
| P2-7 素材集合变化索引错位 | ⚠️ 已知边界（批次中途不动素材即可） |
| P2-8 调度分支重复 | ✅ 已收敛（`run_next`/`_start_next` 共享逻辑） |
| P2-9 FBX 固定取 control_rigs[0] | ✅ 已改进（按名优先 Face 轨） |

---

## 八、已知问题与路线图

### 8.1 已知边界

- 深度解算需 RTX 独显（SM6）；集显自动回退 D3D11 报 `Unsupported RHI`。
- 30-50s 短数据非阻塞解算未启用（保数据）；深度 `blocking=true` 期间 UI 冻结、取消段间生效（设计取舍）。
- **无头（`-run=pythonscript`）完全不可用于深度解算**（2026-08-27 实测，见 §5.6）：commandlet 强制 NullRHI（`-dx12` / `-d3d12` 均无效），触碰 identity 即 `check(DefaultSolver->PredictiveSolver)` 断言崩溃，**进程直接死亡且 Python 无法捕获**。此前"仅 LS 重建不可用"的记载偏乐观，实际情况更严重。
- 菜单/工具栏图标注册受加载时机影响，属观感问题。
- FBX 平铺后同名 take（不同批次核心名相同）会互相覆盖（当前 take 名全局唯一，风险极低）。

### 8.2 路线图（待办）

1. 阶段 B：pm identity 制作（`/Game/MetaHumans/Ares/Models/PM_ID_PLACEHOLDER`）
2. 全局进度累计 `[x/总]`（当前为组内表演级 `[j/m]`）
3. 多实例开关（16 核+机器分片并行，机制已验证未产品化）
4. 阶段 A 转换与阶段 C 解算流水线化
5. 结构化 JSON 生成（替换手工拼接，消除配置漂移）
6. 日志文件（独立实时落盘，Python 引擎级）

---

## 九、附录

### 9.1 关键路径

| 项 | 路径 |
|---|---|
| 开发工作区 | `e:\Mobu_maya\MetaSol` |
| 同步源（C++） | `e:\Mobu_maya\MetaSol\deploy\MetaHumanSolver\` |
| 同步源（Python） | `e:\Mobu_maya\MetaSol\deploy\Python\MetaHumanSolverEngine\` |
| 当前宿主工程 | `E:\Mobu_maya\SSV_Test\SSV` |
| 引擎 | `D:\UE_5.7`（5.7.4） |
| 编译命令 | `& "D:\UE_5.7\Engine\Build\BatchFiles\Build.bat" SSVEditor Win64 Development -project="E:\Mobu_maya\SSV_Test\SSV\SSV.uproject" -WaitMutex` |

### 9.2 无头生产入口

```powershell
# 阶段 A：批次导入（import_only）
& "D:\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "E:\Mobu_maya\SSV_Test\SSV\SSV.uproject" -run=pythonscript -script="e:\Mobu_maya\MetaSol\tools\run_stage_a.py" -stdout
```
