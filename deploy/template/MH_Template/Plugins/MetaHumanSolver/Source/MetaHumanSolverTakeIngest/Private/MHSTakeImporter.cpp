// MHSTakeImporter.cpp
// 完整导入管线实现。③④⑤ 复刻自引擎官方实现：
//   ③ PrepareAssetsData   ← IngestCaptureDataProcess.cpp（去 NamingTokens 化，固定命名规则）
//   ④ CreateAssets_GameThread ← DataIngestCoreEditor 公开 API 直接调用
//   ⑤ CreateCaptureAsset  ← LiveLinkHubWorker.cpp（组装 UFootageCaptureData）
//   ⑥ SaveCaptureCreatedAssets ← LiveLinkHubWorker.cpp（SavePackages）
#include "MHSTakeImporter.h"

#include "MHSTakeIngestUtils.h"

#include "Async/Async.h"
#include <atomic>
#include "ObjectTools.h"
#include "FileHelpers.h"
#include "Containers/Queue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"
#include "Framework/Application/SlateApplication.h"
#include "MHSProgressMonitor.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/HideWindowsPlatformTypes.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

#include "CaptureDataConverter.h"            // FCaptureDataConverterResult
#include "CaptureManagerTakeMetadata.h"      // FTakeMetadata
#include "IngestCaptureData.h"               // IngestCaptureData::ParseFile
#include "Utils/ParseTakeUtils.h"            // ParseFrameRate / ParseTimecode
#include "Utils/UnrealCalibrationParser.h"   // FUnrealCalibrationParser
#include "IngestAssetCreator.h"              // FIngestAssetCreator / FCreateAssetsData
#include "CaptureData.h"                     // UFootageCaptureData
#include "ImgMediaSource.h"                  // UImgMediaSource（完整类型，日志需要 GetPathName）

DEFINE_LOG_CATEGORY_STATIC(LogMHSTakeImporter, Log, All);

namespace
{
	const TCHAR* kPrefix = TEXT("[MHSTake]");

	// ── 异步导入工作产物：Prepare（①②③，后台线程）产出 → Finalize（④⑤⑥，游戏线程）消费 ──
	struct FMHSImportWork
	{
		bool bOk = false;
		double StartWallClock = 0.0;   // Prepare 开始墙钟（完成后计算纯耗时，供剩余时间外推）
		FString TakeName;          // take.json 生成的 take 名
		FString TakeDirectory;     // 原始 take 目录（规范化全路径；活跃名单匹配用目录 basename）
		FString TakeOutputDirectory;
		UE::CaptureManager::FCreateAssetsData CreateAssetData;
		FIngestCaptureData IngestCaptureData;
		FString ErrorText;
	};

