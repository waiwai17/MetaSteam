// MHSTakeIngestUtils.h
// take 导入管线的公共步骤（①解析 ②转换 ③cparch 解析），spike 验证与正式导入共用。
// 全部步骤均经引擎源码核实 + spike 实测通过。
#pragma once

#include "CoreMinimal.h"

class FTakeMetadata;
struct FCaptureDataConverterParams;
template<typename T, typename E> class TValueOrError;
class FCaptureDataConverterError;

namespace MHSTakeIngest
{
	/** ① 发现候选 take 元数据文件（*.cptake 优先，其次 take.json / 裸 take） */
	TArray<FString> FindTakeMetadataCandidates(const FString& InTakeDirectory);

	/** ① 解析 take 元数据：先新格式 FTakeMetadataParser，逐候选；全失败回退 legacy 目录解析 */
	bool ParseTakeMetadata(const FString& InTakeDirectory, FTakeMetadata& OutTakeMetadata, FString& OutSourceDescription);

	/** ① 后处理：Video/Depth/Audio/Calibration 相对路径转绝对路径（复刻 UTakeArchiveIngestDevice::ReadTake） */
	void NormalizeMetadataPaths(FTakeMetadata& InOutTakeMetadata, const FString& InTakeDirectory);

	/** 由元数据生成 take 名（Slate_TakeNumber，Slate 空则目录名） */
	FString MakeTakeName(const FTakeMetadata& InTakeMetadata, const FString& InTakeDirectory);

	/** ② 组装转换参数（对齐 UBaseIngestLiveLinkDevice::RunConversion；像素格式必须 U8_BGRA） */
	FCaptureDataConverterParams BuildConverterParams(const FTakeMetadata& InTakeMetadata, const FString& InTakeName,
		const FString& InTakeOriginDirectory, const FString& InTakeOutputDirectory);

	/** ② 阻塞执行转换：后台线程跑 FCaptureDataConverter::Run，当前线程 FEvent 等待完成（GameThread 可安全调用） */
	TValueOrError<void, FCaptureDataConverterError> RunConversionBlocking(FCaptureDataConverterParams InParams);
}
