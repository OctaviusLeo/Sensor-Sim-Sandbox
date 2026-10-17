// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"
#include "Pipeline/SensorPipeline.h"
#include "Pipeline/LatencyQueue.h"
#include "Sensors/CameraConfig.h"

/**
 * One captured RGBA frame flowing through the camera pipeline. Pixels are laid
 * out row-major, 4 channels per pixel (R, G, B, A) in [0, 255]; the alpha
 * channel is carried verbatim (not noised, not dropped) so a frame can be
 * re-encoded to PNG/JPEG by the actor without losing opacity. Width*Height*4
 * must equal Pixels.Num().
 */
struct FCameraFrameBuffer
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<uint8> Pixels; // row-major RGBA, 4 bytes/pixel

	bool IsValidForConfig(const FCameraConfig& Config) const
	{
		return Width == Config.Width && Height == Config.Height &&
			Pixels.Num() == static_cast<int64>(Width) * Height * 4;
	}

	int64 NumPixels() const { return static_cast<int64>(Width) * static_cast<int64>(Height); }
};

/**
 * The result of running one captured camera frame through the pipeline: the
 * pristine Ground_Truth frame (captured BEFORE the pipeline), the published
 * (per-pixel-noised) frame, whether the whole frame was dropped, and whether
 * this frame is sampled to disk under the config's sampling interval.
 *
 *   - Truth       the exact engine-rendered RGBA frame, untouched.
 *   - Published   Truth + per-pixel Gaussian noise clamped to [0,255]; EMPTY
 *                 (Pixels.Num()==0) when the frame was dropped (a dropped
 *                 frame is never published, Requirement 4.5).
 *   - bDropped    true iff the whole frame was dropped by the dropout stage.
 *   - bSampled    true iff this frame's images should be written to disk
 *                 (camera_sampling_interval); a dropped frame carries no image
 *                 refs regardless (design "Ground-truth logging format").
 */
struct FCameraFrame
{
	/** Per-sensor frame counter, from 0. */
	int64 Seq = 0;

	/** Monotonic capture time (seconds since run start) of this frame. */
	double CaptureTime = 0.0;

	/** Pristine Ground_Truth frame, captured BEFORE the pipeline. */
	FCameraFrameBuffer Truth;

	/** Published frame (noised, clamped); empty when the frame was dropped. */
	FCameraFrameBuffer Published;

	/** True iff the whole frame was dropped by the dropout stage. */
	bool bDropped = false;

	/** True iff this frame is sampled to disk under the sampling interval. */
	bool bSampled = false;

	/** True iff a published frame exists (not dropped). */
	bool HasPublished() const { return !bDropped && Published.Pixels.Num() > 0; }
};

/**
 * The PURE core of the Camera_Model: config validation (via the shared bound
 * set) plus the seeded per-pixel noise + whole-frame dropout + latency pipeline
 * over a captured frame. It has NO UObject/World/Engine/RHI dependency, so the
 * capture-before-pipeline, clamp-to-[0,255] and frame-dropout rules can be
 * unit/property tested headlessly on SYNTHETIC frames and constructed on the
 * stack (an AActor cannot). ACameraModel owns one of these and adds only the
 * engine scene-capture + GPU readback that produces the truth pixels.
 *
 * This mirrors FLidarPipelineRunner (task 6.1) exactly, so the camera reuses
 * the same shared pipeline and validation contract as every other sensor.
 *
 * Pipeline order (design "Shared SensorModel pipeline" / "Sensor model math"):
 *   1. Capture Ground_Truth   (caller supplies the exact engine RGBA pixels)
 *   2. Seeded per-pixel noise (sigma = pixel_noise_sigma, clamp [0,255];
 *                              sigma 0 is an exact passthrough)
 *   3. Whole-frame dropout    (probability = frame_dropout_prob; a dropped
 *                              frame is not published)
 *   4. Latency queue          (release at the first check where
 *                              capture_time + L <= now)
 *
 * Draw order is fixed for reproducibility: ALL per-pixel noise draws for the
 * frame happen first (R,G,B per pixel in row-major order; alpha carried
 * verbatim, never drawn for), THEN the single whole-frame dropout draw. The
 * draws are made regardless of the dropout outcome, so the per-sensor RNG
 * stream advances identically for a given seed whether or not a frame is
 * dropped (design "Determinism"). Only the RGB channels are noised; the alpha
 * channel passes through untouched so re-encoding keeps a valid image.
 */
