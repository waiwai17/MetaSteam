#include "MetaHumanSolverCommands.h"

#include "Framework/Docking/TabManager.h"
#include "MHSStreamConsole.h"

void UMetaHumanSolverCommands::OpenToolWindow()
{
    // ToolTabId 与 MetaHumanSolverModule.cpp 的静态常量一致（"MetaHumanSolverTab"）
    FGlobalTabmanager::Get()->TryInvokeTab(FName("MetaHumanSolverTab"));
}

void UMetaHumanSolverCommands::ShowStreamConsole(const FString& InInbox, const FString& InOutputDir)
{
    MHSStreamConsole::Get().Launch(InInbox, InOutputDir);
}
