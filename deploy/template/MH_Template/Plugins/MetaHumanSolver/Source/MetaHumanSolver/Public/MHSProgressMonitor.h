// MHSProgressMonitor.h
// 解算进度监控（设计文档《MetaHumanSolver_解算进度监控设计文档.md》落地）。
//
// 第一性原理：深度解算同步阻塞 -> 游戏线程单帧无限延长 -> Slate UI 冻结。
// 要让用户知道"还在跑"，必须在游戏线程之外开辟实时信号通道：
//   1) 逐帧数据源：引擎 OnFrameProcessed 广播（游戏线程调用栈内触发，阻塞期逐帧必然到达）；
//   2) 心跳线程（FRunnable）：每 500ms 读 atomic -> 写 progress.json + 刷新原生小窗；
//   3) 原生 Win32 小窗（独立线程 + 自有消息循环）：完全不走 Slate，与游戏线程解耦。
//
// 线程纪律（严格遵守，违反即竞态）：
//   - 游戏线程：Begin / SetStage / End / ForceStop / 逐帧回调（回调内只做 atomic store
//     + 首帧一次性读 Range 修正总帧数——均在游戏线程，UObject 访问安全）；
//   - 心跳线程：只读 atomic / State.ProgressFilePath / State.AssetName（Begin 写入后
//     线程才启动，happens-before 成立），绝不触碰 UObject；
//   - 窗口线程：窗口的创建 / 消息循环 / 销毁必须同一线程完成（设计文档 7.5）。
//
// 帧号语义（MetaHumanPerformance.cpp:1990/1994 核实）：
//   OnFrameProcessed(Frame) 广播的是 sequencer 帧号，减去 ProcessingLimitFrameRange
//   下界才是 0-based 动画帧号。
//
// 多 Pass 语义（MetaHumanPerformance.cpp:2079-2103 ProcessComplete 核实）：
//   深度解算按 PipelineStage 分段执行——Standard/AdditionalTweakers 走 跟踪解算 ->
//   全局最终解算 -> 滤波 共 3 段；Preview 跳过后处理共 2 段；单目视频单段。
//   每段（Pass）内广播帧号严格 0->N-1 递增（FaceTrackerPostProcessingNode.cpp:76/120
//   FrameNumber 从 0 递增核实），Pass 切换时回绕到 0。
//   => 回调里"收到比当前水位更小的归一化帧号 = 进入新 Pass"，检测不依赖计时猜测。
//   进度按 Pass 权重带加权（.cpp GPassBandStart），叠加防卡死爬行通道（设计文档 11.2）。

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MetaHumanPerformance.h"

#include <atomic>
#include <mutex>
#include <thread>

#include "MHSProgressMonitor.generated.h"

// ── 阶段定义（Python 侧 set_stage 传 int，映射见 .cpp 的 StageNames） ──
enum class EMHSProgressStage : int32
{
    Solve = 0,      // 深度解算（有逐帧百分比）
    ExportAS = 1,   // 导出 AnimSequence（无百分比，仅阶段名 + 计时）
    ExportLS = 2,   // 导出 LevelSequence
    ExportFBX = 3,  // 导出 ControlRig FBX
    Import = 4,     // 素材导入（无帧级数据，阶段名 + 计时 + 批次位置）
};

