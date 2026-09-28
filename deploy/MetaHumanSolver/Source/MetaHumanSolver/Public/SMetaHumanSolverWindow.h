// SMetaHumanSolverWindow.h
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Styling/SlateTypes.h"
#include "Misc/OutputDevice.h"

class SMultiLineEditableTextBox;
class SProgressBar;

// 监听 [MHS_*] 日志的设备（定义在 .cpp）
class FMHSLogDevice;

// 浏览目标：用枚举标识要回填哪个输入框（替代易失效的中文字符串路由）
enum class EBrowseTarget
{
    DepthOutput,      // 磁盘 FBX 输出目录
    VideoInput,
    VideoIdentity,
    VideoOutput,
    AudioInput,
    AudioOutput,
    RomSource,        // 阶段 A：ROM take 文件夹（磁盘）
};

// 解算任务行（阶段 B 任务列表：身份资产 + 表演源文件夹）
struct FSolveTaskRow
{
    TSharedPtr<class SEditableTextBox> IdentityEdit;   // 身份资产路径（/Game/...）
    TSharedPtr<class SEditableTextBox> SourceEdit;      // 表演源文件夹（磁盘）
};

class SMetaHumanSolverWindow : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMetaHumanSolverWindow) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SMetaHumanSolverWindow() override;

