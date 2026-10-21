// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "GroundTruth/GroundTruthLoggerSelfCheck.h"

#include "GroundTruth/GroundTruthLogger.h"
#include "Sensors/LidarModel.h"
#include "Sensors/LidarPipelineRunner.h"
#include "Sensors/LidarConfig.h"
#include "Sensors/LidarScanGeometry.h"

#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogGtLoggerSelfCheck, Log, All);

namespace
{
	constexpr double CmPerMeter = 100.0;

	UStaticMesh* LoadCube()
	{
		return LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	}

	AStaticMeshActor* SpawnBox(UWorld* World, const FVector& PosCm, const FVector& Scale, UStaticMesh* Cube)
	{
		if (!World || !Cube)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(), PosCm, FRotator::ZeroRotator, Params);
		if (!Actor)
		{
			return nullptr;
		}
		if (UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent())
		{
			Comp->SetMobility(EComponentMobility::Movable);
			Comp->SetStaticMesh(Cube);
			Comp->SetWorldScale3D(Scale);
			Comp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Comp->SetCollisionResponseToAllChannels(ECR_Block);
		}
		return Actor;
	}

	void BuildEnclosure(UWorld* World, const FVector& Center)
	{
		UStaticMesh* Cube = LoadCube();
		if (!Cube)
		{
			return;
		}
		const double HalfCm = 1000.0;
		const double WallH = 6.0;
		const double WallT = 0.2;
		const double WallLen = 22.0;
		SpawnBox(World, Center + FVector(+HalfCm, 0, 0), FVector(WallT, WallLen, WallH), Cube);
		SpawnBox(World, Center + FVector(-HalfCm, 0, 0), FVector(WallT, WallLen, WallH), Cube);
		SpawnBox(World, Center + FVector(0, +HalfCm, 0), FVector(WallLen, WallT, WallH), Cube);
		SpawnBox(World, Center + FVector(0, -HalfCm, 0), FVector(WallLen, WallT, WallH), Cube);
		SpawnBox(World, Center + FVector(0, 0, -350), FVector(WallLen, WallLen, WallT), Cube);
		SpawnBox(World, Center + FVector(0, 0, +350), FVector(WallLen, WallLen, WallT), Cube);
		SpawnBox(World, Center + FVector(400, 200, 0), FVector(1.0, 1.0, WallH), Cube);
	}

	/**
	 * Convert an Unreal world transform (left-handed, centimeters) to a REP 103
	 * pose (right-handed, meters): pos = (x/100, -y/100, z/100); quat_ros =
	 * (w, -x, y, -z). This is the design's fixed mapping; the SHARED converter
	 * (ConvertPosition / ConvertRotation) arrives in task 9 and this demo will
	 * switch to it then. Inlined here only so the demo logs a REP 103 pose now.
	 */
	FLogRoverPose PoseFromTransform(const FTransform& T)
	{
		const FVector P = T.GetLocation();
		const FQuat Q = T.GetRotation();
		FLogRoverPose Pose;
		Pose.PosX = P.X / CmPerMeter;
		Pose.PosY = -P.Y / CmPerMeter;
		Pose.PosZ = P.Z / CmPerMeter;
		Pose.QuatW = Q.W;
		Pose.QuatX = -Q.X;
		Pose.QuatY = Q.Y;
		Pose.QuatZ = -Q.Z;
		return Pose;
	}

	/**
	 * A sink that fails on the Nth WriteLine, keeping every earlier line in an
	 * in-memory buffer (which it never discards), so the write-failure path
	 * (5.5) can be exercised without touching the disk: we assert the run
	 * stops, the error is emitted, and the retained lines are intact.
	 */
	class FFailingAtNSink : public ILogRunSink
	{
	public:
		explicit FFailingAtNSink(int32 InFailAtWrite) : FailAtWrite(InFailAtWrite) {}

		virtual bool WriteLine(const FString& Line) override
		{
			++WriteCount;
			if (WriteCount == FailAtWrite)
			{
				bFailed = true;
				return false; // retained lines in `Lines` are NOT discarded
			}
			Lines.Add(Line);
			return true;
		}
		virtual bool Flush() override { return !bFailed; }
		virtual FString Describe() const override { return TEXT("<in-memory failing sink>"); }

		int32 WriteCount = 0;
		int32 FailAtWrite = 0;
		bool bFailed = false;
		TArray<FString> Lines;
	};

	/** Build a synthetic lidar truth array (mix of hits and no-hit gaps). */
	TArray<FSensorValue> SyntheticTruth(int32 N, int32 GapStride)
	{
		TArray<FSensorValue> Truth;
		Truth.Reserve(N);
		for (int32 i = 0; i < N; ++i)
		{
			if (GapStride > 0 && i % GapStride == 0)
			{
				Truth.Add(FSensorValue());
			}
			else
			{
				Truth.Add(FSensorValue(5.0 + (i % 40) * 0.25));
			}
		}
		return Truth;
	}
}

