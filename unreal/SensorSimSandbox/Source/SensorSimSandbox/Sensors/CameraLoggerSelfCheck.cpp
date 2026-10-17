// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Sensors/CameraLoggerSelfCheck.h"

#include "Sensors/CameraConfig.h"
#include "Sensors/CameraPipelineRunner.h"
#include "Sensors/CameraMessage.h"
#include "Sensors/LidarConfig.h"
#include "GroundTruth/GroundTruthLogger.h"
#include "GroundTruth/GroundTruthLoggerSelfCheck.h" // ResolveRunsRoot / ResolveCommitSha
#include "Bridge/RosbridgeClient.h"

#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogCameraLoggerSelfCheck, Log, All);

namespace
{
	constexpr int64 RunSeed = 1234; // Gate A run seed.

	// A synthetic RGBA frame of WxH whose pixel values vary with (seq, x, y) so
	// each produced frame is distinct content (the saved truth PNGs are not all
	// identical, and the JPEG encoder sees real variation). Row-major, 4
	// bytes/pixel, alpha 255.
	FCameraFrameBuffer MakeFrame(int32 W, int32 H, int64 Seq)
	{
		FCameraFrameBuffer Buf;
		Buf.Width = W;
		Buf.Height = H;
		const int64 N = static_cast<int64>(W) * H;
		Buf.Pixels.SetNumUninitialized(N * 4);
		const uint8 Bias = static_cast<uint8>((Seq * 17) & 0x7F);
		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x = 0; x < W; ++x)
			{
				const int64 Base = (static_cast<int64>(y) * W + x) * 4;
				Buf.Pixels[Base + 0] = static_cast<uint8>((x * 255) / FMath::Max(1, W - 1));
				Buf.Pixels[Base + 1] = static_cast<uint8>((y * 255) / FMath::Max(1, H - 1));
				Buf.Pixels[Base + 2] = static_cast<uint8>(128 + (Bias % 64));
				Buf.Pixels[Base + 3] = 255;
			}
		}
		return Buf;
	}

	FString ZeroPad(int64 Seq)
	{
		return FString::Printf(TEXT("%06lld"), Seq);
	}
}

