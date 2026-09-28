// MHSTakeIngestUtils.cpp
#include "MHSTakeIngestUtils.h"

#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

#include "CaptureDataConverter.h"       // FCaptureDataConverter / FCaptureDataConverterParams
#include "CaptureManagerTakeMetadata.h" // FTakeMetadata / FTakeMetadataParser
#include "LiveLinkFaceMetadata.h"       // ParseOldLiveLinkTakeMetadata
#include "MediaSample.h"                // EMediaTexturePixelFormat
#include "IMediaTextureSample.h"        // EMediaOrientation

DEFINE_LOG_CATEGORY_STATIC(LogMHSTakeIngestUtils, Log, All);

namespace
{
	const TCHAR* kPrefix = TEXT("[MHSTake]");
}

namespace MHSTakeIngest
{

TArray<FString> FindTakeMetadataCandidates(const FString& InTakeDirectory)
{
	TArray<FString> CptakeFiles;
	TArray<FString> StandardFiles;

	IFileManager::Get().IterateDirectory(*InTakeDirectory,
		[&CptakeFiles, &StandardFiles](const TCHAR* InFileNameOrDirectory, bool bInIsDirectory)
		{
			if (!bInIsDirectory)
			{
				const FString BaseName = FPaths::GetBaseFilename(InFileNameOrDirectory);
				const FString Extension = FPaths::GetExtension(InFileNameOrDirectory);

				if (Extension == FTakeMetadata::FileExtension)
				{
					CptakeFiles.Add(InFileNameOrDirectory);
				}
				else if (BaseName == TEXT("take") && (Extension == TEXT("json") || Extension.IsEmpty()))
				{
					StandardFiles.Add(InFileNameOrDirectory);
				}
			}
			return true;
		});

	TArray<FString> Candidates;
	Candidates.Append(MoveTemp(CptakeFiles));
	Candidates.Append(MoveTemp(StandardFiles));
	return Candidates;
}

bool ParseTakeMetadata(const FString& InTakeDirectory, FTakeMetadata& OutTakeMetadata, FString& OutSourceDescription)
{
	for (const FString& Candidate : FindTakeMetadataCandidates(InTakeDirectory))
	{
		FTakeMetadataParser Parser;
		TValueOrError<FTakeMetadata, FTakeMetadataParserError> Result = Parser.Parse(Candidate);

		if (Result.IsValid())
		{
			OutTakeMetadata = Result.StealValue();
			OutSourceDescription = FString::Printf(TEXT("新格式 FTakeMetadataParser（%s）"), *FPaths::GetCleanFilename(Candidate));
			return true;
		}

		FTakeMetadataParserError ParserError = Result.StealError();
		UE_LOG(LogMHSTakeIngestUtils, Display,
			TEXT("%s 新格式解析失败（%s）：Origin=%d Message=%s，尝试下一候选/回退 legacy"),
			kPrefix, *FPaths::GetCleanFilename(Candidate),
			static_cast<int32>(ParserError.Origin), *ParserError.Message.ToString());
	}

	TArray<FText> ValidationErrors;
	TOptional<FTakeMetadata> LegacyResult =
		UE::CaptureManager::LiveLinkMetadata::ParseOldLiveLinkTakeMetadata(InTakeDirectory, ValidationErrors);

	if (LegacyResult.IsSet())
	{
		OutTakeMetadata = MoveTemp(LegacyResult.GetValue());
		OutSourceDescription = TEXT("legacy ParseOldLiveLinkTakeMetadata（目录解析）");
		return true;
	}

	UE_LOG(LogMHSTakeIngestUtils, Error, TEXT("%s legacy 解析失败，共 %d 条验证错误："), kPrefix, ValidationErrors.Num());
	for (const FText& Error : ValidationErrors)
	{
		UE_LOG(LogMHSTakeIngestUtils, Error, TEXT("%s   - %s"), kPrefix, *Error.ToString());
	}
	return false;
}

void NormalizeMetadataPaths(FTakeMetadata& InOutTakeMetadata, const FString& InTakeDirectory)
{
	auto ToAbsolute = [&InTakeDirectory](FString& InOutPath)
	{
		if (FPaths::IsRelative(InOutPath))
		{
			InOutPath = FPaths::ConvertRelativePathToFull(InTakeDirectory, InOutPath);
		}
	};

	for (FTakeMetadata::FVideo& Video : InOutTakeMetadata.Video)
	{
		ToAbsolute(Video.Path);
	}
	for (FTakeMetadata::FVideo& Depth : InOutTakeMetadata.Depth)
	{
		ToAbsolute(Depth.Path);
	}
	for (FTakeMetadata::FAudio& Audio : InOutTakeMetadata.Audio)
	{
		ToAbsolute(Audio.Path);
	}
	for (FTakeMetadata::FCalibration& Calibration : InOutTakeMetadata.Calibration)
	{
		ToAbsolute(Calibration.Path);
	}
}

FString MakeTakeName(const FTakeMetadata& InTakeMetadata, const FString& InTakeDirectory)
{
	FString TakeName;
	if (!InTakeMetadata.Slate.IsEmpty())
	{
		TakeName = InTakeMetadata.Slate + TEXT("_") + FString::FromInt(InTakeMetadata.TakeNumber);
	}
	else
	{
		TakeName = FPaths::GetBaseFilename(InTakeDirectory);
	}
	return FPaths::MakeValidFileName(TakeName);
}

FCaptureDataConverterParams BuildConverterParams(const FTakeMetadata& InTakeMetadata, const FString& InTakeName,
	const FString& InTakeOriginDirectory, const FString& InTakeOutputDirectory)
{
	FCaptureDataConverterParams Params;
	Params.TakeMetadata = InTakeMetadata;
	Params.TakeName = InTakeName;
	Params.TakeOriginDirectory = InTakeOriginDirectory;
	Params.TakeOutputDirectory = InTakeOutputDirectory;

	FCaptureConvertVideoOutputParams VideoOutputParams;
	VideoOutputParams.ImageFileName = TEXT("frame");
	VideoOutputParams.Format = TEXT("png"); // Windows WIC writer 注册格式：png/jpg/jpeg
	// 必须用 U8_BGRA：FWindowsImageWriter 仅在该期望格式下执行 I420/NV12→BGRA 前置转换
	//（U8_RGB 不进转换分支，I420 原样到达 WIC 后被拒）——与 UIngestCapability_Options 官方默认一致
	VideoOutputParams.OutputPixelFormat = UE::CaptureManager::EMediaTexturePixelFormat::U8_BGRA;
	VideoOutputParams.Rotation = EMediaOrientation::Original;
	Params.VideoOutputParams = MoveTemp(VideoOutputParams);

	FCaptureConvertAudioOutputParams AudioOutputParams;
	AudioOutputParams.AudioFileName = TEXT("audio");
	AudioOutputParams.Format = TEXT("wav");
	Params.AudioOutputParams = MoveTemp(AudioOutputParams);

	FCaptureConvertDepthOutputParams DepthOutputParams;
	DepthOutputParams.ImageFileName = TEXT("depth");
	DepthOutputParams.bShouldCompressFiles = true;
	DepthOutputParams.Rotation = EMediaOrientation::Original;
	Params.DepthOutputParams = MoveTemp(DepthOutputParams);

	FCaptureConvertCalibrationOutputParams CalibrationOutputParams;
	CalibrationOutputParams.FileName = TEXT("calibration");
	Params.CalibrationOutputParams = MoveTemp(CalibrationOutputParams);

	return Params;
}

TValueOrError<void, FCaptureDataConverterError> RunConversionBlocking(FCaptureDataConverterParams InParams)
{
	FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(false);

	TOptional<FCaptureDataConverterResult<void>> Result;

	AsyncTask(ENamedThreads::AnyThread,
		[&Result, DoneEvent, Params = MoveTemp(InParams)]() mutable
		{
			FCaptureDataConverter Converter;

			FCaptureDataConverter::FProgressReporter ProgressReporter =
				FCaptureDataConverter::FProgressReporter::CreateLambda(
					[LastBucket = -1](double InProgress) mutable
					{
						const int32 Bucket = FMath::Clamp(FMath::FloorToInt(FMath::Abs(InProgress) * 20.0), 0, 20);
						if (Bucket > LastBucket)
						{
							LastBucket = Bucket;
							UE_LOG(LogMHSTakeIngestUtils, Display, TEXT("%s     转换进度 ~%d%%"), kPrefix, Bucket * 5);
						}
					});

			Result = Converter.Run(Params, MoveTemp(ProgressReporter));
			DoneEvent->Trigger();
		});

	DoneEvent->Wait();
	FPlatformProcess::ReturnSynchEventToPool(DoneEvent);

	return MoveTemp(Result.GetValue());
}

}
