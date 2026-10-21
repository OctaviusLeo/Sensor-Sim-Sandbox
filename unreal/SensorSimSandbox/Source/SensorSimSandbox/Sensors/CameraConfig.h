// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Pipeline/SensorConfigValidation.h"

/**
 * Parsed contents of config/camera.yaml (the design's CameraConfig). Pure data
 * plus a minimal, engine-independent parser for the flat `key: value` YAML the
 * file uses, so parsing and validation are unit-testable headlessly. This
 * mirrors FLidarConfig exactly (task 6.1), so the camera uses the same shape
 * and the same shared validation contract.
 *
 * The configurable parameters and their bounds are exactly those the design
 * states (Components -> Camera_Model, Data Models -> CameraConfig, Requirement
 * 4.1): width/height (1-7680, int), rate_hz (0.1-100.0), pixel_noise_sigma
 * (0.0-255.0), frame_dropout_prob (0.0-1.0), latency_ms (0-1000, int). These
 * are the SAME bounds the Property 2 config-bounds test enumerates for the
 * camera (ConfigBoundsEnforcedTest.cpp, task 5.3).
 *
 * Bounds are enforced through the SHARED validation contract (FSensorBoundSet /
 * FSensorConfigValidationResult, task 5.1): an out-of-bound candidate is
 * rejected as a whole, the offending parameter is named, and the holder retains
 * its last valid config. This type does NOT re-implement bound checking; it
 * builds the bound set once (MakeBoundSet) and delegates to it (Validate), so
 * the camera uses the same checker the Property 2 test exercises.
 *
 * `camera_sampling_interval` (the "1/10" ratio) is NOT a bound-checked sensor
 * parameter: it controls how often full Ground_Truth / published frames are
 * saved to disk, and the GroundTruthLogger records it in the run header
 * (task 7.1). It is parsed and carried here as a convenience so the shipped
 * config/camera.yaml is the single source of truth for the camera's run
 * settings; it is excluded from the candidate map and from Validate().
 */
struct FCameraConfig
{
	// --- Gate A defaults (design "Gate A run configuration") ---------------
	// 640 x 480 at 10 Hz; per-pixel noise sigma 5.0 intensity levels; 5% frame
	// dropout; 200 ms latency; frames sampled to disk 1 in 10.
	int32 Width = 640;
	int32 Height = 480;
	double RateHz = 10.0;
	double PixelNoiseSigma = 5.0;
	double FrameDropoutProb = 0.05;
	int32 LatencyMs = 200;

	/**
	 * Frames-logged-per-frames-produced ratio (e.g. "1/10"), recorded in the
	 * Run_Log header when camera frames are sampled rather than logged in full
	 * (Requirement 5.3). Not a bound-checked sensor parameter.
	 */
	FString SamplingInterval = TEXT("1/10");

	/**
	 * Build the shared bound set describing every bound-checked camera
	 * parameter, in the design's declared order (so the "offending parameter"
	 * is deterministic). This is the single source of truth the camera
	 * validates against; it matches the bounds the Property 2 config-bounds
	 * test enumerates for the camera.
	 */
	static FSensorBoundSet MakeBoundSet();

	/** Express this config as a name -> value candidate map for validation. */
	TMap<FString, double> ToCandidateMap() const;

	/**
	 * Validate this config against the shared bound set. Returns a Valid result
	 * when every parameter is within bounds, otherwise an Invalid result naming
	 * the first offending parameter (Requirement 4.1, 4.2). The sampling
	 * interval is not validated here (it is not a bounded sensor parameter).
	 */
	FSensorConfigValidationResult Validate() const;

	/** Latency in seconds (latency_ms / 1000), for the pipeline latency queue. */
	double LatencySeconds() const { return static_cast<double>(LatencyMs) / 1000.0; }

	/** Frame period in seconds (1 / rate_hz). */
	double FramePeriodSeconds() const { return RateHz > 0.0 ? 1.0 / RateHz : 0.0; }

	/** Pixel count of one RGB frame (width * height). */
	int64 NumPixels() const { return static_cast<int64>(Width) * static_cast<int64>(Height); }

	/**
	 * Parse the "N/M" sampling interval into N (logged) and M (produced). A
	 * sampled frame is one whose zero-based seq satisfies `seq % M < N` (so
	 * "1/10" samples 1 of every 10, seq 0, 10, 20, ...). Returns false and
	 * leaves OutN/OutM unchanged when the string is not a positive "N/M" ratio
	 * with N <= M.
	 */
	static bool ParseSamplingInterval(const FString& Ratio, int32& OutN, int32& OutM);

	/**
	 * True iff the frame with zero-based `Seq` is sampled to disk under this
	 * config's sampling interval. A malformed interval samples every frame
	 * (fail-open: never silently drop evidence).
	 */
	bool IsFrameSampled(int64 Seq) const;

	/**
	 * Parse a camera config from flat `key: value` YAML text. Always fills
	 * OutConfig with whatever was parsed (unspecified keys keep their Gate A
	 * default). Returns true on a clean parse; on a value that fails to parse
	 * returns false and fills OutError. NOTE: a successful parse does NOT imply
	 * the values are in-bound — call Validate() for that, exactly as a sensor
	 * applying a config update would.
	 */
	static bool ParseFromString(const FString& YamlText, FCameraConfig& OutConfig, FString& OutError);

	/** Load and parse from a file on disk. */
	static bool LoadFromFile(const FString& FilePath, FCameraConfig& OutConfig, FString& OutError);

	/** Resolve the repo-default config path (config/camera.yaml), or -CameraConfig=. */
	static FString ResolveConfigPath();
};
