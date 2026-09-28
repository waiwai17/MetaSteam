// SMetaHumanSolverWindow.cpp
#include "SMetaHumanSolverWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Styling/SlateBrush.h"
#include "Styling/CoreStyle.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Modules/ModuleManager.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
// Stream 状态灌数（批次5）：解析 stream_state.json
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
// 关闭"后台降频"（编辑器失焦/最小化时 UE 默认节流 → 流式心跳被拖到近乎停滞）
#include "Editor/EditorPerformanceSettings.h"
#include "MHSStreamConsole.h"
#include "Misc/PackageName.h"
#include "DesktopPlatformModule.h"
#include "TimerManager.h"
#include "Widgets/Views/STableViewBase.h"
#include "Widgets/SWindow.h"
#include "Widgets/Images/SImage.h"
#include "Framework/Application/SlateApplication.h"

#define LOCTEXT_NAMESPACE "MetaHumanSolverWindow"
#define MHS_LOG(Fmt, ...) UE_LOG(LogTemp, Log, TEXT("[MetaHumanSolver] " Fmt), ##__VA_ARGS__)

namespace
{
    FSlateBrush MakeBrush(const FLinearColor& InColor)
    {
        FSlateBrush Brush;
        Brush.DrawAs = ESlateBrushDrawType::Box;
        Brush.TintColor = FSlateColor(InColor);
        return Brush;
    }

    FButtonStyle MakeButtonStyle(const FLinearColor& Normal, const FLinearColor& Hover, const FLinearColor& Pressed)
    {
        FButtonStyle Style;
        Style.SetNormal(MakeBrush(Normal));
        Style.SetHovered(MakeBrush(Hover));
        Style.SetPressed(MakeBrush(Pressed));
        return Style;
    }

    const FSlateBrush& BlackBrush()
    {
        static FSlateBrush Brush = MakeBrush(FLinearColor::Black);
        return Brush;
    }
}

// 监听 [MHS_*] 前缀日志（plan 2.7）：后台线程 Serialize -> 切游戏线程回调窗口
// 注意：必须定义在全局（非匿名命名空间），与头文件前置声明 class FMHSLogDevice 匹配
class FMHSLogDevice : public FOutputDevice, public TSharedFromThis<FMHSLogDevice>
{
public:
    TFunction<void(const FString&)> OnLine;

    FMHSLogDevice() { SetAutoEmitLineTerminator(false); }

    virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
    {
        const FString Line(V);
        if (!Line.Contains(TEXT("[MHS_")))
        {
            return;
        }
        TSharedPtr<FMHSLogDevice> Self = AsShared();
        AsyncTask(ENamedThreads::GameThread, [Self, Line]()
        {
            if (Self->OnLine)
            {
                Self->OnLine(Line);
            }
        });
    }
};

void SMetaHumanSolverWindow::InitStyles()
{
    StyleTabActive = MakeButtonStyle(
        ColorAccent, ColorAccentDark, FLinearColor(0.25f, 0.47f, 0.85f, 1.0f));
    StyleTabIdle = MakeButtonStyle(
        ColorTabIdle,
        FLinearColor(0.10f, 0.10f, 0.11f, 1.0f),
        FLinearColor(0.04f, 0.04f, 0.05f, 1.0f));
    // 主按钮（开始解算）：统一用主色蓝，hover 微亮，保证全局视觉焦点一致（绿只留给"完成"状态）
    StylePrimary = MakeButtonStyle(
        ColorAccent,
        FLinearColor(0.14f, 0.38f, 0.72f, 1.0f),
        FLinearColor(0.06f, 0.20f, 0.42f, 1.0f));
    StyleSecondary = MakeButtonStyle(
        FLinearColor(0.03f, 0.03f, 0.035f, 1.0f),
        FLinearColor(0.10f, 0.10f, 0.11f, 1.0f),
        FLinearColor(0.015f, 0.015f, 0.02f, 1.0f));
}

SMetaHumanSolverWindow::~SMetaHumanSolverWindow()
{
    if (LogDevice.IsValid())
    {
        FOutputDeviceRedirector::Get()->RemoveOutputDevice(LogDevice.Get());
        LogDevice.Reset();
    }
    // 清理段间定时器，避免窗口销毁后悬空 lambda 指针
    StopBatchTimer();
}

void SMetaHumanSolverWindow::SetActiveTab(int32 Index)
{
    ActiveTab = Index;
    if (ContentSwitcher.IsValid())
    {
        ContentSwitcher->SetActiveWidgetIndex(Index);
    }
    for (int32 i = 0; i < TabButtons.Num(); ++i)
    {
        if (TabButtons[i].IsValid())
        {
            TabButtons[i]->SetButtonStyle(i == Index ? &StyleTabActive : &StyleTabIdle);
        }
    }
    const TCHAR* Names[] = { TEXT("深度解算"), TEXT("单视频模式"), TEXT("音频解算"), TEXT("Stream") };
    const int32 NameCount = 4;
    MHS_LOG("切换到 Tab: %s", (Index >= 0 && Index < NameCount) ? Names[Index] : TEXT("?"));
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildBrandHeader()
{
    // 顶部品牌区：品牌色条 + 标题 + 副标题，提升整体质感与视觉焦点
    return SNew(SHorizontalBox)
        // 左侧品牌色竖条（强调主色）
        + SHorizontalBox::Slot().AutoWidth().Padding(12, 8, 0, 8)
        [
            SNew(SBox)
            .WidthOverride(4.0f)
            [ SNew(SImage).ColorAndOpacity(ColorAccent).Image(&BlackBrush()) ]
        ]
        + SHorizontalBox::Slot().FillWidth(1.0f).Padding(10, 8, 14, 6)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .Text(LOCTEXT("Title", "MetaHuman Solution"))
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 15))
                .ColorAndOpacity(ColorWhite)
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("Subtitle", "批量面部动画解算与 FBX 交付"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                .ColorAndOpacity(ColorTextDim)
            ]
        ];
}

void SMetaHumanSolverWindow::Construct(const FArguments& InArgs)
{
    InitStyles();

    ContentSwitcher = SNew(SWidgetSwitcher).WidgetIndex(0)
        + SWidgetSwitcher::Slot() [ BuildDepthPanel() ]
        + SWidgetSwitcher::Slot() [ BuildVideoPanel() ]
        + SWidgetSwitcher::Slot() [ BuildAudioPanel() ]
        + SWidgetSwitcher::Slot() [ BuildAutoPanel() ];

    // Stream 模式按钮初始选中态（默认 Semi 高亮；徽标/Manual 行由属性绑定自动跟随）
    UpdateStreamModeUI();

    // Stream 面板配置回填（[保存为默认] 的持久化——解决"每次重填"）
    LoadStreamPanelConfig();

    // 身份分组初始空态（避免分组区空白——未启动监听时也显示占位提示）
    RebuildGroupList(TArray<TPair<FString, FString>>());
    // 绑定目标下拉候选（加载路径子目录 + 根组）
    RefreshBindGroupOptions();

    auto MakeTabButton = [this](int32 Idx, const FText& Label) -> TSharedRef<SButton>
    {
        return SNew(SButton)
            .ButtonStyle(&StyleTabIdle)
            .ContentPadding(FMargin(10, 3))
            .OnClicked_Lambda([this, Idx]()
            {
                SetActiveTab(Idx);
                return FReply::Handled();
            })
            [
                SNew(SBox)
                .HAlign(HAlign_Fill)
                .VAlign(VAlign_Fill)
                [
                    SNew(STextBlock)
                    .Text(Label)
                    .Justification(ETextJustify::Center)
                    .Font(FCoreStyle::GetDefaultFontStyle("SemiBold", 10))
                    .ColorAndOpacity(ColorWhite)
                ]
            ];
    };

    TabButtons.SetNum(4);
    TabButtons[0] = MakeTabButton(0, LOCTEXT("TabDepth", "深度解算"));
    TabButtons[1] = MakeTabButton(1, LOCTEXT("TabVideo", "单视频模式"));
    TabButtons[2] = MakeTabButton(2, LOCTEXT("TabAudio", "音频解算"));
    TabButtons[3] = MakeTabButton(3, LOCTEXT("TabStream", "Stream"));

    // 任务列表默认给出一行空行（用户直接填，或点"＋添加任务"增加）
    AddSolveTaskRow();

    // 注册 [MHS_*] 日志监听（plan 2.7）
    LogDevice = MakeShared<FMHSLogDevice>();
    LogDevice->OnLine = [WeakThis = StaticCastSharedRef<SMetaHumanSolverWindow>(AsShared()).ToWeakPtr()](const FString& Line)
    {
        if (TSharedPtr<SMetaHumanSolverWindow> Window = WeakThis.Pin())
        {
            Window->HandleMHLogLine(Line);
        }
    };
    FOutputDeviceRedirector::Get()->AddOutputDevice(LogDevice.Get());

    ChildSlot
    [
        SNew(SBorder)
        .BorderBackgroundColor(ColorBg)
        .Padding(FMargin(0))
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [ BuildBrandHeader() ]
            + SVerticalBox::Slot().AutoHeight().Padding(12, 0, 12, 8)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0) [ TabButtons[0].ToSharedRef() ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0) [ TabButtons[1].ToSharedRef() ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0) [ TabButtons[2].ToSharedRef() ]
                + SHorizontalBox::Slot().AutoWidth() [ TabButtons[3].ToSharedRef() ]
            ]
            + SVerticalBox::Slot().FillHeight(1.0f).Padding(12, 0, 12, 4)
            [
                SNew(SBorder)
                .BorderImage(&BlackBrush())
                .BorderBackgroundColor(ColorPanel)
                .Padding(FMargin(14, 12))
                [ ContentSwitcher.ToSharedRef() ]
            ]
            // 共享日志区 + 结果清单：放在 ContentSwitcher 外层，切换 Tab 时常驻且引用唯一。
            // 修复：三个面板各自 MakeLogBox() 会 SAssignNew 覆盖 LogEdit 导致只在最后构建的
            // (音频)面板显示日志的问题；改为全局单一日志框，深度/视频解算日志实时可见。
            + SVerticalBox::Slot().AutoHeight().Padding(12, 0, 12, 6)
            [
                SNew(SHorizontalBox)
                // 左：运行日志（原型模块样式：标题条 + 内容）
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 6, 0)
                [
                    MakeStreamModule(LOCTEXT("SharedLogHdr", "日志"),
                        SNew(SBox).HeightOverride(160.0f)
                        [ MakeLogBox() ],
                        TAttribute<FText>())
                ]
                // 右：每段结果清单
                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(6, 0, 0, 0)
                [
                    MakeStreamModule(LOCTEXT("SharedResultHdr", "每段结果"),
                        SNew(SBox).HeightOverride(160.0f)
                        [ MakeResultListBox() ],
                        TAttribute<FText>())
                ]
            ]
            // 共享进度条 + 状态灯：放在 ContentSwitcher 外层，切换 Tab 时进度条常驻且引用唯一
            + SVerticalBox::Slot().AutoHeight().Padding(12, 0, 12, 12)
            [ BuildProgressRow() ]
        ]
    ];

    SetActiveTab(0);
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildProgressRow()
{
    return SNew(SHorizontalBox)
        // 状态指示灯：running=蓝、idle=灰、error=红（用方块 Brush 模拟圆点，避免额外资源）
        + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0).VAlign(VAlign_Center)
        [
            SNew(SBox).WidthOverride(10).HeightOverride(10)
            [
                SAssignNew(StatusDot, SImage)
                .ColorAndOpacity(FSlateColor(ColorTextDim)) // 默认 idle 灰
                .Image(&BlackBrush())
            ]
        ]
        + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 12, 0).VAlign(VAlign_Center)
        [
            SNew(SBox).HeightOverride(8)
            [
                SAssignNew(ProgressBar, SProgressBar).Percent(0.0f).FillColorAndOpacity(ColorAccent)
            ]
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            SAssignNew(ProgressLabel, STextBlock)
            .Text(LOCTEXT("PctZero", "0%"))
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
            .ColorAndOpacity(ColorTextDim)
        ];
}

TSharedRef<SMultiLineEditableTextBox> SMetaHumanSolverWindow::MakeLogBox()
{
    return SAssignNew(LogEdit, SMultiLineEditableTextBox)
        .IsReadOnly(true)
        .AutoWrapText(true)
        // 日志用等宽字体，时间戳/进度对齐更清晰，观感更专业
        .Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
        .ForegroundColor(FSlateColor(ColorTextDim))
        .BackgroundColor(FSlateColor(ColorSurface))
        .Padding(FMargin(6, 4))
        .Text(LOCTEXT("LogReady", "就绪"));
}

TSharedRef<SMultiLineEditableTextBox> SMetaHumanSolverWindow::MakeResultListBox()
{
    return SAssignNew(ResultListEdit, SMultiLineEditableTextBox)
        .IsReadOnly(true)
        .AutoWrapText(true)
        .Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
        .ForegroundColor(FSlateColor(ColorTextDim))
        .BackgroundColor(FSlateColor(ColorSurface))
        .Padding(FMargin(6, 4))
        .Text(LOCTEXT("ResultReady", "（每段解算结果将在此列出）"));
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildDepthPanel()
{
    // 表单较长，用 SScrollBox 滚动。结构对应工作流两阶段：
    // ① 身份创建（ROM 导入：手动做 identity 前的素材导入）
    // ② 解算任务（任务列表每组 = 身份资产 + 表演源文件夹，全自动到 FBX）
    // 行距节奏统一：行间 5px / 分区标题前 12px / 分割线两翼 12px
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().FillHeight(1.0f).Padding(0, 0, 0, 8)
        [
            SNew(SScrollBox)
            + SScrollBox::Slot()
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [ BuildRomImportSection() ]
                + SVerticalBox::Slot().AutoHeight().Padding(18, 12)
                [
                    // 居中分割线（浅灰细线）：①身份创建 与 ②解算任务 的唯一视觉分隔
                    SNew(SBox).HeightOverride(1.0f)
                    [
                        SNew(SBorder)
                        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                        .BorderBackgroundColor(FLinearColor(0.125f, 0.125f, 0.125f, 1.0f))
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight()
                [ BuildTaskListSection() ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
        [ MakeActionRow(LOCTEXT("DepthTab", "深度解算")) ];
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildRomImportSection()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("RomHdr", "① 身份创建"))
            .Font(FCoreStyle::GetDefaultFontStyle("SemiBold", 12))
            .ColorAndOpacity(ColorAccent)
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 5)
        [
            // 一行式：[身份文件][输入框][浏览][导入]——操作集中一行，无独立按钮行
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).WidthOverride(72.0f).HAlign(HAlign_Left)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("RomSource", "身份文件"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorText)
                ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 8, 0).VAlign(VAlign_Center)
            [
                SAssignNew(RomSourceEdit, SEditableTextBox)
                .HintText(LOCTEXT("RomSourceHint", "选择 ROM 数据文件夹（磁盘）"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ForegroundColor(FSlateColor(ColorText))
                .BackgroundColor(FSlateColor(ColorSurface))
                .Padding(FMargin(6, 2))
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0).VAlign(VAlign_Center)
            [
                SNew(SBox).HeightOverride(24.0f)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]()
                    {
                        OnBrowse(EBrowseTarget::RomSource);
                        return FReply::Handled();
                    })
                    [
                        SNew(SBox)
                        .HAlign(HAlign_Fill)
                        .VAlign(VAlign_Fill)
                        [
                            SNew(STextBlock)
                            .Text(LOCTEXT("RomBrowseBtn", "浏览"))
                            .Justification(ETextJustify::Center)
                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                            .ColorAndOpacity(ColorText)
                        ]
                    ]
                ]
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).HeightOverride(24.0f)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]()
                    {
                        OnImportRom();
                        return FReply::Handled();
                    })
                    [
                        SNew(SBox)
                        .HAlign(HAlign_Fill)
                        .VAlign(VAlign_Fill)
                        [
                            SNew(STextBlock)
                            .Text(LOCTEXT("ImportRomBtn", "导入"))
                            .Justification(ETextJustify::Center)
                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                            .ColorAndOpacity(ColorText)
                        ]
                    ]
                ]
            ]
        ];
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildTaskListSection()
{
    // 任务行的添加入口 = 末行行尾 [+]（RebuildTaskListBox 渲染），无独立按钮行。
    // 容器先创建并 Rebuild（渲染 Construct 阶段 AddSolveTaskRow 先行创建的初始行）
    if (!TaskListBox.IsValid())
    {
        SAssignNew(TaskListBox, SVerticalBox);
        RebuildTaskListBox();
    }
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("TaskHdr", "② 解算任务"))
            .Font(FCoreStyle::GetDefaultFontStyle("SemiBold", 12))
            .ColorAndOpacity(ColorAccent)
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 5)
        [
            TaskListBox.ToSharedRef()
        ]
        // 导入目标路径（阶段A新增）：素材导入的 UE 资产目录，可选化避免不同批次重名 take 覆盖
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
        [
            BuildImportRootRow()
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 5)
        [ MakeInputRow(LOCTEXT("OutputPath", "输出路径"), LOCTEXT("OutputPathHint", "指定解算结果输出目录(磁盘 FBX 导出位置)"), EBrowseTarget::DepthOutput, DepthOutputEdit) ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8)
        [ BuildAdvancedSection() ];
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildImportRootRow()
{
    // 导入目标路径行：[导入路径][输入框][浏览][⟲]。
    // 标签列宽 72px 与 MakeInputRow 严格对齐；实际落位 = {路径}/{批次名}/{take}（批次子目录自动创建）。
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).WidthOverride(72.0f).HAlign(HAlign_Left)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("ImportRootLabel", "导入路径"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorText)
                ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 4, 0).VAlign(VAlign_Center)
            [
                SAssignNew(ImportRootEdit, SEditableTextBox)
                .Text(LOCTEXT("ImportRootDefault", "/Game/CaptureManager/Imports"))
                .HintText(LOCTEXT("ImportRootHint", "素材导入的 UE 资产目录（/Game/...）"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ForegroundColor(FSlateColor(ColorText))
                .BackgroundColor(FSlateColor(ColorSurface))
                .Padding(FMargin(6, 2))
            ]
            // 浏览：打开工程 Content 目录选择，选中后换算为 /Game 相对路径
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).HeightOverride(24.0f)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]()
                    {
                        BrowseContentImportDir();
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("ImportRootBrowse", "浏览"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(ColorText)
                    ]
                ]
            ]
            // 重置：恢复默认值（防误改后素材"找不到"）
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
            [
                SNew(SBox).HeightOverride(24.0f)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(6, 2))
                    .OnClicked_Lambda([this]()
                    {
                        if (ImportRootEdit.IsValid())
                        {
                            ImportRootEdit->SetText(FText::FromString(TEXT("/Game/CaptureManager/Imports")));
                        }
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("ImportRootReset", "⟲"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(ColorTextDim)
                    ]
                ]
            ]
        ];
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildTaskRow(TSharedPtr<FSolveTaskRow>& OutRow)
{
    // 只创建控件引用（布局统一由 RebuildTaskListBox 渲染——单一布局源，增删行后整体重建）
    TSharedPtr<FSolveTaskRow> Row = MakeShared<FSolveTaskRow>();
    OutRow = Row;
    SAssignNew(Row->IdentityEdit, SEditableTextBox)
        .HintText(LOCTEXT("TaskIdentityHint", "身份资产（MetaHumanIdentity）"))
        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
        .ForegroundColor(FSlateColor(ColorText))
        .BackgroundColor(FSlateColor(ColorSurface))
        .Padding(FMargin(6, 2));
    SAssignNew(Row->SourceEdit, SEditableTextBox)
        .HintText(LOCTEXT("TaskSourceHint", "表演源文件夹（磁盘）"))
        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
        .ForegroundColor(FSlateColor(ColorText))
        .BackgroundColor(FSlateColor(ColorSurface))
        .Padding(FMargin(6, 2));
    return SNullWidget::NullWidget;
}

void SMetaHumanSolverWindow::AddSolveTaskRow()
{
    TSharedPtr<FSolveTaskRow> Row;
    BuildTaskRow(Row);
    SolveTaskRows.Add(Row);
    RebuildTaskListBox();
}

