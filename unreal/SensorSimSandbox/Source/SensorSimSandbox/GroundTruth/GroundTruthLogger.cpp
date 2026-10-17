// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "GroundTruth/GroundTruthLogger.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

DEFINE_LOG_CATEGORY_STATIC(LogGroundTruthLogger, Log, All);

namespace
{
	/**
	 * Format a double for JSONL in a stable, locale-independent way. Uses up to
	 * 17 significant digits (round-trippable for IEEE-754 doubles) so the
	 * analysis loaders recover the exact value, and strips a trailing ".0"
	 * exponentless formatting is not needed because `%.17g` already yields a
	 * valid JSON number. Non-finite values are written as `null` (the format
	 * has no representation for NaN/Inf and the analysis treats absent as null).
	 */
	FString JsonNumber(double Value)
	{
		if (!FMath::IsFinite(Value))
		{
			return TEXT("null");
		}
		// %.17g is locale-independent in Unreal's printf and round-trips doubles.
		return FString::Printf(TEXT("%.17g"), Value);
	}

	FString JsonNumber(int64 Value)
	{
		return FString::Printf(TEXT("%lld"), Value);
	}

	FString JsonNumber(int32 Value)
	{
		return FString::Printf(TEXT("%d"), Value);
	}

	FString JsonBool(bool bValue)
	{
		return bValue ? TEXT("true") : TEXT("false");
	}

