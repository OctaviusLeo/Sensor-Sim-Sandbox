// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Pipeline/LatencyQueue.h"
#include "Sensors/LidarPipelineRunner.h"
#include "Sensors/CameraPipelineRunner.h"
#include "GroundTruth/GroundTruthLogger.h"
#include "Coords/TfPublisher.h"
#include "SensorRunManager.generated.h"

class ARover;
class ALidarModel;
class ACameraModel;
class ACameraActor;
class FRosbridgeClient;

/**
 * ASensorRunManager — drives a full sensor run in a `-game` session (task 18.1,
 * design "End-to-end run").
 *
 * Spawned by ARoverLevelBuilder when the command line carries `-E2ERun`. It
 * mounts the lidar and camera on the rover, and every game tick:
 *   - captures a lidar scan / camera frame when its period elapses (truth is
 *     captured before the pipeline by the sensor models);
 *   - logs a dropped camera frame immediately, and queues every other sample
 *     in a latency queue released at `capture + latency` (checked per tick);
 *   - on release publishes PointCloud2 / CompressedImage + CameraInfo through
 *     the Rosbridge_Client, writes the sampled camera image pair, and appends
 *     the Run_Log record;
 *   - publishes /tf at >= 10 Hz.
 * It stops once `-MinCameraFrames` frames were produced AND `-MinDurationS`
 * seconds elapsed, ends the Run_Log, writes `run_done.txt` (counts and the
 * average frame time) into the run folder, and requests a clean engine exit.
 *
 * Command line: -E2ERun -RunDir=<abs folder> [-Seed=1234] [-MinCameraFrames=2000]
 *   [-MinDurationS=120] [-CameraSamplingInterval=1/10] [-BridgeHost/-BridgePort]
 *   [-DemoFramesDir=<abs folder>]  (10 s of 10 fps viewport screenshots for the demo GIF)
 */
UCLASS()
class SENSORSIMSANDBOX_API ASensorRunManager : public AActor
{
	GENERATED_BODY()

public:
	ASensorRunManager();

	/** Adopt the rover the sensors are mounted on. Call before BeginPlay runs. */
	void SetRover(ARover* InRover) { Rover = InRover; }

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	struct FPendingScan
	{
		FLidarScan Scan;
		FLogRoverPose Pose;
	};
	struct FPendingFrame
	{
		FCameraFrame Frame;
		FLogRoverPose Pose;
	};

	double Now() const;
	FLogRoverPose CurrentPose() const;
	void CaptureDue(double T);
	void ReleaseDue(double T);
	void PublishScan(const FPendingScan& P, double TPublish);
	void PublishFrame(const FPendingFrame& P, double TPublish);
	void UpdateViewCamera();
	void CaptureDemoFrame(double T);
	void FinishRun(const FString& Reason);

	UPROPERTY() TObjectPtr<ARover> Rover;
	UPROPERTY() TObjectPtr<ALidarModel> Lidar;
	UPROPERTY() TObjectPtr<ACameraModel> Camera;
	UPROPERTY() TObjectPtr<ACameraActor> ViewCamera;

	TSharedPtr<FRosbridgeClient> Client;
	TUniquePtr<FTfPublisher> Tf;
	FGroundTruthLogger Logger;

	TLatencyQueue<FPendingScan> LidarQueue;
	TLatencyQueue<FPendingFrame> CameraQueue;

	FString RunDir;
	FString DemoFramesDir;
	double NextDemoTime = 100.0; // after the 60 s `ros2 topic hz` windows, so screenshots do not perturb them
	int32 DemoFrameIndex = 0;
	int64 Seed = 1234;
	int32 MinCameraFrames = 2000;
	double MinDurationS = 120.0;

	double RunStartSeconds = 0.0;
	double WallStartSeconds = 0.0; // UTC epoch seconds at run start (ROS stamps)
	double NextLidarTime = 0.0;
	double NextCameraTime = 0.0;

	int64 ScansProduced = 0;
	int64 ScansPublished = 0;
	int64 FramesProduced = 0;
	int64 FramesDropped = 0;
	int64 FramesPublished = 0;
	int64 ImagesWritten = 0;
	int64 TickCount = 0;
	double TickSecondsSum = 0.0;

	bool bActive = false;
	bool bFinished = false;
};
