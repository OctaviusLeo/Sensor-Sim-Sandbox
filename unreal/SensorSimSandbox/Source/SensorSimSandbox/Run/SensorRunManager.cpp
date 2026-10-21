// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Run/SensorRunManager.h"

#include "Rover/Rover.h"
#include "Sensors/LidarModel.h"
#include "Sensors/LidarConfig.h"
#include "Sensors/LidarPointCloud.h"
#include "Sensors/CameraModel.h"
#include "Sensors/CameraConfig.h"
#include "Sensors/CameraMessage.h"
#include "Bridge/RosbridgeClient.h"
#include "Coords/TfPublisher.h"
#include "Coords/Rep103.h"
#include "GroundTruth/GroundTruthLoggerSelfCheck.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "CoreGlobals.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogSensorRun, Log, All);

namespace
{
	// Sensor mounts relative to the rover body origin (cm, Unreal frame). The
	// rover body is 160 x 100 x 50 cm; the lidar sits 1 m above its center and
	// the camera on the front edge, looking forward.
	const FVector LidarMountCm(0.0, 0.0, 100.0);
	const FVector CameraMountCm(90.0, 0.0, 40.0);

	FString ZeroPad(int64 Seq) { return FString::Printf(TEXT("%06lld"), Seq); }
}

ASensorRunManager::ASensorRunManager()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
}

double ASensorRunManager::Now() const
{
	return FPlatformTime::Seconds() - RunStartSeconds;
}

FLogRoverPose ASensorRunManager::CurrentPose() const
{
	FLogRoverPose Pose;
	if (Rover)
	{
		const FRep103Position P = FRep103::ConvertPosition(Rover->GetActorLocation());
		const FRep103Quat Q = FRep103::ConvertRotation(Rover->GetActorQuat());
		Pose.PosX = P.X; Pose.PosY = P.Y; Pose.PosZ = P.Z;
		Pose.QuatW = Q.W; Pose.QuatX = Q.X; Pose.QuatY = Q.Y; Pose.QuatZ = Q.Z;
	}
	return Pose;
}

