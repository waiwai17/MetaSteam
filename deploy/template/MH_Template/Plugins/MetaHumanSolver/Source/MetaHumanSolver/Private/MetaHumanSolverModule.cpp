// MetaHumanSolverModule.cpp
#include "MetaHumanSolverModule.h"
#include "SMetaHumanSolverWindow.h"
#include "MHSProgressMonitor.h"
#include "ToolMenus.h"
#include "Framework/Docking/TabManager.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Styling/AppStyle.h"

DEFINE_LOG_CATEGORY_STATIC(LogMetaHumanSolver, Log, All);

#define LOCTEXT_NAMESPACE "MetaHumanSolver"

static const FName ToolTabId("MetaHumanSolverTab");

void FMetaHumanSolverModule::StartupModule()
{
    // 用 RegisterStartupCallback 等待菜单系统就绪再注册，避免 StartupModule 早于主菜单创建导致
    // ExtendMenu("LevelEditor.MainMenu.Window.VirtualProduction") 因父菜单未初始化而失败。
    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FMetaHumanSolverModule::RegisterMenu));

    // 注册 Dockable Tab——类似 LiveLinkHub 的打开方式
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        ToolTabId,
        FOnSpawnTab::CreateRaw(this, &FMetaHumanSolverModule::SpawnToolTab))
        .SetDisplayName(LOCTEXT("TabTitle", "MetaHuman \u89E3\u7B97\u5DE5\u5177"))
        .SetTooltipText(LOCTEXT("TabTooltip", "\u6253\u5F00 MetaHuman \u89E3\u7B97\u5DE5\u5177"))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

    UE_LOG(LogMetaHumanSolver, Log, TEXT("Plugin started, tab registered."));
}

void FMetaHumanSolverModule::ShutdownModule()
{
    // 进度监控兜底清理：停心跳线程、关原生小窗、移除逐帧回调（设计文档 7.7）。
    // 若解算中途关编辑器，这里保证线程/窗口不悬空。
    FMHSProgressMonitor::Get().ForceStop();

    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ToolTabId);

    if (UToolMenus* Menus = UToolMenus::TryGet())
    {
        // 同时删除新位置和旧位置的 section，避免升级插件后旧 Entry 残留
        Menus->RemoveSection("LevelEditor.MainMenu.Window.VirtualProduction", "VirtualProduction");
        Menus->RemoveSection("LevelEditor.MainMenu.Window", "MetaHumanSolver");
        Menus->RemoveSection("LevelEditor.LevelEditorToolBar.User", "MetaHumanSolver");
    }
}

void FMetaHumanSolverModule::RegisterMenu()
{
    UToolMenus* Menus = UToolMenus::Get();
    if (!Menus) return;

    // 注册到"虚拟制片"分组（顶级菜单下的子菜单，与 LiveLinkHub、Switchboard、TrackingAlignment 同组），
    // 符合 MetaHuman 解算工具在虚拟制片流程里的实际用途。
    // 用 ExtendMenu 而非 FindMenu：子菜单不存在时自动创建，避免 FindMenu 返回 null 导致注册失败。
    UToolMenu* WindowMenu = Menus->ExtendMenu("LevelEditor.MainMenu.Window.VirtualProduction");
    if (!WindowMenu) return;

    FToolMenuSection& Section = WindowMenu->FindOrAddSection("VirtualProduction");
    // 图标：Icons.Filter 是 UE 长期稳定的 brush 名（漏斗图标，象征"批量筛选/处理素材"），
    // 在 EditorStyle/AppStyle 中都有 Icon16x16/20x20/40x40 多种尺寸变体，主菜单用 16x16 自动渲染。
    FToolMenuEntry Entry = FToolMenuEntry::InitMenuEntry(
        "MetaHumanSolver",
        LOCTEXT("MenuLabel", "MetaHuman \u89E3\u7B97\u5DE5\u5177"),
        LOCTEXT("MenuTooltip", "\u6253\u5F00 MetaHuman \u89E3\u7B97\u5DE5\u5177"),
        FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Filter")),
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            FGlobalTabmanager::Get()->TryInvokeTab(ToolTabId);
        }))
    );
    Section.AddEntry(Entry);

    // ── 工具栏按钮：Level Editor 工具栏"Actor 后面"的自定义区域（截图里 Actor 右侧的灰色空白区）──
    // 这是 Switchboard / LiveLinkHub 等同款做法，点按钮直接打开工具窗口，曝光度比菜单更高。
    UToolMenu* ToolbarMenu = Menus->ExtendMenu("LevelEditor.LevelEditorToolBar.User");
    if (ToolbarMenu)
    {
        FToolMenuSection& TbSection = ToolbarMenu->FindOrAddSection("MetaHumanSolver");
        FToolMenuEntry TbEntry = FToolMenuEntry::InitToolBarButton(
            "MetaHumanSolver.Toolbar",
            FUIAction(FExecuteAction::CreateLambda([]()
            {
                FGlobalTabmanager::Get()->TryInvokeTab(ToolTabId);
            })),
            LOCTEXT("ToolbarLabel", "MetaHuman \u89E3\u7B97\u5DE5\u5177"),
            LOCTEXT("ToolbarTooltip", "\u6253\u5F00 MetaHuman \u89E3\u7B97\u5DE5\u5177\u7A97\u53E3"),
            FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Filter"))
        );
        TbSection.AddEntry(TbEntry);
    }

    Menus->RefreshAllWidgets();
}

TSharedRef<SDockTab> FMetaHumanSolverModule::SpawnToolTab(const FSpawnTabArgs& Args)
{
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        .Label(LOCTEXT("TabLabel", "MetaHuman \u89E3\u7B97\u5DE5\u5177"))
        [
            SNew(SMetaHumanSolverWindow)
        ];
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMetaHumanSolverModule, MetaHumanSolver)
