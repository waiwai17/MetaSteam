// MHSTakeImportSpike.h
// Spike 验证入口：在编辑器进程内直接调用引擎 CaptureManager 的转换链路，
// 验证 Live Link Face take 目录（take.json + _Face.mov + depth_data.bin + depth_metadata.mhaical）
// 可被解析、转换（mov→图片序列 / 深度→EXR / 音频 / 标定）并产出 take.cparch。
// 用法（Python 控制台）：
//   unreal.MHSTakeImportSpike.run_take_ingest_spike(r"<take目录>", r"<输出根目录>")
// 结果全部通过 LogMHSTakeSpike 日志类别输出（控制台过滤 MHSSpike）。
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MHSTakeImportSpike.generated.h"

UCLASS()
class METAHUMANSOLVERTAKEINGEST_API UMHSTakeImportSpike : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * 运行 take 转换链路 Spike 验证。
	 * 异步执行（不阻塞游戏线程），进度与结论见 LogMHSTakeSpike 日志。
	 * @param InTakeDirectory       Live Link Face App 导出的 take 目录（含 take.json 与 mov）
	 * @param InOutputRootDirectory 转换产物输出根目录（每个 take 在其下建独立子目录）
	 * @param bBlocking             是否阻塞等待转换完成（命令行无头模式必须传 true：
	 *                              -run=pythonscript 脚本返回后引擎即退出，异步任务会被截断）
	 */
	UFUNCTION(Exec, BlueprintCallable, Category = "MetaHumanSolver|TakeIngestSpike")
	static void RunTakeIngestSpike(const FString& InTakeDirectory, const FString& InOutputRootDirectory, bool bBlocking = false);
};
