// MetaHumanSolverTakeIngest.Build.cs
// take 导入模块：验证引擎 CaptureManager 转换链路（spike）+ 正式导入管线（MHSTakeImporter）。
// 与主模块 MetaHumanSolver 完全隔离，整体可移除。
using UnrealBuildTool;

public class MetaHumanSolverTakeIngest : ModuleRules
{
    public MetaHumanSolverTakeIngest(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Kismet",
            // take 元数据解析（FTakeMetadata / FTakeMetadataParser）—— CaptureManagerCore 插件
            "CaptureManagerTakeMetadata",
            // legacy Live Link Face 目录解析回退 —— CaptureManagerApp 插件
            "LiveLinkFaceMetadata",
            // 媒体转换器（FCaptureDataConverter）—— CaptureManagerApp 插件
            "CaptureDataConverter",
            "CaptureManagerPipeline",
            "CaptureManagerMediaRW",
            // 转换产物 .cparch 解析 / ParseFrameRate / FUnrealCalibrationParser —— CaptureManagerCore 插件
            "DataIngestCore",
            // ④ 资产创建（FIngestAssetCreator / FCreateAssetsData）—— CaptureManagerEditor 插件
            "DataIngestCoreEditor",
            // TManagedDelegate（FPerTakeCallback）—— CaptureManagerCore 插件
            "CaptureUtils",
            // UFootageCaptureData / UCameraCalibration —— CaptureData 插件
            "CaptureDataCore",
            // UImgMediaSource 完整类型
            "ImgMedia",
            // EMediaOrientation（IMediaTextureSample.h）
            "Media",
            // ObjectTools::SanitizeObjectName / UEditorLoadingAndSavingUtils
            "UnrealEd",
            // 资产枚举与保存
            "AssetRegistry",
            "AssetTools",
            // 阻塞等待期间泵 Slate 消息（FSlateApplication::PumpMessages / IsInitialized）
            "Slate",
            // 批次导入任务进度（FMHSProgressMonitor::UpdateTaskProgress）—— 主模块
            "MetaHumanSolver",
        });
    }
}