void SMetaHumanSolverWindow::RebuildTaskListBox()
{
    // 唯一布局源：[身份框][…] [表演源框][…] [×]（末行尾追加 [+]）——
    // 每个浏览按钮紧贴其服务的输入框（配对明确）；行间距统一 3px 节奏。
    if (!TaskListBox.IsValid())
    {
        return;
    }
    TaskListBox->ClearChildren();
    const int32 LastIndex = SolveTaskRows.Num() - 1;
    for (int32 RowIdx = 0; RowIdx < SolveTaskRows.Num(); ++RowIdx)
    {
        const TSharedPtr<FSolveTaskRow>& Row = SolveTaskRows[RowIdx];
        TSharedPtr<FSolveTaskRow> RowPtr = Row;
        const bool bIsLast = (RowIdx == LastIndex);

        const TSharedRef<SHorizontalBox> RowBox = SNew(SHorizontalBox)
            // 身份资产框 + 紧贴的浏览按钮（配对 1）
            + SHorizontalBox::Slot().FillWidth(0.42f).Padding(0, 2, 4, 2)
            [
                Row->IdentityEdit.ToSharedRef()
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 2, 4, 2)
            [
                SNew(SButton)
                .ButtonStyle(&StyleSecondary)
                .ContentPadding(FMargin(8, 2))
                .OnClicked_Lambda([this, RowPtr]()
                {
                    BrowseIdentityAsset(RowPtr->IdentityEdit);
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("TaskIdentityBrowse", "…"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorText)
                ]
            ]
            // 表演源文件夹框 + 紧贴的浏览按钮（配对 2）
            + SHorizontalBox::Slot().FillWidth(0.42f).Padding(0, 2, 4, 2)
            [
                Row->SourceEdit.ToSharedRef()
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 2, 4, 2)
            [
                SNew(SButton)
                .ButtonStyle(&StyleSecondary)
                .ContentPadding(FMargin(8, 2))
                .OnClicked_Lambda([this, RowPtr]()
                {
                    BrowseDiskDir(RowPtr->SourceEdit, TEXT("选择表演源文件夹"));
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("TaskSourceBrowse", "…"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorText)
                ]
            ]
            // 删除行按钮
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 2, 2, 2)
            [
                SNew(SButton)
                .ButtonStyle(&StyleSecondary)
                .ContentPadding(FMargin(6, 2))
                .OnClicked_Lambda([this, RowPtr]()
                {
                    SolveTaskRows.RemoveAll([RowPtr](const TSharedPtr<FSolveTaskRow>& R) { return R == RowPtr; });
                    RebuildTaskListBox();
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("TaskRemoveBtn", "×"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorLogWarn)
                ]
            ];

        // 添加行按钮（仅末行行尾：+）——Slate 不支持三元 slot 内容，条件追加
        if (bIsLast)
        {
            RowBox->AddSlot().AutoWidth().Padding(0, 2)
            [
                SNew(SButton)
                .ButtonStyle(&StyleSecondary)
                .ContentPadding(FMargin(6, 2))
                .OnClicked_Lambda([this]()
                {
                    AddSolveTaskRow();
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("TaskAddBtn", "+"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorText)
                ]
            ];
        }

        TaskListBox->AddSlot().AutoHeight().Padding(0, 3)
        [
            RowBox
        ];
    }
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildAdvancedSection()
{
    // 高级选项（默认收起）：不展开 = exclude 空 = 全量解算，普通用户零学习成本。
    // 身份映射已由任务列表取代（每组直接绑定 身份+文件夹）。
    return SNew(SExpandableArea)
        .AreaTitle(LOCTEXT("AdvancedTitle", "高级选项（排除素材等）"))
        .InitiallyCollapsed(true)
        .Padding(FMargin(0, 4))
        .BodyContent()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 4)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("ExcludeLabel", "排除素材（每行一个，如 ROM：CD_face_am01_8 或 face_am01_8）"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                .ColorAndOpacity(ColorTextDim)
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 2)
            [
                SNew(SBox).HeightOverride(40.0f)
                [
                    SAssignNew(ExcludeEdit, SMultiLineEditableTextBox)
                    .Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
                    .ForegroundColor(FSlateColor(ColorTextDim))
                    .BackgroundColor(FSlateColor(ColorSurface))
                    .Padding(FMargin(6, 4))
                    .HintText(LOCTEXT("ExcludeHint", "CD_face_am01_8\nCD_face_pm01_7"))
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                // 纯面部交付（生产默认）：解算跳过舌头 + LS 不导音频轨（深度解算纯视觉，
                // 音频仅关联舌头追踪与 LS 音轨，跳过可省解算时间）
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SAssignNew(NoTongueAudioCheck, SCheckBox)
                    .IsChecked(ECheckBoxState::Checked)
                    .ForegroundColor(FSlateColor(ColorAccent))
                ]
                + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(6, 0, 0, 0)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("NoTongueAudioLabel", "不含舌头与音频（纯面部动画交付）"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                    .ColorAndOpacity(ColorTextDim)
                ]
            ]
        ];
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildVideoPanel()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().FillHeight(1.0f)
        [
            SNew(SSplitter).Orientation(Orient_Vertical)
            + SSplitter::Slot().Value(0.62f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("VideoHdr", "视频输入"))
                    .Font(FCoreStyle::GetDefaultFontStyle("SemiBold", 12))
                    .ColorAndOpacity(ColorAccent)
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 6)
                [ MakeInputRow(LOCTEXT("VideoPath", "输入路径"), LOCTEXT("VideoPathHint", "选择视频捕捉素材"), EBrowseTarget::VideoInput, VideoInputEdit) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 6)
                [ MakeInputRow(LOCTEXT("VideoIdentity", "身份路径"), LOCTEXT("VideoIdentityHint", "选择 MetaHuman 身份资产 (MetaHumanIdentity)"), EBrowseTarget::VideoIdentity, VideoIdentityEdit) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 6)
                [
                    SNew(SBox)
                    .HeightOverride(24.0f)
                    .VAlign(VAlign_Center)
                    [
                        SNew(SHorizontalBox)
                        // 与输入行复用相同的固定标签列和输入区起点
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                        [
                            SNew(SBox).WidthOverride(92.0f).HAlign(HAlign_Left)
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("PathStatusLabel", "路径状态"))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                .ColorAndOpacity(ColorText)
                            ]
                        ]
                        + SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 8, 0).VAlign(VAlign_Center)
                        [
                            SNew(STextBlock)
                            .Text(LOCTEXT("PathChecking", "等待校验"))
                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                            .ColorAndOpacity(ColorStatus)
                        ]
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 6)
                [ MakeInputRow(LOCTEXT("VideoOut", "输出路径"), LOCTEXT("VideoOutHint", "指定解算结果输出目录"), EBrowseTarget::VideoOutput, VideoOutputEdit) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 6)
                [ MakeLimitRow(VideoLimitEdit) ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
        [ MakeActionRow(LOCTEXT("VideoTab", "单视频模式")) ];
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildAudioPanel()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().FillHeight(1.0f)
        [
            SNew(SSplitter).Orientation(Orient_Vertical)
            + SSplitter::Slot().Value(0.62f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("AudioHdr", "音频输入"))
                    .Font(FCoreStyle::GetDefaultFontStyle("SemiBold", 12))
                    .ColorAndOpacity(ColorAccent)
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 6)
                [ MakeInputRow(LOCTEXT("AudioPath", "输入路径"), LOCTEXT("AudioPathHint", "选择 SoundWave 音频资产"), EBrowseTarget::AudioInput, AudioInputEdit) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 6)
                [ MakeInputRow(LOCTEXT("AudioOut", "输出路径"), LOCTEXT("AudioOutHint", "指定解算结果输出目录"), EBrowseTarget::AudioOutput, AudioOutputEdit) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 6)
                [ MakeLimitRow(AudioLimitEdit) ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
        [ MakeActionRow(LOCTEXT("AudioTab", "音频解算")) ];
}

// ── Auto Tab（流式监听模式 · 骨架版）──
// 第一版范围：监听配置真实可用（生成 job_auto.json + precheck_import 预检热文件夹，
// 可直接服务 T2 拷贝行为观察）；队列统计/事件流为占位骨架，Phase 2 接 watcher 数据。

TSharedRef<SWidget> SMetaHumanSolverWindow::MakeStreamStepCard(const FText& Title, const FText& Hint, int32 StepIndex)
{
    // 步骤态：0=已完成（绿）1=进行中（蓝）2=未开始（灰）
    //   配置步：加载路径已填即完成；身份步：已绑定身份即完成；监听步：运行中=进行中
    auto StepState = [this, StepIndex]() -> int32
    {
        const bool bConfigured = AutoInboxEdit.IsValid()
            && !AutoInboxEdit->GetText().ToString().TrimStartAndEnd().IsEmpty();
        // 身份步完成判定：本次会话确认过，或引擎侧已存在绑定（重启编辑器后依然正确）
        const bool bIdentityBound = !SemiIdentityPath.IsEmpty()
            || StreamIdStateText.Contains(TEXT("已绑定"));
        switch (StepIndex)
        {
        case 0: return bConfigured ? 0 : 1;
        case 1: return bIdentityBound ? 0 : (bConfigured ? 1 : 2);
        default: return bAutoListening ? 1 : 2;
        }
    };
    return SNew(SBorder)
        .BorderImage(&BlackBrush())
        .BorderBackgroundColor_Lambda([StepState]()
        {
            const int32 S = StepState();
            return S == 0 ? FLinearColor(0.12f, 0.35f, 0.18f)
                 : (S == 1 ? FLinearColor(0.10f, 0.22f, 0.42f) : ColorSurface);
        })
        .Padding(FMargin(8, 6))
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
            [
                SNew(STextBlock)
                .Text(Title)
                .Font(FCoreStyle::GetDefaultFontStyle("SemiBold", 10))
                .ColorAndOpacity_Lambda([StepState]()
                {
                    const int32 S = StepState();
                    return FSlateColor(S == 0 ? ColorSuccess
                        : (S == 1 ? ColorAccent : ColorTextDim));
                })
            ]
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 1, 0, 0)
            [
                SNew(STextBlock)
                .Text(Hint)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                .ColorAndOpacity(ColorTextDim)
            ]
        ];
}

TSharedRef<SWidget> SMetaHumanSolverWindow::MakeStreamDivider()
{
    // 模块间分割线（参考深度 Tab：浅灰细线，两翼留白由插槽 Padding 提供）
    return SNew(SBox).HeightOverride(1.0f)
        [
            SNew(SBorder)
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor(FLinearColor(0.125f, 0.125f, 0.125f, 1.0f))
        ];
}