void ASensorRunManager::BeginPlay()
{
	Super::BeginPlay();
	UWorld* World = GetWorld();
	const TCHAR* Cmd = FCommandLine::Get();

	if (!FParse::Value(Cmd, TEXT("RunDir="), RunDir) || RunDir.IsEmpty())
	{
		RunDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("e2e_run"));
	}
	FParse::Value(Cmd, TEXT("Seed="), Seed);
	FParse::Value(Cmd, TEXT("MinCameraFrames="), MinCameraFrames);
	FParse::Value(Cmd, TEXT("MinDurationS="), MinDurationS);
	FParse::Value(Cmd, TEXT("DemoFramesDir="), DemoFramesDir);

	FLidarConfig LidarCfg;
	FCameraConfig CameraCfg;
	FString Error;
	if (!FLidarConfig::LoadFromFile(FLidarConfig::ResolveConfigPath(), LidarCfg, Error))
	{
		UE_LOG(LogSensorRun, Error, TEXT("E2E run: lidar config failed: %s"), *Error);
		FinishRun(TEXT("lidar config failed"));
		return;
	}
	if (!FCameraConfig::LoadFromFile(FCameraConfig::ResolveConfigPath(), CameraCfg, Error))
	{
		UE_LOG(LogSensorRun, Error, TEXT("E2E run: camera config failed: %s"), *Error);
		FinishRun(TEXT("camera config failed"));
		return;
	}
	FString Interval;
	if (FParse::Value(Cmd, TEXT("CameraSamplingInterval="), Interval) && !Interval.IsEmpty())
	{
		CameraCfg.SamplingInterval = Interval;
	}

	// Sensors, mounted on the rover so they move with it.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Lidar = World->SpawnActor<ALidarModel>(ALidarModel::StaticClass(), FTransform::Identity, Params);
	Camera = World->SpawnActor<ACameraModel>(ACameraModel::StaticClass(), FTransform::Identity, Params);
	if (!Lidar || !Camera || !Rover)
	{
		UE_LOG(LogSensorRun, Error, TEXT("E2E run: could not spawn sensors or no rover"));
		FinishRun(TEXT("spawn failed"));
		return;
	}
	Lidar->AttachToActor(Rover, FAttachmentTransformRules::KeepRelativeTransform);
	Lidar->SetActorRelativeLocation(LidarMountCm);
	Camera->AttachToActor(Rover, FAttachmentTransformRules::KeepRelativeTransform);
	Camera->SetActorRelativeLocation(CameraMountCm);

	const FSensorConfigValidationResult LidarOk = Lidar->Initialise(Seed, LidarCfg, TEXT("lidar"));
	const FSensorConfigValidationResult CameraOk = Camera->Initialise(Seed, CameraCfg, TEXT("camera"));
	if (!LidarOk.bValid || !CameraOk.bValid)
	{
		UE_LOG(LogSensorRun, Error, TEXT("E2E run: sensor config rejected (lidar: %s; camera: %s)"),
			*LidarOk.ErrorMessage, *CameraOk.ErrorMessage);
		FinishRun(TEXT("config rejected"));
		return;
	}
	// M5 sensors: enabled unless -NoImuGps.
	bImuGpsEnabled = !FParse::Param(Cmd, TEXT("NoImuGps"));
	if (bImuGpsEnabled)
	{
		FImuConfig ImuCfg;
		FGpsConfig GpsCfg;
		if (!FImuConfig::LoadFromFile(FImuConfig::ResolveConfigPath(), ImuCfg, Error)
			|| !FGpsConfig::LoadFromFile(FGpsConfig::ResolveConfigPath(), GpsCfg, Error)
			|| !Imu.Initialise(Seed, ImuCfg, TEXT("imu")).bValid
			|| !Gps.Initialise(Seed, GpsCfg, TEXT("gps")).bValid)
		{
			UE_LOG(LogSensorRun, Error, TEXT("E2E run: IMU/GPS config failed: %s"), *Error);
			FinishRun(TEXT("imu/gps config failed"));
			return;
		}
		Logger.AddSensorConfigJson(TEXT("imu_accel"), FImuGpsMessages::ChannelConfigJson(
			ImuCfg.RateHz, ImuCfg.AccelNoiseSigma, ImuCfg.AccelBias, ImuCfg.DropoutProb, ImuCfg.LatencyMs));
		Logger.AddSensorConfigJson(TEXT("imu_gyro"), FImuGpsMessages::ChannelConfigJson(
			ImuCfg.RateHz, ImuCfg.GyroNoiseSigma, ImuCfg.GyroBias, ImuCfg.DropoutProb, ImuCfg.LatencyMs));
		Logger.AddSensorConfigJson(TEXT("gps"), FImuGpsMessages::ChannelConfigJson(
			GpsCfg.RateHz, GpsCfg.PositionNoiseSigmaM, GpsCfg.PositionBiasM, GpsCfg.DropoutProb, GpsCfg.LatencyMs));
	}

	LidarQueue.SetLatencySeconds(LidarCfg.LatencySeconds());
	CameraQueue.SetLatencySeconds(CameraCfg.LatencySeconds());

	// Bridge + TF.
	Client = MakeShared<FRosbridgeClient>(FRosbridgeConfig::Resolve());
	Client->Advertise(FLidarPointCloud::GetTopic(), FLidarPointCloud::GetMessageType());
	Client->Advertise(FCameraMessage::GetImageTopic(), FCameraMessage::GetImageMessageType());
	Client->Advertise(FCameraMessage::GetCameraInfoTopic(), FCameraMessage::GetCameraInfoMessageType());
	Client->Connect();
	if (bImuGpsEnabled)
	{
		Client->Advertise(FImuGpsMessages::ImuTopic(), FImuGpsMessages::ImuType());
		Client->Advertise(FImuGpsMessages::GpsTopic(), FImuGpsMessages::GpsType());
	}
	Tf = MakeUnique<FTfPublisher>(Client.Get(), FTfPublisher::MinRateHz);
	Tf->Advertise();

	// Run_Log.
	const FString RunLogPath = FPaths::Combine(RunDir, TEXT("run_log.jsonl"));
	if (!Logger.BeginRun(Seed, LidarCfg, FGroundTruthLoggerSelfCheck::ResolveCommitSha(),
		MakeUnique<FFileLogRunSink>(RunLogPath), CameraCfg.SamplingInterval, &CameraCfg))
	{
		UE_LOG(LogSensorRun, Error, TEXT("E2E run: BeginRun failed: %s"), *Logger.GetWriteFailureError());
		FinishRun(TEXT("Run_Log BeginRun failed"));
		return;
	}

	// A chase camera for the window (and the demo GIF).
	ViewCamera = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform::Identity, Params);
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0))
	{
		if (ViewCamera)
		{
			PC->SetViewTarget(ViewCamera);
		}
	}

	RunStartSeconds = FPlatformTime::Seconds();
	WallStartSeconds = static_cast<double>(FDateTime::UtcNow().ToUnixTimestamp())
		+ FDateTime::UtcNow().GetMillisecond() / 1000.0;
	bActive = true;
	UE_LOG(LogSensorRun, Log,
		TEXT("E2E run started: run_dir=%s seed=%lld min_camera_frames=%d min_duration_s=%.1f sampling=%s"),
		*RunDir, Seed, MinCameraFrames, MinDurationS, *CameraCfg.SamplingInterval);
}