// ── 线程安全共享状态：游戏线程写（atomic），心跳线程读 ──
struct FMHSProgressState
{
    std::atomic<int32>  CurrentFrame{0};     // 当前 Pass 内已处理帧（0-based，Pass 内单调递增，游戏线程写）
    std::atomic<int32>  TotalFrames{0};      // 总帧数（Begin / 首帧回调动态修正）
    std::atomic<int32>  FrameLowerBound{0};  // sequencer 帧号下界（换算 0-based 用）
    std::atomic<double> StartTime{0.0};      // 解算开始时刻（FPlatformTime::Seconds）
    std::atomic<bool>   bActive{false};      // 监控激活中
    std::atomic<bool>   bShouldStop{false};  // 通知心跳线程退出
    std::atomic<int32>  Stage{0};            // EMHSProgressStage
    // UE 主窗口句柄（心跳线程 poke 用）：解算阻塞游戏线程 34 分钟后 Windows 将窗口打入
    // 挂起状态、主循环陷入消息等待停滞——组间推进的 AsyncTask/timer 排队等 tick，
    // 需要用户点击（输入消息）才唤醒。心跳线程每 500ms PostMessage(WM_NULL) 程序化
    // 唤醒消息泵（等价于持续轻点窗口，无副作用），根治"点击才推进"。
    std::atomic<void*>  MainWindowHwnd{nullptr};

    // ── 多 Pass 加权进度（设计文档 11.2）：游戏线程写（回绕检测），心跳线程读 ──
    std::atomic<int32>  PassIndex{0};       // 当前 Pass（0-based；帧号回绕时递增）
    std::atomic<int32>  PassCount{1};       // 预期 Pass 数（Begin 写入：深度全量=3 / Preview=2 / 单目=1）

    // ── 回绕诊断（零成本 atomic，心跳日志带出；用于校验各 Stage 帧广播行为） ──
    std::atomic<int32>  BroadcastCount{0};       // 累计帧广播数（回调 fetch_add）
    std::atomic<int32>  LastWrapTriggerFrame{-1}; // 最近一次回绕触发帧号（-1=尚未回绕）
    std::atomic<double> LastWrapTime{0.0};        // 最近一次回绕时刻（相对 StartTime 秒）

    // ── 批处理上下文（跨段持久，不随 Begin/End 重置；batch_runner 每段刷新、批末清空）──
    // 全 atomic 数值（无字符串）：段名复用 AssetName（Begin 时 happens-before 写入）。
    std::atomic<int32>  BatchIndex{0};          // 批次内位置（1-based；0 = 无批次上下文）
    std::atomic<int32>  BatchTotal{0};          // 批次总段数（0 = 无批次上下文）
    std::atomic<double> BatchEtaSeconds{-1.0};  // 批次剩余预估秒（Python 侧基于已完段耗时外推；-1 = 未知）

    // ── 任务进度（批次导入段内，C++ 并发调度逐 take 更新）：TaskTotal>0 时小窗显示任务进度 ──
    std::atomic<int32>  TaskCurrent{0};        // 已完成 take 数（C++ 调度 Finalize 完成后递增）
    std::atomic<int32>  TaskTotal{0};          // 批次 take 总数（0 = 无任务进度；SetTaskContext 写入）
    std::atomic<int32>  TaskConcurrency{0};    // 并发转换数（SetTaskContext 写入，小窗显示"并发N"）
    std::atomic<double> TaskEtaSeconds{-1.0};  // 任务剩余预估秒（C++ 调度外推；-1 = 未知）
    mutable std::mutex  TaskActiveMutex;       // 保护 TaskActiveNames（游戏线程写 / 心跳线程读）
    FString             TaskActiveNames;       // 正在并发转换的 take 名（逗号分隔快照）

    // 只在 Begin（游戏线程）写一次、心跳/窗口线程只读：
    // 线程启动与字符串写入构成 happens-before（std::thread / FRunnableThread::Create）。
    FString AssetName;
    FString ProgressFilePath;                // <Saved>/Config/MetaHumanSolver/progress.json
};

// ── 原生小窗显示数据（心跳线程写 -> PostMessage -> 窗口线程读，互斥锁保护） ──
struct FMHSWindowDisplayData
{
    double Percent = 0.0;   // 0~99（过程钳制），100 仅终态
    int32 Cur = 0;
    int32 Total = 0;
    double Elapsed = 0.0;   // 秒
    double Eta = -1.0;      // 预计剩余秒，-1 = 未知
    int32 Stage = 0;
    bool bFinal = false;    // 终态（success/failed），true 时 Percent 为 0/100
    bool bSuccess = false;