TSharedRef<SWidget> SMetaHumanSolverWindow::MakeStreamModule(const FText& Title, TSharedRef<SWidget> Content,
                                                            TAttribute<FText> RightHint)
{
    // 原型模块样式：外框（ColorPanel 底 + 细边框）+ 独立浅色标题条（ColorSurface）+ 内容区
    return SNew(SBorder)
        .BorderImage(&BlackBrush())
        .BorderBackgroundColor(ColorPanel)
        .Padding(FMargin(1.0f))
        [
            SNew(SVerticalBox)
            // 标题条（浅色，跨整宽——与原型一致）
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SBorder)
                .BorderImage(&BlackBrush())
                .BorderBackgroundColor(ColorSurface)
                .Padding(FMargin(9, 4))
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(Title)
                        .Font(FCoreStyle::GetDefaultFontStyle("SemiBold", 10))
                        .ColorAndOpacity(ColorText)
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Right).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(RightHint)
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                        .ColorAndOpacity(ColorTextDim)
                    ]
                ]
            ]
            // 内容区
            + SVerticalBox::Slot().AutoHeight().Padding(9, 7)
            [
                Content
            ]
        ];
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildAutoPanel()
{
    // 四步卡 / Manual 步骤按钮 数组初始化（Construct 阶段一次性建满 4 槽）
    QueueStatValues.SetNum(4);
    StageActionButtons.SetNum(4);
    return SNew(SVerticalBox)
        // ── 流程步骤条（原型：置顶，明晰当前步；计数/进度一律交给全局控制台）──
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f)
            [
                MakeStreamStepCard(LOCTEXT("StepCfg", "配置"),
                    LOCTEXT("StepCfgHint", "路径"), 0)
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("StepArrowA", "→"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ColorAndOpacity(ColorTextDim)
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f)
            [
                MakeStreamStepCard(LOCTEXT("StepId", "身份"),
                    LOCTEXT("StepIdHint", "制作并确认"), 1)
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("StepArrowB", "→"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ColorAndOpacity(ColorTextDim)
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f)
            [
                MakeStreamStepCard(LOCTEXT("StepListen", "监听"),
                    LOCTEXT("StepListenHint", "自动跑批"), 2)
            ]
        ]
        // 流程模式（三档：Auto 置灰 / Semi 默认 / Manual）
        + SVerticalBox::Slot().AutoHeight().Padding(0, 5)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).WidthOverride(72.0f).HAlign(HAlign_Left)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("StreamModeLabel", "模式"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorText)
                ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 0, 0).VAlign(VAlign_Center)
            [
                MakeStreamModeSelector()
            ]
        ]
        // ── 分割线（模式 ↔ 配置）──
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [ MakeStreamDivider() ]
        // ── 配置模块（原型样式：浅色标题条 + 表单常驻，不折叠）
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [
            MakeStreamModule(LOCTEXT("CfgModuleTitle", "配置"),
                SNew(SVerticalBox)
        // 热文件夹（磁盘 · 上环节同步目标）
        + SVerticalBox::Slot().AutoHeight().Padding(0, 5)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).WidthOverride(72.0f).HAlign(HAlign_Left)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("AutoInboxLabel", "加载路径"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorText)
                ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 4, 0).VAlign(VAlign_Center)
            [
                SAssignNew(AutoInboxEdit, SEditableTextBox)
                .HintText(LOCTEXT("AutoInboxHint", "监听源：上环节同步素材的目标文件夹（磁盘）"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ForegroundColor(FSlateColor(ColorText))
                .BackgroundColor(FSlateColor(ColorSurface))
                .Padding(FMargin(6, 2))
                // 路径改动即刷新绑定目标候选（候选 = 该路径下的子目录 am/ pm/…）——
                // 曾只在面板构造时刷新，改完路径下拉仍只有旧组的选项
                .OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type)
                {
                    RefreshBindGroupOptions();
                })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).HeightOverride(24.0f)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]()
                    {
                        BrowseDiskDir(AutoInboxEdit, TEXT("选择热文件夹（上环节同步目标）"));
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("AutoInboxBrowse", "浏览"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(ColorText)
                    ]
                ]
            ]
        ]
        // 身份导入（UE · ROM/ID 素材落位；留空=随素材导入根）
        + SVerticalBox::Slot().AutoHeight().Padding(0, 5)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).WidthOverride(72.0f).HAlign(HAlign_Left)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("AutoIdImportLabel", "身份导入"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorText)
                ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 4, 0).VAlign(VAlign_Center)
            [
                SAssignNew(AutoIdImportEdit, SEditableTextBox)
                .HintText(LOCTEXT("AutoIdImportHint", "ROM/ID 素材导入落位（留空=随素材导入路径）"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ForegroundColor(FSlateColor(ColorText))
                .BackgroundColor(FSlateColor(ColorSurface))
                .Padding(FMargin(6, 2))
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).HeightOverride(24.0f)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]()
                    {
                        BrowseContentPathUE(AutoIdImportEdit,
                            LOCTEXT("AutoIdImportPick", "选择身份导入目录（/Game 下）"));
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("AutoIdImportBrowse", "浏览"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(ColorText)
                    ]
                ]
            ]
        ]
        // 素材导入（UE · 默认 /Game/CaptureManager/Auto，⟲ 重置）
        + SVerticalBox::Slot().AutoHeight().Padding(0, 5)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).WidthOverride(72.0f).HAlign(HAlign_Left)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("AutoImportLabel", "素材导入"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorText)
                ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 4, 0).VAlign(VAlign_Center)
            [
                SAssignNew(AutoImportRootEdit, SEditableTextBox)
                .Text(LOCTEXT("AutoImportDefault", "/Game/CaptureManager/Auto"))
                .HintText(LOCTEXT("AutoImportHint", "素材导入的 UE 资产目录（/Game/...）"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ForegroundColor(FSlateColor(ColorText))
                .BackgroundColor(FSlateColor(ColorSurface))
                .Padding(FMargin(6, 2))
            ]
            // 浏览：UE 内容浏览器路径选择（/Game 下目录）
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).HeightOverride(24.0f)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]()
                    {
                        BrowseContentPathUE(AutoImportRootEdit, LOCTEXT("AutoImportPick", "选择导入目标目录（/Game 下）"));
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("AutoImportBrowse", "浏览"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(ColorText)
                    ]
                ]
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).HeightOverride(24.0f)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(6, 2))
                    .OnClicked_Lambda([this]()
                    {
                        if (AutoImportRootEdit.IsValid())
                        {
                            AutoImportRootEdit->SetText(FText::FromString(TEXT("/Game/CaptureManager/Auto")));
                        }
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("AutoImportReset", "⟲"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(ColorTextDim)
                    ]
                ]
            ]
        ]
        // FBX 输出路径（磁盘）
        + SVerticalBox::Slot().AutoHeight().Padding(0, 5)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).WidthOverride(72.0f).HAlign(HAlign_Left)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("AutoOutLabel", "导出路径"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorText)
                ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 4, 0).VAlign(VAlign_Center)
            [
                SAssignNew(AutoOutputEdit, SEditableTextBox)
                .HintText(LOCTEXT("AutoOutHint", "FBX 交付输出目录（磁盘）"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ForegroundColor(FSlateColor(ColorText))
                .BackgroundColor(FSlateColor(ColorSurface))
                .Padding(FMargin(6, 2))
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).HeightOverride(24.0f)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]()
                    {
                        BrowseDiskDir(AutoOutputEdit, TEXT("选择 FBX 输出目录"));
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("AutoOutBrowse", "浏览"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(ColorText)
                    ]
                ]
            ]
        ]
                // 高级区（UE 内部项说明 + 保存为默认）
                + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
                [
                    BuildConfigPanel()
                ],
                TAttribute<FText>::CreateLambda([this]()
                {
                    return FText::FromString(AutoInboxEdit.IsValid()
                        ? AutoInboxEdit->GetText().ToString().TrimStartAndEnd() : FString());
                }))
        ]
        // ── 分割线（配置 ↔ 状态）──
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [ MakeStreamDivider() ]
        // ── 状态模块（原型样式：状态灯 + 状态词 + [预检] [启动/停止监听]）──
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [
            MakeStreamModule(LOCTEXT("StatusModuleTitle", "状态"),
            SNew(SHorizontalBox)
            // 状态灯（未启动灰 / 运行中蓝）——属性绑定，状态变化自动刷新
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).WidthOverride(8.0f).HeightOverride(8.0f)
                [
                    SAssignNew(StreamStatusDot, SImage)
                    .Image(FCoreStyle::Get().GetBrush("WhiteBrush"))
                    .ColorAndOpacity_Lambda([this]()
                    {
                        return bAutoListening ? FSlateColor(ColorAccent) : FSlateColor(ColorTextDim);
                    })
                ]
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
            [
                SAssignNew(StreamStatusText, STextBlock)
                .Text_Lambda([this]()
                {
                    return StreamMode == 2
                        ? FText::FromString(TEXT("手动流程"))
                        : (bAutoListening ? FText::FromString(TEXT("运行中")) : FText::FromString(TEXT("未启动")));
                })
                .Font(FCoreStyle::GetDefaultFontStyle("SemiBold", 10))
                .ColorAndOpacity(ColorText)
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f)
            [
                SNew(SSpacer)
            ]
            // [预检]（独立动作——继承自深度Tab"一键预检"的使用习惯）
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
            [
                SNew(SBox).HeightOverride(26.0f)
                [
                    SAssignNew(PrecheckButton, SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(12, 3))
                    .OnClicked_Lambda([this]()
                    {
                        OnPrecheck();
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("StreamPrecheckBtn", "预检"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(ColorText)
                    ]
                ]
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SBox).HeightOverride(28.0f)
                [
                    SAssignNew(AutoStartStopButton, SButton)
                    .ButtonStyle(&StylePrimary)
                    .ContentPadding(FMargin(16, 4))
                    .OnClicked_Lambda([this]()
                    {
                        OnAutoStartStop();
                        return FReply::Handled();
                    })
                    [
                        SAssignNew(AutoStartStopLabel, STextBlock)
                        .Text_Lambda([this]()
                        {
                            // Manual=重置流程（步骤进度归零）；Semi=启动/停止监听（属性绑定自动刷新）
                            if (StreamMode == 2)
                            {
                                return FText::FromString(TEXT("重置"));
                            }
                            return bAutoListening
                                ? FText::FromString(TEXT("停止监听"))
                                : FText::FromString(TEXT("启动监听"));
                        })
                        .Font(FCoreStyle::GetDefaultFontStyle("SemiBold", 10))
                        .ColorAndOpacity(ColorWhite)
                    ]
                ]
            ]
            , TAttribute<FText>())
        ]

        // ── 分割线（状态 ↔ 身份）──
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [ MakeStreamDivider() ]
        // ── 身份模块：ID 状态 + 制作 + 分组 ──
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [
            MakeStreamModule(LOCTEXT("IdentityModuleTitle", "身份"),
                SNew(SVerticalBox)
                // ① 身份资产目录（原型：身份模块内，制作完成的 identity 放置处）
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(SBox).WidthOverride(72.0f).HAlign(HAlign_Left)
                        [
                            SNew(STextBlock)
                            .Text(LOCTEXT("IdAssetDirLabel", "资产目录"))
                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                            .ColorAndOpacity(ColorText)
                        ]
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 4, 0).VAlign(VAlign_Center)
                    [
                        SAssignNew(AutoIdAssetDirEdit, SEditableTextBox)
                        .HintText(LOCTEXT("AutoIdDirHint", "制作完成的 identity 资产目录（/Game/...）"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ForegroundColor(FSlateColor(ColorText))
                        .BackgroundColor(FSlateColor(ColorSurface))
                        .Padding(FMargin(6, 2))
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(SBox).HeightOverride(24.0f)
                        [
                            SNew(SButton)
                            .ButtonStyle(&StyleSecondary)
                            .ContentPadding(FMargin(8, 2))
                            .OnClicked_Lambda([this]()
                            {
                                BrowseContentPathUE(AutoIdAssetDirEdit,
                                    LOCTEXT("AutoIdDirPick", "选择身份资产目录（/Game 下）"));
                                return FReply::Handled();
                            })
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("AutoIdDirBrowse", "浏览"))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                .ColorAndOpacity(ColorText)
                            ]
                        ]
                    ]
                ]
                // ①b 身份资产（多身份场景的显式选择；留空=自动取目录内第一个）
                + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(SBox).WidthOverride(72.0f).HAlign(HAlign_Left)
                        [
                            SNew(STextBlock)
                            .Text(LOCTEXT("IdAssetLabel", "身份资产"))
                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                            .ColorAndOpacity(ColorText)
                        ]
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 4, 0).VAlign(VAlign_Center)
                    [
                        SAssignNew(AutoIdAssetEdit, SEditableTextBox)
                        .HintText(LOCTEXT("AutoIdAssetHint", "留空=自动取目录内第一个；多身份时填 /Game/ID/xxx"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ForegroundColor(FSlateColor(ColorText))
                        .BackgroundColor(FSlateColor(ColorSurface))
                        .Padding(FMargin(6, 2))
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(SBox).HeightOverride(24.0f)
                        [
                            // [▾ 选择]：列出工程内全部 MetaHumanIdentity 精确选择
                            // （原 [检测] 只取"资产目录内最新"——4 个身份的场景必错）
                            SAssignNew(IdAssetPicker, SComboButton)
                            .ComboButtonStyle(&FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>("ComboButton"))
                            .ButtonColorAndOpacity(FSlateColor(ColorSurface))
                            .ContentPadding(FMargin(8, 2))
                            .ToolTipText(LOCTEXT("IdPickTT", "列出工程内全部身份资产精确选择（显示目录/时间/大小；>10MB=已成型）"))
                            .OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
                            {
                                const TArray<FIdentityAssetInfo> Items = CollectIdentityAssets();
                                TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
                                if (Items.Num() == 0)
                                {
                                    Box->AddSlot().AutoHeight().Padding(8, 7)
                                    [
                                        SNew(STextBlock)
                                        .Text(LOCTEXT("IdPickEmpty", "（未找到 MetaHumanIdentity —— 先制作并保存）"))
                                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                        .ColorAndOpacity(ColorTextDim)
                                    ];
                                    return Box;
                                }
                                for (const FIdentityAssetInfo& It : Items)
                                {
                                    Box->AddSlot().AutoHeight()
                                    [
                                        SNew(SButton)
                                        .ButtonStyle(&StyleSecondary)
                                        .ContentPadding(FMargin(8, 5))
                                        .OnClicked_Lambda([this, Path = It.Path]()
                                        {
                                            if (AutoIdAssetEdit.IsValid())
                                            {
                                                AutoIdAssetEdit->SetText(FText::FromString(Path));
                                            }
                                            if (IdAssetPicker.IsValid())
                                            {
                                                IdAssetPicker->SetIsOpen(false);
                                            }
                                            AppendLog(FString::Printf(TEXT("[MHS_LOG] 已选身份资产: %s"), *Path), ColorTextDim);
                                            return FReply::Handled();
                                        })
                                        [
                                            SNew(SVerticalBox)
                                            + SVerticalBox::Slot().AutoHeight()
                                            [
                                                SNew(STextBlock)
                                                .Text(FText::FromString(It.Name))
                                                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
                                                .ColorAndOpacity(ColorText)
                                            ]
                                            + SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
                                            [
                                                SNew(STextBlock)
                                                .Text(FText::FromString(FString::Printf(
                                                    TEXT("%s · %s · %.0f MB%s"), *It.Dir, *It.Modified,
                                                    It.SizeKb / 1024.0,
                                                    It.SizeKb > 10000 ? TEXT("") : TEXT("（疑似空身份）"))))
                                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                                                .ColorAndOpacity(It.SizeKb > 10000
                                                    ? ColorTextDim : ColorLogWarn)
                                            ]
                                        ]
                                    ];
                                }
                                return Box;
                            })
                            .ButtonContent()
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("IdPickBtn", "▾ 选择"))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                .ColorAndOpacity(ColorText)
                            ]
                        ]
                    ]
                ]
                // ② ID 状态条（原型：醒目提示条——一眼知道身份处于哪一步）
                + SVerticalBox::Slot().AutoHeight().Padding(0, 7, 0, 0)
                [
                    SNew(SBorder)
                    .BorderImage(&BlackBrush())
                    .BorderBackgroundColor_Lambda([this]()
                    {
                        return (!StreamIdStateText.IsEmpty() && StreamIdStateText.Contains(TEXT("已绑定")))
                            ? FLinearColor(0.10f, 0.30f, 0.16f)               // 已绑定=绿调
                            : FLinearColor(0.28f, 0.22f, 0.06f);              // 待处理=琥珀调
                    })
                    .Padding(FMargin(8, 5))
                    [
                        SAssignNew(StreamCurrentText, STextBlock)
                        .Text_Lambda([this]()
                        {
                            return FText::FromString(TEXT("ID 状态：")
                                + (StreamIdStateText.IsEmpty() ? TEXT("未就绪") : StreamIdStateText));
                        })
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(ColorText)
                    ]
                ]
                // ③ 身份绑定操作
                //    ★ [导入项目] 已删除：它是同步阻塞导入（游戏线程独占→面板冻结"卡死"），
                //      且功能冗余——ID 素材由监听自动发现导入（幂等）。
                //    Manual 模式下隐藏（Manual 有独立步骤行，避免两个"确认"重名）
                + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                [
                    SNew(SBox)
                    .Visibility_Lambda([this]()
                    {
                        return StreamMode == 2 ? EVisibility::Collapsed : EVisibility::Visible;
                    })
                    [
                    SAssignNew(IdMakeBox, SBox)
                    [
                        SNew(SVerticalBox)
                            // 绑定目标 + 右侧竖排操作钮（确认绑定在上 / 清除绑定在下）
                            + SVerticalBox::Slot().AutoHeight()
                            [
                                SNew(SHorizontalBox)
                                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                                [
                                    SNew(SBox).WidthOverride(72.0f).HAlign(HAlign_Left)
                                    [
                                        SNew(STextBlock)
                                        .Text(LOCTEXT("IdBindTargetLabel", "绑定目标"))
                                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                        .ColorAndOpacity(ColorText)
                                    ]
                                ]
                                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 6, 0).VAlign(VAlign_Center)
                                [
                                    SAssignNew(BindGroupCombo, SComboBox<TSharedPtr<FString>>)
                                    .OptionsSource(&BindGroupOptions)
                                    .OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
                                    {
                                        return SNew(STextBlock)
                                            .Text(FText::FromString(Item.IsValid() ? *Item : TEXT("")))
                                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10));
                                    })
                                    .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
                                    {
                                        SelectedBindGroup = Item.IsValid() ? *Item : FString();
                                        bBindGroupUserPicked = true;   // 用户已显式选择 → 不再被自动推断覆盖
                                    })
                                    [
                                        SNew(STextBlock)
                                        .Text_Lambda([this]()
                                        {
                                            return FText::FromString(SelectedBindGroup.IsEmpty()
                                                ? TEXT("(根 · 全部)") : SelectedBindGroup);
                                        })
                                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                        .ColorAndOpacity(ColorText)
                                    ]
                                ]
                                // 右侧竖排：确认绑定（主）/ 清除绑定（次）——不再横排长条
                                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                                [
                                    SNew(SVerticalBox)
                                    + SVerticalBox::Slot().AutoHeight()
                                    [
                                        SNew(SButton)
                                        .ButtonStyle(&StylePrimary)
                                        .ContentPadding(FMargin(10, 2))
                                        .ToolTipText(LOCTEXT("IdBindTT", "把身份资产绑定给选定分组（该组素材入队即用此身份）"))
                                        .OnClicked_Lambda([this]()
                                        {
                                            OnBindIdentity();
                                            return FReply::Handled();
                                        })
                                        [
                                            SNew(STextBlock)
                                            .Text(LOCTEXT("IdMakeConfirm", "确认绑定"))
                                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                                            .ColorAndOpacity(ColorWhite)
                                        ]
                                    ]
                                    + SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)
                                    [
                                        SNew(SButton)
                                        .ButtonStyle(&StyleSecondary)
                                        .ContentPadding(FMargin(10, 2))
                                        .ToolTipText(LOCTEXT("IdClearTT", "清除选中分组的身份绑定（选“(根 · 全部)”=清除根组绑定）"))
                                        .OnClicked_Lambda([this]()
                                        {
                                            OnClearBinding();
                                            return FReply::Handled();
                                        })
                                        [
                                            SNew(STextBlock)
                                            .Text(LOCTEXT("IdClearBtn", "清除绑定"))
                                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                                            .ColorAndOpacity(ColorText)
                                        ]
                                    ]
                                ]
                            ]
                            // 来源提示：让"自动识别"可见、可核对
                            + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
                            [
                                SAssignNew(IdSourceText, STextBlock)
                                .Text_Lambda([this]()
                                {
                                    return FText::FromString(IdSourceLine.IsEmpty()
                                        ? TEXT("来源：—（启动监听后自动导入 ID 素材）") : IdSourceLine);
                                })
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                                .ColorAndOpacity(ColorTextDim)
                            ]
                    ]
                    ]
                ]
                // 身份分组（显式绑定——继承自深度Tab"任务行=身份↔素材绑定单元"的核心设计）
                + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("IdGroupTitle", "身份分组"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(ColorText)
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)
                    [
                        SAssignNew(GroupListBox, SVerticalBox)
                    ]
                    // 空态由 RebuildGroupList 提供（"暂无分组——素材入队后显示"/待绑定组行）
                ]
                // 异常行（仅失败时出现——常态不占空间，符合信息分层 L2）
                + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
                [
                    SNew(SBox)
                    .Visibility_Lambda([this]()
                    {
                        return StreamFailCount > 0 ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    [
                        SNew(SBorder)
                        .BorderImage(&BlackBrush())
                        .BorderBackgroundColor(ColorSurface)
                        .Padding(FMargin(8, 4))
                        [
                            SAssignNew(AutoFailText, STextBlock)
                            .Text_Lambda([this]()
                            {
                                return FText::FromString(
                                    FString::Printf(TEXT("%d 条未交付 · 详情见事件面板"), StreamFailCount));
                            })
                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                            .ColorAndOpacity(ColorLogError)
                        ]
                    ]
                ]
                // Manual 步骤按钮行（切到 Manual 即显示，无需先点开始；严格顺序：仅当前步可点）
                + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                [
                    SNew(SBox)
                    .Visibility_Lambda([this]()
                    {
                        return (StreamMode == 2)
                            ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 6, 0)
                        [
                            SAssignNew(StageActionButtons[0], SButton)
                            .ButtonStyle(&StyleSecondary)
                            .IsEnabled_Lambda([this]()
                            {
                                return StreamMode == 2 && ManualStageProgress == 0;
                            })
                            .OnClicked_Lambda([this]()
                            {
                                OnManualStageTrigger(0);
                                return FReply::Handled();
                            })
                            .ContentPadding(FMargin(8, 3))
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("ManualStage0", "制作身份"))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                .ColorAndOpacity(ColorText)
                            ]
                        ]
                        + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 6, 0)
                        [
                            SAssignNew(StageActionButtons[1], SButton)
                            .ButtonStyle(&StyleSecondary)
                            .IsEnabled_Lambda([this]()
                            {
                                return StreamMode == 2 && ManualStageProgress == 1;
                            })
                            .OnClicked_Lambda([this]()
                            {
                                OnManualStageTrigger(1);
                                return FReply::Handled();
                            })
                            .ContentPadding(FMargin(8, 3))
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("ManualStage1", "导入"))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                .ColorAndOpacity(ColorText)
                            ]
                        ]
                        + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 6, 0)
                        [
                            SAssignNew(StageActionButtons[2], SButton)
                            .ButtonStyle(&StyleSecondary)
                            .IsEnabled(false)
                            .ToolTipText(LOCTEXT("ManualStage2TT", "解算接线随 S2 引擎层接入"))
                            .ContentPadding(FMargin(8, 3))
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("ManualStage2", "解算"))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                .ColorAndOpacity(ColorTextDim)
                            ]
                        ]
                        + SHorizontalBox::Slot().FillWidth(1.0f)
                        [
                            SAssignNew(StageActionButtons[3], SButton)
                            .ButtonStyle(&StyleSecondary)
                            .IsEnabled(false)
                            .ToolTipText(LOCTEXT("ManualStage3TT", "导出接线随 S2 引擎层接入"))
                            .ContentPadding(FMargin(8, 3))
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("ManualStage3", "导出"))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                .ColorAndOpacity(ColorTextDim)
                            ]
                        ]
                        // [确认完成]：真实确认后才进位（不自动进位——Manual 的核心语义）
                        + SHorizontalBox::Slot().FillWidth(1.0f).Padding(6, 0, 0, 0)
                        [
                            SAssignNew(ConfirmStageButton, SButton)
                            .ButtonStyle(&StyleSecondary)
                            .IsEnabled_Lambda([this]()
                            {
                                // ③ ④ 随 S2 接入后开放；当前仅 ① ② 可确认
                                return StreamMode == 2 && ManualStageProgress < 2;
                            })
                            .ToolTipText(LOCTEXT("ConfirmStageTT", "确认本步已真实完成，才推进到下一步"))
                            .OnClicked_Lambda([this]()
                            {
                                OnConfirmStage();
                                return FReply::Handled();
                            })
                            .ContentPadding(FMargin(8, 3))
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("ConfirmStage", "确认完成"))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                .ColorAndOpacity(ColorText)
                            ]
                        ]
                    ]
                ],
                TAttribute<FText>())
        ]
        // 日志模块已移除：底部共享日志区（日志 + 每段结果）已覆盖该职责，避免重复
        ;
}

// ── L3 按需面板：配置 / 事件 / 交付清单 ──

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildConfigPanel()
{
    // 配置模块尾部：仅保留 [保存为默认]（原"高级"标题与过时说明已删——无实际高级项）
    return SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(SButton)
                .ButtonStyle(&StyleSecondary)
                .ContentPadding(FMargin(12, 3))
                .ToolTipText(LOCTEXT("SaveDefaultTT", "保存当前配置为默认值，下次启动自动回填"))
                .OnClicked_Lambda([this]()
                {
                    SaveStreamPanelConfig();
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("SaveDefaultBtn", "保存为默认"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorText)
                ]
            ]
        ;
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildEventPanel()
{
    // 事件：倒序（最新在顶部，不用滚到底）+ 筛选（全部/失败/待处理）
    return SNew(SExpandableArea)
        .AreaTitle(LOCTEXT("EventPanelTitle", "事件"))
        .InitiallyCollapsed(true)
        .Padding(FMargin(0, 4))
        .BodyContent()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]() { SetEventFilter(0); return FReply::Handled(); })
                    [ SNew(STextBlock).Text(LOCTEXT("EvFilterAll", "全部")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(ColorText) ]
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]() { SetEventFilter(1); return FReply::Handled(); })
                    [ SNew(STextBlock).Text(LOCTEXT("EvFilterFail", "失败")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(ColorText) ]
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]() { SetEventFilter(2); return FReply::Handled(); })
                    [ SNew(STextBlock).Text(LOCTEXT("EvFilterPending", "待处理")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(ColorText) ]
                ]
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SAssignNew(AutoEventLog, SMultiLineEditableTextBox)
                .Text(LOCTEXT("AutoEventEmpty", "（暂无事件）"))
                .IsReadOnly(true)
                .Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
                .ForegroundColor(FSlateColor(ColorTextDim))
                .BackgroundColor(FSlateColor(ColorSurface))
                .Padding(FMargin(6, 4))
            ]
        ];
}

TSharedRef<SWidget> SMetaHumanSolverWindow::BuildManifestPanel()
{
    // 清单 = 对账表（汇总在顶部 + 未交付筛选 + 打开输出文件夹）
    return SNew(SExpandableArea)
        .AreaTitle(LOCTEXT("ManifestPanelTitle", "交付清单"))
        .InitiallyCollapsed(true)
        .Padding(FMargin(0, 4))
        .BodyContent()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("ManifestSummary", "已拍 — · 交付 — · 失败 — · 待处理 —"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                    .ColorAndOpacity(ColorText)
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(10, 2))
                    .OnClicked_Lambda([this]() { OnOpenOutputDir(); return FReply::Handled(); })
                    [ SNew(STextBlock).Text(LOCTEXT("OpenOutBtn", "打开输出")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(ColorText) ]
                ]
            ]
            // 筛选：全部 / 失败 / 待处理 / 未交付（收工对账的核心入口）
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]() { SetManifestFilter(0); return FReply::Handled(); })
                    [ SNew(STextBlock).Text(LOCTEXT("MfAll", "全部")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(ColorText) ]
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]() { SetManifestFilter(1); return FReply::Handled(); })
                    [ SNew(STextBlock).Text(LOCTEXT("MfFail", "失败")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(ColorText) ]
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]() { SetManifestFilter(2); return FReply::Handled(); })
                    [ SNew(STextBlock).Text(LOCTEXT("MfPending", "待处理")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(ColorText) ]
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .ButtonStyle(&StyleSecondary)
                    .ContentPadding(FMargin(8, 2))
                    .OnClicked_Lambda([this]() { SetManifestFilter(3); return FReply::Handled(); })
                    [ SNew(STextBlock).Text(LOCTEXT("MfMissing", "未交付")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(ColorText) ]
                ]
            ]
            + SVerticalBox::Slot().FillHeight(1.0f)
            [
                SAssignNew(ManifestEdit, SMultiLineEditableTextBox)
                .Text(LOCTEXT("ManifestEmpty", "（对账表随 S2 引擎层接入；每完成一条实时追加）"))
                .IsReadOnly(true)
                .Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
                .ForegroundColor(FSlateColor(ColorTextDim))
                .BackgroundColor(FSlateColor(ColorSurface))
                .Padding(FMargin(6, 4))
            ]
        ];
}