	// 泵 Slate 消息（仅游戏线程调用）：阻塞等待后台转换期间保持窗口响应。
	// 不 Tick 整个 Slate（避免从 Exec 调用栈重入 Slate 更新），只泵已排队消息——
	// 输入消息被处理即根治"失焦卡死/需点击才继续"，与 MHSProgressMonitor 的 poke 协同。
	void PumpMessages_GameThread()
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().PumpMessages();
		}
	}

	// ── ③ 复刻 FIngestCaptureDataProcess::ConvertPathsToFull ──────────────────
	void ConvertPathsToFull(const FString& InTakeStoragePath, FIngestCaptureData& InOutIngestCaptureData)
	{
		for (FIngestCaptureData::FVideo& Video : InOutIngestCaptureData.Video)
		{
			Video.Path = FPaths::ConvertRelativePathToFull(InTakeStoragePath, Video.Path);
		}
		for (FIngestCaptureData::FVideo& Depth : InOutIngestCaptureData.Depth)
		{
			Depth.Path = FPaths::ConvertRelativePathToFull(InTakeStoragePath, Depth.Path);
		}
		for (FIngestCaptureData::FAudio& Audio : InOutIngestCaptureData.Audio)
		{
			Audio.Path = FPaths::ConvertRelativePathToFull(InTakeStoragePath, Audio.Path);
		}
		for (FIngestCaptureData::FCalibration& Calibration : InOutIngestCaptureData.Calibration)
		{
			Calibration.Path = FPaths::ConvertRelativePathToFull(InTakeStoragePath, Calibration.Path);
		}
	}

	// 资产名/路径段清理（官方用 Private 的 SanitizeAssetName，此处用 ObjectTools 等价实现）
	FString SanitizeName(const FString& InName)
	{
		return ObjectTools::SanitizeObjectName(InName);
	}

	// ── ③ 复刻 PrepareAssetsData（固定命名规则：CD_/IS_/SW_/CC_/LF_ 对齐官方默认产出）──
	bool PrepareAssetsData(const FIngestCaptureData& InIngestCaptureData, const FString& InTakeName,
		const FString& InPackageRootPath, UE::CaptureManager::FCreateAssetsData& OutCreateAssetData)
	{
		using namespace UE::CaptureManager;

		const FString SlateTake = InIngestCaptureData.Slate.IsEmpty()
			? InTakeName
			: FString::Printf(TEXT("%s_%d"), *InIngestCaptureData.Slate, InIngestCaptureData.TakeNumber);

		OutCreateAssetData.TakeId = 0;
		OutCreateAssetData.PackagePath = InPackageRootPath / SanitizeName(InTakeName);
		OutCreateAssetData.CaptureDataAssetName = SanitizeName(TEXT("CD_") + SlateTake);

		for (const FIngestCaptureData::FVideo& Video : InIngestCaptureData.Video)
		{
			FCreateAssetsData::FImageSequenceData ImageSequenceData;
			ImageSequenceData.AssetName = SanitizeName(TEXT("IS_") + Video.Name + TEXT("_") + SlateTake);
			ImageSequenceData.Name = Video.Name;
			ImageSequenceData.SequenceDirectory = Video.Path;
			ImageSequenceData.FrameRate = Video.FrameRate.IsSet() ? ParseFrameRate(Video.FrameRate.GetValue()) : FFrameRate();
			ImageSequenceData.bTimecodePresent = Video.TimecodeStart.IsSet();
			ImageSequenceData.Timecode = Video.TimecodeStart.IsSet() ? ParseTimecode(Video.TimecodeStart.GetValue()) : FTimecode();
			ImageSequenceData.TimecodeRate = ImageSequenceData.FrameRate;
			OutCreateAssetData.ImageSequences.Add(MoveTemp(ImageSequenceData));
		}

		for (const FIngestCaptureData::FVideo& Depth : InIngestCaptureData.Depth)
		{
			FCreateAssetsData::FImageSequenceData DepthSequenceData;
			DepthSequenceData.AssetName = SanitizeName(TEXT("IS_") + Depth.Name + TEXT("_") + SlateTake);
			DepthSequenceData.Name = Depth.Name;
			DepthSequenceData.SequenceDirectory = Depth.Path;
			DepthSequenceData.FrameRate = Depth.FrameRate.IsSet() ? ParseFrameRate(Depth.FrameRate.GetValue()) : FFrameRate();
			DepthSequenceData.bTimecodePresent = Depth.TimecodeStart.IsSet();
			DepthSequenceData.Timecode = Depth.TimecodeStart.IsSet() ? ParseTimecode(Depth.TimecodeStart.GetValue()) : FTimecode();
			DepthSequenceData.TimecodeRate = DepthSequenceData.FrameRate;
			OutCreateAssetData.DepthSequences.Add(MoveTemp(DepthSequenceData));
		}

		for (const FIngestCaptureData::FAudio& Audio : InIngestCaptureData.Audio)
		{
			FCreateAssetsData::FAudioData AudioData;
			AudioData.AssetName = SanitizeName(TEXT("SW_") + Audio.Name + TEXT("_") + SlateTake);
			AudioData.Name = Audio.Name;
			AudioData.WAVFile = Audio.Path;
			AudioData.bTimecodePresent = Audio.TimecodeStart.IsSet();
			AudioData.Timecode = Audio.TimecodeStart.IsSet() ? ParseTimecode(Audio.TimecodeStart.GetValue()) : FTimecode();
			AudioData.TimecodeRate = Audio.TimecodeRate.IsSet() ? ParseFrameRate(Audio.TimecodeRate.GetValue()) : FFrameRate();
			OutCreateAssetData.AudioClips.Add(MoveTemp(AudioData));
		}

		bool bCalibrationParsed = false;
		for (const FIngestCaptureData::FCalibration& Calibration : InIngestCaptureData.Calibration)
		{
			FCreateAssetsData::FCalibrationData CalibrationData;
			CalibrationData.AssetName = SanitizeName(TEXT("CC_") + SlateTake);
			CalibrationData.Name = Calibration.Name;

			FUnrealCalibrationParser::FParseResult ParseResult = FUnrealCalibrationParser::Parse(Calibration.Path);
			if (ParseResult.HasValue())
			{
				bCalibrationParsed = true;
				CalibrationData.CameraCalibrations = ParseResult.StealValue();
				for (const FCameraCalibration& CamCalib : CalibrationData.CameraCalibrations)
				{
					CalibrationData.LensFileAssetNames.Add(
						CamCalib.CameraId, SanitizeName(TEXT("LF_") + CamCalib.CameraId + TEXT("_") + SlateTake));
				}
			}
			else
			{
				FText Error = ParseResult.StealError();
				UE_LOG(LogMHSTakeImporter, Error, TEXT("%s 标定解析失败（%s）：%s"),
					kPrefix, *Calibration.Path, *Error.ToString());
			}

			OutCreateAssetData.Calibrations.Add(MoveTemp(CalibrationData));
		}

		// 丢帧区间 → 排除帧（复刻官方逻辑）
		if (!InIngestCaptureData.Video.IsEmpty())
		{
			uint32 LastDroppedFrameIndex = 0;
			uint32 FirstDroppedFrameIndex = 0;
			for (const uint32 DroppedFrameIndex : InIngestCaptureData.Video[0].DroppedFrames)
			{
				if (FirstDroppedFrameIndex == 0)
				{
					FirstDroppedFrameIndex = DroppedFrameIndex;
				}

				if (DroppedFrameIndex - LastDroppedFrameIndex == 1)
				{
					LastDroppedFrameIndex = DroppedFrameIndex;
				}
				else
				{
					FFrameRange Range;
					Range.StartFrame = FirstDroppedFrameIndex;
					Range.EndFrame = LastDroppedFrameIndex;
					OutCreateAssetData.CaptureExcludedFrames.Add(MoveTemp(Range));

					FirstDroppedFrameIndex = DroppedFrameIndex;
					LastDroppedFrameIndex = DroppedFrameIndex;
				}
			}
		}

		return bCalibrationParsed;
	}

	// ── ⑤ 复刻 FLiveLinkHubImportWorker::CreateCaptureAsset ──────────────────
	//（差异：资产已存在时仍重新填充内容，保证批处理幂等）
	bool AssembleCaptureData(const UE::CaptureManager::FCreateAssetsData& InCreateAssetData,
		const UE::CaptureManager::FCaptureDataAssetInfo& InAssetInfo, const FIngestCaptureData& InIngestCaptureData)
	{
		using namespace UE::CaptureManager;

		const FString& CaptureDataName = InCreateAssetData.CaptureDataAssetName;

		UFootageCaptureData* CaptureData = FIngestAssetCreator::GetAssetIfExists<UFootageCaptureData>(InCreateAssetData.PackagePath, CaptureDataName);
		if (!CaptureData)
		{
			CaptureData = FIngestAssetCreator::CreateAsset<UFootageCaptureData>(InCreateAssetData.PackagePath, CaptureDataName);
		}

		if (!CaptureData)
		{
			UE_LOG(LogMHSTakeImporter, Error, TEXT("%s 创建 FootageCaptureData 失败：%s/%s"),
				kPrefix, *InCreateAssetData.PackagePath, *CaptureDataName);
			return false;
		}

		CaptureData->ImageSequences.Reset();
		CaptureData->DepthSequences.Reset();
		CaptureData->CameraCalibrations.Reset();
		CaptureData->AudioTracks.Reset();

		for (const FCaptureDataAssetInfo::FImageSequence& ImageSequence : InAssetInfo.ImageSequences)
		{
			CaptureData->ImageSequences.Add(ImageSequence.Asset);
		}
		for (const FCaptureDataAssetInfo::FImageSequence& DepthSequence : InAssetInfo.DepthSequences)
		{
			CaptureData->DepthSequences.Add(DepthSequence.Asset);
		}
		for (const FCaptureDataAssetInfo::FAudio& Audio : InAssetInfo.Audios)
		{
			CaptureData->AudioTracks.Add(Audio.Asset);
		}
		for (const FCaptureDataAssetInfo::FCalibration& Calibration : InAssetInfo.Calibrations)
		{
			CaptureData->CameraCalibrations.Add(Calibration.Asset);
		}

		if (!InIngestCaptureData.Video.IsEmpty() && InIngestCaptureData.Video[0].FrameRate.IsSet())
		{
			CaptureData->Metadata.FrameRate = InIngestCaptureData.Video[0].FrameRate.GetValue();
		}
		CaptureData->Metadata.DeviceModelName = InIngestCaptureData.DeviceModel;
		CaptureData->Metadata.SetDeviceClass(InIngestCaptureData.DeviceModel);
		CaptureData->CaptureExcludedFrames = InAssetInfo.CaptureExcludedFrames;

		return true;
	}

	// ── ⑥ 复刻 SaveCaptureCreatedAssets ─────────────────────────────────────
	void SaveCreatedAssets(const FString& InPackagePath)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

		TArray<FAssetData> AssetsData;
		AssetRegistry.GetAssetsByPath(FName{ *InPackagePath }, AssetsData, true /*bRecursive*/, false);

		if (AssetsData.IsEmpty())
		{
			return;
		}

		TArray<UPackage*> Packages;
		for (const FAssetData& AssetData : AssetsData)
		{
			UPackage* Package = AssetData.GetAsset()->GetPackage();
			if (!Packages.Contains(Package))
			{
				Packages.Add(Package);
			}
		}

		UEditorLoadingAndSavingUtils::SavePackages(Packages, true);
	}

	// ── 单 take 管线拆分（导入异步化）──────────────────────────────────────
	// Prepare（①②③：解析/转换/cparch，纯磁盘+非 GT API）在后台线程执行；
	// Finalize（④⑤⑥：创建/组装/保存 UE 资产，要求 GameThread）回游戏线程执行。
	FMHSImportWork ImportSingleTake_Prepare(const FString& InTakeDirectory, const FString& InOutputRoot,
		const FString& InPackageRootPath)
	{
		using namespace UE::CaptureManager;
		FMHSImportWork Work;
		Work.StartWallClock = FPlatformTime::Seconds();
		Work.TakeDirectory = FPaths::ConvertRelativePathToFull(InTakeDirectory);

		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s ============ 开始导入 take：%s ============"), kPrefix, *InTakeDirectory);

		// ① 解析
		FTakeMetadata TakeMetadata;
		FString ParseSource;
		if (!MHSTakeIngest::ParseTakeMetadata(InTakeDirectory, TakeMetadata, ParseSource))
		{
			Work.ErrorText = FString::Printf(TEXT("① 解析失败：%s"), *InTakeDirectory);
			UE_LOG(LogMHSTakeImporter, Error, TEXT("%s %s"), kPrefix, *Work.ErrorText);
			return Work;
		}
		MHSTakeIngest::NormalizeMetadataPaths(TakeMetadata, InTakeDirectory);
		const FString TakeName = MHSTakeIngest::MakeTakeName(TakeMetadata, InTakeDirectory);
		Work.TakeName = TakeName;
		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s ① 解析成功（%s）TakeName=%s Video=%d Depth=%d Audio=%d Calibration=%d"),
			kPrefix, *ParseSource, *TakeName, TakeMetadata.Video.Num(), TakeMetadata.Depth.Num(),
			TakeMetadata.Audio.Num(), TakeMetadata.Calibration.Num());

		// ② 转换（输出目录清空重建保证幂等）
		const FString TakeOutputDirectory = FPaths::Combine(InOutputRoot, TakeName);
		Work.TakeOutputDirectory = TakeOutputDirectory;
		constexpr bool bRequireExists = false;
		constexpr bool bTree = true;
		IFileManager::Get().DeleteDirectory(*TakeOutputDirectory, bRequireExists, bTree);
		IFileManager::Get().MakeDirectory(*TakeOutputDirectory, true);

		FCaptureDataConverterParams Params = MHSTakeIngest::BuildConverterParams(
			TakeMetadata, TakeName, InTakeDirectory, TakeOutputDirectory);

		// ② 转换（前后采样进程累计 CPU 时间 → 平均核占用，供并发数校准）
