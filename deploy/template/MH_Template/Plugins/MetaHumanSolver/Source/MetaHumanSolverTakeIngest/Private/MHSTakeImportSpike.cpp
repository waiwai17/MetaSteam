// MHSTakeImportSpike.cpp
// Spike 验证入口：在编辑器进程内验证引擎 CaptureManager 的解析+转换链路（①②③）。
// 公共步骤已提取至 MHSTakeIngestUtils，正式导入见 MHSTakeImporter。
#include "MHSTakeImportSpike.h"

#include "MHSTakeIngestUtils.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

#include "CaptureDataConverter.h"
#include "CaptureManagerTakeMetadata.h"
#include "IngestCaptureData.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogMHSTakeSpike, Log, All);

namespace
{
	const TCHAR* kSpikePrefix = TEXT("[MHSSpike]");

	std::atomic<bool> GSpikeRunning{ false };

	int32 CountFiles(const FString& InDirectory, const FString& InPattern)
	{
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *InDirectory, *InPattern);
		return Found.Num();
	}

	struct FSpikeCheck
	{
		FString Item;
		bool bPass = false;
		FString Detail;
	};

	void RunSpikeInternal(const FString& InTakeDirectory, const FString& InOutputRootDirectory)
	{
		UE_LOG(LogMHSTakeSpike, Display, TEXT("%s ============ Spike 开始（后台线程） ============"), kSpikePrefix);
		UE_LOG(LogMHSTakeSpike, Display, TEXT("%s Take 目录：%s"), kSpikePrefix, *InTakeDirectory);

		TArray<FSpikeCheck> Checks;

		// ── 步骤 1：解析 ─────────────────────────────────────────────
		FTakeMetadata TakeMetadata;
		FString ParseSource;
		if (MHSTakeIngest::ParseTakeMetadata(InTakeDirectory, TakeMetadata, ParseSource))
		{
			MHSTakeIngest::NormalizeMetadataPaths(TakeMetadata, InTakeDirectory);

			UE_LOG(LogMHSTakeSpike, Display, TEXT("%s [1] 解析成功 —— %s"), kSpikePrefix, *ParseSource);
			UE_LOG(LogMHSTakeSpike, Display,
				TEXT("%s     Schema=%u.%u Slate=%s TakeNumber=%u Video=%d Depth=%d Audio=%d Calibration=%d"),
				kSpikePrefix, TakeMetadata.Version.Major, TakeMetadata.Version.Minor, *TakeMetadata.Slate,
				TakeMetadata.TakeNumber, TakeMetadata.Video.Num(), TakeMetadata.Depth.Num(),
				TakeMetadata.Audio.Num(), TakeMetadata.Calibration.Num());

			for (const FTakeMetadata::FVideo& Video : TakeMetadata.Video)
			{
				UE_LOG(LogMHSTakeSpike, Display,
					TEXT("%s     Video[0] Name=%s Format=%s 帧率=%.2f 帧数=%s 路径=%s"),
					kSpikePrefix, *Video.Name, *Video.Format, Video.FrameRate,
					Video.FramesCount.IsSet() ? *FString::FromInt(Video.FramesCount.GetValue()) : TEXT("未设置"),
					*Video.Path);
			}
			for (const FTakeMetadata::FVideo& Depth : TakeMetadata.Depth)
			{
				UE_LOG(LogMHSTakeSpike, Display,
					TEXT("%s     Depth[0] Name=%s Format=%s 帧率=%.2f 帧数=%s 路径=%s"),
					kSpikePrefix, *Depth.Name, *Depth.Format, Depth.FrameRate,
					Depth.FramesCount.IsSet() ? *FString::FromInt(Depth.FramesCount.GetValue()) : TEXT("未设置"),
					*Depth.Path);
			}

			Checks.Add({ TEXT("① take 元数据解析"), true, ParseSource });
		}
		else
		{
			UE_LOG(LogMHSTakeSpike, Error, TEXT("%s [1] 解析失败：新格式与 legacy 均拒收"), kSpikePrefix);
			Checks.Add({ TEXT("① take 元数据解析"), false, TEXT("两个 parser 均失败") });
		}

		bool bConversionPassed = false;
		FString TakeOutputDirectory;

		if (Checks.Last().bPass)
		{
			// ── 步骤 2：转换 ─────────────────────────────────────────
			const FString TakeName = MHSTakeIngest::MakeTakeName(TakeMetadata, InTakeDirectory);
			TakeOutputDirectory = FPaths::Combine(InOutputRootDirectory, TakeName);

			constexpr bool bRequireExists = false;
			constexpr bool bTree = true;
			IFileManager::Get().DeleteDirectory(*TakeOutputDirectory, bRequireExists, bTree);
			IFileManager::Get().MakeDirectory(*TakeOutputDirectory, true);

			UE_LOG(LogMHSTakeSpike, Display, TEXT("%s [2] 输出目录就绪：%s"), kSpikePrefix, *TakeOutputDirectory);

			FCaptureDataConverterParams Params = MHSTakeIngest::BuildConverterParams(
				TakeMetadata, TakeName, InTakeDirectory, TakeOutputDirectory);

			UE_LOG(LogMHSTakeSpike, Display, TEXT("%s [3] 转换启动（mov→png / 深度→EXR / 音频→wav / 标定→json）..."), kSpikePrefix);

			FCaptureDataConverterResult<void> Result = MHSTakeIngest::RunConversionBlocking(MoveTemp(Params));

			if (Result.HasValue())
			{
				bConversionPassed = true;
				Checks.Add({ TEXT("② FCaptureDataConverter 转换"), true, TEXT("Run 返回成功") });
				UE_LOG(LogMHSTakeSpike, Display, TEXT("%s [3] 转换成功"), kSpikePrefix);
			}
			else
			{
				FCaptureDataConverterError Error = Result.StealError();
				TArray<FText> Errors = Error.GetErrors();
				UE_LOG(LogMHSTakeSpike, Error, TEXT("%s [3] 转换失败，共 %d 条错误："), kSpikePrefix, Errors.Num());
				for (const FText& Message : Errors)
				{
					UE_LOG(LogMHSTakeSpike, Error, TEXT("%s     - %s"), kSpikePrefix, *Message.ToString());
				}
				Checks.Add({ TEXT("② FCaptureDataConverter 转换"), false,
					FString::Printf(TEXT("%d 条错误（详见上方日志）"), Errors.Num()) });
			}
		}

		if (bConversionPassed)
		{
			// ── 步骤 3：产物校验（复刻 FCaptureValidationNode 的验收标准）──
			UE_LOG(LogMHSTakeSpike, Display, TEXT("%s [4] 校验转换产物..."), kSpikePrefix);

			for (const FTakeMetadata::FVideo& Video : TakeMetadata.Video)
			{
				const FString VideoDir = FPaths::Combine(TakeOutputDirectory, TEXT("Video"), Video.Name);
				const int32 FrameCount = CountFiles(VideoDir, TEXT("*.png"));
				const bool bPass = FrameCount > 0;
				Checks.Add({ FString::Printf(TEXT("③ 视频序列 %s"), *Video.Name), bPass,
					FString::Printf(TEXT("%d 帧 png @ %s"), FrameCount, *VideoDir) });
			}

			for (const FTakeMetadata::FVideo& Depth : TakeMetadata.Depth)
			{
				const FString DepthDir = FPaths::Combine(TakeOutputDirectory, TEXT("Depth"), Depth.Name);
				const int32 FrameCount = CountFiles(DepthDir, TEXT("*.exr"));
				const bool bPass = FrameCount > 0;
				Checks.Add({ FString::Printf(TEXT("③ 深度序列 %s"), *Depth.Name), bPass,
					FString::Printf(TEXT("%d 帧 EXR @ %s"), FrameCount, *DepthDir) });
			}

			for (const FTakeMetadata::FAudio& Audio : TakeMetadata.Audio)
			{
				const FString AudioDir = FPaths::Combine(TakeOutputDirectory, TEXT("Audio"), Audio.Name);
				const FString AudioFile = FPaths::Combine(AudioDir, TEXT("audio.wav"));
				const bool bPass = IFileManager::Get().FileExists(*AudioFile);
				Checks.Add({ FString::Printf(TEXT("③ 音频 %s"), *Audio.Name), bPass, AudioFile });
			}

			for (const FTakeMetadata::FCalibration& Calibration : TakeMetadata.Calibration)
			{
				const FString CalibFile = FPaths::Combine(TakeOutputDirectory, TEXT("Calibration"), Calibration.Name, TEXT("calibration.json"));
				const bool bPass = IFileManager::Get().FileExists(*CalibFile);
				Checks.Add({ FString::Printf(TEXT("③ 标定 %s"), *Calibration.Name), bPass, CalibFile });
			}

			const FString CparchFile = FPaths::Combine(TakeOutputDirectory,
				TEXT("take.") + FIngestCaptureData::Extension);
			const bool bCparchExists = IFileManager::Get().FileExists(*CparchFile);
			Checks.Add({ TEXT("③ take.cparch 产物"), bCparchExists, CparchFile });

			// ── 步骤 4：反向解析 take.cparch ─────────────────────────
			if (bCparchExists)
			{
				UE::CaptureManager::IngestCaptureData::FParseResult ParseResult =
					UE::CaptureManager::IngestCaptureData::ParseFile(CparchFile);

				if (ParseResult.IsValid())
				{
					FIngestCaptureData IngestData = ParseResult.StealValue();
					Checks.Add({ TEXT("④ take.cparch 反向解析"), true,
						FString::Printf(TEXT("Video=%d Depth=%d Audio=%d Calibration=%d"),
							IngestData.Video.Num(), IngestData.Depth.Num(),
							IngestData.Audio.Num(), IngestData.Calibration.Num()) });

					for (const FIngestCaptureData::FVideo& Video : IngestData.Video)
					{
						UE_LOG(LogMHSTakeSpike, Display, TEXT("%s     cparch Video[%s] -> %s"), kSpikePrefix, *Video.Name, *Video.Path);
					}
					for (const FIngestCaptureData::FVideo& Depth : IngestData.Depth)
					{
						UE_LOG(LogMHSTakeSpike, Display, TEXT("%s     cparch Depth[%s] -> %s"), kSpikePrefix, *Depth.Name, *Depth.Path);
					}
					for (const FIngestCaptureData::FAudio& Audio : IngestData.Audio)
					{
						UE_LOG(LogMHSTakeSpike, Display, TEXT("%s     cparch Audio[%s] -> %s"), kSpikePrefix, *Audio.Name, *Audio.Path);
					}
					for (const FIngestCaptureData::FCalibration& Calibration : IngestData.Calibration)
					{
						UE_LOG(LogMHSTakeSpike, Display, TEXT("%s     cparch Calibration[%s] -> %s"), kSpikePrefix, *Calibration.Name, *Calibration.Path);
					}
				}
				else
				{
					Checks.Add({ TEXT("④ take.cparch 反向解析"), false, ParseResult.StealError().ToString() });
				}
			}
		}

		// ── 汇总 ────────────────────────────────────────────────────
		bool bAllPassed = true;
		UE_LOG(LogMHSTakeSpike, Display, TEXT("%s ============ Spike 结果汇总 ============"), kSpikePrefix);
		for (const FSpikeCheck& Check : Checks)
		{
			UE_LOG(LogMHSTakeSpike, Display, TEXT("%s   [%s] %s —— %s"),
				kSpikePrefix, Check.bPass ? TEXT("PASS") : TEXT("FAIL"), *Check.Item, *Check.Detail);
			bAllPassed &= Check.bPass;
		}
		if (bAllPassed)
		{
			UE_LOG(LogMHSTakeSpike, Display,
				TEXT("%s ============ Spike 总判定：全部通过 —— 编辑器进程内转换链路可用 ============"), kSpikePrefix);
		}
		else
		{
			UE_LOG(LogMHSTakeSpike, Error,
				TEXT("%s ============ Spike 总判定：存在失败项 —— 详见上方日志 ============"), kSpikePrefix);
		}

		GSpikeRunning.store(false);
	}
}

