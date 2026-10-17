// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"
#include "Sensors/LidarConfig.h"

/**
 * One ray's direction in the lidar's local frame, together with the azimuth and
 * elevation that generated it (kept for logging/debugging and for building the
 * point cloud later in task 12).
 *
 * Direction is a unit vector in Unreal's left-handed sensor-local frame:
 * +X forward, +Y right, +Z up. Azimuth sweeps around +Z (yaw), elevation tilts
 * above/below the horizontal plane (pitch).
 */
struct FLidarRayDir
{
	/** Unit direction in the sensor-local frame. */
	FVector Direction = FVector::ForwardVector;
	/** Azimuth angle in degrees within [azimuth_start, azimuth_start+h_fov). */
	double AzimuthDeg = 0.0;
	/** Elevation angle in degrees within [-v_fov/2, +v_fov/2]. */
	double ElevationDeg = 0.0;
	/** Channel (row) index, 0 = lowest elevation. */
	int32 Channel = 0;
	/** Azimuth sample (column) index, 0 = azimuth_start. */
	int32 AzimuthIndex = 0;
};

/**
 * Pure, engine-independent generation of a scan's ray directions from a
 * LidarConfig (design "Lidar_Model" scan geometry). Kept separate from the
 * engine raycast (ULidarModel) so the geometry can be unit/property tested
 * headlessly, exactly as the pipeline and the waypoint state machine were kept
 * pure.
 *
 * Geometry (a standard spinning-lidar layout):
 *   - Azimuth samples sweep the horizontal FOV `h_fov_deg` in steps of
 *     `angular_resolution_deg`. The number of columns is
 *     `NumAzimuthSamples = round(h_fov_deg / angular_resolution_deg)`, except a
 *     full 360 deg sweep uses exactly that many columns without duplicating the
 *     wrap-around sample (so 360 / 1.0 = 360 distinct columns, not 361).
 *   - `channels` rows are spaced evenly across the vertical FOV `v_fov_deg`,
 *     centered on the horizontal plane: elevations run from `-v_fov/2` to
 *     `+v_fov/2`. With one channel the single row sits on the horizontal plane.
 *   - Total rays per scan = channels * NumAzimuthSamples (16 * 360 = 5,760 at
 *     the Gate A config, matching the design).
 *
 * Rays are emitted channel-major (all azimuths of channel 0, then channel 1,
 * ...), giving a fixed, reproducible ray order so the per-ray truth and
 * published arrays line up by index with the pipeline output.
 */
class FLidarScanGeometry
{
public:
	/**
	 * Number of azimuth columns for a config: round(h_fov / resolution),
	 * at least 1. A 360 deg sweep is treated as periodic so the final column
	 * is not a duplicate of the first.
	 */
	static int32 NumAzimuthSamples(const FLidarConfig& Config);

	/** Total rays per scan = channels * NumAzimuthSamples (>= 1). */
	static int32 NumRays(const FLidarConfig& Config);

	/**
	 * Generate all ray directions for one scan, in channel-major order. The
	 * returned array has NumRays(Config) entries. AzimuthStartDeg lets a caller
	 * phase the sweep (default 0); it does not change the ray count.
	 */
	static TArray<FLidarRayDir> GenerateRays(const FLidarConfig& Config, double AzimuthStartDeg = 0.0);

	/** Elevation (deg) of channel `Channel` of `NumChannels` across `VFovDeg`. */
	static double ElevationForChannel(int32 Channel, int32 NumChannels, double VFovDeg);
};
