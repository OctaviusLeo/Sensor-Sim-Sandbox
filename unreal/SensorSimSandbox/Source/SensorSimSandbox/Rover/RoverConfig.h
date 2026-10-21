// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"

/**
 * Parsed contents of config/level.yaml: the RoverLevel content (ground, lights,
 * obstacles, landmarks) and the scripted waypoint route. Pure data plus a
 * minimal, engine-independent parser for the small YAML subset the config file
 * uses, so the whole thing is unit-testable headlessly (design "Level content",
 * Requirement 2.2-2.6).
 *
 * The parser is intentionally narrow: it understands indentation-based nested
 * maps, `key: value` scalars, inline flow arrays like `[1.0, 2.0, 3.0]`, and
 * block sequences of maps introduced by `- key: value`. It is NOT a general
 * YAML implementation; it exists only to read this project's own config without
 * adding a third-party YAML dependency (keeping THIRD_PARTY.md unchanged).
 */

/** One placed static mesh (obstacle or landmark) built from an engine shape. */
struct FRoverPlacedActor
{
	FString Name;
	/** Engine basic shape: Cube, Sphere, Cylinder, Cone, Plane. */
	FString Shape = TEXT("Cube");
	/** World position in Unreal centimeters [x, y, z]. */
	FVector PositionCm = FVector::ZeroVector;
	/** Uniform/per-axis scale applied to the basic shape. */
	FVector Scale = FVector(1.0, 1.0, 1.0);
	/** Linear RGB color (0..1) applied via a dynamic material instance. */
	FLinearColor Color = FLinearColor::Gray;
};

/** Directional + sky lighting so camera frames are not black. */
struct FRoverLighting
{
	double DirectionalPitchDeg = -45.0;
	double DirectionalYawDeg = 0.0;
	double DirectionalIntensityLux = 8.0;
	double SkyIntensity = 1.0;
};

/** Ground plane parameters. */
struct FRoverGround
{
	double SizeM = 50.0;
	double ZCm = 0.0;
};

/**
 * The scripted waypoint route and tolerances. Positions are stored in Unreal
 * centimeters as authored; use GetWaypointsXYMeters() to get the (x, y) meter
 * pairs the pure state machine consumes.
 */
struct FRoverWaypointRoute
{
	double ArrivalToleranceM = 1.5;
	double WaypointTimeoutS = 30.0;
	/** Ordered route points as Unreal world (x, y) in centimeters. */
	TArray<TPair<double, double>> RouteCm;

	/** Convert the route to (x, y) pairs in meters (cm / 100). */
	TArray<TPair<double, double>> GetWaypointsXYMeters() const;
};

/** The full parsed level configuration. */
struct FRoverLevelConfig
{
	FRoverGround Ground;
	FRoverLighting Lighting;
	TArray<FRoverPlacedActor> Obstacles;
	TArray<FRoverPlacedActor> Landmarks;
	FRoverWaypointRoute Waypoints;

	/** True when the parsed config satisfies the design's minimum content. */
	bool MeetsMinimumContent(FString& OutReason) const;

	/**
	 * Count distinguishable landmarks: a landmark is distinguishable from
	 * another when it differs in shape OR color (Requirement 2.2). Returns the
	 * number of landmarks that are unique under (shape, quantized color).
	 */
	int32 CountDistinguishableLandmarks() const;

	/**
	 * Parse level config from YAML text. Returns true on success; on failure
	 * returns false and fills OutError. Pure: no file or engine access.
	 */
	static bool ParseFromString(const FString& YamlText, FRoverLevelConfig& OutConfig, FString& OutError);

	/** Load and parse from a file on disk. */
	static bool LoadFromFile(const FString& FilePath, FRoverLevelConfig& OutConfig, FString& OutError);
};