void ASensorRunManager::CaptureDue(double T)
{
	const double LidarPeriod = Lidar->ScanPeriodSeconds();
	if (T >= NextLidarTime)
	{
		FPendingScan P;
		P.Scan = Lidar->PerformScan(T);
		P.Pose = CurrentPose();
		const int64 Seq = P.Scan.Seq;
		LidarQueue.Enqueue(Seq, T, P);
		++ScansProduced;
		// Keep the schedule (catch up after a hitch, one capture per tick); only
		// resync after falling more than a second behind.
		NextLidarTime += LidarPeriod;
		if (NextLidarTime < T - 1.0) { NextLidarTime = T + LidarPeriod; }
	}

	const double CameraPeriod = Camera->FramePeriodSeconds();
	if (T >= NextCameraTime)
	{
		FPendingFrame P;
		P.Frame = Camera->CaptureFrame(T);
		P.Pose = CurrentPose();
		++FramesProduced;
		if (P.Frame.bDropped)
		{
			// Dropped frame: logged at capture, never published (design).
			FLogSample S;
			S.Sensor = TEXT("camera");
			S.Seq = P.Frame.Seq;
			S.TCapture = T;
			S.bDropped = true;
			S.bIsCameraFrame = true;
			S.Pose = P.Pose;
			Logger.LogSample(S);
			++FramesDropped;
		}
		else
		{
			const int64 Seq = P.Frame.Seq;
			CameraQueue.Enqueue(Seq, T, P);
		}
		NextCameraTime += CameraPeriod;
		if (NextCameraTime < T - 1.0) { NextCameraTime = T + CameraPeriod; }
	}
}

void ASensorRunManager::PublishScan(const FPendingScan& P, double TPublish)
{
	// ROS header.stamp = UTC wall time at capture.
	FLidarScan Stamped = P.Scan;
	Stamped.CaptureTime = WallStartSeconds + P.Scan.CaptureTime;
	Client->Publish(FLidarPointCloud::GetTopic(), FLidarPointCloud::BuildMessage(Stamped));
	++ScansPublished;

	FLogSample S;
	S.Sensor = TEXT("lidar");
	S.Seq = P.Scan.Seq;
	S.TCapture = P.Scan.CaptureTime;
	S.TPublish = TPublish;
	S.bDropped = false;
	S.Pose = P.Pose;
	S.bHasRangeArrays = true;
	S.Truth = P.Scan.TruthRangeM;
	S.Published = P.Scan.PublishedRangeM;
	Logger.LogSample(S);
}

