// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Rover/WaypointStateMachine.h"

#include <cmath>

double FWaypointStateMachine::PlanarDistance(double Ax, double Ay, double Bx, double By)
{
	const double Dx = Ax - Bx;
	const double Dy = Ay - By;
	return std::sqrt(Dx * Dx + Dy * Dy);
}

void FWaypointStateMachine::Configure(
	const TArray<TPair<double, double>>& InWaypointsXY,
	double InArrivalToleranceM,
	double InWaypointTimeoutS)
{
	WaypointsXY = InWaypointsXY;
	ArrivalToleranceM = FMath::Max(0.0, InArrivalToleranceM);
	WaypointTimeoutS = InWaypointTimeoutS; // <= 0 disables timeout; kept as-is

	CurrentIndex = 0;
	TimeOnCurrentWaypoint = 0.0;
	bHoldingAtFinal = false;
	bStoppedTimedOut = false;
	UnreachedIndex = INDEX_NONE;
}

void FWaypointStateMachine::Reset()
{
	WaypointsXY.Reset();
	ArrivalToleranceM = 0.0;
	WaypointTimeoutS = 0.0;
	CurrentIndex = 0;
	TimeOnCurrentWaypoint = 0.0;
	bHoldingAtFinal = false;
	bStoppedTimedOut = false;
	UnreachedIndex = INDEX_NONE;
}

bool FWaypointStateMachine::IsActive() const
{
	return WaypointsXY.Num() > 0 && !bHoldingAtFinal && !bStoppedTimedOut;
}

FWaypointStep FWaypointStateMachine::MakeIdleStep() const
{
	FWaypointStep Step;
	Step.Action = EWaypointAction::Idle;
	Step.CurrentIndex = CurrentIndex;
	return Step;
}

FWaypointStep FWaypointStateMachine::Update(double RoverX, double RoverY, double DeltaSeconds)
{
	FWaypointStep Step;
	Step.CurrentIndex = CurrentIndex;

	// 1. No route configured -> idle, no state change.
	if (WaypointsXY.Num() == 0)
	{
		return MakeIdleStep();
	}

	// Terminal states are latched: once holding or stopped we keep reporting
	// the same action with no further progress (holds with zero throttle/steer
	// at the final waypoint — 2.5; stays stopped after a timeout — 2.6).
	if (bHoldingAtFinal)
	{
		const TPair<double, double>& Final = WaypointsXY.Last();
		Step.Action = EWaypointAction::HoldAtFinal;
		Step.CurrentIndex = WaypointsXY.Num() - 1;
		Step.TargetX = Final.Key;
		Step.TargetY = Final.Value;
		Step.DistanceToTarget = PlanarDistance(RoverX, RoverY, Final.Key, Final.Value);
		return Step;
	}
	if (bStoppedTimedOut)
	{
		Step.Action = EWaypointAction::StoppedTimedOut;
		Step.CurrentIndex = CurrentIndex;
		const TPair<double, double>& Target = WaypointsXY[CurrentIndex];
		Step.TargetX = Target.Key;
		Step.TargetY = Target.Value;
		Step.DistanceToTarget = PlanarDistance(RoverX, RoverY, Target.Key, Target.Value);
		return Step;
	}

	// 2. Accumulate time against the current waypoint. Negative dt is ignored.
	TimeOnCurrentWaypoint += FMath::Max(0.0, DeltaSeconds);

	const TPair<double, double>& Target = WaypointsXY[CurrentIndex];
	const double Distance = PlanarDistance(RoverX, RoverY, Target.Key, Target.Value);

	Step.CurrentIndex = CurrentIndex;
	Step.TargetX = Target.Key;
	Step.TargetY = Target.Value;
	Step.DistanceToTarget = Distance;

	// 3. Arrival is checked BEFORE timeout: a waypoint reached exactly as its
	//    timeout elapses still counts as reached.
	if (Distance <= ArrivalToleranceM)
	{
		const bool bIsFinal = (CurrentIndex == WaypointsXY.Num() - 1);
		if (bIsFinal)
		{
			// Final waypoint reached: hold with zero throttle/steer (2.5).
			bHoldingAtFinal = true;
			Step.Action = EWaypointAction::HoldAtFinal;
			Step.bReachedFinalThisStep = true;
			return Step;
		}

		// Advance to the next waypoint in configured order (2.4).
		++CurrentIndex;
		TimeOnCurrentWaypoint = 0.0;

		const TPair<double, double>& Next = WaypointsXY[CurrentIndex];
		Step.Action = EWaypointAction::DriveToTarget;
		Step.CurrentIndex = CurrentIndex;
		Step.TargetX = Next.Key;
		Step.TargetY = Next.Value;
		Step.DistanceToTarget = PlanarDistance(RoverX, RoverY, Next.Key, Next.Value);
		Step.bAdvancedThisStep = true;
		return Step;
	}

	// 4. Not arrived. If a timeout is configured and it elapsed, stop and record
	//    the unreached waypoint (2.6).
	if (WaypointTimeoutS > 0.0 && TimeOnCurrentWaypoint >= WaypointTimeoutS)
	{
		bStoppedTimedOut = true;
		UnreachedIndex = CurrentIndex;
		Step.Action = EWaypointAction::StoppedTimedOut;
		Step.bTimedOutThisStep = true;
		return Step;
	}

	// 5. Keep driving toward the current target.
	Step.Action = EWaypointAction::DriveToTarget;
	return Step;
}
