// MetaHumanSolverCommands.h
// Python/蓝图可调的工具窗口命令（自动化与远程操作入口）
#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "MetaHumanSolverCommands.generated.h"

UCLASS()
class UMetaHumanSolverCommands : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // 打开 MetaHuman 解算工具主窗口（面板已打开时幂等聚焦）。
    // Python: unreal.MetaHumanSolverCommands.open_tool_window()
    // 用途：自动化测试打开面板（日志设备随面板 Construct 注册）/ 远程流程拉起 UI。
    UFUNCTION(BlueprintCallable, Category = "MetaHumanSolver|Window")
    static void OpenToolWindow();

    // 拉起全局进度控制台（方案 A：conhost 分区渲染，解算阻塞期间照常刷新）。
    // Python: unreal.MetaHumanSolverCommands.show_stream_console(inbox, output_dir)
    // 用途：自动化测试 / 无面板场景（命令行流式）也能查看全局进度。
    UFUNCTION(BlueprintCallable, Category = "MetaHumanSolver|Window")
    static void ShowStreamConsole(const FString& InInbox, const FString& InOutputDir);
};
