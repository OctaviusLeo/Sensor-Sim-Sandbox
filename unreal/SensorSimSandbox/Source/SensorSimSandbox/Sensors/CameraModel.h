// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sensors/CameraPipelineRunner.h"
#include "Sensors/CameraConfig.h"
#include "CameraModel.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

/**
 * Camera_Model (design "Camera_Model", Requirement 4.1, 4.2, 4.5, 4.6).
 *
 * A scene-capture camera actor. It renders the scene to a render target at
 * width x height, reads back the noise-free Ground_Truth RGBA pixels, then runs
 * the SHARED seeded pipeline over the captured truth:
 *
 *   1. Capture Ground_Truth    (render target readback; captured BEFORE noise)
 *   2. Seeded per-pixel noise  (sigma = pixel_noise_sigma, clamp [0,255];
 *                               sigma 0 passthrough)
 *   3. Whole-frame dropout     (probability = frame_dropout_prob; a dropped
 *                               frame is not published)
 *   4. Latency queue           (release at capture_time + L <= now)
 *
 * The actor is a THIN engine wrapper: all config validation and the pure
 * noise/dropout/latency stages live in FCameraPipelineRunner (so they are
 * headlessly testable on synthetic frames and stack-constructible). The actor
 * adds only the engine-dependent steps — configuring the USceneCaptureComponent2D
 * + render target and the GPU readback that produces the truth pixels — and it
 * runs that readback entirely BEFORE handing the captured truth to the runner,
 * so the recorded Ground_Truth is always the pristine engine render.
 *
 * Config bounds are enforced through the SHARED validation contract
 * (FCameraConfig::Validate -> FSensorBoundSet): an out-of-bound candidate is
 * rejected as a whole, the offending parameter is logged, and the last valid
 * config is retained (Requirement 4.2). The in-bound accept path is the subject
 * of task 13.2's example test.
 *
 * This mirrors ALidarModel (task 6.1): the pure core is reusable and testable;
 * only the one engine-dependent capture step lives in the actor.
 */
UCLASS()
class SENSORSIMSANDBOX_API ACameraModel : public AActor
{
	GENERATED_BODY()

public:
	ACameraModel();

	/**
	 * Initialise the model: validate `Config` through the shared contract and,
	 * if valid, adopt it, (re)create the per-sensor pipeline from `RunSeed` and
	 * the sensor name, and size the render target to width x height. Returns the
	 * validation result; on an invalid config the model keeps its previous
	 * config (Requirement 4.2).
	 */
	FSensorConfigValidationResult Initialise(int64 InRunSeed, const FCameraConfig& Config, const FString& InSensorName = TEXT("camera"));

	/**
	 * Apply a new config at runtime. Validates through the shared contract; on
	 * success the config is adopted and the render target resized, on failure it
	 * is rejected as a whole, the offending parameter is logged, and the last
	 * valid config is retained (Requirement 4.2).
	 */
	FSensorConfigValidationResult ApplyConfig(const FCameraConfig& Candidate);

	/** The current (last valid) config. */
	const FCameraConfig& GetConfig() const { return Runner.GetConfig(); }

	/**
	 * Capture one frame at capture time `CaptureTime` (seconds). Renders the
	 * scene to the render target, reads back the pristine Ground_Truth RGBA
	 * pixels BEFORE the pipeline, then runs per-pixel noise + whole-frame
	 * dropout via the runner. Requires a valid World and a GPU-backed RHI; with
	 * no world / render target the frame is returned with an empty truth buffer
	 * so headless callers degrade gracefully (and the frame is marked dropped).
	 */
	FCameraFrame CaptureFrame(double CaptureTime);

	/** Frame period (s) from the config rate; useful for the demo scheduler. */
	double FramePeriodSeconds() const { return Runner.GetConfig().FramePeriodSeconds(); }

	/** True once Initialise adopted a valid config and built the pipeline. */
	bool IsReady() const { return Runner.IsReady(); }

	/** The derived per-sensor seed, for logging/repro. */
	uint64 GetSensorSeed() const { return Runner.GetSensorSeed(); }

	/** Access to the pure runner (for tests / headless pure processing). */
	FCameraPipelineRunner& GetRunner() { return Runner; }

	/** The scene-capture component (for callers that place/aim the camera). */
	USceneCaptureComponent2D* GetCaptureComponent() const { return CaptureComponent; }

private:
	/** The pure config + pipeline core (validation, noise, dropout, latency). */
	FCameraPipelineRunner Runner;

	int64 NextSeq = 0;

	/** Scene-capture component rendering the scene into RenderTarget. */
	UPROPERTY()
	TObjectPtr<USceneCaptureComponent2D> CaptureComponent = nullptr;

	/** Render target sized to the configured width x height. */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> RenderTarget = nullptr;

	/** (Re)create / resize the render target to the current config resolution. */
	void EnsureRenderTarget();

	/**
	 * Render the scene and read back the pristine truth RGBA pixels into
	 * OutTruth (row-major, 4 bytes/pixel). This is the ONLY engine/RHI step; it
	 * runs entirely before any pipeline stage. Returns false (OutTruth empty)
	 * when no render target / RHI is available (e.g. a -nullrhi headless run).
	 */
	bool CaptureTruthPixels(FCameraFrameBuffer& OutTruth) const;
};