void ASensorRunManager::PublishFrame(const FPendingFrame& P, double TPublish)
{
	const FCameraFrame& F = P.Frame;
	const double Stamp = WallStartSeconds + F.CaptureTime;
	TArray<uint8> Jpeg;
	if (FCameraMessage::EncodeRgbaToJpeg(F.Published.Width, F.Published.Height, F.Published.Pixels, Jpeg))
	{
		Client->Publish(FCameraMessage::GetCameraInfoTopic(),
			FCameraMessage::BuildCameraInfoMessage(F.Published.Width, F.Published.Height, Stamp));
		Client->Publish(FCameraMessage::GetImageTopic(), FCameraMessage::BuildCompressedImageMessage(Jpeg, Stamp));
		++FramesPublished;
	}

	FLogSample S;
	S.Sensor = TEXT("camera");
	S.Seq = F.Seq;
	S.TCapture = F.CaptureTime;
	S.TPublish = TPublish;
	S.bDropped = false;
	S.bIsCameraFrame = true;
	S.Pose = P.Pose;
	if (F.bSampled)
	{
		const FString TruthRel = FString::Printf(TEXT("frames/cam_%s.png"), *ZeroPad(F.Seq));
		const FString PubRel = FString::Printf(TEXT("frames/cam_%s_pub.jpg"), *ZeroPad(F.Seq));
		TArray<uint8> TruthPng;
		if (FCameraMessage::EncodeRgbaToPng(F.Truth.Width, F.Truth.Height, F.Truth.Pixels, TruthPng)
			&& FFileHelper::SaveArrayToFile(TruthPng, *FPaths::Combine(RunDir, TruthRel))
			&& FFileHelper::SaveArrayToFile(Jpeg, *FPaths::Combine(RunDir, PubRel)))
		{
			ImagesWritten += 2;
			S.TruthRef = TruthRel;
			S.PublishedRef = PubRel;
		}
		else
		{
			UE_LOG(LogSensorRun, Warning, TEXT("E2E run: could not write image pair for frame %lld"), F.Seq);
		}
	}
	Logger.LogSample(S);
}

void ASensorRunManager::ReleaseDue(double T)
{
	for (const TReleasedSample<FPendingScan>& R : LidarQueue.ReleaseDue(T))
	{
		PublishScan(R.Payload, R.PublishTime);
	}
	for (const TReleasedSample<FPendingFrame>& R : CameraQueue.ReleaseDue(T))
	{
		PublishFrame(R.Payload, R.PublishTime);
	}
}

void ASensorRunManager::UpdateViewCamera()
{
	if (!ViewCamera || !Rover)
	{
		return;
	}
	const FTransform RoverXform = Rover->GetActorTransform();
	const FVector Eye = RoverXform.TransformPosition(FVector(-700.0, 0.0, 400.0));
	const FVector Look = RoverXform.GetLocation() + FVector(0.0, 0.0, 50.0);
	ViewCamera->SetActorLocationAndRotation(Eye, (Look - Eye).Rotation());
}

void ASensorRunManager::UpdateMotionTruth(float DeltaSeconds)
{
	if (!Rover || DeltaSeconds <= 0.0f)
	{
		return;
	}
	const FVector PosCm = Rover->GetActorLocation();
	const FQuat Quat = Rover->GetActorQuat();
	if (bHasPrevMotion)
	{
		const FVector VelCmS = (PosCm - PrevPosCm) / DeltaSeconds;
		const FVector AccelCmS2 = (VelCmS - PrevVelCmS) / DeltaSeconds;
		// Specific force in the body frame: a - g, with g = (0, 0, -9.80665) m/s^2.
		const FVector SpecificWorldCm = AccelCmS2 + FVector(0.0, 0.0, 980.665);
		const FVector BodyCm = Quat.UnrotateVector(SpecificWorldCm);
		TruthAccelRos = FVector(BodyCm.X, -BodyCm.Y, BodyCm.Z) / 100.0;

		// Body angular velocity from the rotation delta; REP 103 mirror of a
		// pseudovector across the y axis: (-wx, wy, -wz).
		const FQuat Delta = PrevQuat.Inverse() * Quat;
		FVector Axis;
		double Angle = 0.0;
		Delta.ToAxisAndAngle(Axis, Angle);
		if (Angle > PI) { Angle -= 2.0 * PI; }
		const FVector OmegaUe = Axis * (Angle / DeltaSeconds);
		TruthGyroRos = FVector(-OmegaUe.X, OmegaUe.Y, -OmegaUe.Z);
		PrevVelCmS = VelCmS;
	}
	PrevPosCm = PosCm;
	PrevQuat = Quat;
	bHasPrevMotion = true;
}

void ASensorRunManager::LogVectorChannel(const TCHAR* Sensor, const FVectorSample& S, int32 First, int32 Count,
	const FLogRoverPose& Pose, TOptional<double> TPublish)
{
	FLogSample L;
	L.Sensor = Sensor;
	L.Seq = S.Seq;
	L.TCapture = S.CaptureTime;
	L.TPublish = TPublish;
	L.bDropped = S.bDropped;
	L.Pose = Pose;
	L.bHasRangeArrays = true;
	for (int32 i = First; i < First + Count; ++i)
	{
		L.Truth.Add(FSensorValue(S.Truth[i]));
		L.Published.Add(S.bDropped ? FSensorValue() : FSensorValue(S.Published[i]));
	}
	Logger.LogSample(L);
}

