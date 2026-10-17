// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Misc/AutomationTest.h"

#include "Rover/Rover.h"
#include "Rover/RoverConfig.h"
#include "Rover/WaypointStateMachine.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Misc/Paths.h"

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 4.3 — Unreal Automation EXAMPLE tests for
// rover edge cases. These are concrete scenario assertions (not property /
// generator loops; those live in WaypointOrderPreservedTest.cpp for task 4.2).
// Each exercises exactly one acceptance criterion against the real production
// code, headlessly under -nullrhi:
//
//   * SensorSimSandbox.Rover.FinalWaypointHold         -> Requirement 2.5
//   * SensorSimSandbox.Rover.WaypointTimeoutReport     -> Requirement 2.6
//   * SensorSimSandbox.Rover.LandmarkDistinguishability-> Requirement 2.2
//
// The waypoint tests spawn an ARover in a transient game world (the
// CreateWorld / SpawnActor / Tick / Destroy pattern used by
// WaypointOrderPreservedTest.cpp) and assert at the ARover command seam
// (GetCommandedThrottle / GetCommandedSteer) that the pawn actually stops, so
// the "zero commanded throttle and steering" clause of 2.5/2.6 is verified end
// to end rather than only at the pure state machine. The landmark test loads
// the real config/level.yaml with the same parser the level builder uses.
// ---------------------------------------------------------------------------

namespace
{
	// Spin up a minimal transient game world so an ARover can be spawned and
	// ticked headlessly (-nullrhi). Returns nullptr on failure.
	UWorld* CreateTransientGameWorld(FAutomationTestBase& Test, FWorldContext*& OutContext)
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
		if (World == nullptr)
		{
			Test.AddError(TEXT("failed to create a transient game world."));
			OutContext = nullptr;
			return nullptr;
		}
		OutContext = &GEngine->CreateNewWorldContext(EWorldType::Game);
		OutContext->SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		return World;
	}

	void DestroyTransientGameWorld(UWorld* World, FWorldContext* Context)
	{
		if (World == nullptr)
		{
			return;
		}
		if (Context != nullptr)
		{
			GEngine->DestroyWorldContext(World);
		}
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	}

	ARover* SpawnRover(UWorld* World)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return World->SpawnActor<ARover>(ARover::StaticClass(), FTransform::Identity, SpawnParams);
	}
}

// ===========================================================================
// 2.5 — Final-waypoint hold with zero throttle/steer.
//
// Drive an ARover in Waypoint mode along a short two-leg route. Place the rover
// exactly on the final waypoint and Tick: the pure machine detects arrival at
// the final waypoint (distance <= tolerance), latches HoldAtFinal, and the pawn
// must command ZERO throttle and ZERO steer while holding. We then Tick several
// more times (still on the final waypoint) and confirm the hold latches: the
// command stays exactly zero and HasReachedFinal() remains true.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxRoverFinalWaypointHoldTest,
	"SensorSimSandbox.Rover.FinalWaypointHold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxRoverFinalWaypointHoldTest::RunTest(const FString& Parameters)
{
	FWorldContext* Context = nullptr;
	UWorld* World = CreateTransientGameWorld(*this, Context);
	if (World == nullptr)
	{
		return false;
	}

	bool bOk = true;

	ARover* Rover = SpawnRover(World);
	if (Rover == nullptr)
	{
		AddError(TEXT("2.5 FAILED: could not spawn ARover."));
		bOk = false;
	}
	else
	{
		// A short route in meters. The final waypoint is at (10, 10) m.
		TArray<TPair<double, double>> Route;
		Route.Add(TPair<double, double>(10.0, 0.0));
		Route.Add(TPair<double, double>(10.0, 10.0));
		const double ToleranceM = 1.5;
		const double FinalXMeters = 10.0;
		const double FinalYMeters = 10.0;

		Rover->SetMode(ERoverMode::Waypoint);
		Rover->ConfigureWaypoints(Route, ToleranceM, /*TimeoutS*/ 0.0);

		// Advance the machine onto the first leg so it is actively driving,
		// then place the rover ON the final waypoint so arrival is detected at
		// the final index. Positions are Unreal centimeters (meters * 100).
		// First tick near the first waypoint to advance the index to the final.
		Rover->SetActorLocation(FVector(10.0 * 100.0, 0.0 * 100.0, 0.0)); // on waypoint 0
		Rover->SetActorRotation(FRotator::ZeroRotator);
		Rover->Tick(0.05f); // detects arrival at wp0, advances to final (wp1)

		if (Rover->GetWaypointMachine().HasReachedFinal())
		{
			AddError(TEXT("2.5 FAILED: machine latched final after only reaching waypoint 0."));
			bOk = false;
		}

		// Now place the rover on the final waypoint and tick: arrival at final.
		Rover->SetActorLocation(FVector(FinalXMeters * 100.0, FinalYMeters * 100.0, 0.0));
		Rover->Tick(0.05f);

		if (!Rover->GetWaypointMachine().HasReachedFinal())
		{
			AddError(TEXT("2.5 FAILED: rover on the final waypoint did not latch HoldAtFinal."));
			bOk = false;
		}
		if (!TestEqual(TEXT("2.5: commanded throttle is zero while holding at final"),
				Rover->GetCommandedThrottle(), 0.0f))
		{
			bOk = false;
		}
		if (!TestEqual(TEXT("2.5: commanded steer is zero while holding at final"),
				Rover->GetCommandedSteer(), 0.0f))
		{
			bOk = false;
		}

		// The hold must latch: further ticks keep zero command and stay final,
		// even if the rover is nudged slightly off the exact waypoint.
		Rover->SetActorLocation(FVector((FinalXMeters + 0.2) * 100.0, (FinalYMeters - 0.2) * 100.0, 0.0));
		for (int32 Extra = 0; Extra < 5; ++Extra)
		{
			Rover->Tick(0.05f);
			if (!Rover->GetWaypointMachine().HasReachedFinal())
			{
				AddError(FString::Printf(
					TEXT("2.5 FAILED: hold did not latch on extra tick %d."), Extra));
				bOk = false;
				break;
			}
			if (!FMath::IsNearlyZero(Rover->GetCommandedThrottle())
				|| !FMath::IsNearlyZero(Rover->GetCommandedSteer()))
			{
				AddError(FString::Printf(
					TEXT("2.5 FAILED: non-zero command while holding (tick %d: throttle %.6f, steer %.6f)."),
					Extra, Rover->GetCommandedThrottle(), Rover->GetCommandedSteer()));
				bOk = false;
				break;
			}
		}

		Rover->Destroy();
	}

	DestroyTransientGameWorld(World, Context);
	return bOk;
}