FCameraLoggerSelfCheck::FResult FCameraLoggerSelfCheck::Run(
	const FString& CameraConfigPath, const FString& RunDir)
{
	FResult Result;

	// --- Load + validate the Gate A camera config --------------------------
	const FString ConfigPath = CameraConfigPath.IsEmpty()
		? FCameraConfig::ResolveConfigPath()
		: CameraConfigPath;

	FCameraConfig Config;
	FString LoadError;
	if (!FCameraConfig::LoadFromFile(ConfigPath, Config, LoadError))
	{
		Result.Detail = FString::Printf(TEXT("could not load %s: %s"), *ConfigPath, *LoadError);
		UE_LOG(LogCameraLoggerSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}
	if (!Config.Validate().bValid)
	{
		Result.Detail = FString::Printf(TEXT("config %s out of bounds"), *ConfigPath);
		UE_LOG(LogCameraLoggerSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}
	Result.SamplingInterval = Config.SamplingInterval;

	// Use small frames for the headless check: the sampling / dropout / record
	// logic is independent of resolution, and tiny frames keep the saved PNG/JPEG
	// pairs small. The real resolution is exercised by the live CameraPublish /
	// CameraMessageSelfCheck path. 32x24 still JPEG/PNG-encodes to a real image.
	const int32 W = 32;
	const int32 H = 24;

	// A frame count large enough that the Gate A dropout (0.05) yields at least
	// one dropped frame deterministically under this seed, and the "1/10"
	// interval yields several sampled and many unsampled frames.
	const int32 NumFrames = 40;

	// --- Resolve the Run_Log path + frames/ dir under runs/<run_id>/ -------
	const FString RunId = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString RunFolder = RunDir.IsEmpty()
		? FPaths::Combine(FGroundTruthLoggerSelfCheck::ResolveRunsRoot(),
			FString::Printf(TEXT("camlog_selfcheck_%s"), *RunId))
		: RunDir;
	const FString RunLogPath = FPaths::Combine(RunFolder, TEXT("run_log.jsonl"));
	const FString FramesDir = FPaths::Combine(RunFolder, TEXT("frames"));
	Result.RunLogPath = RunLogPath;

	// --- Rosbridge_Client: non-dropped frames publish CompressedImage +
	// --- CameraInfo through it. The client builds and (when connected) sends
	// --- the ops; headless there may be no server, so we count the ops BUILT
	// --- (Publish returns the built op JSON regardless of connection), proving
	// --- the publish path is wired for every non-dropped frame. ------------
	const FRosbridgeConfig BridgeConfig = FRosbridgeConfig::Resolve();
	TSharedRef<FRosbridgeClient> Client = MakeShared<FRosbridgeClient>(BridgeConfig);
	Client->Advertise(FCameraMessage::GetImageTopic(), FCameraMessage::GetImageMessageType());
	Client->Advertise(FCameraMessage::GetCameraInfoTopic(), FCameraMessage::GetCameraInfoMessageType());

	// --- Drive the camera pipeline + logger end to end ---------------------
	FCameraPipelineRunner Runner;
	Runner.Initialise(RunSeed, Config, TEXT("camera"));

	FLidarConfig LidarCfg; // Gate A defaults — the header's sensor_config.lidar.
	FGroundTruthLogger Logger;
	if (!Logger.BeginRun(RunSeed, LidarCfg,
		FGroundTruthLoggerSelfCheck::ResolveCommitSha(),
		MakeUnique<FFileLogRunSink>(RunLogPath),
		/*CameraSamplingInterval*/ Config.SamplingInterval, &Config))
	{
		Result.Detail = FString::Printf(TEXT("BeginRun failed: %s"), *Logger.GetWriteFailureError());
		UE_LOG(LogCameraLoggerSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}

	const double L = Config.LatencySeconds();
	int32 Produced = 0, Sampled = 0, Unsampled = 0, Dropped = 0, Published = 0, ImageFiles = 0;

	for (int32 i = 0; i < NumFrames; ++i)
	{
		const int64 Seq = i;
		const double CaptureTime = i * Config.FramePeriodSeconds();

		const FCameraFrameBuffer Truth = MakeFrame(W, H, Seq);
		const FCameraFrame Frame = Runner.ProcessCapturedFrame(Seq, CaptureTime, Truth);
		++Produced;

		FLogSample Sample;
		Sample.Sensor = TEXT("camera");
		Sample.Seq = Seq;
		Sample.TCapture = CaptureTime;
		Sample.bIsCameraFrame = true;
		Sample.Pose = FLogRoverPose(); // rover pose present as for lidar (identity here).

		if (Frame.bDropped)
		{
			// A dropped frame: dropped=true, t_publish null, no image refs, not
			// published (design "Ground-truth logging format").
			Sample.bDropped = true;
			Sample.TPublish.Reset();
			++Dropped;
		}
		else
		{
			// A published frame: release at capture + latency.
			Sample.bDropped = false;
			Sample.TPublish = CaptureTime + L;

			// Publish the CompressedImage + CameraInfo through the client (one
			// pair per published frame). Encode the published RGBA to JPEG.
			const FCameraFrameBuffer& Pub = Frame.Published;
			TArray<uint8> Jpeg;
			const bool bJpegOk = FCameraMessage::EncodeRgbaToJpeg(Pub.Width, Pub.Height, Pub.Pixels, Jpeg);
			if (bJpegOk)
			{
				const double Stamp = Sample.TPublish.GetValue();
				TSharedRef<FJsonObject> ImgMsg = FCameraMessage::BuildCompressedImageMessage(Jpeg, Stamp);
				TSharedRef<FJsonObject> InfoMsg = FCameraMessage::BuildCameraInfoMessage(Config.Width, Config.Height, Stamp);
				const FString InfoOp = Client->Publish(FCameraMessage::GetCameraInfoTopic(), InfoMsg);
				const FString ImgOp = Client->Publish(FCameraMessage::GetImageTopic(), ImgMsg);
				if (!InfoOp.IsEmpty() && !ImgOp.IsEmpty())
				{
					++Published;
				}
			}

			if (Frame.bSampled)
			{
				// A SAMPLED non-dropped frame carries both refs; its truth PNG
				// and published JPEG are written to disk under the run folder.
				++Sampled;
				const FString TruthRel = FString::Printf(TEXT("frames/cam_%s.png"), *ZeroPad(Seq));
				const FString PubRel = FString::Printf(TEXT("frames/cam_%s_pub.jpg"), *ZeroPad(Seq));
				const FString TruthAbs = FPaths::Combine(RunFolder, TruthRel);
				const FString PubAbs = FPaths::Combine(RunFolder, PubRel);

				TArray<uint8> TruthPng;
				if (FCameraMessage::EncodeRgbaToPng(Truth.Width, Truth.Height, Truth.Pixels, TruthPng))
				{
					if (FFileHelper::SaveArrayToFile(TruthPng, *TruthAbs)) { ++ImageFiles; }
				}
				// The published JPEG for the saved pair (re-encode the published pixels).
				TArray<uint8> PubJpeg;
				if (FCameraMessage::EncodeRgbaToJpeg(Pub.Width, Pub.Height, Pub.Pixels, PubJpeg))
				{
					if (FFileHelper::SaveArrayToFile(PubJpeg, *PubAbs)) { ++ImageFiles; }
				}

				Sample.TruthRef = TruthRel;
				Sample.PublishedRef = PubRel;
			}
			else
			{
				// An UNSAMPLED published frame: explicit null refs (refs unset).
				++Unsampled;
				Sample.TruthRef.Reset();
				Sample.PublishedRef.Reset();
			}
		}

		if (!Logger.LogSample(Sample))
		{
			break;
		}
	}

	Logger.EndRun();

	Result.FramesProduced = Produced;
	Result.SampledCount = Sampled;
	Result.UnsampledCount = Unsampled;
	Result.DroppedCount = Dropped;
	Result.PublishedCount = Published;
	Result.ImageFilesWritten = ImageFiles;

	UE_LOG(LogCameraLoggerSelfCheck, Log,
		TEXT("produced=%d sampled=%d unsampled=%d dropped=%d published=%d image_files=%d (interval='%s')"),
		Produced, Sampled, Unsampled, Dropped, Published, ImageFiles, *Config.SamplingInterval);

	// The mix MUST contain at least one of each category, else the chosen
	// parameters stopped exercising all three paths — fail loudly, never pass
	// silently.
	const bool bMixOk = (Sampled > 0) && (Unsampled > 0) && (Dropped > 0);
	if (!bMixOk)
	{
		Result.Detail = FString::Printf(
			TEXT("frame mix did not cover all three cases (sampled=%d unsampled=%d dropped=%d); adjust NumFrames/seed"),
			Sampled, Unsampled, Dropped);
		UE_LOG(LogCameraLoggerSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}

	// Every non-dropped frame was published through the client.
	Result.bPublishedNonDropped = (Published == (Sampled + Unsampled));

	// --- Re-read the written Run_Log and assert the camera record contract --
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *RunLogPath) || Lines.Num() == 0)
	{
		Result.Detail = FString::Printf(TEXT("could not re-read Run_Log %s"), *RunLogPath);
		UE_LOG(LogCameraLoggerSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}
	Result.RecordsRead = Lines.Num();

	// Header: camera_sampling_interval == the config ratio (5.3).
	const FString& Header = Lines[0];
	Result.bHeaderHasInterval =
		Header.Contains(TEXT("\"type\":\"run_meta\"")) &&
		Header.Contains(FString::Printf(TEXT("\"camera_sampling_interval\":\"%s\""), *Config.SamplingInterval)) &&
		Header.Contains(TEXT("\"camera\":{\"width\":"));

	// One record per produced frame (dropped included): sample lines == produced.
	const int32 SampleLines = Lines.Num() - 1;
	Result.bRecordPerProducedFrame = (SampleLines == Produced);

	// Walk each sample line, classify, and assert the per-category shape and the
	// monotonic t. We re-derive the expected category from seq (dropped lines
	// carry dropped=true; among non-dropped, sampled iff IsFrameSampled(seq)).
	bool bSampledOk = true, bUnsampledOk = true, bDroppedOk = true, bMonotonic = true;
	int32 SeenSampled = 0, SeenUnsampled = 0, SeenDropped = 0;
	double PrevT = -TNumericLimits<double>::Max();

	auto FieldDouble = [](const FString& Line, const TCHAR* Key, double& Out) -> bool
	{
		const int32 K = Line.Find(Key);
		if (K == INDEX_NONE) { return false; }
		const int32 Start = K + FCString::Strlen(Key);
		int32 End = Start;
		while (End < Line.Len() && Line[End] != TEXT(',') && Line[End] != TEXT('}')) { ++End; }
		const FString Val = Line.Mid(Start, End - Start).TrimStartAndEnd();
		if (Val == TEXT("null")) { return false; }
		Out = FCString::Atod(*Val);
		return true;
	};

	for (int32 i = 1; i < Lines.Num(); ++i)
	{
		const FString& Line = Lines[i];

		// Every record is a camera sample carrying the common fields.
		const bool bIsCamera = Line.Contains(TEXT("\"sensor\":\"camera\""));
		const bool bHasTCapture = Line.Contains(TEXT("\"t_capture\":"));
		const bool bHasTPublish = Line.Contains(TEXT("\"t_publish\":"));
		const bool bHasDropped = Line.Contains(TEXT("\"dropped\":"));

		// Monotonic t in file order (5.1).
		double T = 0.0;
		if (FieldDouble(Line, TEXT("\"t\":"), T))
		{
			if (T + KINDA_SMALL_NUMBER < PrevT) { bMonotonic = false; }
			PrevT = T;
		}

		const bool bDroppedRec = Line.Contains(TEXT("\"dropped\":true"));
		if (bDroppedRec)
		{
			++SeenDropped;
			// Dropped: t_publish null, NO image refs.
			const bool bTPubNull = Line.Contains(TEXT("\"t_publish\":null"));
			const bool bNoRefs = !Line.Contains(TEXT("truth_ref")) && !Line.Contains(TEXT("published_ref"));
			if (!(bIsCamera && bHasTCapture && bHasTPublish && bHasDropped && bTPubNull && bNoRefs))
			{
				bDroppedOk = false;
			}
		}
		else
		{
			// Non-dropped: refs are either both present (sampled) or both null.
			const bool bHasConcreteTruthRef =
				Line.Contains(TEXT("\"truth_ref\":\"")) && Line.Contains(TEXT("\"published_ref\":\""));
			const bool bNullRefs =
				Line.Contains(TEXT("\"truth_ref\":null")) && Line.Contains(TEXT("\"published_ref\":null"));

			if (bHasConcreteTruthRef)
			{
				++SeenSampled;
				// A sampled frame's referenced files must exist on disk.
				// Pull the two ref paths and check existence under the run folder.
				auto ExtractRef = [&Line](const TCHAR* Key) -> FString
				{
					const int32 K = Line.Find(Key);
					if (K == INDEX_NONE) { return FString(); }
					const int32 Q1 = Line.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, K + FCString::Strlen(Key));
					if (Q1 == INDEX_NONE) { return FString(); }
					const int32 Q2 = Line.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, Q1 + 1);
					if (Q2 == INDEX_NONE) { return FString(); }
					return Line.Mid(Q1 + 1, Q2 - Q1 - 1);
				};
				const FString TruthRel = ExtractRef(TEXT("\"truth_ref\":"));
				const FString PubRel = ExtractRef(TEXT("\"published_ref\":"));
				const bool bTruthExists = !TruthRel.IsEmpty() && FPaths::FileExists(FPaths::Combine(RunFolder, TruthRel));
				const bool bPubExists = !PubRel.IsEmpty() && FPaths::FileExists(FPaths::Combine(RunFolder, PubRel));
				if (!(bIsCamera && bHasTPublish && !Line.Contains(TEXT("\"t_publish\":null")) && bTruthExists && bPubExists))
				{
					bSampledOk = false;
				}
			}
			else if (bNullRefs)
			{
				++SeenUnsampled;
				if (!(bIsCamera && bHasTPublish && !Line.Contains(TEXT("\"t_publish\":null"))))
				{
					bUnsampledOk = false;
				}
			}
			else
			{
				// A non-dropped record with neither concrete refs nor null refs
				// is a format violation.
				bUnsampledOk = false;
				bSampledOk = false;
			}
		}
	}

	Result.bSampledRefsPresent = bSampledOk && (SeenSampled == Sampled) && (Sampled > 0);
	Result.bUnsampledRefsNull = bUnsampledOk && (SeenUnsampled == Unsampled) && (Unsampled > 0);
	Result.bDroppedShape = bDroppedOk && (SeenDropped == Dropped) && (Dropped > 0);
	Result.bMonotonicT = bMonotonic && (SampleLines > 0);

	Result.Detail = FString::Printf(
		TEXT("run_log=%s produced=%d (sampled=%d unsampled=%d dropped=%d) records=%d published=%d imageFiles=%d | header=%d perFrame=%d sampledRefs=%d unsampledNull=%d droppedShape=%d monotonic=%d published=%d"),
		*RunLogPath, Produced, Sampled, Unsampled, Dropped, Result.RecordsRead, Published, ImageFiles,
		Result.bHeaderHasInterval ? 1 : 0, Result.bRecordPerProducedFrame ? 1 : 0,
		Result.bSampledRefsPresent ? 1 : 0, Result.bUnsampledRefsNull ? 1 : 0,
		Result.bDroppedShape ? 1 : 0, Result.bMonotonicT ? 1 : 0, Result.bPublishedNonDropped ? 1 : 0);

	// Log a short head so a verify script can eyeball the format: the header,
	// a sampled record, a dropped record, an unsampled record.
	UE_LOG(LogCameraLoggerSelfCheck, Log, TEXT("Run_Log header: %s"), Lines.Num() > 0 ? *Lines[0] : TEXT("<none>"));
	for (int32 i = 1; i < Lines.Num(); ++i)
	{
		if (Lines[i].Contains(TEXT("\"truth_ref\":\"")))
		{
			UE_LOG(LogCameraLoggerSelfCheck, Log, TEXT("sampled   record: %s"), *Lines[i]);
			break;
		}
	}
	for (int32 i = 1; i < Lines.Num(); ++i)
	{
		if (Lines[i].Contains(TEXT("\"dropped\":true")))
		{
			UE_LOG(LogCameraLoggerSelfCheck, Log, TEXT("dropped   record: %s"), *Lines[i]);
			break;
		}
	}
	for (int32 i = 1; i < Lines.Num(); ++i)
	{
		if (Lines[i].Contains(TEXT("\"truth_ref\":null")))
		{
			UE_LOG(LogCameraLoggerSelfCheck, Log, TEXT("unsampled record: %s"), *Lines[i]);
			break;
		}
	}

	Client->Disconnect();
	return Result;
}

// ---------------------------------------------------------------------------
// Console command: `CameraLoggerSelfCheck [configPath] [runDir]`
//
// One-command HEADLESS demo/verify for task 15. No world / RHI needed, so it
// runs under -nullrhi. Writes a real .jsonl Run_Log under runs/<run_id>/ whose
// header records camera_sampling_interval and whose camera records carry
// dropped / t_capture / t_publish and refs on sampled frames, saves the sampled
// image pairs, publishes non-dropped frames through the Rosbridge_Client, and
// logs one PASS/FAIL line plus the counts and the Run_Log head.
// ---------------------------------------------------------------------------
static void ExecuteCameraLoggerSelfCheck(const TArray<FString>& Args)
{
	FString ConfigPath;
	FString RunDir;
	if (Args.Num() >= 1 && !Args[0].IsEmpty()) { ConfigPath = Args[0]; }
	if (Args.Num() >= 2 && !Args[1].IsEmpty()) { RunDir = Args[1]; }

	const FCameraLoggerSelfCheck::FResult Result = FCameraLoggerSelfCheck::Run(ConfigPath, RunDir);

	UE_LOG(LogCameraLoggerSelfCheck, Log, TEXT("CameraLoggerSelfCheck %s: %s"),
		Result.Passed() ? TEXT("PASS") : TEXT("FAIL"), *Result.Detail);
}

static FAutoConsoleCommand GCameraLoggerSelfCheckCommand(
	TEXT("CameraLoggerSelfCheck"),
	TEXT("Headless camera LOGGING demo/verify: drive a deterministic mix of sampled/unsampled/dropped camera frames through the GroundTruthLogger, write a .jsonl Run_Log under runs/<run_id>/ (header records camera_sampling_interval; one record per produced frame incl. dropped; refs only on sampled frames), save the sampled image pairs, and publish non-dropped frames through the Rosbridge_Client. Usage: CameraLoggerSelfCheck [configPath] [runDir]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&ExecuteCameraLoggerSelfCheck));
