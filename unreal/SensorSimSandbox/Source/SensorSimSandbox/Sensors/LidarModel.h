// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sensors/LidarPipelineRunner.h"
#include "Sensors/LidarConfig.h"
#include "Sensors/LidarScanGeometry.h"
#include "LidarModel.generated.h"

/**
 * Lidar_Model (design "Lidar_Model", Requirement 3.1, 3.5, 3.6, 3.7).
 *
 * A raycast lidar actor. It generates a scan's rays from its LidarConfig
 * (FLidarScanGeometry), line-traces each ray against the world to capture the
 * noise-free Ground_Truth range, then runs the SHARED seeded pipeline over the
 * captured truth:
 *
 *   1. Capture Ground_Truth   (line traces; no-hit rays -> empty/null)
 *   2. Seeded Gaussian noise  (sigma = range_noise_sigma_m; sigma 0 passthrough)
 *   3. Per-ray dropout        (probability = dropout_prob; no-hit rays excluded)
 *
 * The actor is a THIN engine wrapper: all config validation and the pure
 * noise/dropout stages live in FLidarPipelineRunner (so they are headlessly
 * testable and stack-constructible). The actor adds only the one
 * engine-dependent step — the line traces that produce the truth ranges —
 * and it runs that step entirely BEFORE handing the captured truth to the
 * runner, so the recorded truth is always the pristine engine value. No-hit
 * rays are excluded from both the noise and dropout statistics. Published
 * ranges are NOT clamped, because clamping would bias the recovered sigma
 * (design "Sensor model math").
 *
 * Config bounds are enforced through the SHARED validation contract
 * (FLidarConfig::Validate -> FSensorBoundSet): an out-of-bound candidate is
 * rejected as a whole, the offending parameter is logged, and the last valid
 * config is retained (Requirement 3.2). The in-bound accept path is the
 * subject of task 6.2's example test.
 */
UCLASS()
class SENSORSIMSANDBOX_API ALidarModel : public AActor
{
	GENERATED_BODY()

public:
	ALidarModel();

	/**
	 * Initialise the model: validate `Config` through the shared contract and,
	 * if valid, adopt it and (re)create the per-sensor pipeline from `RunSeed`
	 * and the sensor name. Returns the validation result; on an invalid config
	 * the model keeps its previous config (Requirement 3.2).
	 */
	FSensorConfigValidationResult Initialise(int64 InRunSeed, const FLidarConfig& Config, const FString& InSensorName = TEXT("lidar"));

	/**
	 * Apply a new config at runtime. Validates through the shared contract; on
	 * success the config is adopted, on failure it is rejected as a whole, the
	 * offending parameter is logged, and the last valid config is retained
	 * (Requirement 3.2).
	 */
	FSensorConfigValidationResult ApplyConfig(const FLidarConfig& Candidate);

	/** The current (last valid) config. */
	const FLidarConfig& GetConfig() const { return Runner.GetConfig(); }

	/**
	 * Perform one scan at capture time `CaptureTime` (seconds). Casts every ray
	 * against the world from this actor's transform, captures per-ray truth
	 * BEFORE the pipeline, then runs seeded noise + per-ray dropout via the
	 * runner. Requires a valid World (line traces); with no World the scan is
	 * returned with all-null truth so headless callers degrade gracefully.
	 */
	FLidarScan PerformScan(double CaptureTime);

	/** Scan period (s) from the config rate; useful for the demo scheduler. */
	double ScanPeriodSeconds() const { return Runner.GetConfig().ScanPeriodSeconds(); }

	/** True once Initialise adopted a valid config and built the pipeline. */
	bool IsReady() const { return Runner.IsReady(); }

	/** The derived per-sensor seed, for logging/repro. */
	uint64 GetSensorSeed() const { return Runner.GetSensorSeed(); }

	/** Access to the pure runner (for tests / headless pure processing). */
	FLidarPipelineRunner& GetRunner() { return Runner; }

private:
	/** The pure config + pipeline core (validation, noise, dropout). */
	FLidarPipelineRunner Runner;

	int64 NextSeq = 0;

	/**
	 * Capture the noise-free truth ranges for `Rays` by line tracing from this
	 * actor's transform. Fills OutTruth (meters, empty = no hit). This is the
	 * ONLY engine-dependent step; it runs entirely before any pipeline stage.
	 */
	void CaptureTruthRanges(const TArray<FLidarRayDir>& Rays, TArray<FSensorValue>& OutTruth) const;
};