// ===========================================================================
// 2.6 — Waypoint-timeout stop + unreached-waypoint report.
//
// Configure a route whose first waypoint the rover never approaches, with a
// finite per-waypoint timeout. Keep the rover stationary at the origin (far
// from the waypoint) and Tick with dt that accumulates past the timeout. The
// machine must transition to StoppedTimedOut, the pawn must command ZERO
// throttle/steer (the rover stops), ARover::HasReportedTimeout() must become
// true, and ARover::GetUnreachedWaypointIndex() must identify the exact
// waypoint that was not reached (index 0 here).
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxRoverWaypointTimeoutReportTest,
	"SensorSimSandbox.Rover.WaypointTimeoutReport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxRoverWaypointTimeoutReportTest::RunTest(const FString& Parameters)
{
	FWorldContext* Context = nullptr;
	UWorld* World = CreateTransientGameWorld(*this, Context);
	if (World == nullptr)
	{
		return false;
	}

	bool bOk = true;

	ARover* Rover = SpawnRover(World);
	if (Rover == nullptr)
	{
		AddError(TEXT("2.6 FAILED: could not spawn ARover."));
		bOk = false;
	}
	else
	{
		// First waypoint sits 1000 m away; the rover will be held at the origin
		// so it can never arrive within the small tolerance before the timeout.
		TArray<TPair<double, double>> Route;
		Route.Add(TPair<double, double>(1000.0, 0.0));  // unreached waypoint (index 0)
		Route.Add(TPair<double, double>(1000.0, 1000.0));
		const double ToleranceM = 0.5;
		const double TimeoutS = 2.0;
		const int32 ExpectedUnreachedIndex = 0;

		Rover->SetMode(ERoverMode::Waypoint);
		Rover->ConfigureWaypoints(Route, ToleranceM, TimeoutS);

		if (Rover->HasReportedTimeout())
		{
			AddError(TEXT("2.6 FAILED: timeout reported before any time elapsed."));
			bOk = false;
		}

		// Pin the rover far from waypoint 0 and advance time well past the
		// timeout. Re-pin every tick because driving would otherwise move it;
		// the point is that it never reaches the waypoint in time.
		const FVector PinnedLocation(0.0f, 0.0f, 0.0f);
		const float DtSeconds = 0.25f;
		const int32 NumTicks = 16; // 16 * 0.25 = 4.0 s > 2.0 s timeout
		for (int32 i = 0; i < NumTicks; ++i)
		{
			Rover->SetActorLocation(PinnedLocation);
			Rover->Tick(DtSeconds);
			if (Rover->GetWaypointMachine().HasTimedOut())
			{
				break;
			}
		}

		if (!Rover->GetWaypointMachine().HasTimedOut())
		{
			AddError(TEXT("2.6 FAILED: machine did not time out after exceeding the configured timeout."));
			bOk = false;
		}
		if (!TestTrue(TEXT("2.6: ARover reports the timeout (HasReportedTimeout)"),
				Rover->HasReportedTimeout()))
		{
			bOk = false;
		}
		if (!TestEqual(TEXT("2.6: unreached waypoint index is identified"),
				Rover->GetUnreachedWaypointIndex(), ExpectedUnreachedIndex))
		{
			bOk = false;
		}

		// The rover must stop: zero commanded throttle/steer on the timeout tick
		// and on subsequent ticks (the StoppedTimedOut state is latched).
		if (!TestEqual(TEXT("2.6: commanded throttle is zero after timeout"),
				Rover->GetCommandedThrottle(), 0.0f))
		{
			bOk = false;
		}
		if (!TestEqual(TEXT("2.6: commanded steer is zero after timeout"),
				Rover->GetCommandedSteer(), 0.0f))
		{
			bOk = false;
		}

		for (int32 Extra = 0; Extra < 5; ++Extra)
		{
			Rover->SetActorLocation(PinnedLocation);
			Rover->Tick(DtSeconds);
			if (!FMath::IsNearlyZero(Rover->GetCommandedThrottle())
				|| !FMath::IsNearlyZero(Rover->GetCommandedSteer()))
			{
				AddError(FString::Printf(
					TEXT("2.6 FAILED: rover did not stay stopped after timeout (tick %d: throttle %.6f, steer %.6f)."),
					Extra, Rover->GetCommandedThrottle(), Rover->GetCommandedSteer()));
				bOk = false;
				break;
			}
			if (Rover->GetUnreachedWaypointIndex() != ExpectedUnreachedIndex)
			{
				AddError(TEXT("2.6 FAILED: reported unreached waypoint index changed after latching."));
				bOk = false;
				break;
			}
		}

		Rover->Destroy();
	}

	DestroyTransientGameWorld(World, Context);
	return bOk;
}