class FCameraPipelineRunner
{
public:
	FCameraPipelineRunner() = default;

	/**
	 * Validate `Config` through the shared contract and, if valid, adopt it and
	 * (re)create the per-sensor pipeline + latency queue from `RunSeed` /
	 * `SensorName`. On an invalid config the previous config is retained
	 * (Requirement 4.2). Returns the validation result.
	 */
	FSensorConfigValidationResult Initialise(int64 InRunSeed, const FCameraConfig& Config, const FString& InSensorName = TEXT("camera"));

	/**
	 * Apply a new config at runtime. Validates through the shared contract; on
	 * success the config is adopted (and the latency queue's latency updated),
	 * on failure it is rejected as a whole and the last valid config is
	 * retained (Requirement 4.2). The RNG stream is NOT reset, preserving
	 * reproducibility of the ongoing run.
	 */
	FSensorConfigValidationResult ApplyConfig(const FCameraConfig& Candidate);

	/** The current (last valid) config. */
	const FCameraConfig& GetConfig() const { return CurrentConfig; }

	/** True once Initialise adopted a valid config and built the pipeline. */
	bool IsReady() const { return Pipeline.IsValid(); }

	/** The derived per-sensor seed, for logging/repro. */
	uint64 GetSensorSeed() const { return Pipeline.IsValid() ? Pipeline->GetSensorSeed() : 0; }

	/**
	 * Run stages 2-3 over an already-captured truth frame (the capture is
	 * assumed to have happened BEFORE this call). Applies per-pixel Gaussian
	 * noise clamped to [0,255], then one whole-frame dropout draw, and marks
	 * whether the frame is sampled to disk. The returned FCameraFrame carries
	 * the pristine truth verbatim and the published (noised) frame, which is
	 * empty when the frame was dropped. Does NOT enqueue onto the latency queue
	 * (callers that need latency call EnqueueForLatency / ReleaseDue).
	 */
	FCameraFrame ProcessCapturedFrame(int64 Seq, double CaptureTime, const FCameraFrameBuffer& Truth);

	// --- Latency stage (shared TLatencyQueue over frame payloads) ----------
	//
	// A camera frame payload is just its seq (the published pixels live in the
	// caller's FCameraFrame); the queue delays the *publish decision* so the
	// actor publishes a frame only once capture_time + L <= now, exactly like
	// the lidar. L = 0 releases a frame in the same step it was captured.

	/** Enqueue a (non-dropped) frame's seq at its capture time for latency. */
	void EnqueueForLatency(int64 Seq, double CaptureTime);

	/** Release every frame whose capture_time + L <= Now, stamping publish_time. */
	TArray<TReleasedSample<int64>> ReleaseDue(double Now);

	/** Number of frames waiting in the latency queue. */
	int32 NumPendingLatency() const { return LatencyQueue.Num(); }

	/** Access to the pure pipeline (for tests). */
	FSensorPipeline* GetPipeline() { return Pipeline.Get(); }

private:
	FCameraConfig CurrentConfig;
	int64 RunSeed = 0;
	FString SensorName = TEXT("camera");

	/** Shared seeded pipeline; created on Initialise, reused across frames. */
	TUniquePtr<FSensorPipeline> Pipeline;

	/** Shared latency queue over frame seqs. */
	TLatencyQueue<int64> LatencyQueue;
};
