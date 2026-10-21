// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "Pipeline/SensorRng.h"
#include "Rover/WaypointStateMachine.h"
#include "Rover/Rover.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/WorldSettings.h"

#include <cmath>

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, Property 18: Waypoint order is preserved
//
// *For any* configured waypoint sequence, driving in waypoint mode visits the
// waypoints in the configured order, advancing the index exactly when the
// rover is within the arrival tolerance, and keyboard input is ignored while
// the mode is active.
//
// Validates: Requirements 2.3, 2.4
//
// This drives the PURE waypoint controller (FWaypointStateMachine, task 4.1)
// directly over a seeded generator loop with no world or rendering, exactly as
// the other engine-side property tests (tasks 5.2-5.4) drive the pure pipeline.
// A lightweight kinematic stepper plays the role of the rover: each tick it
// moves toward the state machine's returned Target, so the machine's own
// Update() decides every advance. Over the route we assert:
//
//   - the current index is non-decreasing and advances by EXACTLY 1 per
//     arrival (no skips, no reordering) -> in-order visits (2.4);
//   - an advance happens on a tick IFF the rover is within the arrival
//     tolerance of the target that was current entering that tick -> the index
//     advances exactly when within tolerance (2.4);
//   - the final waypoint latches HoldAtFinal and stays latched (2.5 / order).
//
// The "keyboard ignored in waypoint mode" clause of Property 18 (2.3) is a
// property of the ARover seam, not of the pure machine, so it is asserted at
// that seam: an ARover is spawned in a minimal game world, put in Waypoint
// mode, and Ticked twice with DIFFERENT injected keyboard axis values. In
// Waypoint mode the commanded throttle/steer must be identical across the two
// injected-axis values (the keyboard has no effect) and must equal the command
// derived purely from the state machine. The same injected axis in Manual mode
// DOES change the command, proving the injection path is real and that the
// Waypoint-mode invariance is a genuine ignore, not a dead input path.
//
// Unreal's Automation framework has no native property generator, so a
// lightweight *seeded* generator loop drives >= 100 iterations (design
// "Testing Strategy" / "Property-based testing"). Every generated route is
// reproducible from a fixed base seed, so a failing iteration is a bug to
// investigate at a known case, NEVER a reason to lower the iteration count or
// weaken an assertion.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxWaypointOrderPreservedTest,
	"SensorSimSandbox.Rover.Property18_WaypointOrderPreserved",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
{
	// Number of generated routes. Property-based tests in this project run
	// >= 100 iterations (design "Testing Strategy"). Do not lower this to make
	// the test pass; a failing case is a real ordering/advance bug.
	constexpr int32 NumIterations = 128;

	// Fixed base seed for the generator loop so the generated routes are
	// themselves reproducible across runs and machines.
	constexpr uint64 GeneratorBaseSeed = 0xC3D7A95E1F284B60ull;

	// One generated route: ordered waypoints (meters), arrival tolerance (m),
	// and the per-step travel speed the kinematic stepper uses (m / step).
	struct FGeneratedRoute
	{
		TArray<TPair<double, double>> Waypoints;
		double ToleranceM = 0.0;
		double StepDistanceM = 0.0; // how far the stepper closes per tick
		double DeltaSeconds = 0.0;  // dt handed to Update() each tick
	};

	// Generate route `Index` deterministically from the base seed. A dedicated
	// FSensorRng per iteration keeps the generated routes reproducible and
	// independent of any other RNG stream in the project.
	FGeneratedRoute GenerateRoute(int32 Index)
	{
		FGeneratedRoute Route;

		FSensorRng Gen(
			static_cast<uint64>(GeneratorBaseSeed),
			FString::Printf(TEXT("route|%d"), Index));

		// Count in [2, 9] so there is always at least one advance plus a final
		// hold, and the ordering claim is exercised over multi-leg routes.
		const int32 Count = 2 + static_cast<int32>(Gen.NextUInt64() % 8);

		// Arrival tolerance in [0.05, 2.5] m, varied per route.
		Route.ToleranceM = 0.05 + Gen.NextUniform() * 2.45;

		// Positions spread over a +/-50 m square, including occasional near
		// duplicates so consecutive waypoints can sit within a tolerance of
		// each other (a legitimate route the machine must still order).
		Route.Waypoints.Reserve(Count);
		double PrevX = (Gen.NextUniform() * 2.0 - 1.0) * 50.0;
		double PrevY = (Gen.NextUniform() * 2.0 - 1.0) * 50.0;
		Route.Waypoints.Add(TPair<double, double>(PrevX, PrevY));
		for (int32 i = 1; i < Count; ++i)
		{
			double X, Y;
			if (Gen.NextUInt64() % 7 == 0)
			{
				// Near-duplicate of the previous waypoint (small offset).
				X = PrevX + (Gen.NextUniform() * 2.0 - 1.0) * Route.ToleranceM;
				Y = PrevY + (Gen.NextUniform() * 2.0 - 1.0) * Route.ToleranceM;
			}
			else
			{
				X = (Gen.NextUniform() * 2.0 - 1.0) * 50.0;
				Y = (Gen.NextUniform() * 2.0 - 1.0) * 50.0;
			}
			Route.Waypoints.Add(TPair<double, double>(X, Y));
			PrevX = X;
			PrevY = Y;
		}

		// Step distance a fraction of the tolerance so the stepper cannot jump
		// over a waypoint's arrival band in a single tick: it always lands
		// inside the band on some tick, letting the machine detect arrival on
		// the tick it is within tolerance (and not before).
		Route.StepDistanceM = Route.ToleranceM * (0.2 + Gen.NextUniform() * 0.3); // [0.2, 0.5] * tol
		Route.StepDistanceM = FMath::Max(Route.StepDistanceM, 1e-3);

		// A plausible dt in [1/120, 1/30] s. The pure machine's advance logic is
		// independent of dt (no timeout here), but using a realistic dt keeps the
		// drive loop representative.
		Route.DeltaSeconds = 1.0 / (30.0 + Gen.NextUniform() * 90.0);

		return Route;
	}

	// Advance the simulated rover one step toward (TargetX, TargetY) by at most
	// StepDistanceM, writing the new position back into (X, Y). Moving toward
	// the machine's returned target (rather than teleporting onto it) means the
	// machine itself decides, on its own Update(), the tick on which the rover
	// first enters the arrival band.
	void StepToward(double& X, double& Y, double TargetX, double TargetY, double StepDistanceM)
	{
		const double Dx = TargetX - X;
		const double Dy = TargetY - Y;
		const double Dist = std::sqrt(Dx * Dx + Dy * Dy);
		if (Dist <= StepDistanceM || Dist == 0.0)
		{
			X = TargetX;
			Y = TargetY;
			return;
		}
		const double Scale = StepDistanceM / Dist;
		X += Dx * Scale;
		Y += Dy * Scale;
	}
}