	/** JSON string literal with the minimal escaping the format needs. */
	FString JsonString(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	/** The full lidar config as the header's sensor_config.lidar object. */
	FString SerializeLidarConfigObject(const FLidarConfig& C)
	{
		// Field names mirror config/lidar.yaml and FLidarConfig::ToCandidateMap
		// so the analysis loader reads the same keys the sensor validated.
		FString S = TEXT("{");
		S += FString::Printf(TEXT("\"channels\":%s,"),               *JsonNumber(C.Channels));
		S += FString::Printf(TEXT("\"h_fov_deg\":%s,"),              *JsonNumber(C.HFovDeg));
		S += FString::Printf(TEXT("\"v_fov_deg\":%s,"),              *JsonNumber(C.VFovDeg));
		S += FString::Printf(TEXT("\"angular_resolution_deg\":%s,"), *JsonNumber(C.AngularResolutionDeg));
		S += FString::Printf(TEXT("\"max_range_m\":%s,"),            *JsonNumber(C.MaxRangeM));
		S += FString::Printf(TEXT("\"rate_hz\":%s,"),                *JsonNumber(C.RateHz));
		S += FString::Printf(TEXT("\"range_noise_sigma_m\":%s,"),    *JsonNumber(C.RangeNoiseSigmaM));
		S += FString::Printf(TEXT("\"dropout_prob\":%s,"),           *JsonNumber(C.DropoutProb));
		S += FString::Printf(TEXT("\"latency_ms\":%s"),              *JsonNumber(C.LatencyMs));
		S += TEXT("}");
		return S;
	}

	FString SerializeCameraConfigObject(const FCameraConfig& C)
	{
		// Field names mirror config/camera.yaml.
		FString S = TEXT("{");
		S += FString::Printf(TEXT("\"width\":%s,"),              *JsonNumber(C.Width));
		S += FString::Printf(TEXT("\"height\":%s,"),             *JsonNumber(C.Height));
		S += FString::Printf(TEXT("\"rate_hz\":%s,"),            *JsonNumber(C.RateHz));
		S += FString::Printf(TEXT("\"pixel_noise_sigma\":%s,"),  *JsonNumber(C.PixelNoiseSigma));
		S += FString::Printf(TEXT("\"frame_dropout_prob\":%s,"), *JsonNumber(C.FrameDropoutProb));
		S += FString::Printf(TEXT("\"latency_ms\":%s"),          *JsonNumber(C.LatencyMs));
		S += TEXT("}");
		return S;
	}
}

// ---------------------------------------------------------------------------
// FGroundTruthLogFormat — the exact on-disk format
// ---------------------------------------------------------------------------

FString FGroundTruthLogFormat::SerializeRangeArray(const TArray<FSensorValue>& Values)
{
	FString S = TEXT("[");
	for (int32 i = 0; i < Values.Num(); ++i)
	{
		if (i > 0)
		{
			S += TEXT(",");
		}
		S += Values[i].IsSet() ? JsonNumber(Values[i].GetValue()) : FString(TEXT("null"));
	}
	S += TEXT("]");
	return S;
}

FString FGroundTruthLogFormat::SerializeHeader(
	int64 Seed,
	const FString& CommitSha,
	const FLidarConfig& LidarConfig,
	const FString& CameraSamplingInterval,
	const FCameraConfig* CameraConfig,
	const TArray<TPair<FString, FString>>* ExtraSensorConfigs)
{
	// {"type":"run_meta","seed":...,"commit":"...","sensor_config":{"lidar":{...}}[,"camera_sampling_interval":"1/10"]}
	FString S = TEXT("{");
	S += TEXT("\"type\":\"run_meta\",");
	S += FString::Printf(TEXT("\"seed\":%s,"), *JsonNumber(Seed));
	S += FString::Printf(TEXT("\"commit\":%s,"), *JsonString(CommitSha));
	S += FString::Printf(TEXT("\"sensor_config\":{\"lidar\":%s"),
		*SerializeLidarConfigObject(LidarConfig));
	if (CameraConfig != nullptr)
	{
		S += FString::Printf(TEXT(",\"camera\":%s"), *SerializeCameraConfigObject(*CameraConfig));
	}
	if (ExtraSensorConfigs != nullptr)
	{
		for (const TPair<FString, FString>& Extra : *ExtraSensorConfigs)
		{
			S += FString::Printf(TEXT(",%s:%s"), *JsonString(Extra.Key), *Extra.Value);
		}
	}
	S += TEXT("}");
	// camera_sampling_interval present ONLY when camera frames are sampled (5.3).
	if (!CameraSamplingInterval.IsEmpty())
	{
		S += FString::Printf(TEXT(",\"camera_sampling_interval\":%s"),
			*JsonString(CameraSamplingInterval));
	}
	S += TEXT("}");
	return S;
}

FString FGroundTruthLogFormat::SerializeSample(const FLogSample& Sample, double TWritten)
{
	FString S = TEXT("{");
	S += TEXT("\"type\":\"sample\",");
	S += FString::Printf(TEXT("\"sensor\":%s,"), *JsonString(Sample.Sensor));
	S += FString::Printf(TEXT("\"seq\":%s,"), *JsonNumber(Sample.Seq));
	S += FString::Printf(TEXT("\"t\":%s,"), *JsonNumber(TWritten));
	S += FString::Printf(TEXT("\"t_capture\":%s,"), *JsonNumber(Sample.TCapture));
	// t_publish is null for a dropped sample.
	S += FString::Printf(TEXT("\"t_publish\":%s,"),
		Sample.TPublish.IsSet() ? *JsonNumber(Sample.TPublish.GetValue()) : TEXT("null"));
	S += FString::Printf(TEXT("\"dropped\":%s,"), *JsonBool(Sample.bDropped));

	// rover_pose: {"pos":[x,y,z],"quat":[w,x,y,z]}
	S += FString::Printf(
		TEXT("\"rover_pose\":{\"pos\":[%s,%s,%s],\"quat\":[%s,%s,%s,%s]}"),
		*JsonNumber(Sample.Pose.PosX), *JsonNumber(Sample.Pose.PosY), *JsonNumber(Sample.Pose.PosZ),
		*JsonNumber(Sample.Pose.QuatW), *JsonNumber(Sample.Pose.QuatX),
		*JsonNumber(Sample.Pose.QuatY), *JsonNumber(Sample.Pose.QuatZ));

	// Payload: lidar per-ray arrays, or camera image refs. A dropped camera
	// frame carries no refs (design: "dropped: true, no image refs"); an
	// unsampled published camera frame carries explicit null refs.
	if (Sample.bHasRangeArrays)
	{
		S += FString::Printf(TEXT(",\"truth\":%s,\"published\":%s"),
			*SerializeRangeArray(Sample.Truth),
			*SerializeRangeArray(Sample.Published));
	}
	else if (Sample.bIsCameraFrame && !Sample.bDropped)
	{
		const FString TruthRef = Sample.TruthRef.IsSet() ? JsonString(Sample.TruthRef.GetValue()) : FString(TEXT("null"));
		const FString PubRef = Sample.PublishedRef.IsSet() ? JsonString(Sample.PublishedRef.GetValue()) : FString(TEXT("null"));
		S += FString::Printf(TEXT(",\"truth_ref\":%s,\"published_ref\":%s"), *TruthRef, *PubRef);
	}
	// A dropped sample (camera frame or otherwise) emits no payload beyond the
	// common fields, exactly as the design's dropped-frame example shows.

	S += TEXT("}");
	return S;
}

// ---------------------------------------------------------------------------
// FFileLogRunSink — default append-to-file sink
// ---------------------------------------------------------------------------

FFileLogRunSink::FFileLogRunSink(const FString& InFilePath)
	: FilePath(InFilePath)
{
}

bool FFileLogRunSink::WriteLine(const FString& Line)
{
	// Ensure the parent directory (runs/<run_id>/) exists before the first write.
	if (!bEverWritten)
	{
		const FString Dir = FPaths::GetPath(FilePath);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (!Dir.IsEmpty() && !PF.DirectoryExists(*Dir))
		{
			PF.CreateDirectoryTree(*Dir);
		}
	}

	// Append the line plus a trailing newline. FILEWRITE_Append keeps every
	// previously written record on disk, so a later failure never loses what
	// was already flushed (Requirement 5.5).
	const FString Payload = Line + LINE_TERMINATOR;
	const uint32 Flags = bEverWritten ? FILEWRITE_Append : 0u;
	const bool bOk = FFileHelper::SaveStringToFile(
		Payload, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), Flags);
	if (bOk)
	{
		bEverWritten = true;
	}
	return bOk;
}