void SMetaHumanSolverWindow::OnPrecheck()
{
    // [预检] 独立动作（继承自深度Tab"一键预检"）：生成 job_stream.json + 素材盘点（秒级，不改状态）
    const FString Inbox = AutoInboxEdit.IsValid() ? AutoInboxEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (Inbox.IsEmpty())
    {
        AppendLog(TEXT("[MHS_ERROR] 请先填写热文件夹"), ColorLogError);
        return;
    }
    const FString JobJson = BuildStreamJobJson();
    const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job_stream.json");
    if (JobJson.IsEmpty() || !FFileHelper::SaveStringToFile(JobJson, *JobPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        AppendLog(FString::Printf(TEXT("[MHS_ERROR] 无法写入 job_stream.json: %s"), *JobPath), ColorLogError);
        return;
    }
    PushStreamEvent(TEXT("信息"), FString::Printf(TEXT("预检热文件夹 %s"), *Inbox));
    AppendLog(FString::Printf(TEXT("[MHS_LOG] Stream 预检: %s"), *Inbox), ColorTextDim);
    FString Escaped = JobPath;
    Escaped.ReplaceInline(TEXT("\\"), TEXT("/"));
    Escaped.ReplaceInline(TEXT("'"), TEXT("\\'"));
    const FString Cmd = FString::Printf(
        TEXT("py import MetaHumanSolverEngine.bridge; MetaHumanSolverEngine.bridge.precheck_import(r'%s')"), *Escaped);
    GEngine->Exec(nullptr, *Cmd);
}

void SMetaHumanSolverWindow::PushStreamEvent(const FString& Level, const FString& Text)
{
    // 结构化事件（倒序存储：新事件插到数组头，视图最新在顶部）
    FStreamEvent Ev;
    Ev.Time = FDateTime::Now().ToString(TEXT("%H:%M:%S"));
    Ev.Level = Level;
    Ev.Text = Text;
    StreamEvents.Insert(Ev, 0);
    if (StreamEvents.Num() > 200)
    {
        StreamEvents.SetNum(200);
    }
    RefreshEventView();
}

void SMetaHumanSolverWindow::SetEventFilter(int32 Filter)
{
    EventFilter = Filter;
    RefreshEventView();
}

void SMetaHumanSolverWindow::RefreshEventView()
{
    if (!AutoEventLog.IsValid())
    {
        return;
    }
    FString Out;
    for (const FStreamEvent& Ev : StreamEvents)
    {
        // 筛选：1=失败 2=待处理
        if (EventFilter == 1 && Ev.Level != TEXT("失败")) { continue; }
        if (EventFilter == 2 && Ev.Level != TEXT("待处理")) { continue; }
        Out += FString::Printf(TEXT("%s  %-4s  %s\n"), *Ev.Time, *Ev.Level, *Ev.Text);
    }
    AutoEventLog->SetText(FText::FromString(Out.IsEmpty() ? TEXT("（无匹配事件）") : Out));
}

void SMetaHumanSolverWindow::OnConfirmStage()
{
    if (StreamMode == 0)
    {
        return;   // Auto 置灰不可操作
    }

    if (StreamMode == 2)
    {
        // Manual：步骤推进（点了确认才进位，不自动进位）
        if (ManualStageProgress >= 2)
        {
            return;
        }
        const int32 Stage = ManualStageProgress;
        ++ManualStageProgress;
        PushStreamEvent(TEXT("完成"), FString::Printf(TEXT("步骤 %d 已确认完成，下一步 %d"), Stage + 1, ManualStageProgress + 1));
        AppendLog(FString::Printf(TEXT("[MHS_LOG] Manual 步骤 %d 已确认完成"), Stage + 1), ColorTextDim);
        return;
    }

    // ── Semi：身份就绪确认（先校验资产真实存在——状态诚实，不允许空确认）──
    // 身份资产解析：显式填写的优先（多身份场景——am/pm 各一个身份时必须指定），
    // 留空则自动取目录内第一个（单身份场景零操作）。
    const FString IdDir = AutoIdAssetDirEdit.IsValid()
        ? AutoIdAssetDirEdit->GetText().ToString().TrimStartAndEnd() : FString();
    const FString AssetField = AutoIdAssetEdit.IsValid()
        ? AutoIdAssetEdit->GetText().ToString().TrimStartAndEnd() : FString();
    FString Found;
    if (!AssetField.IsEmpty())
    {
        if (!IsIdentityAssetAt(AssetField))
        {
            AppendLog(FString::Printf(
                TEXT("[MHS_ERROR] 指定的身份资产不存在或不是 MetaHumanIdentity：%s"), *AssetField), ColorLogError);
            PushStreamEvent(TEXT("失败"), FString::Printf(TEXT("身份确认失败：资产无效 %s"), *AssetField));
            return;
        }
        Found = AssetField;
    }
    else
    {
        Found = FindIdentityInDir(IdDir);
    }
    if (Found.IsEmpty())
    {
        AppendLog(TEXT("[MHS_ERROR] ID资产目录下未找到 MetaHumanIdentity 资产，无法确认完成"), ColorLogError);
        PushStreamEvent(TEXT("失败"), TEXT("身份确认失败：目录内无 identity 资产"));
        return;
    }
    if (AutoIdAssetEdit.IsValid())
    {
        AutoIdAssetEdit->SetText(FText::FromString(Found));   // 回填：明确显示本次绑定的身份
    }
    bSemiIdentityReady = true;
    SemiIdentityPath = Found;

    // 绑定写入引擎层（批次5）：活跃组 → 该身份（无活跃组绑根组作兜底；
    // pending 池内该组素材即刻冲刷入队——上下午归属由"入队时快照"保证）
    const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job_stream.json");
    if (FPaths::FileExists(JobPath))
    {
        FString EscapedJob = JobPath;
        EscapedJob.ReplaceInline(TEXT("\\"), TEXT("/"));
        EscapedJob.ReplaceInline(TEXT("'"), TEXT("\\'"));
        FString EscapedId = Found;
        EscapedId.ReplaceInline(TEXT("'"), TEXT("\\'"));
        const FString BindCmd = FString::Printf(
            TEXT("py import MetaHumanSolverEngine.bridge; MetaHumanSolverEngine.bridge.stream_set_binding(r'%s', '%s', '%s')"),
            *EscapedJob, *StreamActiveGroup, *EscapedId);
        GEngine->Exec(nullptr, *BindCmd);
    }
    else
    {
        AppendLog(TEXT("[MHS_WARN] job_stream.json 不存在（先点[启动监听]再确认），本次仅本地记录身份"), ColorLogWarn);
    }

    if (QueueStatValues.IsValidIndex(0) && QueueStatValues[0].IsValid())
    {
        QueueStatValues[0]->SetText(FText::FromString(TEXT("1")));
        QueueStatValues[0]->SetColorAndOpacity(FSlateColor(ColorSuccess));
    }
    PushStreamEvent(TEXT("完成"), FString::Printf(TEXT("身份已就绪：%s（绑定分组 '%s'）"), *Found, *StreamActiveGroup));
    AppendLog(FString::Printf(TEXT("[MHS_LOG] Semi 身份已就绪：%s（已绑定分组 '%s'）"), *Found, *StreamActiveGroup), ColorTextDim);
}

bool SMetaHumanSolverWindow::IsIdentityAssetAt(const FString& AssetPath) const
{
    // 精确校验：该包路径下确有 MetaHumanIdentity 资产。
    // 用 PackageNames（精确包名匹配）——曾用 PackagePaths + 非递归：资产注册表
    // 把 PackagePaths 当"目录前缀"处理，精确的资产包路径（无尾随 /）匹配不上，
    // 导致真实存在的身份资产被误判"无效"（用户 [确认绑定] 被拦）。
    if (AssetPath.IsEmpty())
    {
        return false;
    }
    IAssetRegistry* Registry = IAssetRegistry::Get();
    if (!Registry)
    {
        return false;
    }
    FARFilter Filter;
    Filter.PackageNames.Add(FName(*AssetPath));
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/MetaHumanIdentity"), TEXT("MetaHumanIdentity")));
    TArray<FAssetData> Assets;
    Registry->GetAssets(Filter, Assets);
    return Assets.Num() > 0;
}

TArray<SMetaHumanSolverWindow::FIdentityAssetInfo> SMetaHumanSolverWindow::CollectIdentityAssets() const
{
    // [▾ 选择] 的数据源：工程内全部 MetaHumanIdentity（注册表内存查询，非磁盘扫描）
    // 与 asset_index.py（外部消费者用）同源同语义：名称/路径/目录/时间/大小。
    TArray<FIdentityAssetInfo> Out;
    IAssetRegistry* Registry = IAssetRegistry::Get();
    if (!Registry)
    {
        return Out;
    }
    FARFilter Filter;
    Filter.PackagePaths.Add(TEXT("/Game"));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/MetaHumanIdentity"), TEXT("MetaHumanIdentity")));
    TArray<FAssetData> Assets;
    Registry->GetAssets(Filter, Assets);
    for (const FAssetData& A : Assets)
    {
        FIdentityAssetInfo Info;
        Info.Path = A.PackageName.ToString();
        Info.Name = A.AssetName.ToString();
        Info.Dir = FPackageName::GetLongPackagePath(Info.Path);
        const FString File = FPackageName::LongPackageNameToFilename(
            Info.Path, FPackageName::GetAssetPackageExtension());
        const FDateTime Ts = IFileManager::Get().GetTimeStamp(*File);
        const int64 Size = IFileManager::Get().FileSize(*File);
        Info.Modified = Ts.ToString(TEXT("%m-%d %H:%M"));
        Info.ModifiedTs = static_cast<double>(Ts.ToUnixTimestamp());
        Info.SizeKb = Size > 0 ? Size / 1024 : 0;
        Out.Add(Info);
    }
    Out.Sort([](const FIdentityAssetInfo& A, const FIdentityAssetInfo& B)
    {
        return A.ModifiedTs > B.ModifiedTs;      // 最新在前
    });
    return Out;
}

FString SMetaHumanSolverWindow::FindIdentityInDir(const FString& Dir) const
{
    // 在 ID 资产目录下查找 MetaHumanIdentity（供 Semi 确认完成 / 打开编辑器使用）
    if (Dir.IsEmpty())
    {
        return FString();
    }
    IAssetRegistry* Registry = IAssetRegistry::Get();
    if (!Registry)
    {
        return FString();
    }
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Dir));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/MetaHumanIdentity"), TEXT("MetaHumanIdentity")));
    TArray<FAssetData> Assets;
    Registry->GetAssets(Filter, Assets);
    if (Assets.Num() == 0)
    {
        return FString();
    }
    // 取"最新修改"的一个（同组二次绑定 → 自动用刚做的身份）；
    // 旧实现返回 Assets[0]（注册表顺序不确定）——多身份目录下会随机取错
    auto TimeOf = [](const FAssetData& D) -> FDateTime
    {
        const FString File = FPackageName::LongPackageNameToFilename(
            D.PackageName.ToString(), FPackageName::GetAssetPackageExtension());
        return IFileManager::Get().GetTimeStamp(*File);
    };
    Assets.Sort([&TimeOf](const FAssetData& A, const FAssetData& B)
    {
        return TimeOf(A) < TimeOf(B);
    });
    return Assets.Last().PackageName.ToString();
}

void SMetaHumanSolverWindow::OnImportIdFootage()
{
    // ① 导入项目：把「加载路径/_id」下的 ROM 素材导入 UE
    //    （与监听自动导入同源——不再单独配置 ROM 目录；落位=配置区的「身份导入」）
    const FString Inbox = AutoInboxEdit.IsValid()
        ? AutoInboxEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (Inbox.IsEmpty())
    {
        AppendLog(TEXT("[MHS_ERROR] 请先在配置区填写加载路径"), ColorLogError);
        PushStreamEvent(TEXT("失败"), TEXT("导入项目失败：加载路径为空"));
        return;
    }
    FString Rom = (Inbox / TEXT("_id")).Replace(TEXT("\\"), TEXT("/"));
    if (!FPaths::DirectoryExists(Rom))
    {
        // 兼容：ID 素材直接放在加载路径下（非 _id 子目录）
        Rom = Inbox;
    }
    if (!FPaths::DirectoryExists(Rom))
    {
        AppendLog(FString::Printf(TEXT("[MHS_ERROR] 未找到 ID 素材目录（期望 %s/_id）"), *Inbox), ColorLogError);
        PushStreamEvent(TEXT("失败"), TEXT("导入项目失败：未找到 ID 素材"));
        return;
    }
    FString ImportRoot = AutoIdImportEdit.IsValid()
        ? AutoIdImportEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (ImportRoot.IsEmpty())
    {
        ImportRoot = AutoImportRootEdit.IsValid()
            ? AutoImportRootEdit->GetText().ToString().TrimStartAndEnd() : FString();
    }
    if (ImportRoot.IsEmpty())
    {
        ImportRoot = TEXT("/Game/CaptureManager/Auto");
    }
    while (ImportRoot.Len() > 1 && ImportRoot.EndsWith(TEXT("/")))
    {
        ImportRoot = ImportRoot.LeftChop(1);
    }
    const FString MediaRoot = (FPaths::ProjectSavedDir() / TEXT("MetaHumanSolver/MediaOut"))
        .Replace(TEXT("\\"), TEXT("/"));
    const FString JobJson = FString::Printf(TEXT(
        "{\n"
        "  \"mode\": \"depth\",\n"
        "  \"identity_path\": \"\",\n"
        "  \"capture_root\": \"%s\",\n"
        "  \"import_mode\": \"cpp\",\n"
        "  \"import_footage\": true,\n"
        "  \"footage_source_dir\": \"%s\",\n"
        "  \"media_output_root\": \"%s\",\n"
        "  \"import_only\": true,\n"
        "  \"exclude\": []\n"
        "}"),
        *JsonEscape(ImportRoot), *JsonEscape(Rom), *JsonEscape(MediaRoot));
    const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job_stream_id.json");
    if (!FFileHelper::SaveStringToFile(JobJson, *JobPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        AppendLog(FString::Printf(TEXT("[MHS_ERROR] 无法写入 job_stream_id.json: %s"), *JobPath), ColorLogError);
        return;
    }
    PushStreamEvent(TEXT("信息"), FString::Printf(TEXT("ROM 导入中：%s"), *Rom));
    AppendLog(FString::Printf(TEXT("[MHS_LOG] Stream ID 导入：%s → %s"), *Rom, *ImportRoot), ColorTextDim);
    FString Escaped = JobPath;
    Escaped.ReplaceInline(TEXT("\\"), TEXT("/"));
    Escaped.ReplaceInline(TEXT("'"), TEXT("\\'"));
    const FString Cmd = FString::Printf(
        TEXT("py import MetaHumanSolverEngine.bridge; MetaHumanSolverEngine.bridge.run(r'%s')"), *Escaped);
    GEngine->Exec(nullptr, *Cmd);
    PushStreamEvent(TEXT("完成"), TEXT("ROM 导入已执行，详见日志"));
}

void SMetaHumanSolverWindow::OnOpenIdentityEditor()
{
    // ② 打开编辑器：真的打开 Identity 编辑器（目录内无身份资产时先创建）
    const FString IdDir = AutoIdAssetDirEdit.IsValid()
        ? AutoIdAssetDirEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (IdDir.IsEmpty())
    {
        AppendLog(TEXT("[MHS_ERROR] 请先填写 ID资产目录"), ColorLogError);
        PushStreamEvent(TEXT("失败"), TEXT("打开编辑器失败：ID资产目录未填写"));
        return;
    }

    FString Path = FindIdentityInDir(IdDir);
    if (Path.IsEmpty())
    {
        // 目录内无 Identity → 用 Python 创建（避免新增 C++ 模块依赖；API 已在 T1 探针验证）
        const FString Name = FString::Printf(TEXT("ID_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d")));
        const FString PyCmd = FString::Printf(
            TEXT("py import unreal; unreal.AssetToolsHelpers.get_asset_tools().create_asset("
                 "asset_name='%s', package_path='%s', asset_class=unreal.MetaHumanIdentity, "
                 "factory=unreal.MetaHumanIdentityFactoryNew())"),
            *Name, *IdDir);
        GEngine->Exec(nullptr, *PyCmd);
        Path = FindIdentityInDir(IdDir);
        if (!Path.IsEmpty())
        {
            PushStreamEvent(TEXT("信息"), FString::Printf(TEXT("已创建 identity 资产：%s"), *Path));
            AppendLog(FString::Printf(TEXT("[MHS_LOG] 已创建 identity：%s"), *Path), ColorTextDim);
        }
    }
    if (Path.IsEmpty())
    {
        AppendLog(TEXT("[MHS_ERROR] 无法创建/定位 identity 资产，请检查 ID资产目录"), ColorLogError);
        PushStreamEvent(TEXT("失败"), TEXT("打开编辑器失败：无法创建 identity"));
        return;
    }

    UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
    if (!Asset)
    {
        AppendLog(FString::Printf(TEXT("[MHS_ERROR] 无法加载 identity：%s"), *Path), ColorLogError);
        PushStreamEvent(TEXT("失败"), TEXT("打开编辑器失败：资产加载失败"));
        return;
    }
    if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
    {
        Subsystem->OpenEditorForAsset(Asset);
        PushStreamEvent(TEXT("信息"), TEXT("已打开 Identity 编辑器——制作完成后点 [确认完成]"));
    }
}

void SMetaHumanSolverWindow::OnOpenOutputDir()
{
    // 可达性：打开 FBX 输出目录
    const FString Out = AutoOutputEdit.IsValid() ? AutoOutputEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (Out.IsEmpty())
    {
        AppendLog(TEXT("[MHS_ERROR] 请先填写输出路径"), ColorLogError);
        return;
    }
    FPlatformProcess::ExploreFolder(*Out);
    PushStreamEvent(TEXT("信息"), FString::Printf(TEXT("打开输出目录 %s"), *Out));
}

void SMetaHumanSolverWindow::SetManifestFilter(int32 Filter)
{
    ManifestFilter = Filter;
    RefreshManifestView();
}

void SMetaHumanSolverWindow::RefreshManifestView()
{
    // 清单 = 对账表：按状态筛选渲染（未交付 = 非"已交付"状态的全部条目）
    if (!ManifestEdit.IsValid())
    {
        return;
    }
    if (ManifestLines.Num() == 0)
    {
        ManifestEdit->SetText(FText::FromString(TEXT("（对账表随 S2 引擎层接入；每完成一条实时追加）")));
        return;
    }
    FString Out;
    for (const FString& Line : ManifestLines)
    {
        FString Status;
        {
            FString Left;
            Line.Split(TEXT("|"), &Left, nullptr);
            Status = Left;
        }
        bool bMatch = true;
        if (ManifestFilter == 1) { bMatch = (Status == TEXT("失败")); }
        else if (ManifestFilter == 2) { bMatch = (Status == TEXT("待处理")); }
        else if (ManifestFilter == 3) { bMatch = (Status != TEXT("已交付")); }
        if (!bMatch) { continue; }
        FString Rest = Line;
        {
            FString Left, Right;
            if (Line.Split(TEXT("|"), &Left, &Right)) { Rest = Right; }
        }
        Out += FString::Printf(TEXT("%-6s %s\n"), *Status, *Rest.Replace(TEXT("|"), TEXT("  ")));
    }
    ManifestEdit->SetText(FText::FromString(Out.IsEmpty() ? TEXT("（无匹配条目）") : Out));
}

void SMetaHumanSolverWindow::OnIdCardClicked()
{
    // ID 卡点击 → 展开/收起 ID 制作区（规范：ID 为 0 时卡片可点）
    if (!IdMakeBox.IsValid())
    {
        return;
    }
    const bool bVisible = IdMakeBox->GetVisibility() == EVisibility::Visible;
    IdMakeBox->SetVisibility(bVisible ? EVisibility::Collapsed : EVisibility::Visible);
    if (!bVisible)
    {
        PushStreamEvent(TEXT("信息"), TEXT("展开 ID 制作：导入项目 → 编辑器制作 → 确认完成"));
    }
}

TSharedRef<SWidget> SMetaHumanSolverWindow::MakeStageCard(const FText& Abbr, int32 StageIndex, TSharedPtr<STextBlock>& OutValue)
{
    // 四步卡：[简写 + 模式徽标] / 计数。徽标用属性绑定——模式切换时自动刷新，无需手动 Update。
    // ID 卡（StageIndex 0）可点击——点开展开 ID 制作区（规范：ID 为 0 时卡片可点）
    TSharedRef<SWidget> Body =
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 5, 0)
                [
                    SNew(STextBlock)
                    .Text(Abbr)
                    .Font(FCoreStyle::GetDefaultFontStyle("SemiBold", 11))
                    .ColorAndOpacity(ColorText)
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text_Lambda([this, StageIndex]()
                    {
                        return IsStageManual(StageIndex)
                            ? FText::FromString(TEXT("MANUAL"))
                            : FText::FromString(TEXT("AUTO"));
                    })
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                    .ColorAndOpacity(FLinearColor(0.45f, 0.45f, 0.48f, 1.0f))
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 2, 0, 0)
            [
                SAssignNew(OutValue, STextBlock)
                .Text(FText::FromString(TEXT("-")))
                .Font(FCoreStyle::GetDefaultFontStyle("SemiBold", 13))
                .ColorAndOpacity(ColorTextDim)
            ];

    if (StageIndex == 0)
    {
        // ID 卡：外框与其他卡一致，内部可点
        return SNew(SBorder)
            .BorderImage(&BlackBrush())
            .BorderBackgroundColor(ColorSurface)
            [
                SNew(SButton)
                .ButtonStyle(&StyleSecondary)
                .ContentPadding(FMargin(8, 5))
                .OnClicked_Lambda([this]()
                {
                    OnIdCardClicked();
                    return FReply::Handled();
                })
                [ Body ]
            ];
    }
    return SNew(SBorder)
        .BorderImage(&BlackBrush())
        .BorderBackgroundColor(ColorSurface)
        .Padding(FMargin(8, 5))
        [ Body ];
}

bool SMetaHumanSolverWindow::IsStageManual(int32 StageIndex) const
{
    // 模式位图语义：Manual=全手动；Semi=仅 ID(0) 手动；Auto=全自动（当前置灰不可选）
    if (StreamMode == 2)
    {
        return true;
    }
    if (StreamMode == 1)
    {
        return StageIndex == 0;
    }
    return false;
}

TSharedRef<SWidget> SMetaHumanSolverWindow::MakeStreamModeSelector()
{
    // 流程模式三档：Auto（置灰，待 T3 + 牙齿桥验证后开放）/ Semi（默认）/ Manual
    auto MakeModeButton = [this](int32 ModeIdx, const FText& Label, bool bEnabled, const FText& Tooltip) -> TSharedRef<SButton>
    {
        return SAssignNew(StreamModeButtons[ModeIdx], SButton)
            .ButtonStyle(&StyleSecondary)
            .IsEnabled(bEnabled)
            .ToolTipText(Tooltip)
            .ContentPadding(FMargin(14, 3))
            .OnClicked_Lambda([this, ModeIdx]()
            {
                SetStreamMode(ModeIdx);
                return FReply::Handled();
            })
            [
                SNew(STextBlock)
                .Text(Label)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ColorAndOpacity(bEnabled ? ColorText : ColorTextDim)
            ];
    };
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
        [
            MakeModeButton(0, LOCTEXT("StreamModeAuto", "Auto"), false,
                LOCTEXT("StreamModeAutoTT", "待身份全自动链路验证（云DNA + 牙齿C++桥）后开放"))
        ]
        + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
        [
            MakeModeButton(1, LOCTEXT("StreamModeSemi", "Semi"), true, FText::GetEmpty())
        ]
        + SHorizontalBox::Slot().AutoWidth()
        [
            MakeModeButton(2, LOCTEXT("StreamModeManual", "Manual"), true, FText::GetEmpty())
        ];
}

