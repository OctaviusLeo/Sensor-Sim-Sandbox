// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Sensors/LidarScanGeometry.h"

int32 FLidarScanGeometry::NumAzimuthSamples(const FLidarConfig& Config)
{
	if (Config.AngularResolutionDeg <= 0.0)
	{
		return 1;
	}
	const int32 Samples = FMath::RoundToInt(Config.HFovDeg / Config.AngularResolutionDeg);
	return FMath::Max(Samples, 1);
}

int32 FLidarScanGeometry::NumRays(const FLidarConfig& Config)
{
	const int32 Channels = FMath::Max(Config.Channels, 1);
	return Channels * NumAzimuthSamples(Config);
}

double FLidarScanGeometry::ElevationForChannel(int32 Channel, int32 NumChannels, double VFovDeg)
{
	if (NumChannels <= 1)
	{
		// A single channel sits on the horizontal plane.
		return 0.0;
	}
	// Spread channels evenly from -VFov/2 to +VFov/2 inclusive.
	const double Lo = -VFovDeg / 2.0;
	const double Step = VFovDeg / static_cast<double>(NumChannels - 1);
	return Lo + Step * static_cast<double>(Channel);
}

TArray<FLidarRayDir> FLidarScanGeometry::GenerateRays(const FLidarConfig& Config, double AzimuthStartDeg)
{
	const int32 Channels = FMath::Max(Config.Channels, 1);
	const int32 Columns = NumAzimuthSamples(Config);

	// A full 360 sweep wraps, so the azimuth step divides the FOV evenly over
	// `Columns` distinct columns. For a partial FOV the step is the configured
	// resolution and the sweep spans [start, start + h_fov].
	const bool bFullSweep = FMath::IsNearlyEqual(Config.HFovDeg, 360.0, 1e-6);
	const double AzStep = bFullSweep
		? (360.0 / static_cast<double>(Columns))
		: Config.AngularResolutionDeg;

	TArray<FLidarRayDir> Rays;
	Rays.Reserve(Channels * Columns);

	for (int32 Ch = 0; Ch < Channels; ++Ch)
	{
		const double ElevDeg = ElevationForChannel(Ch, Channels, Config.VFovDeg);
		const double ElevRad = FMath::DegreesToRadians(ElevDeg);
		const double CosElev = FMath::Cos(ElevRad);
		const double SinElev = FMath::Sin(ElevRad);

		for (int32 Col = 0; Col < Columns; ++Col)
		{
			const double AzDeg = AzimuthStartDeg + AzStep * static_cast<double>(Col);
			const double AzRad = FMath::DegreesToRadians(AzDeg);

			// Left-handed sensor-local frame: +X forward, +Y right, +Z up.
			// Azimuth rotates in the XY plane; elevation tilts toward +Z.
			FLidarRayDir Ray;
			Ray.Direction = FVector(
				CosElev * FMath::Cos(AzRad),
				CosElev * FMath::Sin(AzRad),
				SinElev);
			Ray.Direction.Normalize();
			Ray.AzimuthDeg = AzDeg;
			Ray.ElevationDeg = ElevDeg;
			Ray.Channel = Ch;
			Ray.AzimuthIndex = Col;
			Rays.Add(Ray);
		}
	}

	return Rays;
}