    // ── 多 Pass / 批次上下文快照（Worker 线程填充，渲染端零逻辑） ──
    int32 Pass = 0;         // 0-based
    int32 PassCount = 1;
    int32 BatchIndex = 0;   // 1-based；0 = 无批次上下文
    int32 BatchTotal = 0;
    double BatchEta = -1.0; // 批次剩余预估秒，-1 = 未知
    FString Label;          // 段名（控制台行 1 前缀）
    FString PassSummary;    // 预拼好的 Pass 摘要（"Pass1 03:12 ✓ · Pass2 进行中"）

    // ── 任务进度（批次导入段内）──
    int32 TaskCurrent = 0;  // 已完成 take 数
    int32 TaskTotal = 0;    // 批次 take 总数（0 = 无任务进度）
    int32 TaskConcurrency = 0;
    double TaskEta = -1.0;
    FString TaskActiveNames; // 正在并发的 take 名快照
};

// ── 原生 Win32 置顶小窗（独立线程 + 自有消息循环，不走 Slate） ──
// 非 Windows 平台退化为空实现（只落 progress.json，不弹窗）。
#if PLATFORM_WINDOWS
class FMHSProgressWindow
{
public:
    // 启动窗口线程并创建小窗（游戏线程调用，Launch 后立即返回）。
    // InRefHwnd：UE 主窗口句柄（小窗跟随其所在显示器右下角；空则退回鼠标所在屏）。
    void Launch(const FString& InAssetName, void* InRefHwnd);
    // 通知窗口线程退出消息循环并 join（必须先停心跳线程再调用，避免 PostMessage 竞态）。
    void Shutdown();
    // 心跳线程调用：更新显示数据并触发重绘（PostMessage，非阻塞，绝不直接画）。
    void PostUpdate(const FMHSWindowDisplayData& InData);
    // 终态显示（成功 100% / 失败保持当前值），由 End 在停心跳后调用一次。
    void PostFinal(const FMHSWindowDisplayData& InData);

    // ── 以下三个仅供窗口过程（WndProc，窗口线程）内部调用，勿在别处使用 ──
    void SyncTitle(struct HWND__* Hwnd);   // WM_MHS_UPDATE：同步标题栏文本
    bool IsFinalSuccess() const;           // WM_CLOSE：终态是否成功（决定停留时长）
    bool IsFinal() const;                  // WM_CLOSE：是否终态（运行中点 X = 最小化而非销毁）
    void Paint(struct HWND__* Hwnd);       // WM_PAINT 自绘（双缓冲防闪）

private:
    void WindowThreadMain();          // 窗口线程主体：分流（本机定案=控制台模式）
    void WindowThreadMainGraphical(); // 图形小窗实现（本机停用；环境正常的机器可切回）
    void EnterConsoleFallback();      // 控制台模式：conhost 渲染，与 DWM/TSF 解耦
    void WriteConsoleLine(const FMHSWindowDisplayData& D); // 控制台模式：标题+首行重写
    void* GetHwnd() const { return Hwnd.load(); }

    std::thread Thread;
    std::atomic<void*> Hwnd{nullptr}; // HWND 以 void* 存储，避免头文件引入平台头
    void* RefHwnd = nullptr;          // UE 主窗口句柄（Launch 前写一次，窗口线程启动后只读）
    std::atomic<bool> bConsoleMode{false}; // true=控制台兜底模式（窗口线程决定，心跳线程读）
    std::atomic<bool> bOwnedConsole{false}; // true=控制台由我们 AllocConsole 创建（Shutdown 只 Free 自己的，
                                            // 修复：-stdout 无头模式下误摘 UE 自带 stdout 控制台）
    mutable std::mutex DataLock;      // 保护 DisplayData（const 读取亦可加锁）
    FMHSWindowDisplayData DisplayData;
};
#else
class FMHSProgressWindow
{
public:
    void Launch(const FString&, void*) {}
    void Shutdown() {}
    void PostUpdate(const FMHSWindowDisplayData&) {}
    void PostFinal(const FMHSWindowDisplayData&) {}
};
#endif

