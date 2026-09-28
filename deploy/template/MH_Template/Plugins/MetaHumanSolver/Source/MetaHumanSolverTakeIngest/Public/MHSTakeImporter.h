// MHSTakeImporter.h
// 正式 take 导入管线（①解析 → ②媒体转换 → ③cparch 解析 → ④创建 UE 资产 → ⑤组装 UFootageCaptureData → ⑥保存）。
// 产出 7 个资产/每个 take：CD_(FootageCaptureData) IS_Video_ IS_Depth_ SW_Audio_ CC_ LF_Video_ LF_Depth_，
// 与 LiveLinkHub 捕获管理器导入产物完全兼容，可直接作为深度解算链路的输入。
// 用法（Python 控制台 / 无头 -run=pythonscript）：
//   unreal.MHSTakeImporter.import_take_directory(r"<take目录>", r"<转换输出根目录>")
//   unreal.MHSTakeImporter.import_takes_from_root(r"<take根目录>", r"<转换输出根目录>")  # 递归批处理
// 结果通过 LogMHSTakeImporter 日志类别输出（控制台过滤 MHSTake）。
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MHSTakeImporter.generated.h"

UCLASS()
class METAHUMANSOLVERTAKEINGEST_API UMHSTakeImporter : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * 导入单个 take 目录（完整管线：解析→转换→建资产→组装→保存）。
	 * @param InTakeDirectory    Live Link Face App 导出的 take 目录（含 take.json 与 mov）
	 * @param InOutputRoot       媒体转换产物输出根目录（磁盘路径；每 take 建独立子目录，已存在则清空重建）
	 * @param InPackageRootPath  UE 资产存放根路径（如 /Game/CaptureManager/Imports）
	 * @param bBlocking          true=当前线程同步执行（无头批处理必须）；false=投递到游戏线程异步执行
	 */
	UFUNCTION(Exec, BlueprintCallable, Category = "MetaHumanSolver|TakeImport")
	static void ImportTakeDirectory(const FString& InTakeDirectory, const FString& InOutputRoot,
		const FString& InPackageRootPath = TEXT("/Game/CaptureManager/Imports"), bool bBlocking = true);

	/**
	 * 递归批处理导入：扫描根目录下所有含 take 元数据文件的子目录，逐个执行完整导入管线。
	 * 转换（①~③，耗时大头）并发在后台线程执行，并发数受 InConcurrency 控制。
	 * @param InTakeRootDirectory take 根目录（其下任意层级含 take.json / *.cptake 的目录都会被导入）
	 * @param InConcurrency       最大并发转换数；0/负 = auto（按本机物理核数自动计算），>0 = 固定值
	 */
	UFUNCTION(Exec, BlueprintCallable, Category = "MetaHumanSolver|TakeImport")
	static void ImportTakesFromRoot(const FString& InTakeRootDirectory, const FString& InOutputRoot,
		const FString& InPackageRootPath = TEXT("/Game/CaptureManager/Imports"), bool bBlocking = true,
		int32 InConcurrency = 0);

	/**
	 * 导入并发信息（供预检报告/前端展示）：与实际导入完全一致的计算结果。
	 * @param InConcurrency 与 ImportTakesFromRoot 相同的语义（0/负=auto，>0=固定值）
	 * @return [0]=物理核数, [1]=逻辑核数, [2]=实际生效并发数（auto 或固定值，均已 clamp 到 [1,16]）
	 */
	UFUNCTION(BlueprintCallable, Category = "MetaHumanSolver|TakeImport")
	static TArray<int32> GetRecommendedImportConcurrency(int32 InConcurrency = 0);

	/**
	 * 显式 take 列表导入（供 Python 面板混合段批量并发）：按给定 take 目录列表并发转换。
	 * 与 ImportTakesFromRoot 的区别：不扫描目录，只导入传入的 take（可精确跳过 ROM/已排除项）。
	 * @param InTakeDirectories  待导入的 take 目录列表（全部导入，Python 侧自行做幂等过滤）
	 * @param InConcurrency      最大并发转换数；0/负 = auto（按本机物理核数自动计算），>0 = 固定值
	 */
	UFUNCTION(Exec, BlueprintCallable, Category = "MetaHumanSolver|TakeImport")
	static void ImportTakes(const TArray<FString>& InTakeDirectories, const FString& InOutputRoot,
		const FString& InPackageRootPath = TEXT("/Game/CaptureManager/Imports"),
		bool bBlocking = true, int32 InConcurrency = 0);
};
