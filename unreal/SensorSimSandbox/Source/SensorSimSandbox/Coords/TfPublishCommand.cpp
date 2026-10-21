// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Coords/TfPublisher.h"
#include "Bridge/RosbridgeClient.h"

#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "CoreGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogTfPublish, Log, All);

// ---------------------------------------------------------------------------
// Console command: `TfPublish [seconds]`
//
// Task 12.3 [HOST] demo/verify (Requirement 7.4): wire the FTfPublisher >= 10 Hz
// scheduler through the LIVE Rosbridge_Client (task 11, which implements
// ITfTransport) and publish /tf over the real WebSocket for a fixed window, so
// `ros2 topic hz /tf` in WSL can measure the achieved rate against the 10 Hz
// floor. The rover body + lidar + camera frames are published each tick; the
// transforms are fixed poses (the rate, not the pose, is what 7.4 is about).
//
// It mirrors RosbridgePublish/LidarPublish: resolve the bridge config, connect,
// advertise /tf, then drive the scheduler on the engine ticker until the window
// elapses, then disconnect and request a clean engine exit so the driving
// script does not depend on a separate ;Quit. The FTfPublisher clock is the
// real wall clock (FPlatformTime), so the published rate is the genuine live
// rate the hz measurement will see.
//
// Default window 20 s gives `ros2 topic hz /tf` a comfortable sample window.
// ---------------------------------------------------------------------------

namespace
{
	// Kept alive across the publish window so the socket stays open.
	TSharedPtr<FRosbridgeClient> GTfPublishClient;
	FTSTicker::FDelegateHandle GTfPublishTickerHandle;
	FTfPublisher* GTfPublisher = nullptr;
	double GTfPublishStartSeconds = 0.0;
	double GTfPublishWindowSeconds = 20.0;
	// First/last publish wall times so the reported rate is measured over the
	// actual publish timeline ((n-1)/span), matching the automation test and
	// the design's achieved-rate form — not count/elapsed, which under-reports
	// by one period because the first publish lands a period after start.
	double GTfFirstPublishSeconds = -1.0;
	double GTfLastPublishSeconds = -1.0;
	int32 GTfPrevCount = 0;

	TArray<FTfFrame> MakeLiveFrames()
	{
		TArray<FTfFrame> Frames;
		Frames.Add(FTfFrame(SensorFrames::Odom, SensorFrames::BaseLink,
			FVector(250.0, -100.0, 0.0), FQuat(FVector::UpVector, FMath::DegreesToRadians(30.0))));
		Frames.Add(FTfFrame(SensorFrames::BaseLink, SensorFrames::LidarLink,
			FVector(0.0, 0.0, 50.0), FQuat::Identity));
		Frames.Add(FTfFrame(SensorFrames::BaseLink, SensorFrames::CameraLink,
			FVector(30.0, 0.0, 40.0), FQuat::Identity));
		return Frames;
	}

	void CleanupTfPublish()
	{
		if (GTfPublishTickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GTfPublishTickerHandle);
			GTfPublishTickerHandle.Reset();
		}
		if (GTfPublisher != nullptr)
		{
			delete GTfPublisher;
			GTfPublisher = nullptr;
		}
		if (GTfPublishClient.IsValid())
		{
			GTfPublishClient->Disconnect();
			GTfPublishClient.Reset();
		}
	}
}

static void ExecuteTfPublish(const TArray<FString>& Args)
{
	if (Args.Num() >= 1 && !Args[0].IsEmpty())
	{
		const double Parsed = FCString::Atod(*Args[0]);
		if (Parsed > 0.0)
		{
			GTfPublishWindowSeconds = Parsed;
		}
	}

	// --- Connect the live bridge and advertise /tf -------------------------
	const FRosbridgeConfig BridgeConfig = FRosbridgeConfig::Resolve();
	GTfPublishClient = MakeShared<FRosbridgeClient>(BridgeConfig);
	GTfPublishClient->Connect();

	// FTfPublisher sends pre-built /tf ops straight through the client
	// (ITfTransport). The scheduler clamps UP to MinRateHz (10 Hz).
	GTfPublisher = new FTfPublisher(GTfPublishClient.Get(), FTfPublisher::MinRateHz);
	GTfPublisher->Advertise(); // advertise /tf through the client

	GTfPublishStartSeconds = FPlatformTime::Seconds();

	UE_LOG(LogTfPublish, Log,
		TEXT("TfPublish: publishing /tf at >= %.0f Hz through the Rosbridge_Client to %s for %.1f s."),
		FTfPublisher::MinRateHz, *BridgeConfig.ToUrl(), GTfPublishWindowSeconds);

	// Drive the scheduler on the engine ticker using the real wall clock so the
	// live published rate is genuine. The ticker fires every frame; the
	// scheduler itself only emits a publish when a >= 10 Hz period has elapsed.
	GTfPublishTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float /*Dt*/) -> bool
		{
			const double Now = FPlatformTime::Seconds();
			const double Elapsed = Now - GTfPublishStartSeconds;

			if (GTfPublisher != nullptr)
			{
				// Stamp each TFMessage with the elapsed seconds (ROS header
				// time); the frames are fixed poses.
				GTfPublisher->Tick(Elapsed, MakeLiveFrames());

				// Record the wall time of each actual publish so the rate is
				// measured over the publish timeline, not count/elapsed.
				const int32 CountNow = GTfPublisher->GetPublishCount();
				if (CountNow > GTfPrevCount)
				{
					if (GTfFirstPublishSeconds < 0.0)
					{
						GTfFirstPublishSeconds = Now;
					}
					GTfLastPublishSeconds = Now;
					GTfPrevCount = CountNow;
				}
			}

			if (Elapsed >= GTfPublishWindowSeconds)
			{
				const int32 Count = (GTfPublisher != nullptr) ? GTfPublisher->GetPublishCount() : 0;
				// Achieved rate over the actual publish span ((n-1)/T), the same
				// form the automation test and design use. Falls back to 0 if
				// fewer than two publishes landed.
				const double Span = (GTfLastPublishSeconds > GTfFirstPublishSeconds)
					? (GTfLastPublishSeconds - GTfFirstPublishSeconds) : 0.0;
				const double Rate = (Count >= 2 && Span > 0.0)
					? (static_cast<double>(Count - 1) / Span) : 0.0;
				const bool bConnected = GTfPublishClient.IsValid() && GTfPublishClient->IsConnected();
				UE_LOG(LogTfPublish, Log,
					TEXT("TfPublish done: %d /tf publishes over a %.3f s publish span => %.4f Hz ")
					TEXT("((n-1)/T), socket connected=%d. Disconnecting and requesting engine exit."),
					Count, Span, Rate, bConnected ? 1 : 0);
				CleanupTfPublish();
				RequestEngineExit(TEXT("TfPublish complete"));
				return false; // stop ticking
			}
			return true; // keep ticking
		}), 0.0f);
}

static FAutoConsoleCommand GTfPublishCommand(
	TEXT("TfPublish"),
	TEXT("Publish /tf at >= 10 Hz through the live Rosbridge_Client for a fixed window so `ros2 topic hz /tf` can measure the rate (Requirement 7.4). Usage: TfPublish [seconds]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&ExecuteTfPublish));
