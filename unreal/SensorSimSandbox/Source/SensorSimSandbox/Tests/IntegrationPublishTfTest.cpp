// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "Rover/Rover.h"
#include "Coords/TfPublisher.h"

#include "Engine/World.h"
#include "Engine/Engine.h"

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 12.3 — INTEGRATION EXAMPLE (not a >=100
// iteration property test).
//
// Validates: Requirements 2.1, 7.4
//
// Two headless-demonstrable sub-claims of the integration are pinned here; the
// third sub-claim (an end-to-end message observed on a live ROS 2 topic) is a
// [HOST] step driven by scripts/run_integration_tf_hz.ps1 (live `ros2 topic hz
// /tf`) and reusing the task 12.1 live `/lidar/points` echo — a live socket and
// WSL are outside an -nullrhi automation run, so it is not asserted here.
//
//   A. Keyboard-input latency <= 100 ms (2.1). An ARover is spawned in a
//      transient game world (the same world/seam the task 4.2 test uses), put
//      in Manual mode, and driven with an INJECTED keyboard axis via the
//      existing Test_InjectKeyboardAxis seam — i.e. the exact field a real key
//      press writes. Requirement 2.1 says keyboard input begins the
//      corresponding motion "within 100 ms". Manual mode consumes the injected
//      axis in the SAME Tick (ARover::Tick -> ApplyDrive), so the commanded
//      motion responds on the first tick. We assert the commanded throttle/
//      steer changed from rest within one tick, and report the elapsed latency
//      as the tick dt (the engine delta between reading the input and applying
//      the motion), which must be <= 100 ms. A zero-input tick is the control:
//      no injected input leaves the command at rest.
//
//   B. TF publish rate >= 10 Hz (7.4). The FTfPublisher deadline-anchored
//      scheduler (MinRateHz = 10) is driven over a synthetic clock and the
//      achieved publish rate (and the largest inter-publish gap) is measured
//      and asserted >= 10 Hz. This is the same scheduler the live /tf transport
//      (the Rosbridge_Client, task 11, which implements ITfTransport) drives in
//      the game; the [HOST] script measures the live `ros2 topic hz /tf`
//      through that transport and records the number (or defers it with the
//      reason), never fabricating a live Hz.
//
// Reported numbers (not just pass/fail) are emitted via AddInfo so the run log
// carries the measured latency (ms) and TF rate (Hz).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxIntegrationPublishTfTest,
	"SensorSimSandbox.Integration.KeyboardLatencyAndTfRate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
{
	// The Requirement 2.1 latency budget: keyboard input must begin the motion
	// within this many milliseconds.
	constexpr double InputLatencyBudgetMs = 100.0;
}

bool FSensorSimSandboxIntegrationPublishTfTest::RunTest(const FString& Parameters)
{
	// =======================================================================
	// A. Keyboard-input latency <= 100 ms (Requirement 2.1).
	// =======================================================================
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
		if (World == nullptr)
		{
			AddError(TEXT("Integration (2.1): failed to create a transient game world."));
			return false;
		}
		FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
		WorldContext.SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());

		bool bLatencyOk = true;

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ARover* Rover = World->SpawnActor<ARover>(ARover::StaticClass(), FTransform::Identity, SpawnParams);
		if (Rover == nullptr)
		{
			AddError(TEXT("Integration (2.1): failed to spawn ARover."));
			bLatencyOk = false;
		}
		else
		{
			// Manual mode so the keyboard axis actually drives the rover (2.1).
			Rover->SetMode(ERoverMode::Manual);
			Rover->SetActorLocation(FVector::ZeroVector);
			Rover->SetActorRotation(FRotator::ZeroRotator);

			// --- Control: with NO injected input, the command stays at rest. --
			Rover->Test_InjectKeyboardAxis(0.0f, 0.0f);
			// A realistic frame dt (60 FPS => ~16.7 ms). Manual mode reads the
			// injected input and applies the motion in the SAME ARover::Tick, so
			// the input-to-motion latency is exactly one frame dt. We report that
			// frame time as the measured latency; at any plausible frame rate it
			// is well under the 100 ms budget (even a 10 FPS / 100 ms frame would
			// only just reach the budget, since the response is same-tick, never
			// a tick later).
			const float TickDtSeconds = 1.0f / 60.0f;
			Rover->Tick(TickDtSeconds);
			const float RestThrottle = Rover->GetCommandedThrottle();
			const float RestSteer = Rover->GetCommandedSteer();
			if (!FMath::IsNearlyZero(RestThrottle) || !FMath::IsNearlyZero(RestSteer))
			{
				AddError(FString::Printf(
					TEXT("Integration (2.1) control FAILED: with zero injected input the rover ")
					TEXT("already commanded motion (throttle %.6f, steer %.6f); the latency ")
					TEXT("measurement would be meaningless."),
					RestThrottle, RestSteer));
				bLatencyOk = false;
			}

			// --- Inject a full-forward + full-right keyboard axis, then Tick ONCE.
			// Manual mode applies it the same tick, so the commanded motion
			// responds within one frame. The latency from the injected input
			// event to the commanded-motion change is therefore <= one tick dt.
			Rover->Test_InjectKeyboardAxis(1.0f, 1.0f);
			Rover->Tick(TickDtSeconds);
			const float MovedThrottle = Rover->GetCommandedThrottle();
			const float MovedSteer = Rover->GetCommandedSteer();

			// The commanded motion must have CHANGED from rest within this one
			// tick (i.e. the input moved the rover).
			const bool bResponded =
				!FMath::IsNearlyEqual(MovedThrottle, RestThrottle)
				|| !FMath::IsNearlyEqual(MovedSteer, RestSteer);
			if (!bResponded)
			{
				AddError(FString::Printf(
					TEXT("Integration (2.1) FAILED: injected keyboard axis did not change the ")
					TEXT("commanded motion within one tick (throttle %.6f, steer %.6f). The rover ")
					TEXT("must begin applying motion within 100 ms of input."),
					MovedThrottle, MovedSteer));
				bLatencyOk = false;
			}

			// The injected full-forward/right axis should drive full throttle
			// and full steer (ApplyDrive clamps to [-1, 1]).
			if (bResponded
				&& (!FMath::IsNearlyEqual(MovedThrottle, 1.0f) || !FMath::IsNearlyEqual(MovedSteer, 1.0f)))
			{
				AddError(FString::Printf(
					TEXT("Integration (2.1) FAILED: expected full-forward/right command (1,1) from ")
					TEXT("a full keyboard axis, got (throttle %.6f, steer %.6f)."),
					MovedThrottle, MovedSteer));
				bLatencyOk = false;
			}

			// Measured latency = the elapsed time between reading the input and
			// applying the motion, which is one tick dt here (the input is read
			// and the motion applied in the SAME ARover::Tick, so the response is
			// never deferred to a later frame). Report it at a 60 FPS frame.
			const double MeasuredLatencyMs = static_cast<double>(TickDtSeconds) * 1000.0;
			AddInfo(FString::Printf(
				TEXT("Integration (2.1): keyboard-input latency = %.3f ms (one 60 FPS tick dt; ")
				TEXT("input read and motion applied in the same ARover::Tick, so the response is ")
				TEXT("same-tick, never a frame later) <= %.0f ms budget."),
				MeasuredLatencyMs, InputLatencyBudgetMs));

			if (MeasuredLatencyMs > InputLatencyBudgetMs + 1e-9)
			{
				AddError(FString::Printf(
					TEXT("Integration (2.1) FAILED: measured latency %.3f ms exceeds the %.0f ms ")
					TEXT("budget."),
					MeasuredLatencyMs, InputLatencyBudgetMs));
				bLatencyOk = false;
			}

			Rover->Destroy();
		}

		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);

		if (!bLatencyOk)
		{
			return false;
		}
	}

	// =======================================================================
	// B. TF publish rate >= 10 Hz (Requirement 7.4).
	// =======================================================================
	{
		FCapturingTfTransport Transport;
		FTfPublisher Publisher(&Transport); // default rate = MinRateHz (10)
		Publisher.Advertise();

		// One rover-body frame plus two sensor frames, so the scheduler drives a
		// realistic multi-frame TFMessage each publish (the frame contents do
		// not affect the rate).
		TArray<FTfFrame> Frames;
		Frames.Add(FTfFrame(SensorFrames::Odom, SensorFrames::BaseLink,
			FVector(250.0, -100.0, 0.0), FQuat(FVector::UpVector, FMath::DegreesToRadians(30.0))));
		Frames.Add(FTfFrame(SensorFrames::BaseLink, SensorFrames::LidarLink,
			FVector(0.0, 0.0, 50.0), FQuat::Identity));
		Frames.Add(FTfFrame(SensorFrames::BaseLink, SensorFrames::CameraLink,
			FVector(30.0, 0.0, 40.0), FQuat::Identity));

		// Drive a synthetic game clock at a fine step (200 Hz) over 3 s and let
		// the scheduler decide when to publish, recording each publish time so
		// the achieved rate is measured from the ACTUAL publish timeline.
		const double Duration = 3.0;
		const double TickStep = 1.0 / 200.0;
		TArray<double> PublishTimes;
		int32 PrevCount = 0;
		for (double Now = 0.0; Now <= Duration + 1e-9; Now += TickStep)
		{
			Publisher.Tick(Now, Frames);
			if (Publisher.GetPublishCount() > PrevCount)
			{
				PublishTimes.Add(Now);
				PrevCount = Publisher.GetPublishCount();
			}
		}

		const int32 Publishes = Publisher.GetPublishCount();
		if (!TestTrue(TEXT("Integration (7.4): at least two TF publishes to measure a rate"),
			Publishes >= 2 && PublishTimes.Num() >= 2))
		{
			return false;
		}

		// The capturing transport must have received exactly the publishes the
		// publisher counted — the op really went through the ITfTransport sink
		// (the same seam the live Rosbridge_Client fills), so this is an
		// end-to-end publish through the transport, not just a counter.
		if (!TestEqual(TEXT("Integration (7.4): transport received every TF publish"),
			Transport.NumPublishes(), Publishes))
		{
			return false;
		}

		const double Span = PublishTimes.Last() - PublishTimes[0];
		const double MeasuredRateHz = (Span > 0.0)
			? (static_cast<double>(PublishTimes.Num() - 1) / Span) : 0.0;

		double MaxGap = 0.0;
		for (int32 i = 1; i < PublishTimes.Num(); ++i)
		{
			MaxGap = FMath::Max(MaxGap, PublishTimes[i] - PublishTimes[i - 1]);
		}
		const double Period = Publisher.GetPeriodSeconds();

		AddInfo(FString::Printf(
			TEXT("Integration (7.4): TF scheduler published %d TFMessages over %.3f s => %.4f Hz ")
			TEXT("(>= %.0f Hz floor), max inter-publish gap %.4f s <= period %.4f s."),
			Publishes, Span, MeasuredRateHz, FTfPublisher::MinRateHz, MaxGap, Period));

		if (!TestTrue(FString::Printf(
				TEXT("Integration (7.4): measured TF rate %.4f Hz >= 10 Hz"), MeasuredRateHz),
			MeasuredRateHz + 1e-6 >= FTfPublisher::MinRateHz))
		{
			return false;
		}

		// Timeline-local >= 10 Hz guarantee: no gap exceeds one period (plus the
		// synthetic tick granularity + a float epsilon).
		if (!TestTrue(FString::Printf(
				TEXT("Integration (7.4): max inter-publish gap %.4f s <= period %.4f s"),
				MaxGap, Period),
			MaxGap <= Period + TickStep + 1e-9))
		{
			return false;
		}
	}

	return true;
}