// ===========================================================================
// 2.2 — Landmark distinguishability over the real config/level.yaml.
//
// Load the project's actual level config with the same parser the level
// builder uses, and assert it carries the design's minimum content: >= 3
// obstacles and >= 3 landmarks, every landmark pairwise distinguishable from
// every other by shape OR color (the design's definition, implemented by
// CountDistinguishableLandmarks), so CountDistinguishableLandmarks() equals the
// landmark count and MeetsMinimumContent() is true. The config path is resolved
// relative to the repo root exactly as ARoverLevelBuilder::ResolveConfigPath
// does (two levels up from the uproject's directory).
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxRoverLandmarkDistinguishabilityTest,
	"SensorSimSandbox.Rover.LandmarkDistinguishability",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxRoverLandmarkDistinguishabilityTest::RunTest(const FString& Parameters)
{
	// Resolve <repo>/config/level.yaml the same way the level builder does:
	// the uproject lives at <repo>/unreal/SensorSimSandbox/, so the repo root
	// is two levels up from the project directory.
	const FString ProjectDir = FPaths::ProjectDir();
	const FString RepoRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(ProjectDir, TEXT("../../")));
	const FString ConfigPath = FPaths::Combine(RepoRoot, TEXT("config"), TEXT("level.yaml"));

	FRoverLevelConfig Config;
	FString Error;
	if (!TestTrue(
			FString::Printf(TEXT("2.2: config/level.yaml loads and parses (path: %s; error: %s)"),
				*ConfigPath, *Error),
			FRoverLevelConfig::LoadFromFile(ConfigPath, Config, Error)))
	{
		return false;
	}

	bool bOk = true;

	// >= 3 obstacles.
	if (!TestTrue(
			FString::Printf(TEXT("2.2: level has >= 3 obstacles (found %d)"), Config.Obstacles.Num()),
			Config.Obstacles.Num() >= 3))
	{
		bOk = false;
	}

	// >= 3 landmarks.
	if (!TestTrue(
			FString::Printf(TEXT("2.2: level has >= 3 landmarks (found %d)"), Config.Landmarks.Num()),
			Config.Landmarks.Num() >= 3))
	{
		bOk = false;
	}

	// >= 3 distinguishable landmarks (by shape or color — the design's
	// definition). CountDistinguishableLandmarks returns the number of landmarks
	// that are unique under (shape, quantized color).
	const int32 Distinguishable = Config.CountDistinguishableLandmarks();
	if (!TestTrue(
			FString::Printf(TEXT("2.2: level has >= 3 distinguishable landmarks (found %d distinct)"),
				Distinguishable),
			Distinguishable >= 3))
	{
		bOk = false;
	}

	// Every landmark distinguishable from every other => the distinct count
	// equals the total landmark count (no two landmarks collide on shape+color).
	if (!TestEqual(
			TEXT("2.2: all landmarks are pairwise distinguishable (distinct count == landmark count)"),
			Distinguishable, Config.Landmarks.Num()))
	{
		bOk = false;
	}

	// The single roll-up contract the level builder relies on.
	FString Reason;
	if (!TestTrue(
			FString::Printf(TEXT("2.2: MeetsMinimumContent is satisfied (%s)"), *Reason),
			Config.MeetsMinimumContent(Reason)))
	{
		AddError(FString::Printf(TEXT("2.2 FAILED: MeetsMinimumContent reason: %s"), *Reason));
		bOk = false;
	}

	return bOk;
}
