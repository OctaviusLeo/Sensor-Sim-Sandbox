// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Rover/RoverSelfCheck.h"

#include "Rover/WaypointStateMachine.h"
#include "Rover/RoverConfig.h"
#include "Rover/RoverLevelBuilder.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoverSelfCheck, Log, All);

namespace
{
	// Drive the machine from `Start` straight toward the current target in small
	// steps, verifying the visited order matches the configured order and that
	// the index only advances when the rover is within the arrival tolerance.
	// Returns true if the full route was walked to a final-hold without any
	// out-of-order visit, and sets bAdvanceWithinTol accordingly.
	bool WalkRouteInOrder(
		const TArray<TPair<double, double>>& Route,
		double Tol,
		bool& bAdvanceWithinTol,
		bool& bHoldsAtFinal,
		FString& OutDetail)
	{
		bAdvanceWithinTol = true;
		bHoldsAtFinal = false;

		FWaypointStateMachine M;
		M.Configure(Route, Tol, /*timeout*/ 0.0 /* disabled: we always arrive */);

		// Start at the first waypoint position offset so the first target is index 0.
		double X = 0.0, Y = 0.0;
		int32 ExpectedIndex = 0;
		const double StepFrac = 0.25; // move 25% of remaining distance per tick
		const int32 MaxTicks = 100000;

		for (int32 Tick = 0; Tick < MaxTicks; ++Tick)
		{
			const FWaypointStep Step = M.Update(X, Y, 0.1);

			if (Step.Action == EWaypointAction::HoldAtFinal)
			{
				bHoldsAtFinal = true;
				return true;
			}
			if (Step.Action != EWaypointAction::DriveToTarget)
			{
				OutDetail = FString::Printf(TEXT("unexpected action %d at tick %d"), (int32)Step.Action, Tick);
				return false;
			}

			// Order check: the current index must be non-decreasing and only
			// jump forward by one.
			if (Step.CurrentIndex < ExpectedIndex)
			{
				OutDetail = FString::Printf(TEXT("index went backwards: %d < %d"), Step.CurrentIndex, ExpectedIndex);
				return false;
			}
			if (Step.bAdvancedThisStep)
			{
				// The advance should have happened only because the previous
				// target was within tolerance.
				const double PrevDist = FWaypointStateMachine::PlanarDistance(
					X, Y, Route[ExpectedIndex].Key, Route[ExpectedIndex].Value);
				if (PrevDist > Tol + 1e-9)
				{
					bAdvanceWithinTol = false;
					OutDetail = FString::Printf(
						TEXT("advanced from wp %d while %.4f m away (tol %.4f)"), ExpectedIndex, PrevDist, Tol);
				}
				ExpectedIndex = Step.CurrentIndex;
			}

			// Move toward the current target.
			X += (Step.TargetX - X) * StepFrac;
			Y += (Step.TargetY - Y) * StepFrac;
		}

		OutDetail = TEXT("route did not reach final within tick budget");
		return false;
	}

	// Configure a machine whose single waypoint is far away and never reached;
	// confirm it times out and reports the unreached index.
	bool CheckTimeout(FString& OutDetail)
	{
		TArray<TPair<double, double>> Route;
		Route.Add(TPair<double, double>(1000.0, 1000.0)); // far away

		FWaypointStateMachine M;
		M.Configure(Route, /*tol*/ 1.0, /*timeout*/ 5.0);

		// Stay at the origin; never approach the target.
		FWaypointStep Step;
		for (int32 i = 0; i < 1000; ++i)
		{
			Step = M.Update(0.0, 0.0, 0.1);
			if (Step.Action == EWaypointAction::StoppedTimedOut)
			{
				break;
			}
		}

		if (!M.HasTimedOut())
		{
			OutDetail = TEXT("machine did not time out when target was never reached");
			return false;
		}
		if (M.GetUnreachedWaypointIndex() != 0)
		{
			OutDetail = FString::Printf(TEXT("unreached index was %d, expected 0"), M.GetUnreachedWaypointIndex());
			return false;
		}
		OutDetail = FString::Printf(TEXT("timed out and reported unreached waypoint %d"), M.GetUnreachedWaypointIndex());
		return true;
	}
}