void SMetaHumanSolverWindow::SetStreamMode(int32 NewMode)
{
    if (NewMode == 0)
    {
        return;   // Auto 置灰不可选
    }
    StreamMode = NewMode;
    UpdateStreamModeUI();
}

void SMetaHumanSolverWindow::UpdateStreamModeUI()
{
    // 刷新三档按钮选中态（选中=主色，未选=次要）；徽标/Manual 行/步骤按钮由属性绑定自动跟随
    for (int32 i = 0; i < 3; ++i)
    {
        if (StreamModeButtons[i].IsValid())
        {
            StreamModeButtons[i]->SetButtonStyle(i == StreamMode ? &StylePrimary : &StyleSecondary);
        }
    }
}

void SMetaHumanSolverWindow::OnManualStageTrigger(int32 Stage)
{
    // Manual 严格顺序：仅当前步有效（按钮已按此约束可用性，此处双保险）
    if (StreamMode != 2 || Stage != ManualStageProgress)
    {
        return;
    }
    const FString Inbox = AutoInboxEdit.IsValid()
        ? AutoInboxEdit->GetText().ToString().TrimStartAndEnd() : FString();
    const FString IdDir = AutoIdAssetDirEdit.IsValid()
        ? AutoIdAssetDirEdit->GetText().ToString().TrimStartAndEnd() : FString();

    if (Stage == 0)
    {
        // ① ID：人工在编辑器制作 identity 并放入目录（S2 接就绪判据自动校验，第一版事件流引导）
        AutoAppendEvent(FString::Printf(
            TEXT("① ID：请在编辑器完成 identity 制作并放入 %s，做完后点 [确认完成]"),
            IdDir.IsEmpty() ? TEXT("ID资产目录（未填写）") : *IdDir));
        AppendLog(TEXT("[MHS_LOG] Manual ① ID：制作完成后点 [确认完成] 才推进"), ColorTextDim);
        // 不自动进位——Manual 语义是"真实确认后才推进"
        return;
    }
    if (Stage == 1)
    {
        // ② Ingest：precheck 盘点热文件夹（S2 接完整导入）
        if (Inbox.IsEmpty())
        {
            AppendLog(TEXT("[MHS_ERROR] 请先填写热文件夹"), ColorLogError);
            return;
        }
        AutoAppendEvent(TEXT("② Ingest：预检热文件夹（素材盘点）……"));
        AppendLog(FString::Printf(TEXT("[MHS_LOG] Stream Ingest 预检: %s"), *Inbox), ColorTextDim);
        const FString JobJson = BuildStreamJobJson();
        const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job_stream.json");
        if (!JobJson.IsEmpty() && FFileHelper::SaveStringToFile(JobJson, *JobPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            FString Escaped = JobPath;
            Escaped.ReplaceInline(TEXT("\\"), TEXT("/"));
            Escaped.ReplaceInline(TEXT("'"), TEXT("\\'"));
            const FString Cmd = FString::Printf(
                TEXT("py import MetaHumanSolverEngine.bridge; MetaHumanSolverEngine.bridge.precheck_import(r'%s')"), *Escaped);
            GEngine->Exec(nullptr, *Cmd);
        }
        // 不自动进位——导入完成后点 [确认完成] 才推进
        return;
    }
    // Stage 2/3（Solve/Export）：S2 引擎层接线，当前按钮已置灰不可达
}

FString SMetaHumanSolverWindow::GetStreamModeValue() const
{
    // 0=auto 1=semi 2=manual
    switch (StreamMode)
    {
    case 0: return TEXT("auto");
    case 2: return TEXT("manual");
    default: return TEXT("semi");
    }
}

FString SMetaHumanSolverWindow::BuildStreamJobJson() const
{
    // Stream job：流式模式配置（stage_automation 位图 = 三档模式的内部表示，未来加档零成本）
    const FString Inbox = AutoInboxEdit.IsValid()
        ? AutoInboxEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (Inbox.IsEmpty())
    {
        return FString();
    }
    const FString IdAssetDir = AutoIdAssetDirEdit.IsValid()
        ? AutoIdAssetDirEdit->GetText().ToString().TrimStartAndEnd() : FString();
    FString ImportRoot = AutoImportRootEdit.IsValid()
        ? AutoImportRootEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (ImportRoot.IsEmpty())
    {
        ImportRoot = TEXT("/Game/CaptureManager/Auto");
    }
    while (ImportRoot.Len() > 1 && ImportRoot.EndsWith(TEXT("/")))
    {
        ImportRoot = ImportRoot.LeftChop(1);
    }
    const FString FbxOut = AutoOutputEdit.IsValid()
        ? AutoOutputEdit->GetText().ToString().TrimStartAndEnd() : FString();
    const FString MediaRoot = (FPaths::ProjectSavedDir() / TEXT("MetaHumanSolver/MediaOut"))
        .Replace(TEXT("\\"), TEXT("/"));
    // 身份导入（UE 路径）：ID/ROM 素材落位；留空回落素材导入根
    FString IdImportRoot = AutoIdImportEdit.IsValid()
        ? AutoIdImportEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (IdImportRoot.IsEmpty())
    {
        IdImportRoot = ImportRoot;
    }
    while (IdImportRoot.Len() > 1 && IdImportRoot.EndsWith(TEXT("/")))
    {
        IdImportRoot = IdImportRoot.LeftChop(1);
    }

    // stage_automation：每阶段自动化开关（三档 = 预设组合）
    const bool bIdManual = IsStageManual(0);
    const bool bIngestManual = IsStageManual(1);
    const bool bSolveManual = IsStageManual(2);
    const bool bExportManual = IsStageManual(3);

    // 字段对齐流式执行器（stream_executor）+ 批次3闭环验证过的 job 结构：
    //   inbox = 热文件夹（发现层扫描根）；import_then_solve = FBX 交付命名分支；
    //   解算/导出配置与深度 Tab 混合 job 一致；stream_dashboard_port = Web/手机监控（0=禁用）。
    // identity_path 留空（Semi 身份走绑定表快照；import_only 放宽必填校验）。
    return FString::Printf(TEXT(
        "{\n"
        "  \"mode\": \"depth\",\n"
        "  \"stream_mode\": \"%s\",\n"
        "  \"stage_automation\": { \"identity\": %s, \"ingest\": %s, \"solve\": %s, \"export\": %s },\n"
        "  \"identity_asset_dir\": \"%s\",\n"
        "  \"identity_import_root\": \"%s\",\n"
        "  \"identity_path\": \"\",\n"
        "  \"inbox\": \"%s\",\n"
        "  \"capture_root\": \"%s\",\n"
        "  \"storage_path\": \"/Game/MH_Results\",\n"
        "  \"import_mode\": \"cpp\",\n"
        "  \"import_footage\": true,\n"
        "  \"import_then_solve\": true,\n"
        "  \"footage_source_dir\": \"%s\",\n"
        "  \"media_output_root\": \"%s\",\n"
        "  \"fbx_output_dir\": \"%s\",\n"
        "  \"skeleton_path\": \"/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton\",\n"
        "  \"auto_prepare_identity\": false,\n"
        "  \"export_anim_sequence\": true,\n"
        "  \"export_fbx\": true,\n"
        "  \"export_level_sequence\": true,\n"
        "  \"export_audio_track\": false,\n"
        "  \"depth\": { \"using_livelinkface_data\": true, \"blocking\": true, \"skip_tongue_solve\": true },\n"
        "  \"stream_dashboard_port\": 8902,\n"
        "  \"exclude\": []\n"
        "}"),
        *GetStreamModeValue(),
        bIdManual ? TEXT("false") : TEXT("true"),
        bIngestManual ? TEXT("false") : TEXT("true"),
        bSolveManual ? TEXT("false") : TEXT("true"),
        bExportManual ? TEXT("false") : TEXT("true"),
        *JsonEscape(IdAssetDir), *JsonEscape(IdImportRoot), *JsonEscape(Inbox),
        *JsonEscape(ImportRoot), *JsonEscape(Inbox), *JsonEscape(MediaRoot),
        *JsonEscape(FbxOut));
}

void SMetaHumanSolverWindow::OnAutoStartStop()
{
    // Manual：重置流程（步骤进度归零，① 重新点亮；Label 由属性绑定自动显示）
    if (StreamMode == 2)
    {
        ManualStageProgress = 0;
        AutoAppendEvent(TEXT("Manual 流程重置：从 ① ID 重新开始"));
        AppendLog(TEXT("[MHS_LOG] Stream Manual 流程重置"), ColorTextDim);
        return;
    }

    if (bAutoListening)
    {
        // 停止：请求标记（解算阻塞期 UI 冻结，本就只能在段间点击——当前段 Exec 返回后
        // 下一轮 Tick 走收尾：stream_stop 丢弃单例，队列已持久化可续跑）
        bStreamStopRequested = true;
        bAutoListening = false;
        AutoAppendEvent(TEXT("停止请求（当前段完成后停止）"));
        AppendLog(TEXT("[MHS_LOG] Stream 停止请求：当前段完成后停止，队列已持久化"), ColorTextDim);
        return;
    }

    // Semi 开始：校验必填 -> 生成 job_stream.json -> 启动监听
    const FString Inbox = AutoInboxEdit.IsValid()
        ? AutoInboxEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (Inbox.IsEmpty())
    {
        AppendLog(TEXT("[MHS_ERROR] 请先填写加载路径"), ColorLogError);
        return;
    }
    // 导出路径必填：缺失则解算完成后 FBX 无处交付（在启动前拦下，避免 34 分钟后才发现）
    const FString FbxOutCheck = AutoOutputEdit.IsValid()
        ? AutoOutputEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (FbxOutCheck.IsEmpty())
    {
        AppendLog(TEXT("[MHS_ERROR] 请先填写导出路径（FBX 交付输出目录）"), ColorLogError);
        return;
    }

    const FString JobJson = BuildStreamJobJson();
    const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job_stream.json");
    if (JobJson.IsEmpty() || !FFileHelper::SaveStringToFile(JobJson, *JobPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        AppendLog(FString::Printf(TEXT("[MHS_ERROR] 无法写入 job_stream.json: %s"), *JobPath), ColorLogError);
        return;
    }

    bAutoListening = true;
    AutoAppendEvent(FString::Printf(TEXT("监听启动（Semi 流式）：加载路径 %s"), *Inbox));
    AppendLog(FString::Printf(TEXT("[MHS_LOG] Stream 监听启动: %s"), *Inbox), ColorTextDim);
    AppendLog(TEXT("[MHS_LOG] 驱动模式：队列消费 + 幂等补扫（空队列 5s 轮询等新素材）"), ColorTextDim);

    // 拉起全局控制台（方案 A：与单任务进度共用 conhost，上下分区）
    // 必须先于任何单任务监控启动——确保本控制台持有 console（单任务窗不会误 FreeConsole）
    const FString FbxOutForConsole = AutoOutputEdit.IsValid()
        ? AutoOutputEdit->GetText().ToString().TrimStartAndEnd() : FString();
    MHSStreamConsole::Get().Launch(Inbox, FbxOutForConsole);
    AppendLog(TEXT("[MHS_LOG] 全局控制台已启动（实时进度见控制台窗口，本面板解算期间不刷新）"), ColorTextDim);

    // 关闭后台降频：UE 默认在编辑器失焦/最小化时大幅节流（bThrottleCPUWhenNotForeground
    // 默认 true）——本产品的形态就是"UE 在后台干活、用户只看控制台/Web"，节流会让
    // 段间的心跳提交被拖到近乎停滞（表现为"卡住，点一下编辑器才好"）。
    if (UEditorPerformanceSettings* Perf = GetMutableDefault<UEditorPerformanceSettings>())
    {
        if (Perf->bThrottleCPUWhenNotForeground)
        {
            Perf->bThrottleCPUWhenNotForeground = false;
            Perf->SaveConfig();
            AppendLog(TEXT("[MHS_LOG] 已关闭 UE 后台降频（失焦/最小化不再拖慢流式心跳）"), ColorTextDim);
        }
    }

    // 启动流式定时器：逐次提交 stream_next（阻塞一条全链路），
    // [MHS_STREAM] REMAINING 驱动续跑。
    RefreshBindGroupOptions();   // 启动即以当前加载路径刷新绑定目标候选
    StartStreamTimer(JobPath);
}

void SMetaHumanSolverWindow::AutoAppendEvent(const FString& Line)
{
    // 统一走结构化事件（倒序存储 + 筛选渲染）
    PushStreamEvent(TEXT("信息"), Line);
}

TSharedRef<SHorizontalBox> SMetaHumanSolverWindow::MakeInputRow(
    const FText& Label, const FText& Placeholder, EBrowseTarget BrowseTarget, TSharedPtr<SEditableTextBox>& OutEdit)
{
    return SNew(SHorizontalBox)
        // 固定标签列（72px）：所有输入行左边缘严格对齐；四字标签 + 呼吸位
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            SNew(SBox).WidthOverride(72.0f).HAlign(HAlign_Left)
            [
                SNew(STextBlock)
                .Text(Label)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ColorAndOpacity(ColorText)
            ]
        ]
        + SHorizontalBox::Slot().FillWidth(1.0f).Padding(8, 0, 8, 0).VAlign(VAlign_Center)
        [
            SAssignNew(OutEdit, SEditableTextBox)
            .HintText(Placeholder)
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
            .ForegroundColor(FSlateColor(ColorText))
            .BackgroundColor(FSlateColor(ColorSurface))
            .Padding(FMargin(6, 2))
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            SNew(SBox).HeightOverride(24.0f)
            [
                SNew(SButton)
                .ButtonStyle(&StyleSecondary)
                .ContentPadding(FMargin(8, 2))
                .OnClicked_Lambda([this, BrowseTarget]()
                {
                    OnBrowse(BrowseTarget);
                    return FReply::Handled();
                })
                [
                    SNew(SBox)
                    .HAlign(HAlign_Fill)
                    .VAlign(VAlign_Fill)
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("BrowseBtn", "浏览"))
                        .Justification(ETextJustify::Center)
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(ColorText)
                    ]
                ]
            ]
        ];
}

TSharedRef<SHorizontalBox> SMetaHumanSolverWindow::MakeLimitRow(TSharedPtr<SEditableTextBox>& OutEdit)
{
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            SNew(SBox).WidthOverride(72.0f).HAlign(HAlign_Left)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("LimitLabel", "分批数量"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ColorAndOpacity(ColorText)
            ]
        ]
        + SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 8, 0).VAlign(VAlign_Center)
        [
            SNew(SBox).WidthOverride(88.0f)
            [
                SAssignNew(OutEdit, SEditableTextBox)
                .Text(LOCTEXT("LimitDefault", ""))
                .HintText(LOCTEXT("LimitHint", "留空=全部"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ForegroundColor(FSlateColor(ColorTextDim))
                .BackgroundColor(FSlateColor(ColorSurface))
                .Padding(FMargin(6, 2))
            ]
        ]
        + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("LimitNote", "深度解算为阻塞式，可先设数量分批次测试，避免 UI 长时间锁定"))
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
            .ColorAndOpacity(FLinearColor(ColorTextDim.R, ColorTextDim.G, ColorTextDim.B, 0.6f))
            .AutoWrapText(true)
        ];
}

TSharedRef<SHorizontalBox> SMetaHumanSolverWindow::MakeActionRow(const FText& TabName)
{
    TSharedRef<SButton> CancelBtn = SNew(SButton)
        .ButtonStyle(&StyleSecondary)
        .ContentPadding(FMargin(14, 5))
        .OnClicked_Lambda([this, TabName]()
        {
            OnCancel(TabName);
            return FReply::Handled();
        })
        [
            SNew(SBox)
            .HAlign(HAlign_Fill)
            .VAlign(VAlign_Fill)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("CancelBtn", "取消"))
                .Justification(ETextJustify::Center)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
                .ColorAndOpacity(ColorText)
            ]
        ];

    TSharedRef<SButton> StartBtn = SNew(SButton)
        .ButtonStyle(&StylePrimary)
        .ContentPadding(FMargin(16, 5))
        .OnClicked_Lambda([this, TabName]()
        {
            OnStart(TabName);
            return FReply::Handled();
        })
        [
            SNew(SBox)
            .HAlign(HAlign_Fill)
            .VAlign(VAlign_Fill)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("StartBtn", "开始解算"))
                .Justification(ETextJustify::Center)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ColorAndOpacity(ColorWhite)
            ]
        ];

    StartButtons.Add(StartBtn);

    // 预检按钮：快速扫描素材 + 校验依赖，不实际解算（不受阻塞冻结影响，防错用）
    TSharedRef<SButton> PrecheckBtn = SNew(SButton)
        .ButtonStyle(&StyleSecondary)
        .ContentPadding(FMargin(12, 5))
        .OnClicked_Lambda([this, TabName]()
        {
            OnPrecheck(TabName);
            return FReply::Handled();
        })
        [
            SNew(SBox)
            .HAlign(HAlign_Fill)
            .VAlign(VAlign_Fill)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("PrecheckBtn", "预检"))
                .Justification(ETextJustify::Center)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ColorAndOpacity(ColorText)
            ]
        ];

    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1.0f)
        [ SNew(SSpacer) ]
        + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)
        [ PrecheckBtn ]
        + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)
        [ CancelBtn ]
        + SHorizontalBox::Slot().AutoWidth()
        [ StartBtn ];
}

void SMetaHumanSolverWindow::UpdateButtonsState()
{
    for (const TSharedPtr<SButton>& Btn : StartButtons)
    {
        if (Btn.IsValid())
        {
            Btn->SetEnabled(!bProcessing);
        }
    }
}

// ── 桥接实现（plan 2.7 / 2.8） ──

void SMetaHumanSolverWindow::OnBrowse(EBrowseTarget InTarget)
{
    // 用枚举映射到具体输入框（替代易失效的中文字符串路由）
    TSharedPtr<SEditableTextBox>* Target = nullptr;
    switch (InTarget)
    {
        case EBrowseTarget::DepthOutput:    Target = &DepthOutputEdit;    break;
        case EBrowseTarget::VideoInput:     Target = &VideoInputEdit;     break;
        case EBrowseTarget::VideoIdentity:  Target = &VideoIdentityEdit;  break;
        case EBrowseTarget::VideoOutput:    Target = &VideoOutputEdit;    break;
        case EBrowseTarget::AudioInput:     Target = &AudioInputEdit;     break;
        case EBrowseTarget::AudioOutput:    Target = &AudioOutputEdit;    break;
        case EBrowseTarget::RomSource:      Target = &RomSourceEdit;      break;
    }

    if (!Target)
    {
        MHS_LOG("未知浏览目标: %d", (int32)InTarget);
        return;
    }

    // 磁盘目录字段（FBX 输出 / ROM take 文件夹）用系统磁盘目录对话框。
    const bool bIsDiskDirTarget =
        (InTarget == EBrowseTarget::DepthOutput || InTarget == EBrowseTarget::VideoOutput
         || InTarget == EBrowseTarget::AudioOutput || InTarget == EBrowseTarget::RomSource);
    if (bIsDiskDirTarget)
    {
        const FString DialogTitle = (InTarget == EBrowseTarget::RomSource)
            ? TEXT("选择 ROM take 文件夹")
            : TEXT("选择 FBX 输出目录");
        FString Current = (*Target)->GetText().ToString();
        FString ChosenDir;
        if (IDesktopPlatform* Desktop = FDesktopPlatformModule::Get())
        {
            const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
            const bool bOk = Desktop->OpenDirectoryDialog(ParentWindow, DialogTitle, Current, ChosenDir);
            if (bOk && !ChosenDir.IsEmpty())
            {
                (*Target)->SetText(FText::FromString(ChosenDir));
                MHS_LOG("选择磁盘目录: %s", *ChosenDir);
            }
        }
        return;
    }

    // 用 UE 内容浏览器选择器，直接打开到项目 /Game 根目录，可选中资产或文件夹，
    // 避免系统磁盘目录对话框无法浏览 UE 虚拟资产目录的问题。
    FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
    const FText CurrentText = (*Target)->GetText();
    const FString CurrentPath = CurrentText.ToString();

    // 目录型字段（捕获数据/输出目录）用路径选择器；其余（身份/音频）用资产选择器。
    const bool bIsIdentityTarget = (InTarget == EBrowseTarget::VideoIdentity);
    const bool bIsAssetTarget = (bIsIdentityTarget || InTarget == EBrowseTarget::AudioInput);

    // 用 TSharedPtr 便于分支内初始化（TSharedRef 不可默认构造）
    TSharedPtr<SWidget> PickerWidget;
    if (bIsAssetTarget)
    {
        // ── 资产选择器：选择单个资产，填入 /Game/... 资产路径 ──
        FAssetPickerConfig AssetPickerConfig;
        AssetPickerConfig.Filter.bRecursiveClasses = true;
        AssetPickerConfig.bAllowDragging = false;
        AssetPickerConfig.bCanShowClasses = false;
        // 按目标类型精准筛选：身份只显示 MetaHumanIdentity，音频只显示 SoundWave
        if (bIsIdentityTarget)
        {
            AssetPickerConfig.Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/MetaHumanIdentity"), TEXT("MetaHumanIdentity")));
        }
        else
        {
            AssetPickerConfig.Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("SoundWave")));
        }
        AssetPickerConfig.OnAssetDoubleClicked = FOnAssetDoubleClicked::CreateLambda(
            [Target](const FAssetData& AssetData)
            {
                (*Target)->SetText(FText::FromString(AssetData.PackageName.ToString()));
            });
        PickerWidget = ContentBrowserModule.Get().CreateAssetPicker(AssetPickerConfig);
    }
    else
    {
        // ── 路径选择器：选择 /Game 下任意文件夹，填入目录路径 ──
        FPathPickerConfig PathPickerConfig;
        PathPickerConfig.DefaultPath = CurrentPath.StartsWith(TEXT("/Game/")) ? CurrentPath : TEXT("/Game");
        PathPickerConfig.bAddDefaultPath = true;
        PathPickerConfig.bFocusSearchBoxWhenOpened = true;
        PathPickerConfig.OnPathSelected = FOnPathSelected::CreateLambda(
            [Target](const FString& Path)
            {
                (*Target)->SetText(FText::FromString(Path));
            });
        PickerWidget = ContentBrowserModule.Get().CreatePathPicker(PathPickerConfig);
    }

    // 将选择器放入一个模态弹出窗口
    TSharedRef<SWindow> PickerWindow = SNew(SWindow)
        .Title(LOCTEXT("BrowseTitle", "选择 MetaHuman 资产路径"))
        .SizingRule(ESizingRule::UserSized)
        .ClientSize(FVector2D(600, 500))
        .SupportsMaximize(false)
        .SupportsMinimize(false)
        [ SNew(SVerticalBox)
            + SVerticalBox::Slot().FillHeight(1.0f).Padding(6)
            [ PickerWidget.ToSharedRef() ]
        ];

    FSlateApplication::Get().AddWindow(PickerWindow);
    MHS_LOG("已打开 UE 内容浏览器选择器 (目标: %d)", (int32)InTarget);
}

