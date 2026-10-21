// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Pipeline/SensorPipeline.h"
#include "Sensors/LidarConfig.h"
#include "Sensors/LidarScanGeometry.h"

/**
 * The result of one lidar scan: the per-ray Ground_Truth ranges captured from
 * the engine BEFORE the pipeline runs, alongside the published (noisy, possibly
 * dropped) ranges the pipeline produced.
 *
 * All arrays are indexed by the same ray order FLidarScanGeometry emits
 * (channel-major), so truth[i], published[i], bDropped[i] and the direction all
 * refer to ray i.
 *
 *   - TruthRangeM[i]      = TOptional<double>: the noise-free hit range in
 *                          METERS, or empty when ray i had no hit within
 *                          max_range_m (logged as null, excluded from stats).
 *   - PublishedRangeM[i]  = TOptional<double>: the noisy range in meters, or
 *                          empty when ray i had no hit OR was dropped. Ranges
 *                          are NOT clamped (a noisy range may exceed
 *                          max_range_m or go below 0).
 *   - bDropped[i]         = true iff a HIT ray was dropped by the dropout stage.
 *                          No-hit rays are never counted as dropped.
 */
struct FLidarScan
{
	/** Per-sensor scan counter, from 0. */
	int64 Seq = 0;

	/** Monotonic capture time (seconds since run start) of this scan. */
	double CaptureTime = 0.0;

	/** Ray directions used for this scan (channel-major; same order as below). */
	TArray<FLidarRayDir> Rays;

	/** Noise-free truth range per ray (meters), empty = no hit. */
	TArray<FSensorValue> TruthRangeM;

	/** Published range per ray (meters), empty = no hit or dropped. */
	TArray<FSensorValue> PublishedRangeM;

	/** Dropped flag per ray (true only for hit rays dropped by the pipeline). */
	TArray<bool> bDropped;

	/** Count of rays that hit something (truth not null). */
	int32 NumHits() const;

	/** Count of hit rays that survived (published present). */
	int32 NumReturns() const;

	/** Count of hit rays dropped by the dropout stage. */
	int32 NumDropped() const;
};

/**
 * The PURE core of the Lidar_Model: config validation (via the shared bound
 * set) plus the seeded noise + per-ray dropout pipeline over a captured scan.
 * It has NO UObject/World/Engine dependency, so the capture-before-pipeline,
 * no-hit-excluded and no-clamp rules can be unit/property tested headlessly and
 * constructed on the stack (an AActor cannot). ALidarModel owns one of these
 * and adds only the engine raycast that produces the truth ranges.
 *
 * Pipeline order (design "Shared SensorModel pipeline" / "Sensor model math"):
 *   1. Capture Ground_Truth   (caller supplies per-ray truth; no-hit -> empty)
 *   2. Seeded Gaussian noise  (sigma = range_noise_sigma_m; sigma 0 passthrough)
 *   3. Per-ray dropout        (probability = dropout_prob; no-hit rays excluded)
 * Published ranges are NOT clamped (bClamp = false), because clamping would
 * bias the recovered sigma.
 */
class FLidarPipelineRunner
{
public:
	FLidarPipelineRunner() = default;

	/**
	 * Validate `Config` through the shared contract and, if valid, adopt it and
	 * (re)create the per-sensor pipeline from `RunSeed`/`SensorName`. On an
	 * invalid config the previous config is retained (Requirement 3.2). Returns
	 * the validation result.
	 */
	FSensorConfigValidationResult Initialise(int64 InRunSeed, const FLidarConfig& Config, const FString& InSensorName = TEXT("lidar"));

	/**
	 * Apply a new config at runtime. Validates through the shared contract; on
	 * success the config is adopted, on failure it is rejected as a whole and
	 * the last valid config is retained (Requirement 3.2). The RNG stream is
	 * NOT reset, preserving reproducibility of the ongoing run.
	 */
	FSensorConfigValidationResult ApplyConfig(const FLidarConfig& Candidate);

	/** The current (last valid) config. */
	const FLidarConfig& GetConfig() const { return CurrentConfig; }

	/** True once Initialise adopted a valid config and built the pipeline. */
	bool IsReady() const { return Pipeline.IsValid(); }

	/** The derived per-sensor seed, for logging/repro. */
	uint64 GetSensorSeed() const { return Pipeline.IsValid() ? Pipeline->GetSensorSeed() : 0; }

	/**
	 * Run stages 2-3 over an already-captured truth array. `Rays` is optional
	 * (used only to populate FLidarScan::Rays for logging/point-cloud build);
	 * pass an empty array when geometry is not needed. The supplied truth is
	 * copied verbatim (the capture is assumed to have happened BEFORE this
	 * call), so no-hit (empty) entries pass through untouched and excluded.
	 */
	FLidarScan ProcessCapturedScan(
		int64 Seq,
		double CaptureTime,
		const TArray<FSensorValue>& TruthRangeM,
		const TArray<FLidarRayDir>& Rays);

private:
	FLidarConfig CurrentConfig;
	int64 RunSeed = 0;
	FString SensorName = TEXT("lidar");

	/** Shared seeded pipeline; created on Initialise, reused across scans. */
	TUniquePtr<FSensorPipeline> Pipeline;
};