#if PLATFORM_WINDOWS
		FILETIME FTCr = {}, FTEx = {}, FTK1 = {}, FTU1 = {}, FTK2 = {}, FTU2 = {};
		auto QueryCpuTicks = [](const FILETIME& K, const FILETIME& U)
		{
			uint64 k = ((uint64)K.dwHighDateTime << 32) | (uint64)K.dwLowDateTime;
			uint64 u = ((uint64)U.dwHighDateTime << 32) | (uint64)U.dwLowDateTime;
			return k + u;
		};
		::GetProcessTimes(::GetCurrentProcess(), &FTCr, &FTEx, &FTK1, &FTU1);
		const uint64 TicksA = QueryCpuTicks(FTK1, FTU1);
		const double StartWall = FPlatformTime::Seconds();
#endif
		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s ② 转换启动（输出：%s）..."), kPrefix, *TakeOutputDirectory);
		FCaptureDataConverterResult<void> ConversionResult = MHSTakeIngest::RunConversionBlocking(MoveTemp(Params));

#if PLATFORM_WINDOWS
		::GetProcessTimes(::GetCurrentProcess(), &FTCr, &FTEx, &FTK2, &FTU2);
		const uint64 TicksB = QueryCpuTicks(FTK2, FTU2);
		const double WallSec = FPlatformTime::Seconds() - StartWall;
		const double CpuSec = double(TicksB - TicksA) / 1.0e7; // FILETIME 100ns 单位
		const double AvgCores = (WallSec > 0.01) ? (CpuSec / WallSec) : 0.0;
		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s ② 转换诊断: 耗时 %.1fs · 平均核占用 %.1f 核（本机物理核 %d / 逻辑核 %d）"),
			kPrefix, WallSec, AvgCores, FPlatformMisc::NumberOfCores(), FPlatformMisc::NumberOfCoresIncludingHyperthreads());