void SMetaHumanSolverWindow::OnStart(const FText& TabName)
{
    if (bProcessing)
    {
        AppendLog(TEXT("[MHS_WARN] 已有任务在运行，请等待完成或取消"), ColorLogWarn);
        return;
    }

    // 深度 Tab：任务列表组队列（每组 = 身份资产 + 表演源文件夹，全自动到 FBX）
    if (ActiveTab == 0)
    {
        PendingGroups.Empty();
        int32 SkippedRows = 0;
        for (const TSharedPtr<FSolveTaskRow>& Row : SolveTaskRows)
        {
            if (!Row.IsValid() || !Row->IdentityEdit.IsValid() || !Row->SourceEdit.IsValid())
            {
                continue;
            }
            const FString Identity = Row->IdentityEdit->GetText().ToString().TrimStartAndEnd();
            const FString Source = Row->SourceEdit->GetText().ToString().TrimStartAndEnd();
            if (Identity.IsEmpty() || Source.IsEmpty())
            {
                if (!Identity.IsEmpty() || !Source.IsEmpty())
                {
                    ++SkippedRows;  // 只填了一半的行
                }
                continue;
            }
            PendingGroups.Add(TPair<FString, FString>(Identity, Source));
        }
        if (PendingGroups.Num() == 0)
        {
            AppendLog(TEXT("[MHS_ERROR] 没有完整任务行：请添加任务（身份资产 + 表演源文件夹均必填）"), ColorLogError);
            return;
        }
        if (SkippedRows > 0)
        {
            AppendLog(FString::Printf(TEXT("[MHS_WARN] %d 行只填了一半，已忽略"), SkippedRows), ColorLogWarn);
        }
        if (!DepthOutputEdit.IsValid() || DepthOutputEdit->GetText().ToString().TrimStartAndEnd().IsEmpty())
        {
            AppendLog(TEXT("[MHS_ERROR] 请填写输出路径（磁盘 FBX 导出位置）"), ColorLogError);
            return;
        }

        bProcessing = true;
        bCancelRequested = false;
        CurrentGroupIndex = -1;
        UpdateButtonsState();
        AppendLog(FString::Printf(TEXT("[MHS_LOG] 已提交任务: %s（共 %d 组）"), *TabName.ToString(), PendingGroups.Num()), ColorText);
        StartNextGroup();
        return;
    }

    const FString JobJson = BuildJobJson();
    if (JobJson.IsEmpty())
    {
        AppendLog(TEXT("[MHS_ERROR] 表单校验失败：请检查必填项（捕获数据/身份路径/输出路径/音频路径），视频模式必须显式填写身份路径"), ColorLogError);
        return;
    }

    const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job.json");
    if (!FFileHelper::SaveStringToFile(JobJson, *JobPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        AppendLog(FString::Printf(TEXT("[MHS_ERROR] 无法写入 job.json: %s"), *JobPath), ColorLogError);
        return;
    }

    bProcessing = true;
    UpdateButtonsState();
    AppendLog(FString::Printf(TEXT("[MHS_LOG] 已提交任务: %s"), *TabName.ToString()), ColorText);
    AppendLog(FString::Printf(TEXT("[MHS_LOG] job 写入: %s"), *JobPath), ColorTextDim);

    // 段间刷新：用定时器逐段驱动，每段解算完成后 Exec 返回、游戏线程空出，
    // [MHS_PROGRESS]/[MHS_LOG] 日志得以被 UI 处理，实现段间进度刷新与预估剩余时间。
    StartBatchTimer(JobPath, TabName);
}

void SMetaHumanSolverWindow::OnCancel(const FText& TabName)
{
    if (!bProcessing)
    {
        AppendLog(TEXT("[MHS_LOG] 当前无运行任务"), ColorTextDim);
        return;
    }
    bCancelRequested = true;
    GEngine->Exec(nullptr, TEXT("py import MetaHumanSolverEngine.bridge; MetaHumanSolverEngine.bridge.cancel()"));
    AppendLog(TEXT("[MHS_LOG] 已请求取消（当前段解算完即停）"), ColorLogWarn);
}

void SMetaHumanSolverWindow::OnPrecheck(const FText& TabName)
{
    // 深度 Tab：逐组跑导入预检（纯磁盘扫描，秒级）——数据完整性 + 数量盘点。
    if (ActiveTab == 0)
    {
        int32 GroupIdx = 0;
        for (const TSharedPtr<FSolveTaskRow>& Row : SolveTaskRows)
        {
            if (!Row.IsValid() || !Row->SourceEdit.IsValid())
            {
                continue;
            }
            const FString Source = Row->SourceEdit->GetText().ToString().TrimStartAndEnd();
            const FString Identity = Row->IdentityEdit.IsValid()
                ? Row->IdentityEdit->GetText().ToString().TrimStartAndEnd() : FString();
            if (Source.IsEmpty())
            {
                continue;
            }
            ++GroupIdx;
            const FString MediaRoot = (FPaths::ProjectSavedDir() / TEXT("MetaHumanSolver/MediaOut"))
                .Replace(TEXT("\\"), TEXT("/"));
            const FString JobJson = FString::Printf(TEXT(
                "{\n"
                "  \"mode\": \"depth\",\n"
                "  \"identity_path\": \"%s\",\n"
                "  \"capture_root\": \"%s\",\n"
                "  \"import_mode\": \"cpp\",\n"
                "  \"import_footage\": true,\n"
                "  \"footage_source_dir\": \"%s\",\n"
                "  \"media_output_root\": \"%s\",\n"
                "  \"import_only\": true,\n"
                "  \"exclude\": []\n"
                "}"),
                *JsonEscape(Identity), *JsonEscape(GetImportRoot()), *JsonEscape(Source), *JsonEscape(MediaRoot));
            const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job_precheck_group.json");
            if (FFileHelper::SaveStringToFile(JobJson, *JobPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            {
                AppendLog(FString::Printf(TEXT("[MHS_LOG] ===== 第 %d 组预检: %s ====="), GroupIdx, *Source), ColorTextDim);
                FString Escaped = JobPath;
                Escaped.ReplaceInline(TEXT("\\"), TEXT("/"));
                Escaped.ReplaceInline(TEXT("'"), TEXT("\\'"));
                const FString Cmd = FString::Printf(
                    TEXT("py import MetaHumanSolverEngine.bridge; MetaHumanSolverEngine.bridge.precheck_import(r'%s')"), *Escaped);
                GEngine->Exec(nullptr, *Cmd);
            }
        }
        if (GroupIdx == 0)
        {
            AppendLog(TEXT("[MHS_ERROR] 没有可预检的任务行（表演源文件夹为空）"), ColorLogError);
        }
        else
        {
            AppendLog(FString::Printf(TEXT("[MHS_LOG] 已预检 %d 组（结果见日志）"), GroupIdx), ColorTextDim);
        }
        return;
    }

    // 预检：快速扫描素材 + 校验依赖，不实际解算。结果通过 [MHS_LOG]/[MHS_ERROR] 打日志。
    const FString JobJson = BuildJobJson();
    if (JobJson.IsEmpty())
    {
        AppendLog(TEXT("[MHS_ERROR] 预检失败：表单配置不完整（视频需显式填写身份路径），请检查必填项"), ColorLogError);
        return;
    }
    const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job.json");
    if (!FFileHelper::SaveStringToFile(JobJson, *JobPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        AppendLog(FString::Printf(TEXT("[MHS_ERROR] 无法写入 job.json: %s"), *JobPath), ColorLogError);
        return;
    }
    FString Escaped = JobPath;
    Escaped.ReplaceInline(TEXT("\\"), TEXT("/"));
    Escaped.ReplaceInline(TEXT("'"), TEXT("\\'"));
    const FString Cmd = FString::Printf(
        TEXT("py import MetaHumanSolverEngine.bridge; MetaHumanSolverEngine.bridge.dry_run(r'%s')"), *Escaped);
    GEngine->Exec(nullptr, *Cmd);
    AppendLog(FString::Printf(TEXT("[MHS_LOG] 预检完成（%s），详见上方日志"), *TabName.ToString()), ColorTextDim);
}

void SMetaHumanSolverWindow::BrowseDiskDir(TSharedPtr<SEditableTextBox> Target, const FString& Title)
{
    if (!Target.IsValid())
    {
        return;
    }
    FString Current = Target->GetText().ToString();
    FString ChosenDir;
    if (IDesktopPlatform* Desktop = FDesktopPlatformModule::Get())
    {
        const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
        const bool bOk = Desktop->OpenDirectoryDialog(ParentWindow, Title, Current, ChosenDir);
        if (bOk && !ChosenDir.IsEmpty())
        {
            Target->SetText(FText::FromString(ChosenDir));
        }
    }
}

FString SMetaHumanSolverWindow::GetImportRoot() const
{
    // 读取导入目标路径：Trim；空回落默认；去尾部斜杠（避免拼接出 //批次名）
    const FString Default = TEXT("/Game/CaptureManager/Imports");
    FString Root = ImportRootEdit.IsValid()
        ? ImportRootEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (Root.IsEmpty())
    {
        return Default;
    }
    while (Root.Len() > 1 && Root.EndsWith(TEXT("/")))
    {
        Root = Root.LeftChop(1);
    }
    return Root;
}

void SMetaHumanSolverWindow::BrowseContentImportDir()
{
    // 导入目标浏览：UE 内容浏览器路径选择（/Game 下目录），替代磁盘换算方案
    BrowseContentPathUE(ImportRootEdit, LOCTEXT("ImportRootPickTitle", "选择导入目标目录（/Game 下）"));
}

void SMetaHumanSolverWindow::BrowseContentPathUE(TSharedPtr<SEditableTextBox> Target, const FText& Title)
{
    // UE 资产路径选择：内容浏览器 PathPicker 模态框（仅 /Game 树），选中回填并自动关窗。
    if (!Target.IsValid())
    {
        return;
    }
    FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
    const FString Current = Target->GetText().ToString().TrimStartAndEnd();

    TSharedPtr<SWindow> PickerWindow;
    FPathPickerConfig Config;
    Config.DefaultPath = Current.StartsWith(TEXT("/Game")) ? Current : TEXT("/Game");
    Config.OnPathSelected = FOnPathSelected::CreateLambda(
        [Target, &PickerWindow](const FString& PickedPath)
        {
            if (Target.IsValid() && !PickedPath.IsEmpty())
            {
                Target->SetText(FText::FromString(PickedPath));
            }
            if (PickerWindow.IsValid())
            {
                PickerWindow->RequestDestroyWindow();
            }
        });
    TSharedRef<SWidget> PickerWidget = ContentBrowserModule.Get().CreatePathPicker(Config);

    SAssignNew(PickerWindow, SWindow)
        .Title(Title)
        .SizingRule(ESizingRule::UserSized)
        .ClientSize(FVector2D(450, 550))
        .SupportsMaximize(false)
        .SupportsMinimize(false)
        [ SNew(SVerticalBox)
            + SVerticalBox::Slot().FillHeight(1.0f).Padding(6)
            [ PickerWidget ]
        ];
    FSlateApplication::Get().AddModalWindow(PickerWindow.ToSharedRef(), AsShared());
}

void SMetaHumanSolverWindow::WarnIfBatchDirExists(const FString& BatchName)
{
    // 轻量重名提示：目标批次目录下已有资产时黄色警告。
    // 幂等语义 = 同名 take 会被跳过导入（沿用旧资产）——若旧数据是误导入的，用户需要知道。
    if (BatchName.IsEmpty())
    {
        return;
    }
    IAssetRegistry* Registry = IAssetRegistry::Get();
    if (!Registry)
    {
        return;
    }
    const FString BatchPath = GetImportRoot() / BatchName;
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*BatchPath));
    Filter.bRecursivePaths = true;
    TArray<FAssetData> AssetList;
    Registry->GetAssets(Filter, AssetList);
    if (AssetList.Num() > 0)
    {
        AppendLog(FString::Printf(
            TEXT("[MHS_WARN] 导入目标已存在 %d 个资产于 %s（同名 take 将跳过导入、沿用既有资产；若为旧数据请更换导入路径或先删除）"),
            AssetList.Num(), *BatchPath), ColorLogWarn);
    }
}

void SMetaHumanSolverWindow::BrowseIdentityAsset(TSharedPtr<SEditableTextBox> Target)
{
    if (!Target.IsValid())
    {
        return;
    }
    FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
    FAssetPickerConfig AssetPickerConfig;
    AssetPickerConfig.Filter.bRecursiveClasses = true;
    AssetPickerConfig.bAllowDragging = false;
    AssetPickerConfig.bCanShowClasses = false;
    AssetPickerConfig.Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/MetaHumanIdentity"), TEXT("MetaHumanIdentity")));
    AssetPickerConfig.OnAssetDoubleClicked = FOnAssetDoubleClicked::CreateLambda(
        [Target](const FAssetData& AssetData)
        {
            if (Target.IsValid())
            {
                Target->SetText(FText::FromString(AssetData.PackageName.ToString()));
            }
        });
    TSharedRef<SWidget> PickerWidget = ContentBrowserModule.Get().CreateAssetPicker(AssetPickerConfig);

    TSharedRef<SWindow> PickerWindow = SNew(SWindow)
        .Title(LOCTEXT("BrowseIdentityTitle", "选择身份资产 (MetaHumanIdentity)"))
        .SizingRule(ESizingRule::UserSized)
        .ClientSize(FVector2D(600, 500))
        .SupportsMaximize(false)
        .SupportsMinimize(false)
        [ SNew(SVerticalBox)
            + SVerticalBox::Slot().FillHeight(1.0f).Padding(6)
            [ PickerWidget ]
        ];
    FSlateApplication::Get().AddWindow(PickerWindow);
}

void SMetaHumanSolverWindow::OnImportRom()
{
    if (bProcessing)
    {
        AppendLog(TEXT("[MHS_WARN] 已有任务在运行，请等待完成或取消"), ColorLogWarn);
        return;
    }
    const FString RomSource = RomSourceEdit->GetText().ToString().TrimStartAndEnd();
    if (RomSource.IsEmpty())
    {
        AppendLog(TEXT("[MHS_ERROR] 请先填写 ROM take 文件夹（磁盘目录）"), ColorLogError);
        return;
    }

    const FString JobJson = BuildRomJobJson(RomSource);
    if (JobJson.IsEmpty())
    {
        AppendLog(TEXT("[MHS_ERROR] ROM 导入 job 生成失败"), ColorLogError);
        return;
    }
    const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job_rom.json");
    if (!FFileHelper::SaveStringToFile(JobJson, *JobPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        AppendLog(FString::Printf(TEXT("[MHS_ERROR] 无法写入 job: %s"), *JobPath), ColorLogError);
        return;
    }

    bProcessing = true;
    UpdateButtonsState();
    PendingGroups.Empty();
    CurrentGroupIndex = -1;   // 非组任务：仅本组（ROM 导入段序列）
    AppendLog(TEXT("[MHS_LOG] 已提交 ROM 导入（段驱动，进度见上方进度条）"), ColorText);
    AppendLog(FString::Printf(TEXT("[MHS_LOG] job 写入: %s"), *JobPath), ColorTextDim);
    StartBatchTimer(JobPath, LOCTEXT("RomImport", "ROM 导入"));
}

void SMetaHumanSolverWindow::StartNextGroup()
{
    ++CurrentGroupIndex;
    if (!PendingGroups.IsValidIndex(CurrentGroupIndex))
    {
        return;
    }
    const TPair<FString, FString>& Group = PendingGroups[CurrentGroupIndex];
    const FString JobJson = BuildGroupJobJson(Group.Key, Group.Value);
    if (JobJson.IsEmpty())
    {
        AppendLog(FString::Printf(TEXT("[MHS_ERROR] 任务组 %d/%d job 生成失败（身份/文件夹/输出路径缺失）"),
            CurrentGroupIndex + 1, PendingGroups.Num()), ColorLogError);
        // 跳过本组，试下一组
        StartNextGroup();
        return;
    }
    const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job_group.json");
    if (!FFileHelper::SaveStringToFile(JobJson, *JobPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        AppendLog(FString::Printf(TEXT("[MHS_ERROR] 无法写入 job: %s"), *JobPath), ColorLogError);
        bProcessing = false;
        bCancelRequested = false;
        UpdateButtonsState();
        return;
    }
    AppendLog(FString::Printf(TEXT("[MHS_LOG] ===== 任务组 %d/%d ====="), CurrentGroupIndex + 1, PendingGroups.Num()), ColorText);
    AppendLog(FString::Printf(TEXT("[MHS_LOG] 身份: %s"), *Group.Key), ColorTextDim);
    AppendLog(FString::Printf(TEXT("[MHS_LOG] 表演源: %s"), *Group.Value), ColorTextDim);
    // 导入落位与重名提示：批次名 = 表演源末级目录名，落位 {导入路径}/{批次名}
    {
        const FString BatchName = FPaths::GetBaseFilename(Group.Value);
        AppendLog(FString::Printf(TEXT("[MHS_LOG] 导入目标: %s/%s"), *GetImportRoot(), *BatchName), ColorTextDim);
        WarnIfBatchDirExists(BatchName);
    }
    AppendLog(TEXT("[MHS_LOG] 自动流程：导入表演 → 深度解算 → 导出 FBX（全程进度条）"), ColorTextDim);
    StartBatchTimer(JobPath, LOCTEXT("DepthTab", "深度解算"));
}

void SMetaHumanSolverWindow::SetStatusDot(const FString& State)
{
    if (!StatusDot.IsValid())
    {
        return;
    }
    // 状态指示灯：running=蓝、idle=灰、error=红
    FLinearColor Color = ColorTextDim; // idle 默认灰
    if (State == TEXT("running"))
    {
        Color = ColorAccent;
    }
    else if (State == TEXT("error"))
    {
        Color = ColorLogError;
    }
    StatusDot->SetColorAndOpacity(FSlateColor(Color));
}

// ── 段间刷新：定时器逐段驱动批处理 ──

void SMetaHumanSolverWindow::StartBatchTimer(const FString& JobPath, const FText& TabName)
{
    BatchJobPath = JobPath;
    ActiveTabName = TabName;
    TotalShots = 0;
    CurrentShot = 0;
    SetProgress(0, 0);

    // 用极短延迟启动定时器，让窗口 UI 先刷新一帧再进入第一段解算。
    // Slate widget 非 UObject，不能用 CreateUObject，改用 CreateLambda 捕获 this。
    if (UWorld* World = GEditor->GetEditorWorldContext().World())
    {
        World->GetTimerManager().SetTimer(
            BatchTimer,
            FTimerDelegate::CreateLambda([this]() { TickNextSegment(); }),
            0.1f, false);
    }
    else
    {
        // 无 World 时兜底：直接提交第一段（控制台场景）
        TickNextSegment();
    }
}

void SMetaHumanSolverWindow::StopBatchTimer()
{
    if (UWorld* World = GEditor->GetEditorWorldContext().World())
    {
        World->GetTimerManager().ClearTimer(BatchTimer);
    }
}

void SMetaHumanSolverWindow::LaunchSegment(int32 Index)
{
    FString Escaped = BatchJobPath;
    Escaped.ReplaceInline(TEXT("\\"), TEXT("/"));
    Escaped.ReplaceInline(TEXT("'"), TEXT("\\'"));
    CurrentShot = Index;
    // 逐段提交：Python 单段执行，完成后返回；Exec 同步阻塞该段解算，但段间游戏线程空出可刷 UI。
    // 时间戳诊断（推进链观测）：UE 日志行自带时刻——对照 [MHS_DONE] 收到时刻与提交时刻，
    // 长时间差 = 游戏线程泵延迟（"点击面板才推进"问题的量化依据）。
    MHS_LOG("提交段 %d（组 %d/%d）", Index, CurrentGroupIndex + 1, FMath::Max(1, PendingGroups.Num()));
    const FString Cmd = FString::Printf(
        TEXT("py import MetaHumanSolverEngine.bridge; MetaHumanSolverEngine.bridge.run_next(%d, r'%s')"),
        Index, *Escaped);
    GEngine->Exec(nullptr, *Cmd);
}

void SMetaHumanSolverWindow::TickNextSegment()
{
    if (!bProcessing)
    {
        StopBatchTimer();
        return;
    }

    // 提交当前段。段完成后 [MHS_DONE] 会回调 HandleMHLogLine 停掉定时器；
    // 若还有下一段且未收到 done（异常情况），由 Python 端的 cancel/done 兜底。
    LaunchSegment(CurrentShot);
}

// ── Stream 流式驱动（批次5）──
// 模式：队列消费 + 幂等补扫——定时器逐次提交 bridge.stream_next（阻塞执行一条），
// [MHS_STREAM] REMAINING 协议行驱动续跑决策：>0 立即下一条；=0 转 10s 空转轮询等新素材。
// 停止语义：解算阻塞期 UI 冻结，停止只能段间生效（与深度 Tab 一致）——请求标记后
// 当前段 Exec 自然返回，下一轮 Tick 走收尾（stream_stop 丢弃单例，队列已持久化）。

void SMetaHumanSolverWindow::StartStreamTimer(const FString& JobPath)
{
    StreamJobPath = JobPath;
    bStreamStopRequested = false;
    StreamRemaining = -1;
    if (UWorld* World = GEditor->GetEditorWorldContext().World())
    {
        World->GetTimerManager().SetTimer(
            StreamTimer,
            FTimerDelegate::CreateLambda([this]() { TickStreamNext(); }),
            0.1f, false);
    }
    else
    {
        TickStreamNext();   // 控制台兜底
    }
}

void SMetaHumanSolverWindow::StopStreamTimer()
{
    if (UWorld* World = GEditor->GetEditorWorldContext().World())
    {
        World->GetTimerManager().ClearTimer(StreamTimer);
    }
}

void SMetaHumanSolverWindow::TickStreamNext()
{
    if (!bAutoListening || bStreamStopRequested)
    {
        // 停止收尾：丢弃执行器单例（队列状态文件已持久化，下次开始自动重建续跑）
        GEngine->Exec(nullptr,
            TEXT("py import MetaHumanSolverEngine.bridge; MetaHumanSolverEngine.bridge.stream_stop()"));
        AppendLog(TEXT("[MHS_LOG] Stream 监听已停止（队列已持久化，再点开始可续跑）"), ColorTextDim);
        AutoAppendEvent(TEXT("监听停止（队列已持久化）"));
        return;
    }

    FString Escaped = StreamJobPath;
    Escaped.ReplaceInline(TEXT("\\"), TEXT("/"));
    Escaped.ReplaceInline(TEXT("'"), TEXT("\\'"));
    MHS_LOG("Stream 提交 stream_next（REMAINING=%d）", StreamRemaining);
    GEngine->Exec(nullptr, *FString::Printf(
        TEXT("py import MetaHumanSolverEngine.bridge; MetaHumanSolverEngine.bridge.stream_next(r'%s')"),
        *Escaped));
    // Exec 同步阻塞至本条完成（导入→解算→导出）或空转补扫返回；
    // [MHS_STREAM] REMAINING 行在 Exec 内经日志管线同步处理，StreamRemaining 已最新。

    if (!bAutoListening || bStreamStopRequested)
    {
        TickStreamNext();   // 阻塞期间用户请求了停止 → 收尾
        return;
    }
    if (UWorld* World = GEditor->GetEditorWorldContext().World())
    {
        // 有积压立即下一条；队列空转 5s 轮询（补扫新素材——首轮快照+3s 稳定期，
        // 最坏 ~8s 内新素材入队；10s 会让用户误以为无响应）
        const float Delay = (StreamRemaining > 0) ? 0.05f : 5.0f;
        World->GetTimerManager().SetTimer(
            StreamTimer,
            FTimerDelegate::CreateLambda([this]() { TickStreamNext(); }),
            Delay, false);
    }
}

void SMetaHumanSolverWindow::RefreshStreamUIFromState()
{
    // 读 {Saved}/Config/MetaHumanSolver/stream_state.json（观测层批次4产出，契约与 Web 一致）
    const FString StatePath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/stream_state.json");
    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *StatePath))
    {
        return;   // 尚无状态文件（未启动过）——保持骨架占位显示
    }
    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return;   // 损坏/半截（读方约定：跳过本轮，下次快照再刷）
    }

    // 大数字：今日交付 / 已同步
    int32 Delivered = 0, Synced = 0, Failed = 0, Queued = 0;
    Root->TryGetNumberField(TEXT("delivered"), Delivered);
    Root->TryGetNumberField(TEXT("synced"), Synced);
    Root->TryGetNumberField(TEXT("failed"), Failed);
    Root->TryGetNumberField(TEXT("queued"), Queued);
    StreamFailCount = Failed;
    // 面板兜底：仅更新 ID 状态文本（计数/进度一律由全局控制台呈现）
    {
        const TSharedPtr<FJsonObject>* Ident = nullptr;
        if (Root->TryGetObjectField(TEXT("identity"), Ident) && Ident && (*Ident).IsValid())
        {
            FString State, Detail, Source, Suggested;
            (*Ident)->TryGetStringField(TEXT("state"), State);
            (*Ident)->TryGetStringField(TEXT("detail"), Detail);
            (*Ident)->TryGetStringField(TEXT("source"), Source);
            (*Ident)->TryGetStringField(TEXT("suggested_group"), Suggested);
            StreamIdStateText = Detail.IsEmpty() ? State : (State + TEXT(" · ") + Detail);
            // 来源 + 自动识别分组：让"自动绑定"可见、可核对（不静默绑错）
            IdSuggestedGroup = Suggested;
            IdSourceLine = Source.IsEmpty()
                ? FString()
                : FString::Printf(TEXT("来源 %s · 识别分组 %s"),
                    *Source, Suggested.IsEmpty() ? TEXT("(根 · 全部)") : *Suggested);
            // ID 素材就绪后，绑定目标候选/预选即时跟随（不必重开面板）——
            // 用户未手选时把选中值切到推断组，避免"看着是根组"就点绑定
            if (!bBindGroupUserPicked && !Suggested.IsEmpty() && SelectedBindGroup != Suggested)
            {
                SelectedBindGroup = Suggested;
                if (BindGroupCombo.IsValid())
                {
                    BindGroupCombo->RefreshOptions();
                }
            }
        }
    }

    // 四步卡（累计通过数，递减链条）
    const TArray<TSharedPtr<FJsonValue>>* Stages = nullptr;
    if (Root->TryGetArrayField(TEXT("stage_counts"), Stages) && Stages != nullptr)
    {
        for (int32 i = 0; i < Stages->Num() && i < QueueStatValues.Num(); ++i)
        {
            const int32 V = static_cast<int32>((*Stages)[i]->AsNumber());
            if (QueueStatValues[i].IsValid())
            {
                QueueStatValues[i]->SetText(FText::AsNumber(V));
                QueueStatValues[i]->SetColorAndOpacity(
                    FSlateColor(V > 0 ? ColorSuccess : ColorTextDim));
            }
        }
    }

    // 当前行（快照语义：显示活跃任务或队列/追平预估；"截至"由 updated_at 语义承担）
    int32 PendingBinding = 0;
    Root->TryGetNumberField(TEXT("pending_binding"), PendingBinding);
    FString CurrentLine;
    const TSharedPtr<FJsonObject>* CurrentObj = nullptr;
    if (Root->TryGetObjectField(TEXT("current"), CurrentObj) &&
        CurrentObj != nullptr && CurrentObj->IsValid())
    {
        FString Name, Status;
        (*CurrentObj)->TryGetStringField(TEXT("name"), Name);
        (*CurrentObj)->TryGetStringField(TEXT("status"), Status);
        const TCHAR* StatusCn = (Status == TEXT("importing")) ? TEXT("导入中") :
            (Status == TEXT("solving")) ? TEXT("解算中") :
            (Status == TEXT("exporting")) ? TEXT("导出中") : TEXT("处理中");
        CurrentLine = FString::Printf(TEXT("%s %s"), StatusCn, *Name);
    }
    else if (Queued > 0)
    {
        CurrentLine = FString::Printf(TEXT("待命 · 队列 %d 条"), Queued);
    }
    else if (PendingBinding > 0)
    {
        // 待绑定池：素材已发现但分组无身份——提示下一步动作（Next Action 原则）
        CurrentLine = FString::Printf(TEXT("已发现 %d 条 · 等待身份绑定（ID 制作区点[确认完成]）"), PendingBinding);
    }
    else
    {
        CurrentLine = TEXT("等待素材…");
    }
    if (PendingBinding > 0 && Queued > 0)
    {
        CurrentLine += FString::Printf(TEXT(" · 另有 %d 条待绑定"), PendingBinding);
    }
    double Eta = -1.0;
    if (Root->TryGetNumberField(TEXT("eta_seconds"), Eta) && Queued > 0 && Eta >= 0)
    {
        const FDateTime Done = FDateTime::Now() + FTimespan::FromSeconds(Eta);
        CurrentLine += FString::Printf(TEXT(" · 预计追平 %02d:%02d"),
            Done.GetHour(), Done.GetMinute());
    }
    if (StreamCurrentText.IsValid())
    {
        StreamCurrentText->SetText(FText::FromString(CurrentLine));
    }
    // 灌数摘要（双通道可观测：UI 日志框 AppendLog + 引擎日志 MHS_LOG——后者供自动化验证/运维）
    AppendLog(FString::Printf(
        TEXT("[MHS_LOG] Stream 状态刷新: 交付 %d / 同步 %d · 失败 %d · 队列 %d · 剩余 %d"),
        Delivered, Synced, Failed, Queued, StreamRemaining), ColorTextDim);
    MHS_LOG("Stream 状态刷新: 交付 %d / 同步 %d · 失败 %d · 队列 %d · 剩余 %d",
        Delivered, Synced, Failed, Queued, StreamRemaining);

    // 身份分组（显式绑定 + 分配矩阵）+ 待绑定组（pending 池——不在队列里，单独注入）
    const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
    TArray<TPair<FString, FString>> NewGroups;
    StreamActiveGroup.Reset();   // 每轮重判：活跃组 → 待绑定组 → 空
    if (Root->TryGetArrayField(TEXT("identity_groups"), Groups) && Groups != nullptr)
    {
        for (const TSharedPtr<FJsonValue>& G : *Groups)
        {
            const TSharedPtr<FJsonObject> GO = G->AsObject();
            if (!GO.IsValid())
            {
                continue;
            }
            FString Dir, Id;
            int32 Total = 0, Done = 0, GrpFailed = 0, Pending = 0;
            GO->TryGetStringField(TEXT("dir"), Dir);
            GO->TryGetStringField(TEXT("id"), Id);
            GO->TryGetNumberField(TEXT("total"), Total);
            GO->TryGetNumberField(TEXT("done"), Done);
            GO->TryGetNumberField(TEXT("failed"), GrpFailed);
            GO->TryGetNumberField(TEXT("pending"), Pending);
            FString Stat = FString::Printf(TEXT("%d/%d"), Done, Total);
            if (GrpFailed > 0)
            {
                Stat += FString::Printf(TEXT(" 失败%d"), GrpFailed);
            }
            if (Pending > 0)
            {
                Stat += FString::Printf(TEXT(" 待%d"), Pending);
            }
            NewGroups.Add(TPair<FString, FString>(Dir, Id + TEXT("  ·  ") + Stat));

            // 活跃组（Semi [确认完成] 的绑定目标）
            bool bNow = false;
            if (GO->TryGetBoolField(TEXT("now"), bNow) && bNow)
            {
                StreamActiveGroup = (Dir == TEXT("(根)")) ? FString() : Dir;
                if (StreamActiveGroup.EndsWith(TEXT("/")))
                {
                    StreamActiveGroup.LeftChopInline(1);
                }
            }
        }
    }
    // 待绑定组（pending_groups：组名→数量）：[确认完成] 的绑定目标兜底 + 分组区补显示
    const TSharedPtr<FJsonObject>* PGObj = nullptr;
    if (Root->TryGetObjectField(TEXT("pending_groups"), PGObj) && PGObj != nullptr && (*PGObj).IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PGObj)->Values)
        {
            const FString GroupKey = Pair.Key;
            const FString Dir = GroupKey.IsEmpty() ? TEXT("(根)") : GroupKey + TEXT("/");
            if (StreamActiveGroup.IsEmpty())
            {
                StreamActiveGroup = GroupKey;   // 无活跃组 → 首个待绑定组（单组场景即正确目标）
            }
            const bool bExists = NewGroups.ContainsByPredicate(
                [&Dir](const TPair<FString, FString>& G) { return G.Key == Dir; });
            if (!bExists)
            {
                NewGroups.Add(TPair<FString, FString>(Dir,
                    FString::Printf(TEXT("<未绑定>  ·  待%d"), static_cast<int32>(Pair.Value->AsNumber()))));
            }
        }
    }
    RebuildGroupList(NewGroups);
}