FString FGroundTruthLoggerSelfCheck::ResolveCommitSha()
{
	// Resolve the full commit SHA the way the design's provenance does
	// (git rev-parse HEAD), run against the repo root.
	const FString ProjectDir = FPaths::ProjectDir();
	const FString RepoRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(ProjectDir, TEXT("../../")));

	const FString Args = FString::Printf(TEXT("-C \"%s\" rev-parse HEAD"), *RepoRoot);
	int32 ReturnCode = -1;
	FString StdOut, StdErr;
	const bool bRan = FPlatformProcess::ExecProcess(
		TEXT("git"), *Args, &ReturnCode, &StdOut, &StdErr);
	if (bRan && ReturnCode == 0)
	{
		return StdOut.TrimStartAndEnd();
	}
	UE_LOG(LogGtLoggerSelfCheck, Warning,
		TEXT("could not resolve commit SHA (ran=%d rc=%d err=%s); using 'unknown'"),
		bRan ? 1 : 0, ReturnCode, *StdErr.TrimStartAndEnd());
	return TEXT("unknown");
}

FString FGroundTruthLoggerSelfCheck::ResolveRunsRoot()
{
	const FString ProjectDir = FPaths::ProjectDir();
	const FString RepoRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(ProjectDir, TEXT("../../")));
	return FPaths::Combine(RepoRoot, TEXT("runs"));
}

