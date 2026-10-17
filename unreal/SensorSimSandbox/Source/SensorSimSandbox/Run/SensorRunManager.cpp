// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

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
	LidarQueue.SetLatencySeconds(LidarCfg.LatencySeconds());
	CameraQueue.SetLatencySeconds(CameraCfg.LatencySeconds());

	// Bridge + TF.
	Client = MakeShared<FRosbridgeClient>(FRosbridgeConfig::Resolve());
	Client->Advertise(FLidarPointCloud::GetTopic(), FLidarPointCloud::GetMessageType());
	Client->Advertise(FCameraMessage::GetImageTopic(), FCameraMessage::GetImageMessageType());
	Client->Advertise(FCameraMessage::GetCameraInfoTopic(), FCameraMessage::GetCameraInfoMessageType());
	Client->Connect();
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
		// Keep the schedule; if the game fell more than a period behind, resync.
		NextLidarTime += LidarPeriod;
		if (NextLidarTime < T) { NextLidarTime = T + LidarPeriod; }
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
		if (NextCameraTime < T) { NextCameraTime = T + CameraPeriod; }
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
	CaptureDue(T);
	ReleaseDue(T);

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
		TEXT("camera_images_written=%lld\ntf_published=%d\nticks=%lld\navg_frame_time_ms=%.3f\nseed=%lld\n"),
		bComplete ? TEXT("PASS") : TEXT("FAIL"), *Reason, Elapsed, ScansProduced, ScansPublished,
		FramesProduced, FramesDropped, FramesPublished, ImagesWritten, Tf ? Tf->GetPublishCount() : 0,
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