// ── 面板配置持久化（[保存为默认]）──
// 保存：热文件夹/ID资产目录/导入路径/输出路径 → {Saved}/Config/MetaHumanSolver/stream_panel.json
// 回填：Construct 阶段 LoadStreamPanelConfig（面板打开即恢复上次配置）

void SMetaHumanSolverWindow::RefreshBindGroupOptions()
{
    // 候选 = 加载路径下的子目录（am/、pm/ 等）+ 根组（全局兜底）
    BindGroupOptions.Empty();
    const FString Inbox = AutoInboxEdit.IsValid()
        ? AutoInboxEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (!Inbox.IsEmpty() && FPaths::DirectoryExists(Inbox))
    {
        TArray<FString> Names;
        IFileManager::Get().FindFiles(Names, *(Inbox / TEXT("*")), /*Files=*/false, /*Dirs=*/true);
        for (const FString& N : Names)
        {
            if (N == TEXT("_id")) { continue; }   // _id 是 ID 素材专用，不作为绑定组
            BindGroupOptions.Add(MakeShared<FString>(N));
        }
    }
    BindGroupOptions.Add(MakeShared<FString>(TEXT("")));   // 根组：全部

    // 预选：用户手动选过 → 保留；否则跟随自动推断组（来源 ROM 命名 am/pm）
    if (!bBindGroupUserPicked)
    {
        SelectedBindGroup = IdSuggestedGroup;
    }
    if (BindGroupCombo.IsValid())
    {
        BindGroupCombo->RefreshOptions();
    }
}

void SMetaHumanSolverWindow::OnBindIdentity()
{
    // 身份资产解析：① 显式填写的优先（多身份场景——am/pm 各一个时必填）→ 校验
    //              ② 留空 = 取资产目录内"最新修改"的一个（同组二次绑定自动用新身份）
    const FString Dir = AutoIdAssetDirEdit.IsValid()
        ? AutoIdAssetDirEdit->GetText().ToString().TrimStartAndEnd() : FString();
    const FString AssetField = AutoIdAssetEdit.IsValid()
        ? AutoIdAssetEdit->GetText().ToString().TrimStartAndEnd() : FString();
    FString IdentityPath;
    if (!AssetField.IsEmpty())
    {
        if (!IsIdentityAssetAt(AssetField))
        {
            AppendLog(FString::Printf(
                TEXT("[MHS_ERROR] 指定的身份资产无效（不存在或不是 MetaHumanIdentity）：%s"), *AssetField),
                ColorLogError);
            PushStreamEvent(TEXT("失败"), FString::Printf(TEXT("绑定失败：资产无效 %s"), *AssetField));
            return;
        }
        IdentityPath = AssetField;
    }
    else
    {
        if (Dir.IsEmpty())
        {
            AppendLog(TEXT("[MHS_ERROR] 请先填写资产目录（或将身份资产路径直接填入上一行）"), ColorLogError);
            return;
        }
        IdentityPath = FindIdentityInDir(Dir);   // 目录内最新修改的身份
        if (IdentityPath.IsEmpty())
        {
            AppendLog(FString::Printf(TEXT("[MHS_ERROR] 资产目录下未找到身份资产：%s（先制作并保存）"), *Dir), ColorLogError);
            PushStreamEvent(TEXT("失败"), TEXT("绑定失败：目录内无 identity 资产"));
            return;
        }
    }
    if (AutoIdAssetEdit.IsValid())
    {
        AutoIdAssetEdit->SetText(FText::FromString(IdentityPath));   // 回填：明确显示本次绑定的身份
    }

    // 绑定目标：下拉选中值优先；用户未手选时跟随自动推断（ROM 命名 am/pm）——
    // 避免"没选就静默绑到根组"（根组=全局兜底，会影响所有未单独绑定的组）
    FString TargetGroup = SelectedBindGroup;
    bool bAuto = false;
    if (!bBindGroupUserPicked && TargetGroup.IsEmpty() && !IdSuggestedGroup.IsEmpty())
    {
        TargetGroup = IdSuggestedGroup;
        SelectedBindGroup = IdSuggestedGroup;
        bAuto = true;
        if (BindGroupCombo.IsValid())
        {
            BindGroupCombo->RefreshOptions();
        }
    }

    const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job_stream.json");
    FString EscapedJob = JobPath;
    EscapedJob.ReplaceInline(TEXT("\\"), TEXT("/"));
    EscapedJob.ReplaceInline(TEXT("'"), TEXT("\\'"));
    FString EscapedId = IdentityPath;
    EscapedId.ReplaceInline(TEXT("'"), TEXT("\\'"));
    const FString Cmd = FString::Printf(
        TEXT("py import MetaHumanSolverEngine.bridge; MetaHumanSolverEngine.bridge.stream_set_binding(r'%s', '%s', '%s')"),
        *EscapedJob, *TargetGroup, *EscapedId);
    GEngine->Exec(nullptr, *Cmd);

    AppendLog(FString::Printf(TEXT("[MHS_LOG] 身份绑定：%s -> 分组 '%s'%s（来源：%s）"),
        *IdentityPath,
        TargetGroup.IsEmpty() ? TEXT("(根 · 全局兜底)") : *TargetGroup,
        bAuto ? TEXT(" [按 ROM 名自动推断]") : TEXT(""),
        IdSourceLine.IsEmpty() ? TEXT("—") : *IdSourceLine), ColorText);
    if (TargetGroup.IsEmpty())
    {
        AppendLog(TEXT("[MHS_WARN] 绑定到根组 = 全局兜底：所有未单独绑定的组都会用它。"
                       "多演员/多组场景请在上方选择具体组"), ColorLogWarn);
    }
}

void SMetaHumanSolverWindow::OnClearBinding()
{
    // 清除当前选中组的绑定（选中"(根 · 全部)"=清除根组绑定）
    const FString TargetGroup = SelectedBindGroup;
    const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job_stream.json");
    FString EscapedJob = JobPath;
    EscapedJob.ReplaceInline(TEXT("\\"), TEXT("/"));
    EscapedJob.ReplaceInline(TEXT("'"), TEXT("\\'"));
    const FString Cmd = FString::Printf(
        TEXT("py import MetaHumanSolverEngine.bridge; MetaHumanSolverEngine.bridge.stream_set_binding(r'%s', '%s', '')"),
        *EscapedJob, *TargetGroup);
    GEngine->Exec(nullptr, *Cmd);
    AppendLog(FString::Printf(TEXT("[MHS_LOG] 已清除绑定：分组 '%s'"),
        TargetGroup.IsEmpty() ? TEXT("(根 · 全局兜底)") : *TargetGroup), ColorTextDim);
    PushStreamEvent(TEXT("绑定"), FString::Printf(TEXT("已清除绑定：分组 %s"),
        TargetGroup.IsEmpty() ? TEXT("(根)") : *TargetGroup));
}