bool FSensorSimSandboxWaypointOrderPreservedTest::RunTest(const FString& Parameters)
{
	int32 RoutesChecked = 0;
	int32 TotalAdvancesChecked = 0;

	// -----------------------------------------------------------------------
	// Part A: ordering + advance-within-tolerance + final-hold, over the pure
	// state machine (2.4), >= 100 seeded routes.
	// -----------------------------------------------------------------------
	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		const FGeneratedRoute Route = GenerateRoute(Iteration);
		const int32 NumWaypoints = Route.Waypoints.Num();

		FWaypointStateMachine Machine;
		// No timeout (<= 0 disables it) so this route always completes; the
		// timeout path is covered by the task 4.3 example tests.
		Machine.Configure(Route.Waypoints, Route.ToleranceM, /*TimeoutS*/ 0.0);

		// Start the rover away from the first waypoint so the first leg is a
		// real drive (offset along +X by 10 tolerances, min 1 m).
		double RoverX = Route.Waypoints[0].Key + FMath::Max(1.0, Route.ToleranceM * 10.0);
		double RoverY = Route.Waypoints[0].Value;

		int32 PrevIndex = Machine.GetCurrentIndex();
		int32 ExpectedNextVisit = 0; // the waypoint index we expect to reach next, in order
		bool bReachedFinal = false;

		// Bound the loop generously: each leg takes about (leg length / step)
		// ticks; cap well above any route this generator makes so a stuck
		// machine fails loudly instead of hanging.
		const int32 MaxTicks = 200000;
		int32 Tick = 0;
		for (; Tick < MaxTicks; ++Tick)
		{
			// The index that is current ENTERING this tick, and the rover's
			// distance to that target before Update() runs. This is the
			// distance the advance decision for this tick is made against.
			const int32 IndexEntering = Machine.GetCurrentIndex();
			const TPair<double, double>& TargetEntering = Route.Waypoints[IndexEntering];
			const double DistEntering = FWaypointStateMachine::PlanarDistance(
				RoverX, RoverY, TargetEntering.Key, TargetEntering.Value);

			const FWaypointStep Step = Machine.Update(RoverX, RoverY, Route.DeltaSeconds);

			// (1) Index is non-decreasing and never jumps by more than 1.
			if (Step.CurrentIndex < PrevIndex)
			{
				AddError(FString::Printf(
					TEXT("Property 18 FAILED (iter %d, tick %d): waypoint index went BACKWARD ")
					TEXT("%d -> %d. Waypoints must be visited in configured order."),
					Iteration, Tick, PrevIndex, Step.CurrentIndex));
				return false;
			}
			if (Step.CurrentIndex - PrevIndex > 1)
			{
				AddError(FString::Printf(
					TEXT("Property 18 FAILED (iter %d, tick %d): waypoint index SKIPPED ")
					TEXT("%d -> %d. The index must advance by exactly 1 per arrival."),
					Iteration, Tick, PrevIndex, Step.CurrentIndex));
				return false;
			}

			if (Step.bAdvancedThisStep)
			{
				// (2) An advance means the index moved up by exactly 1 ...
				if (Step.CurrentIndex != IndexEntering + 1)
				{
					AddError(FString::Printf(
						TEXT("Property 18 FAILED (iter %d, tick %d): bAdvancedThisStep set but ")
						TEXT("index went %d -> %d (expected +1)."),
						Iteration, Tick, IndexEntering, Step.CurrentIndex));
					return false;
				}
				// ... and it is exactly the next waypoint in order.
				if (IndexEntering != ExpectedNextVisit)
				{
					AddError(FString::Printf(
						TEXT("Property 18 FAILED (iter %d, tick %d): reached waypoint %d out of ")
						TEXT("order (expected to reach %d next)."),
						Iteration, Tick, IndexEntering, ExpectedNextVisit));
					return false;
				}
				// (3) The advance coincides with being within tolerance of the
				// waypoint that was current entering this tick.
				if (DistEntering > Route.ToleranceM)
				{
					AddError(FString::Printf(
						TEXT("Property 18 FAILED (iter %d, tick %d): advanced from waypoint %d ")
						TEXT("while distance %.9g m EXCEEDED tolerance %.9g m. The index must ")
						TEXT("advance only within the arrival tolerance."),
						Iteration, Tick, IndexEntering, DistEntering, Route.ToleranceM));
					return false;
				}
				++ExpectedNextVisit;
				++TotalAdvancesChecked;
			}
			else if (Step.bReachedFinalThisStep)
			{
				// (3) Reaching the final waypoint also requires being within
				// tolerance of it this tick.
				if (DistEntering > Route.ToleranceM)
				{
					AddError(FString::Printf(
						TEXT("Property 18 FAILED (iter %d, tick %d): reached FINAL waypoint %d ")
						TEXT("while distance %.9g m EXCEEDED tolerance %.9g m."),
						Iteration, Tick, IndexEntering, DistEntering, Route.ToleranceM));
					return false;
				}
			}
			else
			{
				// No advance this tick: the rover must NOT yet be within the
				// arrival band of the current (non-final) target, otherwise the
				// machine failed to advance when it should have. (It is fine to
				// be within tolerance only on the exact tick it advances, which
				// is the branch above.) We only assert this while driving.
				if (Step.Action == EWaypointAction::DriveToTarget
					&& DistEntering <= Route.ToleranceM)
				{
					AddError(FString::Printf(
						TEXT("Property 18 FAILED (iter %d, tick %d): within tolerance of waypoint ")
						TEXT("%d (dist %.9g m <= tol %.9g m) but the index did NOT advance."),
						Iteration, Tick, IndexEntering, DistEntering, Route.ToleranceM));
					return false;
				}
			}

			if (Step.Action == EWaypointAction::HoldAtFinal)
			{
				// The final waypoint latched. It must be the LAST configured
				// waypoint, and we must have visited every one in order.
				if (Step.CurrentIndex != NumWaypoints - 1)
				{
					AddError(FString::Printf(
						TEXT("Property 18 FAILED (iter %d, tick %d): HoldAtFinal latched at index ")
						TEXT("%d, not the last waypoint %d."),
						Iteration, Tick, Step.CurrentIndex, NumWaypoints - 1));
					return false;
				}
				bReachedFinal = true;
				break;
			}

			PrevIndex = Step.CurrentIndex;

			// Drive the simulated rover toward whatever target the machine is
			// now pointing at.
			StepToward(RoverX, RoverY, Step.TargetX, Step.TargetY, Route.StepDistanceM);
		}

		if (!bReachedFinal)
		{
			AddError(FString::Printf(
				TEXT("Property 18 FAILED (iter %d): route of %d waypoints did not reach the final ")
				TEXT("hold within %d ticks (tol=%.4g m, step=%.4g m). Investigate at this seed."),
				Iteration, NumWaypoints, Tick, Route.ToleranceM, Route.StepDistanceM));
			return false;
		}

		// After HoldAtFinal, the hold must latch: further Update()s keep
		// reporting HoldAtFinal at the last index and never advance or regress.
		for (int32 Extra = 0; Extra < 5; ++Extra)
		{
			const FWaypointStep Held = Machine.Update(RoverX, RoverY, Route.DeltaSeconds);
			if (Held.Action != EWaypointAction::HoldAtFinal
				|| Held.CurrentIndex != NumWaypoints - 1
				|| Held.bAdvancedThisStep)
			{
				AddError(FString::Printf(
					TEXT("Property 18 FAILED (iter %d): final hold did not latch (got action=%d, ")
					TEXT("index=%d, advanced=%d)."),
					Iteration, static_cast<int32>(Held.Action), Held.CurrentIndex,
					Held.bAdvancedThisStep ? 1 : 0));
				return false;
			}
		}

		// Every waypoint must have been visited exactly once, in order.
		if (ExpectedNextVisit != NumWaypoints - 1)
		{
			AddError(FString::Printf(
				TEXT("Property 18 FAILED (iter %d): visited %d intermediate waypoints before the ")
				TEXT("final, expected %d. Not all waypoints were visited in order."),
				Iteration, ExpectedNextVisit, NumWaypoints - 1));
			return false;
		}

		++RoutesChecked;
	}

	// Guard against a loop that silently ran too few cases.
	if (!TestTrue(TEXT("waypoint ordering checked >= 100 generated routes"), RoutesChecked >= 100))
	{
		return false;
	}
	// Each route performs (NumWaypoints - 1) >= 1 advances, so >= 100 routes
	// means well over 100 advance decisions were exercised.
	if (!TestTrue(TEXT("waypoint advance decisions checked >= 100"), TotalAdvancesChecked >= 100))
	{
		return false;
	}

	// -----------------------------------------------------------------------
	// Part B: keyboard ignored in Waypoint mode (2.3), asserted at the ARover
	// seam. Spawn an ARover in a minimal game world, drive it with the SAME
	// configured route, inject conflicting keyboard axis values, and confirm
	// the commanded motion in Waypoint mode is invariant to the injected axis
	// (and equals the Manual-mode command only when the mode is actually
	// Manual). This runs headless under -nullrhi (no rendering needed to Tick
	// a pawn's logic).
	// -----------------------------------------------------------------------
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
	if (World == nullptr)
	{
		AddError(TEXT("Property 18 (keyboard-ignore seam): failed to create a transient game world."));
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());

	bool bSeamOk = true;

	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ARover* Rover = World->SpawnActor<ARover>(ARover::StaticClass(), FTransform::Identity, SpawnParams);
		if (Rover == nullptr)
		{
			AddError(TEXT("Property 18 (keyboard-ignore seam): failed to spawn ARover."));
			bSeamOk = false;
		}
		else
		{
			// A short route with the rover placed well short of the first
			// waypoint, so Waypoint mode is actively DRIVING (not holding) and
			// a non-zero command is expected from the state machine.
			TArray<TPair<double, double>> Route;
			Route.Add(TPair<double, double>(10.0, 0.0));
			Route.Add(TPair<double, double>(10.0, 10.0));
			const double ToleranceM = 0.5;

			// Two conflicting keyboard inputs: full forward/right vs. full
			// reverse/left. In Waypoint mode neither may influence the command.
			const float AxisA_Throttle = 1.0f, AxisA_Steer = 1.0f;
			const float AxisB_Throttle = -1.0f, AxisB_Steer = -1.0f;

			// --- Waypoint mode: command must be identical for both axis sets. ---
			Rover->SetMode(ERoverMode::Waypoint);

			Rover->ConfigureWaypoints(Route, ToleranceM, /*TimeoutS*/ 0.0);
			Rover->SetActorLocation(FVector::ZeroVector);
			Rover->SetActorRotation(FRotator::ZeroRotator);
			Rover->Test_InjectKeyboardAxis(AxisA_Throttle, AxisA_Steer);
			Rover->Tick(0.016f);
			const float WpThrottleA = Rover->GetCommandedThrottle();
			const float WpSteerA = Rover->GetCommandedSteer();

			// Reset to the identical starting state and drive with the OTHER
			// injected axis. Same pose + same route => same state-machine
			// decision, so any difference in command could only come from the
			// keyboard axis.
			Rover->ConfigureWaypoints(Route, ToleranceM, /*TimeoutS*/ 0.0);
			Rover->SetActorLocation(FVector::ZeroVector);
			Rover->SetActorRotation(FRotator::ZeroRotator);
			Rover->Test_InjectKeyboardAxis(AxisB_Throttle, AxisB_Steer);
			Rover->Tick(0.016f);
			const float WpThrottleB = Rover->GetCommandedThrottle();
			const float WpSteerB = Rover->GetCommandedSteer();

			if (!FMath::IsNearlyEqual(WpThrottleA, WpThrottleB)
				|| !FMath::IsNearlyEqual(WpSteerA, WpSteerB))
			{
				AddError(FString::Printf(
					TEXT("Property 18 FAILED (keyboard-ignore, 2.3): in Waypoint mode the commanded ")
					TEXT("motion changed with the injected keyboard axis (throttle %.6f vs %.6f, ")
					TEXT("steer %.6f vs %.6f). Keyboard input must be ignored in Waypoint mode."),
					WpThrottleA, WpThrottleB, WpSteerA, WpSteerB));
				bSeamOk = false;
			}

			// Positive control: the SAME injection path DOES move the rover in
			// Manual mode, proving the injection is real and the Waypoint-mode
			// invariance above is a genuine ignore, not a dead input path.
			Rover->SetMode(ERoverMode::Manual);
			Rover->SetActorLocation(FVector::ZeroVector);
			Rover->SetActorRotation(FRotator::ZeroRotator);
			Rover->Test_InjectKeyboardAxis(AxisA_Throttle, AxisA_Steer);
			Rover->Tick(0.016f);
			const float ManThrottleA = Rover->GetCommandedThrottle();
			const float ManSteerA = Rover->GetCommandedSteer();

			Rover->SetMode(ERoverMode::Manual);
			Rover->SetActorLocation(FVector::ZeroVector);
			Rover->SetActorRotation(FRotator::ZeroRotator);
			Rover->Test_InjectKeyboardAxis(AxisB_Throttle, AxisB_Steer);
			Rover->Tick(0.016f);
			const float ManThrottleB = Rover->GetCommandedThrottle();
			const float ManSteerB = Rover->GetCommandedSteer();

			// In Manual mode the two opposite axis injections must produce
			// different commands (otherwise the injection path is not real and
			// the Waypoint-mode check above would be vacuous).
			if (FMath::IsNearlyEqual(ManThrottleA, ManThrottleB)
				&& FMath::IsNearlyEqual(ManSteerA, ManSteerB))
			{
				AddError(FString::Printf(
					TEXT("Property 18 FAILED (keyboard-ignore control): in Manual mode the injected ")
					TEXT("keyboard axis did NOT change the command (throttle %.6f vs %.6f, steer ")
					TEXT("%.6f vs %.6f). The injection path must be real for the Waypoint-mode ")
					TEXT("ignore check to be meaningful."),
					ManThrottleA, ManThrottleB, ManSteerA, ManSteerB));
				bSeamOk = false;
			}

			Rover->Destroy();
		}
	}

	// Tear down the transient world so the test leaves no artifacts.
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);

	if (!bSeamOk)
	{
		return false;
	}

	return true;
}
