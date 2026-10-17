// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"
#include "Pipeline/SensorRng.h"
#include "Pipeline/LatencyQueue.h"

/**
 * A single scalar sample value flowing through the pipeline (one lidar ray
 * range, or one camera pixel/channel intensity). A `TOptional` empty value
 * represents "no return" (a lidar ray with no hit, logged as null); such
 * entries are passed through untouched and excluded from noise and dropout.
 */
using FSensorValue = TOptional<double>;

/**
 * Output of the noise + dropout stages for one capture, before latency.
 *
 *   - `Published[i]` is the noisy value, or empty when the entry was dropped
 *     (per-ray dropout) or had no return.
 *   - `bDropped[i]` records whether entry i was dropped by the dropout stage.
 */
struct FSensorStageOutput
{
	TArray<FSensorValue> Published;
	TArray<bool> bDropped;
};

/**
 * Shared, pure, seeded sensor-model pipeline (design: "Shared SensorModel
 * pipeline" and "Sensor model math"). The ordered stages are:
 *
 *   1. Capture Ground_Truth    (caller supplies the exact engine values)
 *   2. Add seeded noise        (Gaussian, sigma from config; sigma = 0 is an
 *                               exact passthrough)
 *   3. Apply dropout           (per-element Bernoulli with probability p, or
 *                               whole-sample Bernoulli for frame dropout)
 *   4. Enqueue with timestamp   onto a latency queue (see TLatencyQueue)
 *   5. Release after latency    at the first check where capture+L <= now
 *
 * All randomness comes from a per-sensor RNG derived from the run seed
 * (`sensor_seed = hash(run_seed, sensor_name)`), so identical seed + config
 * reproduce every draw exactly (Requirement 0.2, 0.3). Published ranges are
 * NOT clamped (clamping lidar ranges would bias the recovered sigma); the
 * optional clamp is off by default and used only by sensors that need it
 * (e.g. camera pixel intensities, in task 13).
 *
 * This class has no UObject/World/Engine dependency and is unit/property
 * testable headlessly. It is reused by lidar, camera, IMU and GPS.
 */
class FSensorPipeline
{
public:
	/**
	 * @param RunSeed     the single run seed every sensor derives from
	 * @param SensorName  names this sensor's independent RNG sub-stream
	 */
	FSensorPipeline(int64 RunSeed, const FString& SensorName);

	/** The derived per-sensor seed (`hash(run_seed, sensor_name)`). */
	uint64 GetSensorSeed() const { return Rng.GetSeed(); }

	/** Reset the RNG stream to its initial seed (reproduces the same draws). */
	void ResetRng() { Rng.Reset(); }

	/** Direct access to the per-sensor RNG (for sensors reusing the stream). */
	FSensorRng& GetRng() { return Rng; }

	/**
	 * Stage 2: add Gaussian noise N(0, Sigma^2) to each present value. Empty
	 * (no-return) entries are left empty. Sigma = 0 is an exact passthrough:
	 * no RNG draw is made and the values are returned unchanged. When
	 * bClamp is true each noisy value is clamped to [ClampMin, ClampMax].
	 */
	TArray<FSensorValue> AddGaussianNoise(
		const TArray<FSensorValue>& Truth,
		double Sigma,
		bool bClamp = false,
		double ClampMin = 0.0,
		double ClampMax = 0.0);

	/**
	 * Stage 3 (per-element): drop each present value independently with
	 * probability p (p = 0 keeps all, p = 1 drops all). A dropped entry becomes
	 * empty. Empty (no-return) entries are not counted and not drawn for.
	 * Returns the dropped-flag per entry alongside the surviving values.
	 */
	FSensorStageOutput ApplyPerElementDropout(const TArray<FSensorValue>& Values, double DropoutProb);

	/**
	 * Stage 3 (whole-sample, e.g. camera frame): return true iff this sample
	 * is dropped, drawing one Bernoulli(p). p = 0 never drops, p = 1 always
	 * drops.
	 */
	bool ApplyFrameDropout(double DropoutProb);

	/**
	 * Convenience: run stage 2 then stage 3 (per-element) over a captured
	 * scan in one call, with a single, fixed draw order (noise first for the
	 * whole scan, then dropout for the whole scan) so the output is
	 * reproducible for a given seed.
	 */
	FSensorStageOutput ProcessScan(
		const TArray<FSensorValue>& Truth,
		double Sigma,
		double DropoutProb,
		bool bClamp = false,
		double ClampMin = 0.0,
		double ClampMax = 0.0);

private:
	FSensorRng Rng;
};