void UMHSTakeImportSpike::RunTakeIngestSpike(const FString& InTakeDirectory, const FString& InOutputRootDirectory, bool bBlocking)
{
	if (GSpikeRunning.exchange(true))
	{
		UE_LOG(LogMHSTakeSpike, Warning, TEXT("%s 已有 Spike 在运行，忽略本次调用"), kSpikePrefix);
		return;
	}

	const FString TakeDirectory = FPaths::ConvertRelativePathToFull(InTakeDirectory);
	const FString OutputRootDirectory = FPaths::ConvertRelativePathToFull(InOutputRootDirectory);

	if (!FPaths::DirectoryExists(TakeDirectory))
	{
		UE_LOG(LogMHSTakeSpike, Error, TEXT("%s take 目录不存在：%s"), kSpikePrefix, *TakeDirectory);
		GSpikeRunning.store(false);
		return;
	}
	if (OutputRootDirectory.IsEmpty())
	{
		UE_LOG(LogMHSTakeSpike, Error, TEXT("%s 输出根目录为空"), kSpikePrefix);
		GSpikeRunning.store(false);
		return;
	}

	IFileManager::Get().MakeDirectory(*OutputRootDirectory, true);
	UE_LOG(LogMHSTakeSpike, Display, TEXT("%s Spike 任务提交（异步执行，日志类别 LogMHSTakeSpike）"), kSpikePrefix);
	UE_LOG(LogMHSTakeSpike, Display, TEXT("%s   Take 目录：%s"), kSpikePrefix, *TakeDirectory);
	UE_LOG(LogMHSTakeSpike, Display, TEXT("%s   输出根目录：%s"), kSpikePrefix, *OutputRootDirectory);

	AsyncTask(ENamedThreads::AnyThread, [TakeDirectory, OutputRootDirectory]()
	{
		RunSpikeInternal(TakeDirectory, OutputRootDirectory);
	});

	UE_LOG(LogMHSTakeSpike, Display, TEXT("%s Spike 已在后台启动，请留意后续 [MHSSpike] 日志"), kSpikePrefix);

	if (bBlocking)
	{
		UE_LOG(LogMHSTakeSpike, Display, TEXT("%s 阻塞等待模式：等待转换完成（命令行无头运行用）..."), kSpikePrefix);
		while (GSpikeRunning.load())
		{
			FPlatformProcess::Sleep(0.05f);
		}
		UE_LOG(LogMHSTakeSpike, Display, TEXT("%s 阻塞等待结束"), kSpikePrefix);
	}
}