#endif

		if (ConversionResult.HasError())
		{
			FCaptureDataConverterError Error = ConversionResult.StealError();
			TArray<FText> Errors = Error.GetErrors();
			UE_LOG(LogMHSTakeImporter, Error, TEXT("%s ② 转换失败，共 %d 条错误："), kPrefix, Errors.Num());
			for (const FText& Message : Errors)
			{
				UE_LOG(LogMHSTakeImporter, Error, TEXT("%s     - %s"), kPrefix, *Message.ToString());
			}
			Work.ErrorText = FString::Printf(TEXT("② 转换失败，共 %d 条错误"), Errors.Num());
			return Work;
		}
		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s ② 转换成功"), kPrefix);

		// ③ 解析 take.cparch
		const FString CparchFile = FPaths::Combine(TakeOutputDirectory, TEXT("take.") + FIngestCaptureData::Extension);
		IngestCaptureData::FParseResult ParseResult = IngestCaptureData::ParseFile(CparchFile);
		if (ParseResult.HasError())
		{
			Work.ErrorText = FString::Printf(TEXT("③ take.cparch 解析失败（%s）"), *CparchFile);
			UE_LOG(LogMHSTakeImporter, Error, TEXT("%s %s：%s"), kPrefix, *Work.ErrorText, *ParseResult.StealError().ToString());
			return Work;
		}

		Work.IngestCaptureData = ParseResult.StealValue();
		ConvertPathsToFull(TakeOutputDirectory, Work.IngestCaptureData);

		FCreateAssetsData CreateAssetData;
		const bool bCalibrationParsed = PrepareAssetsData(Work.IngestCaptureData, TakeName, InPackageRootPath, CreateAssetData);
		Work.CreateAssetData = MoveTemp(CreateAssetData);
		UE_LOG(LogMHSTakeImporter, Display,
			TEXT("%s ③ cparch 解析+资产数据就绪（包路径：%s；IS_Video=%d IS_Depth=%d SW=%d CC=%d%s）"),
			kPrefix, *Work.CreateAssetData.PackagePath, Work.CreateAssetData.ImageSequences.Num(),
			Work.CreateAssetData.DepthSequences.Num(), Work.CreateAssetData.AudioClips.Num(),
			Work.CreateAssetData.Calibrations.Num(), bCalibrationParsed ? TEXT("") : TEXT("（⚠ 标定未解析）"));

		Work.bOk = true;
		return Work;
	}

	bool ImportSingleTake_Finalize(const FMHSImportWork& InWork)
	{
		using namespace UE::CaptureManager;
		const FCreateAssetsData& CreateAssetData = InWork.CreateAssetData;
		const FIngestCaptureData& IngestCaptureData = InWork.IngestCaptureData;

		// ④ 创建资产（引擎公开 API，创建 IS/SW/CC/LF；要求 GameThread）
		FIngestAssetCreator::FPerTakeCallback PerTakeCallback = FIngestAssetCreator::FPerTakeCallback(
			FIngestAssetCreator::FPerTakeCallback::Type::CreateLambda(
				[](TPair<int32, FIngestAssetCreator::FAssetCreationResult> InResult)
				{
					if (InResult.Value.HasError())
					{
						FAssetCreationError Error = InResult.Value.StealError();
						UE_LOG(LogMHSTakeImporter, Error, TEXT("%s ④ 资产创建失败（TakeId=%d）：%s"),
							kPrefix, InResult.Key, *Error.GetMessage().ToString());
					}
				}), EDelegateExecutionThread::InternalThread);

		TArray<FCreateAssetsData> AssetsDataList;
		AssetsDataList.Add(CreateAssetData);

		TArray<FCaptureDataAssetInfo> AssetInfos = FIngestAssetCreator::CreateAssets_GameThread(AssetsDataList, MoveTemp(PerTakeCallback));
		if (AssetInfos.IsEmpty())
		{
			UE_LOG(LogMHSTakeImporter, Error, TEXT("%s ④ 资产创建失败（详见上方错误日志）"), kPrefix);
			return false;
		}

		int32 LensFileCount = 0;
		for (const FCreateAssetsData::FCalibrationData& Calib : CreateAssetData.Calibrations)
		{
			LensFileCount += Calib.LensFileAssetNames.Num();
		}
		UE_LOG(LogMHSTakeImporter, Display,
			TEXT("%s ④ 资产创建成功（IS=%d Depth=%d SW=%d CC=%d LF=%d）"),
			kPrefix, AssetInfos[0].ImageSequences.Num(), AssetInfos[0].DepthSequences.Num(),
			AssetInfos[0].Audios.Num(), AssetInfos[0].Calibrations.Num(), LensFileCount);

		// ⑤ 组装 FootageCaptureData
		if (!AssembleCaptureData(CreateAssetData, AssetInfos[0], IngestCaptureData))
		{
			return false;
		}
		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s ⑤ FootageCaptureData 组装成功：%s/%s"),
			kPrefix, *CreateAssetData.PackagePath, *CreateAssetData.CaptureDataAssetName);

		// ⑥ 保存
		SaveCreatedAssets(CreateAssetData.PackagePath);
		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s ⑥ 资产已保存：%s"), kPrefix, *CreateAssetData.PackagePath);

		// 产物清单
		auto LogAsset = [&CreateAssetData](const TCHAR* InTag, const UObject* InAsset)
		{
			if (InAsset)
			{
				UE_LOG(LogMHSTakeImporter, Display, TEXT("%s   %s %s"), kPrefix, InTag, *InAsset->GetPathName());
			}
		};
		for (const FCaptureDataAssetInfo::FImageSequence& IS : AssetInfos[0].ImageSequences) { LogAsset(TEXT("IS_Video"), IS.Asset.Get()); }
		for (const FCaptureDataAssetInfo::FImageSequence& DS : AssetInfos[0].DepthSequences) { LogAsset(TEXT("IS_Depth"), DS.Asset.Get()); }
		for (const FCaptureDataAssetInfo::FAudio& AU : AssetInfos[0].Audios) { LogAsset(TEXT("SW_Audio"), AU.Asset.Get()); }
		for (const FCaptureDataAssetInfo::FCalibration& CA : AssetInfos[0].Calibrations) { LogAsset(TEXT("CC"), CA.Asset.Get()); }

		return true;
	}

	// ── 导入转换并发数计算 ──
	// auto（InConcurrency<=0）：物理核 / 每 take 转换核占用。
	// 2026-08-26 用户实测：24 核机器单 take 转换占 ~10%（≈2.4 核）→ kCoresPerTake 保守取 3，
	// 留出系统/磁盘余量，auto 并发 = 24/3 = 8。探测日志"② 转换诊断"可校准该系数。
	int32 ComputeImportConcurrency(int32 InConcurrency)
	{
		constexpr int32 MaxN = 16; // 硬上限：防止磁盘 IO 争抢反噬
		if (InConcurrency > 0)
		{
			return FMath::Min(InConcurrency, MaxN);
		}
		constexpr int32 kCoresPerTake = 3;
		const int32 Physical = FPlatformMisc::NumberOfCores();
		return FMath::Clamp(Physical / kCoresPerTake, 1, MaxN);
	}

	// ── 并发批量导入（阻塞，游戏线程调用）：同时最多 Concurrency 个 Prepare 在后台线程转换；
	// 每个完成后由游戏线程串行 Finalize（建资产/保存），释放槽位拉下一个 take。
	bool ImportTakesBlocking_Concurrent(const TArray<FString>& InTakeDirectories, const FString& InOutputRoot,
		const FString& InPackageRootPath, int32 InConcurrency)
	{
		const TArray<FString>& TakeDirectories = InTakeDirectories;
		const int32 NumTakes = TakeDirectories.Num();
		if (NumTakes == 0)
		{
			return true;
		}

		std::atomic<int32> Next{0};
		TQueue<FMHSImportWork, EQueueMode::Mpsc> Done; // 多生产者(线程池) / 单消费者(游戏线程)
		FEvent* WorkDone = FPlatformProcess::GetSynchEventFromPool();

		// 用真实并发数覆盖 Python 传入值（auto 时 Python 传 0，这里显示实际生效值，避免"并发1"假象）
		FMHSProgressMonitor::Get().SetTaskContext(NumTakes, InConcurrency);

		// ── 任务进度（小窗实时显示）：全部在本函数游戏线程内访问（Launch/完成处理），无锁 ──
		TArray<FString> ActiveNames;   // 正在并发的 take 名（显示名）
		ActiveNames.Reserve(NumTakes);
		int32 Completed = 0;
		double DoneDurationSum = 0.0;  // 已完成 take 的纯耗时总和（不含进行中的 take）

		auto PushTaskProgress = [&]()
		{
			// 平均耗时只统计"已完成 take 的纯耗时"（不含当前进行中 take）：
			// ETA 在完成事件时更新，转换期间保持估计值（不随已耗漂移，避免"剩余一直增加"）。
			double Eta = -1.0;
			if (Completed > 0 && Completed < NumTakes)
			{
				const double AvgDone = DoneDurationSum / Completed;
				Eta = AvgDone * (NumTakes - Completed);
			}
			const FString Active = FString::Join(ActiveNames, TEXT(", "));
			FMHSProgressMonitor::Get().UpdateTaskProgress(Completed, Eta, Active);
		};

		auto Launch = [&]()
		{
			const int32 Index = Next.fetch_add(1);
			if (Index >= NumTakes)
			{
				return;
			}
			ActiveNames.Add(FPaths::GetCleanFilename(TakeDirectories[Index]));
			UE_LOG(LogMHSTakeImporter, Display, TEXT("%s ---- [%d/%d] 转换启动：%s ----"),
				kPrefix, Index + 1, NumTakes, *TakeDirectories[Index]);
			PushTaskProgress(); // 并发集合变化即刷新（用户能看到当前正在转哪些）
			Async(EAsyncExecution::ThreadPool, [Index, &TakeDirectories, &Done, WorkDone, InOutputRoot, InPackageRootPath]()
			{
				FMHSImportWork Work = ImportSingleTake_Prepare(TakeDirectories[Index], InOutputRoot, InPackageRootPath);
				Done.Enqueue(MoveTemp(Work));
				WorkDone->Trigger();
			});
		};

		for (int32 i = 0; i < InConcurrency; ++i)
		{
			Launch();
		}

		int32 Succeeded = 0;
		int32 Failed = 0;
		while (Completed < NumTakes)
		{
			FMHSImportWork Work;
			if (Done.Dequeue(Work))
			{
				++Completed;
				// 已完成 take 纯耗时（完成时刻 - 该 take Prepare 开始时刻），供剩余时间均值外推
				if (Work.StartWallClock > 0.0)
				{
					DoneDurationSum += (FPlatformTime::Seconds() - Work.StartWallClock);
				}
				// 活跃名单以目录 basename 为准（与 Launch 的 Add 同源）
				ActiveNames.RemoveSingle(FPaths::GetCleanFilename(Work.TakeDirectory));
				if (Work.bOk && ImportSingleTake_Finalize(Work))
				{
					++Succeeded;
				}
				else
				{
					++Failed;
					UE_LOG(LogMHSTakeImporter, Error, TEXT("%s take 导入失败：%s"), kPrefix, *Work.ErrorText);
				}
				PushTaskProgress(); // 每完成一个 take 刷新（completed/eta/活跃名单）
				Launch(); // 释放槽位，拉下一个 take
				continue;
			}
			PumpMessages_GameThread(); // 等待期间保持窗口响应
			FPlatformProcess::Sleep(0.002f);
		}
		FMHSProgressMonitor::Get().UpdateTaskProgress(NumTakes, -1.0, TEXT("")); // 批末清空任务进度
		FPlatformProcess::ReturnSynchEventToPool(WorkDone);

		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s ======== 批量导入完成：成功 %d / 失败 %d / 共 %d ========"),
			kPrefix, Succeeded, Failed, NumTakes);
		return Succeeded > 0 || Failed == 0;
	}

	// ── 阻塞导入（异步化入口）：后台线程 Prepare + 游戏线程等待泵消息 + 游戏线程 Finalize ──
	bool ImportTakeBlocking(const FString& InTakeDirectory, const FString& InOutputRoot, const FString& InPackageRootPath)
	{
		TSharedFuture<FMHSImportWork> Future = Async(EAsyncExecution::ThreadPool,
			[TakeDirectory = InTakeDirectory, OutputRoot = InOutputRoot, PackageRoot = InPackageRootPath]()
			{
				return ImportSingleTake_Prepare(TakeDirectory, OutputRoot, PackageRoot);
			});

		// 等待期间泵 Slate 消息：媒体转换在后台线程跑，游戏线程不被占满，
		// 窗口保持响应（根治"失焦卡死/需点击才继续"）。
		while (!Future.WaitFor(FTimespan::FromMilliseconds(10)))
		{
			PumpMessages_GameThread();
			FPlatformProcess::Sleep(0.002f);
		}

		FMHSImportWork Work = Future.Get();
		if (!Work.bOk)
		{
			UE_LOG(LogMHSTakeImporter, Error, TEXT("%s 导入失败：%s"), kPrefix, *Work.ErrorText);
			return false;
		}
		return ImportSingleTake_Finalize(Work);
	}

	// ── 非阻塞导入（bBlocking=false）：后台 Prepare，完成后自动回游戏线程 Finalize ──
	void ImportTakeAsyncDispatch(const FString& InTakeDirectory, const FString& InOutputRoot, const FString& InPackageRootPath)
	{
		TSharedFuture<FMHSImportWork> Future = Async(EAsyncExecution::ThreadPool,
			[TakeDirectory = InTakeDirectory, OutputRoot = InOutputRoot, PackageRoot = InPackageRootPath]()
			{
				return ImportSingleTake_Prepare(TakeDirectory, OutputRoot, PackageRoot);
			});

		AsyncTask(ENamedThreads::GameThread,
			[Future]() mutable
			{
				while (!Future.WaitFor(FTimespan::FromMilliseconds(10)))
				{
					PumpMessages_GameThread();
					FPlatformProcess::Sleep(0.002f);
				}

				FMHSImportWork Work = Future.Get();
				if (!Work.bOk)
				{
					UE_LOG(LogMHSTakeImporter, Error, TEXT("%s 导入失败：%s"), kPrefix, *Work.ErrorText);
					return;
				}
				ImportSingleTake_Finalize(Work);
			});
	}

	// 递归收集所有含 take 元数据文件的目录（去重、排序保证批处理顺序稳定）
	TArray<FString> CollectTakeDirectories(const FString& InTakeRootDirectory)
	{
		TSet<FString> TakeDirectories;

		IFileManager::Get().IterateDirectoryRecursively(*InTakeRootDirectory,
			[&TakeDirectories](const TCHAR* InFileNameOrDirectory, bool bInIsDirectory)
			{
				if (!bInIsDirectory)
				{
					const FString BaseName = FPaths::GetBaseFilename(InFileNameOrDirectory);
					const FString Extension = FPaths::GetExtension(InFileNameOrDirectory);

					const bool bIsTakeMetadata =
						(Extension == FTakeMetadata::FileExtension) ||
						(BaseName == TEXT("take") && (Extension == TEXT("json") || Extension.IsEmpty()));

					if (bIsTakeMetadata)
					{
						TakeDirectories.Add(FPaths::GetPath(InFileNameOrDirectory));
					}
				}
				return true;
			});

		TArray<FString> Result = TakeDirectories.Array();
		Result.Sort();
		return Result;
	}
}

