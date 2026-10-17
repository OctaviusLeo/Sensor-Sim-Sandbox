// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"
#include "Pipeline/SensorPipeline.h"
#include "Pipeline/SensorConfigValidation.h"
#include "Pipeline/LatencyQueue.h"

class FJsonObject;

/**
 * IMU_Model and GPS_Model (M5, tasks 20/21, Requirements 11.1, 11.2).
 *
 * Both reuse the shared seeded pipeline exactly like the lidar and camera:
 * truth (computed by the caller from the rover's motion) -> per-axis Gaussian
 * noise -> constant per-axis bias -> whole-sample dropout -> latency queue.
 * Everything here is pure (no UObject / World / socket), so the property and
 * example tests drive it headlessly.
 *
 * Draw order per sample (fixed, so a seed reproduces every draw):
 *   IMU: accel noise (3 draws), gyro noise (3 draws), dropout (1 draw).
 *   GPS: position noise (3 draws), dropout (1 draw).
 * Noise is drawn even for a sample that is then dropped, so the RNG stream does
 * not depend on dropout outcomes.
 */

/** IMU config (config/imu.yaml). Bias is applied to each axis. */
struct FImuConfig
{
	double RateHz = 50.0;
	double AccelNoiseSigma = 0.05;  // m/s^2
	double AccelBias = 0.02;        // m/s^2, every axis
	double GyroNoiseSigma = 0.005;  // rad/s
	double GyroBias = 0.001;        // rad/s, every axis
	double DropoutProb = 0.02;
	int32 LatencyMs = 20;

	static FSensorBoundSet MakeBoundSet();
	TMap<FString, double> ToCandidateMap() const;
	FSensorConfigValidationResult Validate() const;
	double LatencySeconds() const { return LatencyMs / 1000.0; }
	double PeriodSeconds() const { return RateHz > 0.0 ? 1.0 / RateHz : 0.0; }

	static bool ParseFromString(const FString& YamlText, FImuConfig& Out, FString& OutError);
	static bool LoadFromFile(const FString& FilePath, FImuConfig& Out, FString& OutError);
	/** -ImuConfig=<path>, else <repo>/config/imu.yaml. */
	static FString ResolveConfigPath();
};

/** GPS config (config/gps.yaml). Noise and bias in meters, bias on every axis. */
struct FGpsConfig
{
	double RateHz = 10.0;
	double PositionNoiseSigmaM = 0.5;
	double PositionBiasM = 0.3;
	double DropoutProb = 0.05;
	int32 LatencyMs = 100;
	// Geodetic origin of the odom frame (x east, y north, z up) for NavSatFix.
	double OriginLatDeg = 47.6205;
	double OriginLonDeg = -122.3493;
	double OriginAltM = 50.0;

	static FSensorBoundSet MakeBoundSet();
	TMap<FString, double> ToCandidateMap() const;
	FSensorConfigValidationResult Validate() const;
	double LatencySeconds() const { return LatencyMs / 1000.0; }
	double PeriodSeconds() const { return RateHz > 0.0 ? 1.0 / RateHz : 0.0; }

	static bool ParseFromString(const FString& YamlText, FGpsConfig& Out, FString& OutError);
	static bool LoadFromFile(const FString& FilePath, FGpsConfig& Out, FString& OutError);
	/** -GpsConfig=<path>, else <repo>/config/gps.yaml. */
	static FString ResolveConfigPath();
};

/** One processed IMU or GPS sample. Values are REP 103 (body frame for IMU, odom ENU for GPS). */
struct FVectorSample
{
	int64 Seq = 0;
	double CaptureTime = 0.0;
	TArray<double> Truth;     // IMU: ax ay az gx gy gz; GPS: x y z
	TArray<double> Published; // same layout; meaningless when bDropped
	bool bDropped = false;
};

class SENSORSIMSANDBOX_API FImuPipelineRunner
{
public:
	FSensorConfigValidationResult Initialise(int64 RunSeed, const FImuConfig& Config, const FString& SensorName = TEXT("imu"));
	bool IsReady() const { return Pipeline.IsValid(); }
	const FImuConfig& GetConfig() const { return Config; }

	/** Truth -> noise -> bias -> dropout. `Accel` m/s^2 and `Gyro` rad/s in the body frame. */
	FVectorSample Process(int64 Seq, double CaptureTime, const FVector& Accel, const FVector& Gyro);

	void EnqueueForLatency(const FVectorSample& Sample) { Queue.Enqueue(Sample.Seq, Sample.CaptureTime, Sample); }
	TArray<TReleasedSample<FVectorSample>> ReleaseDue(double Now) { return Queue.ReleaseDue(Now); }

private:
	FImuConfig Config;
	TUniquePtr<FSensorPipeline> Pipeline;
	TLatencyQueue<FVectorSample> Queue;
};

class SENSORSIMSANDBOX_API FGpsPipelineRunner
{
public:
	FSensorConfigValidationResult Initialise(int64 RunSeed, const FGpsConfig& Config, const FString& SensorName = TEXT("gps"));
	bool IsReady() const { return Pipeline.IsValid(); }
	const FGpsConfig& GetConfig() const { return Config; }

	/** Truth position (meters, odom frame x east / y north / z up) -> noise -> bias -> dropout. */
	FVectorSample Process(int64 Seq, double CaptureTime, const FVector& PositionM);

	void EnqueueForLatency(const FVectorSample& Sample) { Queue.Enqueue(Sample.Seq, Sample.CaptureTime, Sample); }
	TArray<TReleasedSample<FVectorSample>> ReleaseDue(double Now) { return Queue.ReleaseDue(Now); }

private:
	FGpsConfig Config;
	TUniquePtr<FSensorPipeline> Pipeline;
	TLatencyQueue<FVectorSample> Queue;
};

/** sensor_msgs/Imu and sensor_msgs/NavSatFix builders (rosbridge JSON). */
class SENSORSIMSANDBOX_API FImuGpsMessages
{
public:
	static FString ImuTopic() { return TEXT("/imu"); }
	static FString ImuType() { return TEXT("sensor_msgs/msg/Imu"); }
	static FString GpsTopic() { return TEXT("/gps/fix"); }
	static FString GpsType() { return TEXT("sensor_msgs/msg/NavSatFix"); }

	/** Imu from a published sample; orientation not provided (covariance[0] = -1). */
	static TSharedRef<FJsonObject> BuildImu(const FVectorSample& Sample, const FImuConfig& Config, double StampSeconds);

	/** NavSatFix from a published sample, converted to lat/lon/alt about the config origin. */
	static TSharedRef<FJsonObject> BuildNavSatFix(const FVectorSample& Sample, const FGpsConfig& Config, double StampSeconds);

	/** Local tangent plane (east, north, up meters) -> WGS84 lat/lon/alt (spherical approximation). */
	static void EnuToGeodetic(const FGpsConfig& Config, double East, double North, double Up,
		double& OutLatDeg, double& OutLonDeg, double& OutAltM);

	/** Run_Log header object for one logged channel (generic keys read by analyze.py). */
	static FString ChannelConfigJson(double RateHz, double NoiseSigma, double Bias, double DropoutProb, int32 LatencyMs);
};