// ── 心跳线程（FRunnable）：每 500ms 读 atomic -> 算多 Pass 加权百分比/ETA -> 写 json + 刷小窗 ──
class FMHSProgressWorker : public FRunnable
{
public:
    FMHSProgressWorker(FMHSProgressState& InState, FMHSProgressWindow& InWindow);

    virtual uint32 Run() override;

private:
    void TickOnce();                       // 单次心跳：计算 + 写 json + PostUpdate
    void WriteJson(const FMHSWindowDisplayData& Data) const;

public:
    // 供 Monitor::End 在停心跳后、销毁 Worker 前快照（终态行 2 Pass 摘要）
    const TArray<double>& GetCompletedPassDurations() const { return CompletedPassDurations; }
    double GetCurrentPassElapsed() const { return FPlatformTime::Seconds() - PassStartTime; }

private:
    FMHSProgressState& State;
    FMHSProgressWindow& Window;
    double EmaEta = -1.0;                  // ETA 指数滑动平均（Worker 线程独占，无锁）

    // ── 多 Pass / 爬行 / 单调显示（均 Worker 线程独占，无锁） ──
    int32 LastSeenPass = 0;                // 上 tick 观测到的 Pass（变化 -> 打耗时日志 + 计时复位）
    double PassStartTime = 0.0;            // 当前 Pass 首次被观测到的时刻
    TArray<double> CompletedPassDurations; // 已完成 Pass 耗时（首跑即得权重校准数据）
    int32 LastTickCur = -1;                // 上 tick 帧号（静止计时用）
    double StallSeconds = 0.0;             // 帧号连续静止秒数（> 阈值 -> 启动爬行）
    bool bCreeping = false;                // 爬行通道激活中
    double CreepPercent = 0.0;             // 爬行通道百分比（单调，显示取 max(帧驱动, 爬行)）
    double DisplayHighWater = 0.0;         // 显示值高水位（跨 Pass 单调不回退）
};

// ── 监控单例：Python 三个入口的真正实现 ──
// METAHUMANSOLVER_API：跨模块导出（MetaHumanSolverTakeIngest 链接调用 Get/UpdateTaskProgress）。
class METAHUMANSOLVER_API FMHSProgressMonitor
{
public:
    static FMHSProgressMonitor& Get();

    // 解算开始前调用（游戏线程）：注册逐帧回调 + 启动心跳线程 + 弹出小窗。
    // InExpectedPasses：解算 Pass 数（深度全量=3 / Preview=2 / 单目视频=1），按此选权重带。
    void Begin(UMetaHumanPerformance* InPerf, const FString& InAssetName, int32 InExpectedPasses = 1);
    // 无 perf 的任务级监控（素材导入段）：跳过帧范围/逐帧回调，仅窗口 + 心跳（阶段名+计时+批次位置）。
    void BeginTask(const FString& InTaskName, int32 InStage = static_cast<int32>(EMHSProgressStage::Import));
    // 阶段切换（游戏线程）：Solve -> ExportAS -> ExportLS -> ExportFBX。
    void SetStage(int32 NewStage);
    // 批次上下文（游戏线程，可任意时刻调用）：跨段持久，不随 Begin/End 重置。
    // batch_runner 每段启动时刷新（index 从 1 计）、批末清空（index/total <= 0）。
    void SetBatchContext(int32 InIndex, int32 InTotal, double InEtaSeconds);
    // 单段结束（游戏线程）：停心跳 -> 关小窗 -> 移除回调 -> 写终态 json。
    // bSuccess=true 时终态 percent 强制 100（100% 由这里硬触发，绝不依赖帧数计算）。
    void End(bool bSuccess);
    // ShutdownModule 兜底清理（游戏线程）。
    void ForceStop();

