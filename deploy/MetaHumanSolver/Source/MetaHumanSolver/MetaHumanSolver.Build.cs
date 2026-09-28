// MetaHumanSolver.Build.cs
using UnrealBuildTool;

public class MetaHumanSolver : ModuleRules
{
    public MetaHumanSolver(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            // 进度监控：注册 UMetaHumanPerformance::OnFrameProcessed 逐帧回调（硬前提，设计文档 12.1）
            "MetaHumanPerformance",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate",
            "SlateCore",
            "ToolMenus",
            "EditorSubsystem",
            "EditorStyle",
            "InputCore",
            "WorkspaceMenuStructure",
            "DesktopPlatform",
            "ContentBrowser",
            "AssetRegistry",
            "UnrealEd",
            // Stream 状态灌数（批次5）：解析 stream_state.json
            "Json",
        });
    }
}