void UMHSTakeImporter::ImportTakeDirectory(const FString& InTakeDirectory, const FString& InOutputRoot,
	const FString& InPackageRootPath, bool bBlocking)
{
	const FString TakeDirectory = FPaths::ConvertRelativePathToFull(InTakeDirectory);
	const FString OutputRoot = FPaths::ConvertRelativePathToFull(InOutputRoot);

	if (!FPaths::DirectoryExists(TakeDirectory))
	{
		UE_LOG(LogMHSTakeImporter, Error, TEXT("%s take 目录不存在：%s"), kPrefix, *TakeDirectory);
		return;
	}
	IFileManager::Get().MakeDirectory(*OutputRoot, true);

	if (bBlocking)
	{
		ImportTakeBlocking(TakeDirectory, OutputRoot, InPackageRootPath);
	}
	else
	{
		ImportTakeAsyncDispatch(TakeDirectory, OutputRoot, InPackageRootPath);
		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s 导入任务已异步化（后台转换，完成后回游戏线程创建资产）"), kPrefix);
	}
}

void UMHSTakeImporter::ImportTakesFromRoot(const FString& InTakeRootDirectory, const FString& InOutputRoot,
	const FString& InPackageRootPath, bool bBlocking, int32 InConcurrency)
{
	const FString TakeRootDirectory = FPaths::ConvertRelativePathToFull(InTakeRootDirectory);
	const FString OutputRoot = FPaths::ConvertRelativePathToFull(InOutputRoot);

	if (!FPaths::DirectoryExists(TakeRootDirectory))
	{
		UE_LOG(LogMHSTakeImporter, Error, TEXT("%s take 根目录不存在：%s"), kPrefix, *TakeRootDirectory);
		return;
	}
	IFileManager::Get().MakeDirectory(*OutputRoot, true);

	const int32 Concurrency = ComputeImportConcurrency(InConcurrency);

	auto RunBatch = [TakeRootDirectory, OutputRoot, InPackageRootPath, Concurrency, InConcurrency]()
	{
		TArray<FString> TakeDirectories = CollectTakeDirectories(TakeRootDirectory);
		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s ======== 批处理导入开始：共 %d 个 take 目录（转换并发 %d，auto=%s）========"),
			kPrefix, TakeDirectories.Num(), Concurrency,
			InConcurrency > 0 ? TEXT("否") : TEXT("是"));
		ImportTakesBlocking_Concurrent(TakeDirectories, OutputRoot, InPackageRootPath, Concurrency);
	};

	if (bBlocking)
	{
		RunBatch();
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, MoveTemp(RunBatch));
		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s 批处理任务已投递到游戏线程（异步执行，转换并发 %d）"), kPrefix, Concurrency);
	}
}

