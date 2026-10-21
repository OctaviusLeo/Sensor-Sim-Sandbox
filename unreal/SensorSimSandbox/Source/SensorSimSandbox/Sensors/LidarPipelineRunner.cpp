// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Sensors/LidarPipelineRunner.h"

DEFINE_LOG_CATEGORY_STATIC(LogLidarRunner, Log, All);

// ---------------------------------------------------------------------------
// FLidarScan helpers
// ---------------------------------------------------------------------------

int32 FLidarScan::NumHits() const
{
	int32 Count = 0;
	for (const FSensorValue& V : TruthRangeM)
	{
		if (V.IsSet())
		{
			++Count;
		}
	}
	return Count;
}

int32 FLidarScan::NumReturns() const
{
	int32 Count = 0;
	for (const FSensorValue& V : PublishedRangeM)
	{
		if (V.IsSet())
		{
			++Count;
		}
	}
	return Count;
}

int32 FLidarScan::NumDropped() const
{
	int32 Count = 0;
	for (bool bD : bDropped)
	{
		if (bD)
		{
			++Count;
		}
	}
	return Count;
}

// ---------------------------------------------------------------------------
// FLidarPipelineRunner
// ---------------------------------------------------------------------------

FSensorConfigValidationResult FLidarPipelineRunner::Initialise(int64 InRunSeed, const FLidarConfig& Config, const FString& InSensorName)
{
	const FSensorConfigValidationResult Result = Config.Validate();
	if (!Result.bValid)
	{
		UE_LOG(LogLidarRunner, Error,
			TEXT("Lidar Initialise rejected config: %s (offending '%s'). Retaining last valid config."),
			*Result.ErrorMessage, *Result.OffendingParameter);
		return Result;
	}

	RunSeed = InRunSeed;
	SensorName = InSensorName.IsEmpty() ? TEXT("lidar") : InSensorName;
	CurrentConfig = Config;
	Pipeline = MakeUnique<FSensorPipeline>(RunSeed, SensorName);

	UE_LOG(LogLidarRunner, Log,
		TEXT("Lidar runner initialised: seed=%lld sensor='%s' sensor_seed=0x%016llX -> %d rays/scan, sigma=%.4f p=%.4f latency=%d ms"),
		RunSeed, *SensorName, Pipeline->GetSensorSeed(),
		FLidarScanGeometry::NumRays(CurrentConfig),
		CurrentConfig.RangeNoiseSigmaM, CurrentConfig.DropoutProb, CurrentConfig.LatencyMs);

	return Result;
}

FSensorConfigValidationResult FLidarPipelineRunner::ApplyConfig(const FLidarConfig& Candidate)
{
	const FSensorConfigValidationResult Result = Candidate.Validate();
	if (!Result.bValid)
	{
		UE_LOG(LogLidarRunner, Error,
			TEXT("Lidar ApplyConfig rejected: %s (offending '%s'). Last valid config retained."),
			*Result.ErrorMessage, *Result.OffendingParameter);
		return Result;
	}
	CurrentConfig = Candidate;
	return Result;
}

FLidarScan FLidarPipelineRunner::ProcessCapturedScan(
	int64 Seq,
	double CaptureTime,
	const TArray<FSensorValue>& TruthRangeM,
	const TArray<FLidarRayDir>& Rays)
{
	FLidarScan Scan;
	Scan.Seq = Seq;
	Scan.CaptureTime = CaptureTime;
	Scan.Rays = Rays;
	Scan.TruthRangeM = TruthRangeM; // captured truth, verbatim (captured first)

	if (!Pipeline.IsValid())
	{
		Scan.PublishedRangeM.SetNum(TruthRangeM.Num());
		Scan.bDropped.Init(false, TruthRangeM.Num());
		return Scan;
	}

	// Stages 2+3: seeded noise over the whole scan, then per-ray dropout over
	// the whole scan, in that fixed order (reproducible for the seed). No-hit
	// (empty) rays pass through untouched and are never drawn for. No clamping.
	const FSensorStageOutput Stage = Pipeline->ProcessScan(
		TruthRangeM,
		CurrentConfig.RangeNoiseSigmaM,
		CurrentConfig.DropoutProb,
		/*bClamp*/ false);

	Scan.PublishedRangeM = Stage.Published;
	Scan.bDropped = Stage.bDropped;
	return Scan;
}