FGroundTruthLoggerSelfCheck::FResult FGroundTruthLoggerSelfCheck::Run(
	UWorld* World, const FString& LidarConfigPath, const FString& RunDir)
{
	FResult Result;

	// --- Load + validate the Gate A lidar config ---------------------------
	const FString ConfigPath = LidarConfigPath.IsEmpty()
		? FLidarConfig::ResolveConfigPath()
		: LidarConfigPath;
	FLidarConfig Config;
	FString LoadError;
	if (!FLidarConfig::LoadFromFile(ConfigPath, Config, LoadError))
	{
		Result.Detail = FString::Printf(TEXT("could not load %s: %s"), *ConfigPath, *LoadError);
		UE_LOG(LogGtLoggerSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}
	if (!Config.Validate().bValid)
	{
		Result.Detail = FString::Printf(TEXT("config %s is out of bounds"), *ConfigPath);
		UE_LOG(LogGtLoggerSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}

	const int64 Seed = 1234; // Gate A run seed.
	const FString CommitSha = ResolveCommitSha();

	// --- Resolve the Run_Log path under runs/<run_id>/ ---------------------
	const FString RunId = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString RunFolder = RunDir.IsEmpty()
		? FPaths::Combine(ResolveRunsRoot(), FString::Printf(TEXT("gtlog_selfcheck_%s"), *RunId))
		: RunDir;
	const FString RunLogPath = FPaths::Combine(RunFolder, TEXT("run_log.jsonl"));
	Result.RunLogPath = RunLogPath;

	// --- Drive the logger: BeginRun -> LogSample* -> EndRun ----------------
	FGroundTruthLogger Logger;
	// Camera sampling interval is recorded when camera frames are sampled; the
	// camera arrives in M3, so record the Gate A ratio here to prove the header
	// field is plumbed and present (design 5.3). Set empty to omit it.
	const FString CameraSamplingInterval = TEXT("1/10");

	if (!Logger.BeginRun(Seed, Config, CommitSha,
		MakeUnique<FFileLogRunSink>(RunLogPath), CameraSamplingInterval))
	{
		Result.Detail = FString::Printf(TEXT("BeginRun failed: %s"), *Logger.GetWriteFailureError());
		UE_LOG(LogGtLoggerSelfCheck, Error, TEXT("%s"), *Result.Detail);
		return Result;
	}

	int32 ScansLogged = 0;

	if (World != nullptr)
	{
		// Real raycast lidar scans against a built enclosure.
		const FVector SensorCenter(0.0, 0.0, 50000.0);
		BuildEnclosure(World, SensorCenter);

		FActorSpawnParameters LidarParams;
		LidarParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ALidarModel* Lidar = World->SpawnActor<ALidarModel>(
			ALidarModel::StaticClass(), SensorCenter, FRotator::ZeroRotator, LidarParams);
		if (Lidar)
		{
			Lidar->Initialise(Seed, Config, TEXT("lidar"));

			// A moving rover pose for the records: use the lidar actor's own
			// transform (stand-in until the Rover drives the sensor in a full
			// run); nudge it each scan so the logged pose varies.
			const int32 NumScans = 5;
			for (int32 s = 0; s < NumScans; ++s)
			{
				const double CaptureTime = s * Config.ScanPeriodSeconds();
				const FLidarScan Scan = Lidar->PerformScan(CaptureTime);

				FLogSample Sample;
				Sample.Sensor = TEXT("lidar");
				Sample.Seq = Scan.Seq;
				Sample.TCapture = CaptureTime;
				// Published release time = capture + latency (design latency semantics).
				Sample.TPublish = CaptureTime + Config.LatencySeconds();
				Sample.bDropped = false; // lidar dropout is per-ray, not whole-sample
				Sample.Pose = PoseFromTransform(Lidar->GetActorTransform());
				Sample.bHasRangeArrays = true;
				Sample.Truth = Scan.TruthRangeM;
				Sample.Published = Scan.PublishedRangeM;

				if (!Logger.LogSample(Sample))
				{
					break;
				}
				++ScansLogged;
			}
		}
	}

	if (ScansLogged == 0)
	{
		// No world (or spawn failed): drive synthetic lidar samples so the
		// format/monotonic assertions still run headlessly.
		const int32 NumScans = 5;
		for (int32 s = 0; s < NumScans; ++s)
		{
			FLogSample Sample;
			Sample.Sensor = TEXT("lidar");
			Sample.Seq = s;
			Sample.TCapture = s * Config.ScanPeriodSeconds();
			Sample.TPublish = Sample.TCapture + Config.LatencySeconds();
			Sample.bDropped = false;
			Sample.Pose = FLogRoverPose();
			Sample.bHasRangeArrays = true;
			Sample.Truth = SyntheticTruth(64, 7);
			FLidarPipelineRunner Runner;
			Runner.Initialise(Seed, Config, TEXT("lidar"));
			const FLidarScan Scan = Runner.ProcessCapturedScan(s, Sample.TCapture, Sample.Truth, {});
			Sample.Published = Scan.PublishedRangeM;
			if (!Logger.LogSample(Sample))
			{
				break;
			}
			++ScansLogged;
		}
	}

	Logger.EndRun();
	Result.bRecordsWritten = (ScansLogged > 0) && !Logger.HasWriteFailure();

	// --- Re-read the written Run_Log and verify header + monotonic t -------
	TArray<FString> FileLines;
	if (FFileHelper::LoadFileToStringArray(FileLines, *RunLogPath) && FileLines.Num() > 0)
	{
		Result.RecordsRead = FileLines.Num();

		// Header: first line is a run_meta carrying the seed and sensor config.
		const FString& Header = FileLines[0];
		Result.bHeaderOk =
			Header.Contains(TEXT("\"type\":\"run_meta\"")) &&
			Header.Contains(TEXT("\"seed\":1234")) &&
			Header.Contains(TEXT("\"sensor_config\":{\"lidar\":")) &&
			Header.Contains(TEXT("\"camera_sampling_interval\":\"1/10\"")) &&
			Header.Contains(FString::Printf(TEXT("\"commit\":\"%s\""), *CommitSha));

		// Monotonic `t`: parse each sample line's "t": value, assert
		// non-decreasing in file order.
		bool bMonotonic = true;
		double PrevT = -TNumericLimits<double>::Max();
		int32 SampleCount = 0;
		for (int32 i = 1; i < FileLines.Num(); ++i)
		{
			const FString& Line = FileLines[i];
			int32 Key = Line.Find(TEXT("\"t\":"));
			if (Key == INDEX_NONE)
			{
				continue;
			}
			const int32 ValStart = Key + 4;
			int32 ValEnd = ValStart;
			while (ValEnd < Line.Len() && Line[ValEnd] != TEXT(','))
			{
				++ValEnd;
			}
			const double T = FCString::Atod(*Line.Mid(ValStart, ValEnd - ValStart));
			if (T + KINDA_SMALL_NUMBER < PrevT)
			{
				bMonotonic = false;
				UE_LOG(LogGtLoggerSelfCheck, Error,
					TEXT("non-monotonic t at line %d: %.6f < prev %.6f"), i, T, PrevT);
				break;
			}
			PrevT = T;
			++SampleCount;
		}
		Result.bMonotonicT = bMonotonic && (SampleCount > 0);
	}
	else
	{
		Result.Detail = FString::Printf(TEXT("could not re-read Run_Log %s"), *RunLogPath);
	}

	// --- Write-failure handling (5.5): inject a sink that fails on the 3rd ---
	// --- write; assert the run stops, the error is emitted, and the two ------
	// --- earlier lines are retained intact. ---------------------------------
	{
		FGroundTruthLogger FailLogger;
		FFailingAtNSink* FailSink = new FFailingAtNSink(/*FailAtWrite*/ 3);
		// Header is write #1, first sample write #2, second sample write #3 (fails).
		const bool bBegan = FailLogger.BeginRun(Seed, Config, CommitSha,
			TUniquePtr<ILogRunSink>(FailSink), FString());

		FLogSample S;
		S.Sensor = TEXT("lidar");
		S.bHasRangeArrays = true;
		S.Truth = { FSensorValue(1.0), FSensorValue(2.0) };
		S.Published = { FSensorValue(1.01), FSensorValue(2.02) };
		S.Seq = 0; S.TCapture = 0.0; S.TPublish = 0.2;
		const bool bFirst = FailLogger.LogSample(S);  // write #2, ok
		S.Seq = 1; S.TCapture = 0.1; S.TPublish = 0.3;
		const bool bSecond = FailLogger.LogSample(S); // write #3, fails

		const bool bStopped = !FailLogger.IsRunning() && FailLogger.HasWriteFailure();
		const bool bErrorEmitted = !FailLogger.GetWriteFailureError().IsEmpty();
		// Retained data: the two successful writes (header + first sample) are
		// still in the sink buffer, untouched.
		const bool bRetained = (FailSink->Lines.Num() == 2) &&
			FailSink->Lines[0].Contains(TEXT("run_meta")) &&
			FailSink->Lines[1].Contains(TEXT("\"seq\":0"));
		// A sample after the failure is a no-op (run already stopped).
		S.Seq = 2; const bool bAfter = FailLogger.LogSample(S);

		Result.bWriteFailureHandled =
			bBegan && bFirst && !bSecond && bStopped && bErrorEmitted && bRetained && !bAfter;

		UE_LOG(LogGtLoggerSelfCheck, Log,
			TEXT("write-failure check: began=%d first=%d second(expect fail)=%d stopped=%d error=%d retained=%d(lines=%d) after(expect noop)=%d"),
			bBegan, bFirst, bSecond, bStopped, bErrorEmitted, bRetained, FailSink->Lines.Num(), bAfter);
	}

	Result.Detail = FString::Printf(
		TEXT("run_log=%s scans=%d lines=%d | header=%d records=%d monotonic=%d writeFailure=%d"),
		*RunLogPath, ScansLogged, Result.RecordsRead,
		Result.bHeaderOk ? 1 : 0, Result.bRecordsWritten ? 1 : 0,
		Result.bMonotonicT ? 1 : 0, Result.bWriteFailureHandled ? 1 : 0);

	// Log a short head of the file so a verify script can eyeball the format.
	if (FileLines.Num() > 0)
	{
		UE_LOG(LogGtLoggerSelfCheck, Log, TEXT("Run_Log head:"));
		for (int32 i = 0; i < FMath::Min(3, FileLines.Num()); ++i)
		{
			UE_LOG(LogGtLoggerSelfCheck, Log, TEXT("  %s"), *FileLines[i]);
		}
	}

	return Result;
}

// ---------------------------------------------------------------------------
// Console command: `GroundTruthLoggerSelfCheck [configPath] [runDir]`
// ---------------------------------------------------------------------------
static void ExecuteGroundTruthLoggerSelfCheck(const TArray<FString>& Args, UWorld* World)
{
	FString ConfigPath;
	FString RunDir;
	if (Args.Num() >= 1 && !Args[0].IsEmpty())
	{
		ConfigPath = Args[0];
	}
	if (Args.Num() >= 2 && !Args[1].IsEmpty())
	{
		RunDir = Args[1];
	}

	const FGroundTruthLoggerSelfCheck::FResult Result =
		FGroundTruthLoggerSelfCheck::Run(World, ConfigPath, RunDir);

	UE_LOG(LogGtLoggerSelfCheck, Log, TEXT("GroundTruthLoggerSelfCheck %s: %s"),
		Result.Passed() ? TEXT("PASS") : TEXT("FAIL"), *Result.Detail);
}

static FAutoConsoleCommandWithWorldAndArgs GGroundTruthLoggerSelfCheckCommand(
	TEXT("GroundTruthLoggerSelfCheck"),
	TEXT("Headless logger demo/verify: write a .jsonl Run_Log under runs/<run_id>/ whose header carries the seed/config and whose t values are non-decreasing, and exercise write-failure handling. Usage: GroundTruthLoggerSelfCheck [configPath] [runDir]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ExecuteGroundTruthLoggerSelfCheck));
