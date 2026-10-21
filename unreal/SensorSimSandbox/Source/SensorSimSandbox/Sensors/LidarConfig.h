// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Pipeline/SensorConfigValidation.h"

/**
 * Parsed contents of config/lidar.yaml (the design's LidarConfig). Pure data
 * plus a minimal, engine-independent parser for the flat `key: value` YAML the
 * file uses, so parsing and validation are unit-testable headlessly.
 *
 * The configurable parameters and their bounds are exactly those the design
 * states (Components -> Lidar_Model, Data Models -> LidarConfig, Requirement
 * 3.1): channels (1-128, int), h_fov_deg (0 exclusive-360], v_fov_deg
 * (0 exclusive-180], angular_resolution_deg (0.01-10.0), max_range_m
 * (0.1-1000.0), rate_hz (0.1-100.0), range_noise_sigma_m (0.0-10.0),
 * dropout_prob (0.0-1.0), latency_ms (0-1000, int).
 *
 * Bounds are enforced through the SHARED validation contract (FSensorBoundSet /
 * FSensorConfigValidationResult, task 5.1): an out-of-bound candidate is
 * rejected as a whole, the offending parameter is named, and the holder retains
 * its last valid config. This type does NOT re-implement bound checking; it
 * builds the bound set once (MakeBoundSet) and delegates to it (Validate), so
 * the lidar uses the same checker the Property 2 test exercises.
 */
struct FLidarConfig
{
	// --- Gate A defaults (design "Gate A run configuration") ---------------
	// 16 channels x 360 deg at 1.0 deg = 360 azimuth samples x 16 channels =
	// 5,760 rays per scan.
	int32 Channels = 16;
	double HFovDeg = 360.0;
	double VFovDeg = 30.0;
	double AngularResolutionDeg = 1.0;
	double MaxRangeM = 100.0;
	double RateHz = 10.0;
	double RangeNoiseSigmaM = 0.02;
	double DropoutProb = 0.10;
	int32 LatencyMs = 200;

	/**
	 * Build the shared bound set describing every lidar parameter's bound, in
	 * the design's declared order (so the "offending parameter" is
	 * deterministic). This is the single source of truth the lidar validates
	 * against; it matches the bounds the Property 2 config-bounds test
	 * enumerates.
	 */
	static FSensorBoundSet MakeBoundSet();

	/** Express this config as a name -> value candidate map for validation. */
	TMap<FString, double> ToCandidateMap() const;

	/**
	 * Validate this config against the shared bound set. Returns a Valid result
	 * when every parameter is within bounds, otherwise an Invalid result naming
	 * the first offending parameter (Requirement 3.1, 3.2).
	 */
	FSensorConfigValidationResult Validate() const;

	/** Latency in seconds (latency_ms / 1000), for the pipeline latency queue. */
	double LatencySeconds() const { return static_cast<double>(LatencyMs) / 1000.0; }

	/** Scan period in seconds (1 / rate_hz). */
	double ScanPeriodSeconds() const { return RateHz > 0.0 ? 1.0 / RateHz : 0.0; }

	/**
	 * Parse a lidar config from flat `key: value` YAML text. Always fills
	 * OutConfig with whatever was parsed (unspecified keys keep their Gate A
	 * default). Returns true on a clean parse; on a value that fails to parse
	 * returns false and fills OutError. NOTE: a successful parse does NOT imply
	 * the values are in-bound — call Validate() for that, exactly as a sensor
	 * applying a config update would.
	 */
	static bool ParseFromString(const FString& YamlText, FLidarConfig& OutConfig, FString& OutError);

	/** Load and parse from a file on disk. */
	static bool LoadFromFile(const FString& FilePath, FLidarConfig& OutConfig, FString& OutError);

	/** Resolve the repo-default config path (config/lidar.yaml), or -LidarConfig=. */
	static FString ResolveConfigPath();
};