bool FFileLogRunSink::Flush()
{
	// Each WriteLine already persisted via SaveStringToFile, so there is no
	// extra buffered state to flush. Report success.
	return true;
}

// ---------------------------------------------------------------------------
// FGroundTruthLogger
// ---------------------------------------------------------------------------

void FGroundTruthLogger::FailRun(const FString& Context)
{
	bWriteFailed = true;
	bRunning = false;
	const FString SinkDesc = Sink.IsValid() ? Sink->Describe() : FString(TEXT("<no sink>"));
	WriteFailureError = FString::Printf(
		TEXT("Run_Log write failure: %s (sink: %s). Run stopped; %lld record(s) already written are retained."),
		*Context, *SinkDesc, SamplesWritten);
	UE_LOG(LogGroundTruthLogger, Error, TEXT("%s"), *WriteFailureError);
}

bool FGroundTruthLogger::BeginRun(
	int64 Seed,
	const FLidarConfig& LidarConfig,
	const FString& CommitSha,
	TUniquePtr<ILogRunSink> InSink,
	const FString& CameraSamplingInterval,
	const FCameraConfig* CameraConfig)
{
	Sink = MoveTemp(InSink);
	bRunning = false;
	bWriteFailed = false;
	WriteFailureError.Reset();
	LastT = 0.0;
	bHasLastT = false;
	SamplesWritten = 0;

	if (!Sink.IsValid())
	{
		FailRun(TEXT("no sink provided to BeginRun"));
		return false;
	}

	const FString Header = FGroundTruthLogFormat::SerializeHeader(
		Seed, CommitSha, LidarConfig, CameraSamplingInterval, CameraConfig, &ExtraSensorConfigs);
	if (!Sink->WriteLine(Header))
	{
		FailRun(TEXT("could not write metadata header"));
		return false;
	}

	bRunning = true;
	UE_LOG(LogGroundTruthLogger, Log,
		TEXT("BeginRun: seed=%lld commit=%s camera_sampling_interval='%s' header written to %s"),
		Seed, *CommitSha, *CameraSamplingInterval, *Sink->Describe());
	return true;
}

bool FGroundTruthLogger::LogSample(const FLogSample& Sample)
{
	if (!bRunning)
	{
		// A sample after a write failure (or before BeginRun) is a no-op; the
		// run is already stopped and its data retained.
		return false;
	}

	// Requested record time: release time for a published sample, capture time
	// for a dropped sample (design). Enforce monotonic non-decreasing `t` by
	// carrying the previous value forward (5.1): a late-released dropped frame
	// whose capture time precedes an earlier published release never regresses
	// file order.
	const double Requested = Sample.TPublish.IsSet() ? Sample.TPublish.GetValue() : Sample.TCapture;
	const double TWritten = bHasLastT ? FMath::Max(LastT, Requested) : Requested;

	const FString Record = FGroundTruthLogFormat::SerializeSample(Sample, TWritten);
	if (!Sink->WriteLine(Record))
	{
		FailRun(FString::Printf(TEXT("could not write sample record (sensor=%s seq=%lld)"),
			*Sample.Sensor, Sample.Seq));
		return false;
	}

	LastT = TWritten;
	bHasLastT = true;
	++SamplesWritten;
	return true;
}

bool FGroundTruthLogger::EndRun()
{
	if (!Sink.IsValid())
	{
		return false;
	}
	const bool bOk = Sink->Flush();
	if (!bOk && bRunning)
	{
		FailRun(TEXT("could not flush Run_Log on EndRun"));
		return false;
	}
	bRunning = false;
	UE_LOG(LogGroundTruthLogger, Log, TEXT("EndRun: flushed %lld sample record(s) to %s"),
		SamplesWritten, *Sink->Describe());
	return bOk;
}