private:
    // ── 配色（参考 Mobu/Maya：外层深灰 + 黑色面板 + 深蓝文字 + 绿色强调） ──
    static inline const FLinearColor ColorBg        = FLinearColor(0.137f, 0.137f, 0.137f, 1.0f);  // 外层 #232323
    static inline const FLinearColor ColorPanel     = FLinearColor(0.000f, 0.000f, 0.000f, 1.0f);  // 面板纯黑
    static inline const FLinearColor ColorSurface   = FLinearColor(0.055f, 0.055f, 0.063f, 1.0f);  // 输入/日志 #0E0E10
    static inline const FLinearColor ColorText      = FLinearColor(0.878f, 0.878f, 0.878f, 1.0f);  // 主文字
    static inline const FLinearColor ColorTextDim   = FLinearColor(0.690f, 0.690f, 0.690f, 1.0f);  // 输入文字浅灰
    static inline const FLinearColor ColorAccent    = FLinearColor(0.102f, 0.310f, 0.620f, 1.0f);  // 深蓝 #1A4FA0
    static inline const FLinearColor ColorAccentDark= FLinearColor(0.078f, 0.231f, 0.478f, 1.0f);  // 深蓝 hover
    static inline const FLinearColor ColorSuccess   = FLinearColor::FromSRGBColor(FColor(76, 175, 80, 255));  // 标准绿 #4CAF50
    static inline const FLinearColor ColorSuccessDark=FLinearColor::FromSRGBColor(FColor(55, 138, 62, 255));  // 绿 hover #378A3E
    static inline const FLinearColor ColorTabIdle   = FLinearColor(0.176f, 0.176f, 0.188f, 1.0f);  // Tab 闲置 #2D2D30
    static inline const FLinearColor ColorStatus    = FLinearColor(0.690f, 0.690f, 0.690f, 1.0f);  // 状态浅灰
    static inline const FLinearColor ColorWhite     = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    static inline const FLinearColor ColorLogError  = FLinearColor::FromSRGBColor(FColor(230, 80, 80, 255));   // 日志红
    static inline const FLinearColor ColorLogWarn   = FLinearColor::FromSRGBColor(FColor(240, 180, 80, 255));   // 日志黄
    static inline const FLinearColor ColorLogOk     = FLinearColor::FromSRGBColor(FColor(120, 200, 120, 255));  // 日志绿

    // ── 控件引用 / 状态 ──
    TSharedPtr<class SWidgetSwitcher> ContentSwitcher;
    TArray<TSharedPtr<class SButton>> TabButtons;
    TArray<TSharedPtr<class SButton>> StartButtons;   // 三个 Tab 的开始按钮（解算中禁用）
    int32 ActiveTab = 0;
    bool bProcessing = false;

    // ── 表单输入框引用（浏览回填 / 生成 job.json） ──
    TSharedPtr<class SEditableTextBox> DepthOutputEdit;   // 深度：全局 FBX 输出目录
    // 导入目标路径（UE 资产目录，/Game/...）：素材导入落位 {路径}/{批次名}/{take}，
    // 可选化避免不同批次重名 take 互相覆盖；空 = 默认 /Game/CaptureManager/Imports
    TSharedPtr<class SEditableTextBox> ImportRootEdit;
    TSharedPtr<class SEditableTextBox> VideoInputEdit, VideoIdentityEdit, VideoOutputEdit;
    TSharedPtr<class SEditableTextBox> AudioInputEdit, AudioOutputEdit;
    // 分批数量（视频/音频 Tab 独立，空=全部处理）
    TSharedPtr<class SEditableTextBox> VideoLimitEdit, AudioLimitEdit;
    // 阶段 A：ROM take 文件夹（磁盘）
    TSharedPtr<class SEditableTextBox> RomSourceEdit;
    // 阶段 B：解算任务列表（动态行：身份资产 + 表演源文件夹）
    TArray<TSharedPtr<struct FSolveTaskRow>> SolveTaskRows;
    TSharedPtr<class SVerticalBox> TaskListBox;        // 任务行容器（增删行用）
    // 高级选项（默认折叠）：排除素材（每行一个，兼容 CD 资产名 / take 目录名）
    TSharedPtr<class SMultiLineEditableTextBox> ExcludeEdit;
    // 高级选项：不含舌头与音频（纯面部动画交付：skip_tongue_solve + 不导 LS 音频轨），默认勾选
    TSharedPtr<class SCheckBox> NoTongueAudioCheck;

    // ── Auto Tab（Stream 流式模式 · S1 重构；命名沿用 Auto*，S2 统一改 Stream*）──
    // 监听配置
    TSharedPtr<class SEditableTextBox> AutoInboxEdit;
    TSharedPtr<class SEditableTextBox> AutoIdImportEdit;   // 身份导入（UE 路径：ROM/ID 素材落位）        // 热文件夹（磁盘，上环节同步目标）
    TSharedPtr<class SEditableTextBox> AutoIdAssetDirEdit;   // ID 资产目录（UE，Semi/Manual 模式人工放置处）
    TSharedPtr<class SEditableTextBox> AutoIdAssetEdit;      // 身份资产（留空=自动取目录内第一个；多身份时必须显式指定）
    TSharedPtr<class SEditableTextBox> AutoImportRootEdit;   // 导入目标路径（UE，默认 /Game/CaptureManager/Auto）
    TSharedPtr<class SEditableTextBox> AutoOutputEdit;       // FBX 输出路径（磁盘）
    // 流程模式三档：0=Auto(置灰预留) 1=Semi(默认) 2=Manual；stage_automation 位图见 job 生成
    int32 StreamMode = 1;
    TSharedPtr<class SButton> StreamModeButtons[3];
    // 控制与状态
    bool bAutoListening = false;                            // 监听/流程进行中（S3 接 watcher）
    TSharedPtr<class SButton> AutoStartStopButton;
    TSharedPtr<class STextBlock> AutoStartStopLabel;
    TSharedPtr<class STextBlock> AutoActiveIdText;          // 活跃 ID 显示
    TSharedPtr<class STextBlock> AutoFailText;              // 失败计数（与活跃 ID 同行）
    TArray<TSharedPtr<class STextBlock>> QueueStatValues;   // 四步卡数值（ID/Ingest/Solve/Export 计数）
    TSharedPtr<class STextBlock> AutoEtaText;               // 预计追平时刻
    TSharedPtr<class SMultiLineEditableTextBox> AutoEventLog;// 事件流
    // Manual 模式：严格顺序步骤按钮（仅当前步可点；完成进位；不可跳过）
    TArray<TSharedPtr<class SButton>> StageActionButtons;   // 4 个（① ID ② Ingest ③ Solve ④ Export）
    int32 ManualStageProgress = 0;                          // 当前应执行步骤 0-3
    TSharedPtr<class SWidget> ManualButtonRow;              // 步骤按钮行（仅 Manual 可见）
    TSharedPtr<class SButton> ConfirmStageButton;           // Manual [确认完成]（真实确认后才进位）

    // ── 主界面（视觉重排后）──
    TSharedPtr<class SImage> StreamStatusDot;               // 状态灯（未启动灰 / 运行中蓝 / 异常红）
    TSharedPtr<class STextBlock> StreamStatusText;          // 状态词
    TSharedPtr<class STextBlock> StreamBigText;             // 大数字（今日交付 / 已同步）
    TSharedPtr<class STextBlock> StreamCurrentText;         // 当前任务行（含"截至"时间戳）
    int32 StreamFailCount = 0;                              // 失败条数（>0 时才显示异常行）
    TSharedPtr<class SButton> PrecheckButton;               // [预检]（独立动作，继承自深度Tab）
    TSharedPtr<class SVerticalBox> GroupListBox;            // 身份分组区容器（目录 → 身份，显式绑定）
    TArray<TPair<FString, FString>> IdentityGroups;         // (目录, 身份资产路径)
    TSharedPtr<class SMultiLineEditableTextBox> ManifestEdit;// 清单面板（对账表）
    TArray<FString> ManifestLines;                          // 清单行（"状态|名称|时间|组"，随 S2 灌入）
    int32 ManifestFilter = 0;                               // 清单筛选 0=全部 1=失败 2=待处理 3=未交付
    void SetManifestFilter(int32 Filter);
    void RefreshManifestView();
    // ID 卡（流水线第一步）点击展开制作区——规范：ID 为 0 时卡片可点
    TSharedPtr<class SWidget> IdMakeBox;
    void OnIdCardClicked();
    TSharedPtr<class SEditableTextBox> RomDirEdit;  // ROM 素材目录（ID 制作区内，磁盘路径）
    bool bSemiIdentityReady = false;                // Semi：身份已就绪
    FString SemiIdentityPath;                       // Semi：就绪的 identity 资产路径
    // ID 制作三步（均真实执行，不做假动作）
    void OnImportIdFootage();                       // ① 导入项目：ROM 素材真的导入 UE
    void OnOpenIdentityEditor();                    // ② 打开编辑器：真的打开/创建 Identity 编辑器
    FString FindIdentityInDir(const FString& Dir) const;  // 在 ID 资产目录下查找 MetaHumanIdentity
    bool IsIdentityAssetAt(const FString& AssetPath) const;  // 校验指定路径确为 MetaHumanIdentity 资产
    void OnClearBinding();                                   // [清除绑定]：解除选中组的身份绑定（空目标=清除根组）
    bool bBindGroupUserPicked = false;                       // 绑定目标是否被用户手动选过（未选则跟随自动推断）

    // ── 身份资产精确列表（[▾ 选择]）──
    // 曾靠"资产目录内最新"猜测 → 多身份场景必错（本工程实测 4 个身份）。
    // size_kb 是成型判据的提示：~64MB=已 conform；<10MB 多半是空身份。
    struct FIdentityAssetInfo
    {
        FString Name;
        FString Path;
        FString Dir;
        FString Modified;      // 显示文案 MM-DD HH:MM
        double  ModifiedTs = 0.0;
        int64   SizeKb = 0;
    };
    TArray<FIdentityAssetInfo> CollectIdentityAssets() const;   // 扫 /Game 全量 MetaHumanIdentity
    TSharedPtr<class SComboButton> IdAssetPicker;
    // 事件结构化（支持倒序 + 筛选）
    struct FStreamEvent { FString Time; FString Level; FString Text; };
    TArray<FStreamEvent> StreamEvents;
    int32 EventFilter = 0;                                  // 0=全部 1=失败 2=待处理
    // 组队列（任务列表每组 = 一个混合段序列 job；组间由 [MHS_DONE] 推进）
    TArray<TPair<FString, FString>> PendingGroups;     // (identity, source_dir)
    int32 CurrentGroupIndex = -1;

    // ── 进度 / 日志 / 结果清单 / 状态灯 ──
    TSharedPtr<SProgressBar> ProgressBar;
    TSharedPtr<class STextBlock> ProgressLabel;
    TSharedPtr<SMultiLineEditableTextBox> LogEdit;
    TSharedPtr<SMultiLineEditableTextBox> ResultListEdit;   // 每段结果清单
    TSharedPtr<class SImage> StatusDot;                     // 状态指示灯（运行/空闲/出错）
    TSharedPtr<FMHSLogDevice> LogDevice;

    // ── 段间刷新（定时器逐段驱动批处理） ──
    struct FTimerHandle BatchTimer;   // 逐段提交用
    int32 TotalShots = 0;             // 待处理总段数
    int32 CurrentShot = 0;            // 当前已提交的段索引
    bool bCancelRequested = false;    // 用户已请求取消（当前段解算完即停）
    FText ActiveTabName;              // 当前解算的 Tab（用于日志）
    FString BatchJobPath;             // job.json 路径

    // ── 样式（生命周期 = 窗口，供 SetButtonStyle 引用） ──
    FButtonStyle StyleTabActive;
    FButtonStyle StyleTabIdle;
    FButtonStyle StylePrimary;
    FButtonStyle StyleSecondary;

    void InitStyles();
    void SetActiveTab(int32 Index);

    // ── 构建 ──
    TSharedRef<SWidget> BuildBrandHeader();
    TSharedRef<SWidget> BuildDepthPanel();
    TSharedRef<SWidget> BuildVideoPanel();
    TSharedRef<SWidget> BuildAudioPanel();
    // Auto Tab（Stream 流式模式 · S1）：模式选择 + 监听配置 + 四步流水线 + Manual 步骤 + 事件流
    TSharedRef<SWidget> BuildAutoPanel();
    // ── 主界面分区（视觉重排：状态在上、当前在中心、配置折叠）──
    TSharedRef<SWidget> BuildStreamHeader();        // 状态灯 + 状态词 + [预检] [停止/开始]
    TSharedRef<SWidget> BuildStreamBigNumber();     // 大数字 + 标签（今日交付 · 已同步）
    TSharedRef<SWidget> BuildStageRow();            // 四步卡（统一累计通过数）
    TSharedRef<SWidget> BuildIdentityGroups();      // 身份分组区（显式绑定，自深度Tab任务行）
    TSharedRef<SWidget> BuildConfigPanel();         // 配置面板（分组 + 校验 + 保存默认）
    TSharedRef<SWidget> BuildEventPanel();          // 事件面板（倒序 + 筛选）
    TSharedRef<SWidget> BuildManifestPanel();       // 清单面板（对账表）
    void OnPrecheck();                              // [预检] 独立动作
    void PushStreamEvent(const FString& Level, const FString& Text);  // 结构化事件（倒序存）
    void RefreshEventView();                        // 按筛选重绘事件视图
    void SetEventFilter(int32 Filter);
    void OnConfirmStage();                          // Manual [确认完成]（真实确认后进位）
    void OnOpenOutputDir();                         // [打开输出] 可达性
    // 流水线四步卡（英文简写 + 模式徽标 MANUAL/AUTO 属性绑定 + 计数）
    TSharedRef<SWidget> MakeStageCard(const FText& Abbr, int32 StageIndex, TSharedPtr<STextBlock>& OutValue);
    // 阶段是否手动（Manual 全手；Semi 仅 ID(0) 手；Auto 全自动——由模式位图语义决定）
    bool IsStageManual(int32 StageIndex) const;
    // 流程模式三档选择器（Auto 置灰；Semi 默认；Manual）
    TSharedRef<SWidget> MakeStreamModeSelector();
    void SetStreamMode(int32 NewMode);
    void UpdateStreamModeUI();             // 模式/进度变化后刷新徽标、按钮态、Manual 行显隐
    FString GetStreamModeValue() const;    // "auto" / "semi" / "manual"
    // Stream job 生成（job_stream.json：模式 + stage_automation 位图 + 监听配置）；热文件夹为空返回空串
    FString BuildStreamJobJson() const;
    // Manual 步骤触发（严格顺序：仅 ManualStageProgress 当前步有效）
    void OnManualStageTrigger(int32 Stage);
    // 开始/停止监听（Semi: 生成 job_stream.json + precheck 预检；Manual: 进入手动步骤流程）
    void OnAutoStartStop();
    // 事件流追加（带时间戳，保留最近 100 行）
    void AutoAppendEvent(const FString& Line);
    TSharedRef<SWidget> BuildProgressRow();
    TSharedRef<SMultiLineEditableTextBox> MakeLogBox();
    TSharedRef<SMultiLineEditableTextBox> MakeResultListBox();
    TSharedRef<SHorizontalBox> MakeInputRow(const FText& Label, const FText& Placeholder, EBrowseTarget BrowseTarget, TSharedPtr<SEditableTextBox>& OutEdit);
    TSharedRef<SHorizontalBox> MakeLimitRow(TSharedPtr<SEditableTextBox>& OutEdit);
    TSharedRef<SHorizontalBox> MakeActionRow(const FText& TabName);
    // 阶段 A：ROM 导入区（文件夹行 + 导入按钮 + 操作提示）
    TSharedRef<SWidget> BuildRomImportSection();
    // 阶段 B：任务列表区（动态行 + 添加按钮）
    TSharedRef<SWidget> BuildTaskListSection();
    TSharedRef<SWidget> BuildTaskRow(TSharedPtr<struct FSolveTaskRow>& OutRow);
    // 导入目标路径行（任务列表下方：标签 + 输入框 + 浏览 + 重置 + 落位提示）
    TSharedRef<SWidget> BuildImportRootRow();
    void AddSolveTaskRow();
    void RebuildTaskListBox();   // 删行后整体重建（Slate 动态删 slot 无公开 API）
    // 高级选项折叠区（排除列表，默认收起）
    TSharedRef<SWidget> BuildAdvancedSection();
    void UpdateButtonsState();

    // ── 桥接（plan 2.7 / 2.8） ──
    void OnBrowse(EBrowseTarget InTarget);
    // 通用浏览（动态任务行用：目标输入框 + 磁盘目录/资产选择）
    void BrowseDiskDir(TSharedPtr<SEditableTextBox> Target, const FString& Title);
    void BrowseIdentityAsset(TSharedPtr<SEditableTextBox> Target);
    // 导入目标浏览：UE 内容浏览器路径选择（/Game 下目录）
    void BrowseContentImportDir();
    // UE 资产路径选择（内容浏览器 PathPicker 模态框，选 /Game 下目录回填目标输入框）
    void BrowseContentPathUE(TSharedPtr<SEditableTextBox> Target, const FText& Title);
    // 读取导入目标路径（Trim；空回落默认 /Game/CaptureManager/Imports；去尾部斜杠）
    FString GetImportRoot() const;
    // 轻量重名提示：目标批次目录下已有资产时黄色警告（重复 take 幂等跳过，旧数据可能被沿用）
    void WarnIfBatchDirExists(const FString& BatchName);
    void OnStart(const FText& TabName);
    void OnCancel(const FText& TabName);
    void OnPrecheck(const FText& TabName);
    // 阶段 A：ROM 导入（段驱动，有 [i/N] 进度）
    void OnImportRom();
    // 阶段 B：组队列推进（每组一个混合段序列 job：导入表演→解算→导出 FBX）
    void StartNextGroup();
    void SetStatusDot(const FString& State);
    FString BuildJobJson() const;
    // ROM 导入 job（import_only 纯导入段）
    FString BuildRomJobJson(const FString& RomSourceDir) const;
    // 任务组 job（import_then_solve：导入段×N + 解算段×M）
    FString BuildGroupJobJson(const FString& IdentityPath, const FString& SourceDir) const;
    // 高级选项多行文本 -> JSON 片段（exclude 数组；空输入返回空串）
    FString BuildExcludeJson() const;
    void HandleMHLogLine(const FString& Line);
    void AppendLog(const FString& Text, const FLinearColor& Color);
    void SetProgress(int32 Current, int32 Total);
    FString JsonEscape(const FString& In) const;

    // ── 段间刷新（定时器逐段驱动） ──
    void StartBatchTimer(const FString& JobPath, const FText& TabName);
    void TickNextSegment();
    void StopBatchTimer();
    void LaunchSegment(int32 Index);

    // ── Stream 流式驱动（批次5）：定时器提交 stream_next + 状态灌数 ──
    struct FTimerHandle StreamTimer;  // 流式逐条提交用
    bool bStreamStopRequested = false;   // 停止请求（当前段 Exec 返回后生效——解算阻塞期 UI 冻结，本就只能段间停）
    int32 StreamRemaining = -1;          // 最近一次 [MHS_STREAM] REMAINING（-1=未知）
    FString StreamJobPath;               // job_stream.json 路径
    FString StreamActiveGroup;           // 当前活跃分组（Semi 确认绑定的目标组）
    FString StreamIdStateText;           // ID 状态文本（身份带 state，段间更新，面板兜底显示）
    // ── 身份自动绑定 ──
    FString IdSourceLine;                // 来源提示："来源 xxx · 识别分组 pm/"
    FString IdSuggestedGroup;            // 自动推断的绑定组（来自 ID 素材命名 am/pm）
    FString SelectedBindGroup;           // 下拉当前选中组（空=根组/自动）
    TArray<TSharedPtr<FString>> BindGroupOptions;              // 下拉候选（分组列表）
    TSharedPtr<class SComboBox<TSharedPtr<FString>>> BindGroupCombo;
    TSharedPtr<class STextBlock> IdSourceText;
    TSharedPtr<class SMultiLineEditableTextBox> AutoLogEdit;  // 日志（兜底查阅，只读）
    void StartStreamTimer(const FString& JobPath);
    void TickStreamNext();               // 提交一条 stream_next；REMAINING=0 时转 10s 空转轮询（等新素材）
    void StopStreamTimer();
    void RefreshStreamUIFromState();     // 读 stream_state.json → 大数字/四步卡/当前行/失败/分组区
    void RebuildGroupList(const TArray<TPair<FString, FString>>& InGroups);
    // 面板配置持久化（[保存为默认]）：4 个输入框 → stream_panel.json，Construct 回填
    void SaveStreamPanelConfig();
    void LoadStreamPanelConfig();
    // 身份绑定：把资产目录里的 identity 绑定到选定分组（空=按 ID 素材名自动推断 am/pm）
    void OnBindIdentity();
    void RefreshBindGroupOptions();
    // 流程步骤卡（配置/身份/监听——明晰当前步，替代原四步计数卡）
    TSharedRef<SWidget> MakeStreamStepCard(const FText& Title, const FText& Hint, int32 StepIndex);
    // 原型模块框（浅色标题条 + 内容区）——配置/状态/身份/日志统一使用
    TSharedRef<SWidget> MakeStreamModule(const FText& Title, TSharedRef<SWidget> Content,
                                        TAttribute<FText> RightHint = TAttribute<FText>());
    // 模块间分割线（参考深度 Tab）
    TSharedRef<SWidget> MakeStreamDivider();
};