TArray<int32> UMHSTakeImporter::GetRecommendedImportConcurrency(int32 InConcurrency)
{
	TArray<int32> Info;
	Info.Add(FPlatformMisc::NumberOfCores());                          // [0] 物理核
	Info.Add(FPlatformMisc::NumberOfCoresIncludingHyperthreads());     // [1] 逻辑核
	Info.Add(ComputeImportConcurrency(InConcurrency));                 // [2] 实际生效并发（clamp 后）
	return Info;
}

void UMHSTakeImporter::ImportTakes(const TArray<FString>& InTakeDirectories, const FString& InOutputRoot,
	const FString& InPackageRootPath, bool bBlocking, int32 InConcurrency)
{
	const FString OutputRoot = FPaths::ConvertRelativePathToFull(InOutputRoot);

	TArray<FString> TakeDirs;
	for (const FString& D : InTakeDirectories)
	{
		const FString Full = FPaths::ConvertRelativePathToFull(D);
		if (FPaths::DirectoryExists(Full))
		{
			TakeDirs.Add(Full);
		}
		else
		{
			UE_LOG(LogMHSTakeImporter, Warning, TEXT("%s 显式导入跳过（目录不存在）：%s"), kPrefix, *Full);
		}
	}
	if (TakeDirs.IsEmpty())
	{
		UE_LOG(LogMHSTakeImporter, Error, TEXT("%s 显式导入列表为空（没有可导入的 take 目录）"), kPrefix);
		return;
	}
	IFileManager::Get().MakeDirectory(*OutputRoot, true);

	const int32 Concurrency = ComputeImportConcurrency(InConcurrency);

	auto RunBatch = [TakeDirs, OutputRoot, InPackageRootPath, Concurrency]()
	{
		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s ======== 显式导入开始：%d 个 take 目录（转换并发 %d）========"),
			kPrefix, TakeDirs.Num(), Concurrency);
		ImportTakesBlocking_Concurrent(TakeDirs, OutputRoot, InPackageRootPath, Concurrency);
	};

	if (bBlocking)
	{
		RunBatch();
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, MoveTemp(RunBatch));
		UE_LOG(LogMHSTakeImporter, Display, TEXT("%s 显式导入已投递到游戏线程（异步执行，转换并发 %d）"), kPrefix, Concurrency);
	}
}