void ASensorRunManager::CaptureImuGps(double T)
{
	if (T >= NextImuTime)
	{
		const FVectorSample S = Imu.Process(ImuSeq++, T, TruthAccelRos, TruthGyroRos);
		const FLogRoverPose Pose = CurrentPose();
		if (S.bDropped)
		{
			LogVectorChannel(TEXT("imu_accel"), S, 0, 3, Pose, TOptional<double>());
			LogVectorChannel(TEXT("imu_gyro"), S, 3, 3, Pose, TOptional<double>());
		}
		else
		{
			ImuGpsPoses.Add((int64(1) << 40) | S.Seq, Pose);
			Imu.EnqueueForLatency(S);
		}
		NextImuTime += Imu.GetConfig().PeriodSeconds();
		if (NextImuTime < T - 1.0) { NextImuTime = T + Imu.GetConfig().PeriodSeconds(); }
	}
	if (T >= NextGpsTime)
	{
		const FRep103Position P = FRep103::ConvertPosition(Rover->GetActorLocation());
		const FVectorSample S = Gps.Process(GpsSeq++, T, FVector(P.X, P.Y, P.Z));
		const FLogRoverPose Pose = CurrentPose();
		if (S.bDropped)
		{
			LogVectorChannel(TEXT("gps"), S, 0, 3, Pose, TOptional<double>());
		}
		else
		{
			ImuGpsPoses.Add((int64(2) << 40) | S.Seq, Pose);
			Gps.EnqueueForLatency(S);
		}
		NextGpsTime += Gps.GetConfig().PeriodSeconds();
		if (NextGpsTime < T - 1.0) { NextGpsTime = T + Gps.GetConfig().PeriodSeconds(); }
	}
}

void ASensorRunManager::ReleaseImuGps(double T)
{
	for (const TReleasedSample<FVectorSample>& R : Imu.ReleaseDue(T))
	{
		FLogRoverPose Pose;
		ImuGpsPoses.RemoveAndCopyValue((int64(1) << 40) | R.Seq, Pose);
		Client->Publish(FImuGpsMessages::ImuTopic(),
			FImuGpsMessages::BuildImu(R.Payload, Imu.GetConfig(), WallStartSeconds + R.CaptureTime));
		++ImuPublished;
		LogVectorChannel(TEXT("imu_accel"), R.Payload, 0, 3, Pose, R.PublishTime);
		LogVectorChannel(TEXT("imu_gyro"), R.Payload, 3, 3, Pose, R.PublishTime);
	}
	for (const TReleasedSample<FVectorSample>& R : Gps.ReleaseDue(T))
	{
		FLogRoverPose Pose;
		ImuGpsPoses.RemoveAndCopyValue((int64(2) << 40) | R.Seq, Pose);
		Client->Publish(FImuGpsMessages::GpsTopic(),
			FImuGpsMessages::BuildNavSatFix(R.Payload, Gps.GetConfig(), WallStartSeconds + R.CaptureTime));
		++GpsPublished;
		LogVectorChannel(TEXT("gps"), R.Payload, 0, 3, Pose, R.PublishTime);
	}
}

void ASensorRunManager::CaptureDemoFrame(double T)
{
	// Viewport screenshots contain only the game's own render, never other
	// windows on the desktop. 100 frames at 10 fps from t = 100 s (after the
	// rate-measurement windows; screenshot readback costs frame time).
	constexpr int32 DemoFrames = 100;
	if (DemoFramesDir.IsEmpty() || DemoFrameIndex >= DemoFrames || T < NextDemoTime)
	{
		return;
	}
	const FString Path = FPaths::Combine(DemoFramesDir, FString::Printf(TEXT("demo_%04d.png"), DemoFrameIndex));
	FScreenshotRequest::RequestScreenshot(Path, /*bShowUI*/ false, /*bAddFilenameSuffix*/ false);
	++DemoFrameIndex;
	NextDemoTime += 0.1;
}

void ASensorRunManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateViewCamera();
	if (!bActive || bFinished)
	{
		return;
	}
	++TickCount;
	TickSecondsSum += DeltaSeconds;

	const double T = Now();
	UpdateMotionTruth(DeltaSeconds);
	CaptureDue(T);
	if (bImuGpsEnabled)
	{
		CaptureImuGps(T);
		ReleaseImuGps(T);
	}
	ReleaseDue(T);
	CaptureDemoFrame(T);

	if (Rover && Tf)
	{
		TArray<FTfFrame> Frames;
		FTfFrame Base;
		Base.ParentFrameId = SensorFrames::Odom;
		Base.ChildFrameId = SensorFrames::BaseLink;
		Base.UePositionCm = Rover->GetActorLocation();
		Base.UeRotation = Rover->GetActorQuat();
		Frames.Add(Base);
		FTfFrame LidarFrame;
		LidarFrame.ParentFrameId = SensorFrames::BaseLink;
		LidarFrame.ChildFrameId = SensorFrames::LidarLink;
		LidarFrame.UePositionCm = LidarMountCm;
		Frames.Add(LidarFrame);
		FTfFrame CameraFrame;
		CameraFrame.ParentFrameId = SensorFrames::BaseLink;
		CameraFrame.ChildFrameId = SensorFrames::CameraLink;
		CameraFrame.UePositionCm = CameraMountCm;
		Frames.Add(CameraFrame);
		if (bImuGpsEnabled)
		{
			FTfFrame ImuFrame;
			ImuFrame.ParentFrameId = SensorFrames::BaseLink;
			ImuFrame.ChildFrameId = SensorFrames::ImuLink;
			Frames.Add(ImuFrame);
			FTfFrame GpsFrame;
			GpsFrame.ParentFrameId = SensorFrames::BaseLink;
			GpsFrame.ChildFrameId = SensorFrames::GpsLink;
			Frames.Add(GpsFrame);
		}
		Tf->Tick(WallStartSeconds + T, Frames);
	}

	if (!Logger.IsRunning())
	{
		FinishRun(FString::Printf(TEXT("Run_Log write failure: %s"), *Logger.GetWriteFailureError()));
		return;
	}
	if (FramesProduced >= MinCameraFrames && T >= MinDurationS)
	{
		FinishRun(TEXT("complete"));
	}
}

void ASensorRunManager::FinishRun(const FString& Reason)
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	const bool bComplete = Reason == TEXT("complete");
	const double Elapsed = bActive ? Now() : 0.0;
	bActive = false;
	Logger.EndRun();

	const double AvgFrameMs = TickCount > 0 ? 1000.0 * TickSecondsSum / TickCount : 0.0;
	const FString Done = FString::Printf(
		TEXT("status=%s\nreason=%s\nelapsed_s=%.3f\nlidar_scans_produced=%lld\nlidar_scans_published=%lld\n")
		TEXT("camera_frames_produced=%lld\ncamera_frames_dropped=%lld\ncamera_frames_published=%lld\n")
		TEXT("camera_images_written=%lld\nimu_samples_produced=%lld\nimu_published=%lld\ngps_samples_produced=%lld\ngps_published=%lld\n")
		TEXT("tf_published=%d\nticks=%lld\navg_frame_time_ms=%.3f\nseed=%lld\n"),
		bComplete ? TEXT("PASS") : TEXT("FAIL"), *Reason, Elapsed, ScansProduced, ScansPublished,
		FramesProduced, FramesDropped, FramesPublished, ImagesWritten, ImuSeq, ImuPublished, GpsSeq, GpsPublished,
		Tf ? Tf->GetPublishCount() : 0,
		TickCount, AvgFrameMs, Seed);
	FFileHelper::SaveStringToFile(Done, *FPaths::Combine(RunDir, TEXT("run_done.txt")));
	UE_LOG(LogSensorRun, Log, TEXT("E2E run finished (%s): %s"), *Reason, *Done.Replace(TEXT("\n"), TEXT(" ")));

	RequestEngineExit(TEXT("E2E run finished"));
}

void ASensorRunManager::EndPlay(const EEndPlayReason::Type Reason)
{
	if (!bFinished && bActive)
	{
		FinishRun(TEXT("game ended before the run completed"));
	}
	Super::EndPlay(Reason);
}