void SMetaHumanSolverWindow::SaveStreamPanelConfig()
{
    const FString ConfigPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/stream_panel.json");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(ConfigPath), true);

    auto ReadEdit = [](const TSharedPtr<SEditableTextBox>& Edit) -> FString
    {
        return Edit.IsValid() ? Edit->GetText().ToString().TrimStartAndEnd() : FString();
    };
    const FString Json = FString::Printf(TEXT(
        "{\n"
        "  \"inbox\": \"%s\",\n"
        "  \"id_asset_dir\": \"%s\",\n"
        "  \"id_asset\": \"%s\",\n"
        "  \"id_import_root\": \"%s\",\n"
        "  \"import_root\": \"%s\",\n"
        "  \"fbx_output_dir\": \"%s\"\n"
        "}"),
        *JsonEscape(ReadEdit(AutoInboxEdit)),
        *JsonEscape(ReadEdit(AutoIdAssetDirEdit)),
        *JsonEscape(ReadEdit(AutoIdAssetEdit)),
        *JsonEscape(ReadEdit(AutoIdImportEdit)),
        *JsonEscape(ReadEdit(AutoImportRootEdit)),
        *JsonEscape(ReadEdit(AutoOutputEdit)));
    if (FFileHelper::SaveStringToFile(Json, *ConfigPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        AppendLog(FString::Printf(TEXT("[MHS_LOG] Stream 配置已保存为默认: %s"), *ConfigPath), ColorTextDim);
        RefreshBindGroupOptions();   // 加载路径可能刚改过 → 同步刷新绑定目标候选
    }
    else
    {
        AppendLog(FString::Printf(TEXT("[MHS_ERROR] 配置保存失败: %s"), *ConfigPath), ColorLogError);
    }
}

void SMetaHumanSolverWindow::LoadStreamPanelConfig()
{
    const FString ConfigPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/stream_panel.json");
    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *ConfigPath))
    {
        return;   // 未保存过——保持空表单
    }
    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return;
    }
    auto ApplyEdit = [](const TSharedPtr<SEditableTextBox>& Edit, const FString& Key, const TSharedPtr<FJsonObject>& Obj)
    {
        FString Value;
        if (Edit.IsValid() && Obj->TryGetStringField(Key, Value) && !Value.IsEmpty())
        {
            Edit->SetText(FText::FromString(Value));
        }
    };
    ApplyEdit(AutoInboxEdit, TEXT("inbox"), Root);
    ApplyEdit(AutoIdAssetDirEdit, TEXT("id_asset_dir"), Root);
    ApplyEdit(AutoIdAssetEdit, TEXT("id_asset"), Root);
    ApplyEdit(AutoIdImportEdit, TEXT("id_import_root"), Root);
    ApplyEdit(AutoImportRootEdit, TEXT("import_root"), Root);
    ApplyEdit(AutoOutputEdit, TEXT("fbx_output_dir"), Root);
    MHS_LOG("Stream 面板配置已回填: %s", *ConfigPath);
}

void SMetaHumanSolverWindow::RebuildGroupList(const TArray<TPair<FString, FString>>& InGroups)
{
    if (!GroupListBox.IsValid())
    {
        return;
    }
    GroupListBox->ClearChildren();
    if (InGroups.Num() == 0)
    {
        GroupListBox->AddSlot().AutoHeight().Padding(0, 2)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("GroupEmptyLive", "（暂无分组——素材入队后显示）"))
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
            .ColorAndOpacity(FLinearColor(0.45f, 0.45f, 0.48f, 1.0f))
        ];
        return;
    }
    for (const TPair<FString, FString>& G : InGroups)
    {
        GroupListBox->AddSlot().AutoHeight().Padding(0, 2)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(FText::FromString(G.Key))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ColorAndOpacity(ColorTextDim)
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("GroupArrow", "→"))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ColorAndOpacity(ColorTextDim)
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(FText::FromString(G.Value))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                .ColorAndOpacity(ColorText)
            ]
        ];
    }
}

FString SMetaHumanSolverWindow::JsonEscape(const FString& In) const
{
    FString Out = In;
    Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
    Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
    Out.ReplaceInline(TEXT("\n"), TEXT("\\n"));
    return Out;
}

FString SMetaHumanSolverWindow::BuildRomJobJson(const FString& RomSourceDir) const
{
    // ROM 导入 job：import_only 纯导入段序列（run_next 逐段驱动，有 [i/N] 进度）。
    // identity 允许为空（阶段 A 先于身份制作——config.load 对 import_only 已放宽必填）。
    // media_output_root 固定 Saved 目录：媒体转换中间产物是技术细节，不进用户视野。
    if (RomSourceDir.IsEmpty())
    {
        return FString();
    }
    const FString MediaRoot = (FPaths::ProjectSavedDir() / TEXT("MetaHumanSolver/MediaOut"))
        .Replace(TEXT("\\"), TEXT("/"));
    return FString::Printf(TEXT(
        "{\n"
        "  \"mode\": \"depth\",\n"
        "  \"identity_path\": \"\",\n"
        "  \"capture_root\": \"%s\",\n"
        "  \"storage_path\": \"/Game/MH_Results\",\n"
        "  \"import_mode\": \"cpp\",\n"
        "  \"import_footage\": true,\n"
        "  \"footage_source_dir\": \"%s\",\n"
        "  \"media_output_root\": \"%s\",\n"
        "  \"import_only\": true,\n"
        "  \"exclude\": []\n"
        "}"),
        *JsonEscape(GetImportRoot()), *JsonEscape(RomSourceDir), *JsonEscape(MediaRoot));
}

FString SMetaHumanSolverWindow::BuildGroupJobJson(const FString& IdentityPath, const FString& SourceDir) const
{
    // 任务组 job：import_then_solve 混合段序列（导入段×N + 解算段×M），
    // 批次名 = 表演源文件夹末级目录名，镜像导入 capture_root/{批次名}，解算同目录。
    if (IdentityPath.IsEmpty() || SourceDir.IsEmpty())
    {
        return FString();
    }
    const FString FbxOutputDir = DepthOutputEdit.IsValid()
        ? DepthOutputEdit->GetText().ToString().TrimStartAndEnd() : FString();
    if (FbxOutputDir.IsEmpty())
    {
        return FString();
    }
    const FString MediaRoot = (FPaths::ProjectSavedDir() / TEXT("MetaHumanSolver/MediaOut"))
        .Replace(TEXT("\\"), TEXT("/"));
    const FString ExcludeLine = BuildExcludeJson();
    // 纯面部交付（默认勾选）：跳过舌头解算 + LS 不导音频轨
    const bool bNoTongueAudio = NoTongueAudioCheck.IsValid()
        && NoTongueAudioCheck->GetCheckedState() == ECheckBoxState::Checked;
    return FString::Printf(TEXT(
        "{\n"
        "  \"mode\": \"depth\",\n"
        "  \"identity_path\": \"%s\",\n"
        "  \"capture_root\": \"%s\",\n"
        "  \"storage_path\": \"/Game/MH_Results\",\n"
        "  \"import_mode\": \"cpp\",\n"
        "  \"import_footage\": true,\n"
        "  \"import_then_solve\": true,\n"
        "  \"footage_source_dir\": \"%s\",\n"
        "  \"media_output_root\": \"%s\",\n"
        "%s"
        "  \"fbx_output_dir\": \"%s\",\n"
        "  \"skeleton_path\": \"/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton\",\n"
        "  \"auto_prepare_identity\": false,\n"
        "  \"export_anim_sequence\": true,\n"
        "  \"export_fbx\": true,\n"
        "  \"export_level_sequence\": true,\n"
        "  \"export_audio_track\": %s,\n"
        "  \"naming\": { \"pattern\": \"{take_name}_{timecode}\", \"prefix\": \"AS_\" },\n"
        "  \"depth\": { \"using_livelinkface_data\": true, \"blocking\": true, \"skip_tongue_solve\": %s }\n"
        "}"),
        *JsonEscape(IdentityPath), *JsonEscape(GetImportRoot()), *JsonEscape(SourceDir), *JsonEscape(MediaRoot),
        *ExcludeLine, *JsonEscape(FbxOutputDir),
        bNoTongueAudio ? TEXT("false") : TEXT("true"),
        bNoTongueAudio ? TEXT("true") : TEXT("false"));
}

FString SMetaHumanSolverWindow::BuildJobJson() const
{
    // 注意：不使用任何硬编码身份路径兜底——该路径是本机特定资产，迁移到同事机器会失效。
    // 深度/视频必须显式填写身份路径（表单字段），留空则校验失败并提示。
    const FString Skeleton = TEXT("/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton");

    FString Mode;
    FString IdentityPath;
    FString CaptureRoot;
    FString AudioPath;
    // 输出路径字段 = 磁盘 FBX 输出目录（ControlRig FBX 最终导出到该磁盘文件夹）
    FString FbxOutputDir;
    // 动画资产统一存储到项目 /Game/MH_Results（必须是 UE 资产路径，与磁盘 FBX 目录分离）
    const FString StoragePath = TEXT("/Game/MH_Results");

    if (ActiveTab == 0) // 深度：走任务列表组队列（OnStart 深度分支），BuildJobJson 仅服务视频/音频
    {
        return FString();
    }
    if (ActiveTab == 1) // 视频
    {
        Mode = TEXT("video");
        IdentityPath = VideoIdentityEdit->GetText().ToString();
        CaptureRoot = VideoInputEdit->GetText().ToString();
        FbxOutputDir = VideoOutputEdit->GetText().ToString();
    }
    else // 音频
    {
        Mode = TEXT("audio");
        AudioPath = AudioInputEdit->GetText().ToString();
        FbxOutputDir = AudioOutputEdit->GetText().ToString();
    }

    if (IdentityPath.IsEmpty() && Mode != TEXT("audio"))
    {
        return FString(); // 校验失败：深度/视频必须显式填身份路径（无硬编码兜底，避免跨机器误用错误身份）
    }
    if (CaptureRoot.IsEmpty() && Mode != TEXT("audio"))
    {
        return FString(); // 校验失败（OnStart 统一提示）
    }
    if (FbxOutputDir.IsEmpty())
    {
        return FString();
    }
    if (Mode == TEXT("audio") && AudioPath.IsEmpty())
    {
        return FString();
    }

    // 分批数量：按当前 Tab 读取对应输入框，仅填正整数时输出 limit 字段（空 = 全部处理）
    TSharedPtr<SEditableTextBox> LimitSrc;
    if (ActiveTab == 1)          { LimitSrc = VideoLimitEdit; }
    else                         { LimitSrc = AudioLimitEdit; }

    FString LimitStr;
    if (LimitSrc.IsValid())
    {
        const FString RawLimit = LimitSrc->GetText().ToString().TrimStartAndEnd();
        const int32 LimitVal = FCString::Atoi(*RawLimit);
        if (LimitVal > 0)
        {
            LimitStr = FString::Printf(TEXT("%d"), LimitVal);
        }
    }

    // limit 存在则插入 JSON 行，否则省略
    const FString LimitLine = LimitStr.IsEmpty() ? FString()
        : FString::Printf(TEXT("  \"limit\": %s,\n"), *LimitStr);

    // 音频模式：显式开启烘焙骨骼 FBX 导出，使"输出路径"真正生效（否则默认关闭，音频跑完无 FBX 产出）
    const FString ExportBakedLine = (Mode == TEXT("audio"))
        ? FString(TEXT("  \"export_baked_fbx\": true,\n"))
        : FString();

    return FString::Printf(TEXT(
        "{\n"
        "  \"mode\": \"%s\",\n"
        "  \"identity_path\": \"%s\",\n"
        "  \"capture_root\": \"%s\",\n"
        "  \"audio_path\": \"%s\",\n"
        "  \"storage_path\": \"%s\",\n"
        "%s%s"
        "  \"export_anim_sequence\": true,\n"
        "  \"export_fbx\": true,\n"
        "  \"export_level_sequence\": true,\n"
        "  \"fbx_output_dir\": \"%s\",\n"
        "  \"skeleton_path\": \"%s\",\n"
        "  \"auto_prepare_identity\": false,\n"
        "  \"import_footage\": false,\n"
        "  \"naming\": { \"pattern\": \"{take_name}_{timecode}\", \"prefix\": \"AS_\" },\n"
        "  \"depth\": { \"using_livelinkface_data\": true, \"blocking\": true },\n"
        "  \"audio\": { \"mood\": \"Auto\", \"mood_intensity\": 1.0, \"process_mask\": \"FullFace\", \"head_movement_mode\": \"ControlRig\" }\n"
        "}"),
        *JsonEscape(Mode), *JsonEscape(IdentityPath), *JsonEscape(CaptureRoot),
        *JsonEscape(AudioPath), *JsonEscape(StoragePath), *LimitLine, *ExportBakedLine,
        *JsonEscape(FbxOutputDir), *JsonEscape(Skeleton));
}

FString SMetaHumanSolverWindow::BuildExcludeJson() const
{
    // 多行文本（每行一个素材名）-> "exclude": ["a", "b"]，空输入返回空串（字段省略）。
    if (!ExcludeEdit.IsValid())
    {
        return FString();
    }
    TArray<FString> Lines;
    ExcludeEdit->GetText().ToString().ParseIntoArrayLines(Lines);
    TArray<FString> Items;
    for (const FString& Line : Lines)
    {
        const FString Trimmed = Line.TrimStartAndEnd();
        if (!Trimmed.IsEmpty())
        {
            Items.Add(FString::Printf(TEXT("\"%s\""), *JsonEscape(Trimmed)));
        }
    }
    if (Items.IsEmpty())
    {
        return FString();
    }
    return FString::Printf(TEXT("  \"exclude\": [%s],\n"), *FString::Join(Items, TEXT(", ")));
}

void SMetaHumanSolverWindow::HandleMHLogLine(const FString& Line)
{
    // [MHS_STREAM] REMAINING <n> —— stream_next 返回协议（批次5）：
    // 更新剩余数 + 段间灌数（UI 从 stream_state.json 刷新一次；阻塞期 UI 冻结由快照语义承担）
    if (Line.Contains(TEXT("[MHS_STREAM] REMAINING")))
    {
        const int32 RIdx = Line.Find(TEXT("REMAINING"));
        if (RIdx != INDEX_NONE)
        {
            const FString NumStr = Line.RightChop(RIdx + FCString::Strlen(TEXT("REMAINING"))).TrimStartAndEnd();
            StreamRemaining = FCString::Atoi(*NumStr);
        }
        RefreshStreamUIFromState();
        return;
    }

    if (Line.Contains(TEXT("[MHS_PROGRESS]")))
    {
        // 格式: [MHS_PROGRESS] 3/12 Shot_xxx
        const int32 Bracket = Line.Find(TEXT("]"));
        int32 Start = (Bracket != INDEX_NONE) ? Bracket + 1 : 0;
        while (Start < Line.Len() && (Line[Start] == TEXT(' ') || Line[Start] == TEXT('\t')))
        {
            ++Start;
        }
        const FString Rest = Line.Mid(Start);
        FString Counts;
        if (Rest.Split(TEXT(" "), &Counts, nullptr))
        {
            FString Cur, Tot;
            if (Counts.Split(TEXT("/"), &Cur, &Tot))
            {
                const int32 CurNum = FCString::Atoi(*Cur);
                const int32 TotNum = FCString::Atoi(*Tot);
                if (TotNum > 0)
                {
                    TotalShots = TotNum; // 记录总段数，用于段间推进判断
                }
                SetProgress(CurNum, TotNum);
            }
        }
        return;
    }

    // [MHS_DISPLAY] 表演级显示进度：仅驱动进度条（MHS_PROGRESS 的段级 total 供推进，两协议解耦）
    if (Line.Contains(TEXT("[MHS_DISPLAY]")))
    {
        const int32 Bracket = Line.Find(TEXT("]"));
        int32 Start = (Bracket != INDEX_NONE) ? Bracket + 1 : 0;
        while (Start < Line.Len() && (Line[Start] == TEXT(' ') || Line[Start] == TEXT('\t')))
        {
            ++Start;
        }
        const FString Rest = Line.Mid(Start);
        FString Counts;
        if (Rest.Split(TEXT(" "), &Counts, nullptr))
        {
            FString Cur, Tot;
            if (Counts.Split(TEXT("/"), &Cur, &Tot))
            {
                SetProgress(FCString::Atoi(*Cur), FCString::Atoi(*Tot));
            }
        }
        return;
    }

    if (Line.Contains(TEXT("[MHS_RESULT]")))
    {
        // 格式: [MHS_RESULT] name|success|elapsed
        const int32 Bracket = Line.Find(TEXT("]"));
        int32 Start = (Bracket != INDEX_NONE) ? Bracket + 1 : 0;
        while (Start < Line.Len() && (Line[Start] == TEXT(' ') || Line[Start] == TEXT('\t')))
        {
            ++Start;
        }
        const FString Payload = Line.Mid(Start);
        TArray<FString> Parts;
        Payload.ParseIntoArray(Parts, TEXT("|"), true);
        if (Parts.Num() >= 3 && ResultListEdit.IsValid())
        {
            const FString ShotName = Parts[0];
            const bool bOk = Parts[1] == TEXT("1");
            const FString Elapsed = Parts[2];
            // 追加一行结果：✓ 段名 耗时 或 ✗ 段名（失败原因见日志）
            FString NewLine;
            if (bOk)
            {
                NewLine = FString::Printf(TEXT("%s  %s  %ss"), TEXT("[OK]"), *ShotName, *Elapsed);
            }
            else
            {
                NewLine = FString::Printf(TEXT("%s  %s"), TEXT("[FAIL]"), *ShotName);
            }
            const FText CurText = ResultListEdit->GetText();
            const FString Combined = CurText.IsEmpty() ? NewLine : CurText.ToString() + TEXT("\n") + NewLine;
            ResultListEdit->SetText(FText::FromString(Combined));
        }
        return;
    }

    if (Line.Contains(TEXT("[MHS_STATUS]")))
    {
        // 格式: [MHS_STATUS] running|idle|error
        const int32 Bracket = Line.Find(TEXT("]"));
        int32 Start = (Bracket != INDEX_NONE) ? Bracket + 1 : 0;
        while (Start < Line.Len() && (Line[Start] == TEXT(' ') || Line[Start] == TEXT('\t')))
        {
            ++Start;
        }
        const FString State = Line.Mid(Start).TrimStartAndEnd();
        SetStatusDot(State);
        return;
    }

    if (Line.Contains(TEXT("[MHS_DONE]")))
    {
        AppendLog(Line, ColorLogOk);
        // 段间驱动：若还有下一段且未被取消，则继续；否则结束本组
        const bool bMore = (TotalShots > 0) && (CurrentShot + 1 < TotalShots) && !bCancelRequested;
        if (bMore)
        {
            ++CurrentShot;
            if (UWorld* World = GEditor->GetEditorWorldContext().World())
            {
                World->GetTimerManager().SetTimer(
                    BatchTimer,
                    FTimerDelegate::CreateLambda([this]() { TickNextSegment(); }),
                    0.15f, false);
            }
            else
            {
                TickNextSegment();
            }
            return; // bProcessing 保持 true，继续下一段
        }
        // 组间推进：任务列表还有下一组且未被取消 -> 写下一组 job 继续段驱动
        if (!bCancelRequested && PendingGroups.IsValidIndex(CurrentGroupIndex + 1))
        {
            StopBatchTimer();
            SetProgress(0, 0);
            if (UWorld* World = GEditor->GetEditorWorldContext().World())
            {
                World->GetTimerManager().SetTimer(
                    BatchTimer,
                    FTimerDelegate::CreateLambda([this]() { StartNextGroup(); }),
                    0.2f, false);
            }
            else
            {
                StartNextGroup();
            }
            return;
        }
        bProcessing = false;
        bCancelRequested = false;
        CurrentGroupIndex = -1;
        PendingGroups.Empty();
        StopBatchTimer();
        UpdateButtonsState();
        return;
    }
    if (Line.Contains(TEXT("[MHS_ERROR]")))
    {
        AppendLog(Line, ColorLogError);
        return;
    }
    if (Line.Contains(TEXT("[MHS_WARN]")))
    {
        AppendLog(Line, ColorLogWarn);
        return;
    }
    AppendLog(Line, ColorTextDim);
}

void SMetaHumanSolverWindow::AppendLog(const FString& Text, const FLinearColor& Color)
{
    if (!LogEdit.IsValid())
    {
        return;
    }
    FString Current = LogEdit->GetText().ToString();
    if (Current == TEXT("就绪"))
    {
        Current.Empty();
    }
    Current += Text;
    Current += TEXT("\n");
    LogEdit->SetText(FText::FromString(Current));
}

void SMetaHumanSolverWindow::SetProgress(int32 Current, int32 Total)
{
    if (ProgressBar.IsValid())
    {
        ProgressBar->SetPercent(Total > 0 ? FMath::Clamp((float)Current / (float)Total, 0.0f, 1.0f) : 0.0f);
    }
    if (ProgressLabel.IsValid())
    {
        ProgressLabel->SetText(FText::FromString(
            Total > 0 ? FString::Printf(TEXT("%d/%d"), Current, Total) : TEXT("0%")));
    }
}

#undef LOCTEXT_NAMESPACE