    // ── 任务进度（批次导入段内，C++ 并发调度调用；不要求监控激活，激活时实时刷小窗）──
    // 设置任务上下文（Python 在批次导入段 begin 后调用）：激活任务进度显示。
    void SetTaskContext(int32 InTotal, int32 InConcurrency);
    // 更新任务进度（C++ 调度每完成一个 take 调用；游戏线程）。InActiveNames 为逗号分隔的正在转换 take 名。
    void UpdateTaskProgress(int32 InCurrent, double InEtaSeconds, const FString& InActiveNames);

private:
    FMHSProgressMonitor() = default;
    ~FMHSProgressMonitor() = default;
    FMHSProgressMonitor(const FMHSProgressMonitor&) = delete;
    FMHSProgressMonitor& operator=(const FMHSProgressMonitor&) = delete;

    void WriteFinalJson(bool bSuccess);

    FMHSProgressState State;
    FMHSProgressWindow Window;
    FRunnableThread* WorkerThread = nullptr;
    FMHSProgressWorker* Worker = nullptr;
    TWeakObjectPtr<UMetaHumanPerformance> WeakPerf;
    FDelegateHandle FrameHandle;
};

// ── Python 调用入口（BlueprintFunctionLibrary 薄封装） ──
// Python 侧调用名（UE 自动转 snake_case）：
//   unreal.MetaHumanSolverProgressLibrary.begin_progress(perf, asset_name, expected_passes)
//   unreal.MetaHumanSolverProgressLibrary.set_stage(stage)
//   unreal.MetaHumanSolverProgressLibrary.set_batch_context(index, total, eta_seconds)
//   unreal.MetaHumanSolverProgressLibrary.end_progress(success)
UCLASS()
class UMetaHumanSolverProgressLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // 开始监控一段解算。Perf 必须有效；失败只打日志，绝不抛出（进度监控绝不影响解算主流程）。
    // InExpectedPasses：解算 Pass 数（深度全量=3 / Preview=2 / 单目视频=1，0/负值按 1 处理）。
    UFUNCTION(BlueprintCallable, Category = "MetaHumanSolver|Progress")
    static void BeginProgress(UMetaHumanPerformance* InPerformance, const FString& InAssetName, int32 InExpectedPasses = 1);

    // 开始监控一段无帧级数据的任务（素材导入）：阶段名 + 计时 + 批次 [i/N]（无百分比）。
    // InStage：默认 4 = Import（见 EMHSProgressStage）。
    UFUNCTION(BlueprintCallable, Category = "MetaHumanSolver|Progress")
    static void BeginTaskProgress(const FString& InTaskName, int32 InStage = 4);

    // 阶段切换：0=Solve 1=ExportAS 2=ExportLS 3=ExportFBX（见 EMHSProgressStage）。
    UFUNCTION(BlueprintCallable, Category = "MetaHumanSolver|Progress")
    static void SetStage(int32 InStage);

    // 批次上下文：index 从 1 计；index/total <= 0 表示清空（单段模式批末/无批次）。
    // eta_seconds：批次剩余预估秒（-1 = 未知）。跨段持久，不随 begin/end 重置。
    UFUNCTION(BlueprintCallable, Category = "MetaHumanSolver|Progress")
    static void SetBatchContext(int32 InIndex, int32 InTotal, double InBatchEtaSeconds = -1.0);

    // 任务进度上下文（批次导入段）：total=批次 take 总数，concurrency=并发转换数。
    // 设置后小窗显示"x/y take · 并发n · 剩余"；total<=0 表示清除任务进度。
    UFUNCTION(BlueprintCallable, Category = "MetaHumanSolver|Progress")
    static void SetTaskContext(int32 InTotal, int32 InConcurrency);

    // 单段结束：bSuccess=true 时小窗/json 终态强制 100%。
    UFUNCTION(BlueprintCallable, Category = "MetaHumanSolver|Progress")
    static void EndProgress(bool bSuccess);
};