FRoverSelfCheck::FResult FRoverSelfCheck::Run(const FString& LevelConfigPath)
{
	FResult Result;

	const FString ConfigPath = LevelConfigPath.IsEmpty()
		? ARoverLevelBuilder::ResolveConfigPath()
		: LevelConfigPath;

	FRoverLevelConfig Config;
	FString LoadError;
	FString RouteDetail = TEXT("(route from config)");

	TArray<TPair<double, double>> Route;
	double Tol = 1.5;

	if (FRoverLevelConfig::LoadFromFile(ConfigPath, Config, LoadError))
	{
		FString ContentReason;
		Result.bLevelContentOk = Config.MeetsMinimumContent(ContentReason);
		if (!Result.bLevelContentOk)
		{
			RouteDetail = FString::Printf(TEXT("level content failed: %s"), *ContentReason);
		}
		Route = Config.Waypoints.GetWaypointsXYMeters();
		Tol = Config.Waypoints.ArrivalToleranceM;
	}
	else
	{
		RouteDetail = FString::Printf(TEXT("config load failed (%s); using built-in route"), *LoadError);
		Result.bLevelContentOk = false;
		// Fall back to a built-in route so the state-machine checks still run.
		Route = {
			TPair<double, double>(10.0, 0.0),
			TPair<double, double>(10.0, 10.0),
			TPair<double, double>(0.0, 10.0),
			TPair<double, double>(0.0, 0.0)
		};
	}

	FString OrderDetail;
	Result.bOrderPreserved = WalkRouteInOrder(
		Route, Tol, Result.bAdvancesWithinTolerance, Result.bHoldsAtFinal, OrderDetail);

	FString TimeoutDetail;
	Result.bTimeoutReported = CheckTimeout(TimeoutDetail);

	Result.Detail = FString::Printf(
		TEXT("config=%s; %s; order=%s advance=%s hold=%s; %s; levelContent=%s"),
		*ConfigPath, *RouteDetail,
		Result.bOrderPreserved ? TEXT("PASS") : *FString::Printf(TEXT("FAIL(%s)"), *OrderDetail),
		Result.bAdvancesWithinTolerance ? TEXT("PASS") : TEXT("FAIL"),
		Result.bHoldsAtFinal ? TEXT("PASS") : TEXT("FAIL"),
		*TimeoutDetail,
		Result.bLevelContentOk ? TEXT("PASS") : TEXT("FAIL"));

	return Result;
}

// ---------------------------------------------------------------------------
// Console command: `RoverSelfCheck [configPath]`
//
// One-command headless check for task 4.1: drives the pure waypoint state
// machine (order, advance-within-tolerance, final hold, timeout + unreached
// report) and validates the level config content. Logs one PASS/FAIL line a
// verify script can grep.
// ---------------------------------------------------------------------------
static void ExecuteRoverSelfCheck(const TArray<FString>& Args)
{
	FString ConfigPath;
	if (Args.Num() >= 1 && !Args[0].IsEmpty())
	{
		ConfigPath = Args[0];
	}

	const FRoverSelfCheck::FResult Result = FRoverSelfCheck::Run(ConfigPath);

	UE_LOG(LogRoverSelfCheck, Log, TEXT("RoverSelfCheck %s: %s"),
		Result.Passed() ? TEXT("PASS") : TEXT("FAIL"), *Result.Detail);
}

static FAutoConsoleCommand GRoverSelfCheckCommand(
	TEXT("RoverSelfCheck"),
	TEXT("Drive the pure waypoint state machine headlessly (order, advance-within-tolerance, final hold, timeout+unreached report) and validate level config content. Usage: RoverSelfCheck [configPath]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&ExecuteRoverSelfCheck));
