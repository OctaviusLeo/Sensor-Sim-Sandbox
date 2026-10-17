// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"

/**
 * The pure, engine-independent waypoint controller state machine (design:
 * "Rover + Waypoint Controller", Requirement 2.3-2.6).
 *
 * This class holds ALL of the waypoint decision logic that the task title
 * calls out as "isolated from rendering for unit testing": which waypoint is
 * current, whether it has been reached within the arrival tolerance, whether
 * it has timed out, when to advance the index, holding at the final waypoint,
 * and reporting an unreached waypoint. It has no AActor / UWorld / Engine
 * dependency whatsoever, so the seeded-generator-loop automation tests in task
 * 4.2 and the example tests in task 4.3 can drive it directly and headlessly.
 *
 * Coordinates: the state machine works in a planar (X, Y) frame expressed in
 * METERS. Distances (arrival tolerance) are in meters and the timeout is in
 * seconds. The pawn is responsible for converting Unreal world centimeters to
 * these meters before calling Update(); keeping the machine unit-agnostic to
 * the engine's cm scale makes the arrival/timeout maths trivial to verify.
 *
 * Usage from ARover (Waypoint mode):
 *   - Configure() once with the route, arrival tolerance, and per-waypoint
 *     timeout.
 *   - Each tick, call Update(roverX, roverY, dt). The returned command tells
 *     the pawn whether to drive toward the current target, hold (final reached),
 *     or stop (timed out). The pawn applies throttle/steer accordingly and
 *     ignores keyboard input while in Waypoint mode.
 */

/** What the pawn should do this tick, decided purely from the route + pose. */
enum class EWaypointAction : uint8
{
	/** No waypoints configured, or the machine is idle: do nothing. */
	Idle,
	/** Drive toward the current target waypoint. */
	DriveToTarget,
	/** Final waypoint reached: hold position with zero throttle/steer (2.5). */
	HoldAtFinal,
	/** Current waypoint not reached within its timeout: stop (2.6). */
	StoppedTimedOut
};

/** Result of one Update() step. */
struct FWaypointStep
{
	/** The action the pawn must take this tick. */
	EWaypointAction Action = EWaypointAction::Idle;

	/** Index of the waypoint currently targeted (or the one that timed out). */
	int32 CurrentIndex = 0;

	/** Target waypoint position in meters (valid when driving/holding). */
	double TargetX = 0.0;
	double TargetY = 0.0;

	/** Planar distance (m) from the rover to the current target this tick. */
	double DistanceToTarget = 0.0;

	/** True on the single tick where the index advanced to the next waypoint. */
	bool bAdvancedThisStep = false;

	/** True on the single tick where the final waypoint was first reached. */
	bool bReachedFinalThisStep = false;

	/** True on the single tick where a timeout was first detected. */
	bool bTimedOutThisStep = false;
};

class FWaypointStateMachine
{
public:
	FWaypointStateMachine() = default;

	/**
	 * Configure the route and tolerances. Resets all progress.
	 * @param InWaypointsXY        ordered waypoints as (x, y) pairs in meters
	 * @param InArrivalToleranceM  arrival radius in meters (clamped to >= 0)
	 * @param InWaypointTimeoutS   per-waypoint timeout in seconds; <= 0 disables
	 *                             the timeout (never times out)
	 */
	void Configure(
		const TArray<TPair<double, double>>& InWaypointsXY,
		double InArrivalToleranceM,
		double InWaypointTimeoutS);

	/** Clear the route and reset to idle. */
	void Reset();

	/** True once configured with at least one waypoint and not finished/stopped. */
	bool IsActive() const;

	/** True when the final waypoint has been reached and we are holding. */
	bool HasReachedFinal() const { return bHoldingAtFinal; }

	/** True when the current waypoint timed out and the machine stopped. */
	bool HasTimedOut() const { return bStoppedTimedOut; }

	/** Current target index (clamped to the last index once finished). */
	int32 GetCurrentIndex() const { return CurrentIndex; }

	/** Number of configured waypoints. */
	int32 Num() const { return WaypointsXY.Num(); }

	/** Time accumulated against the current waypoint's timeout (seconds). */
	double GetTimeOnCurrent() const { return TimeOnCurrentWaypoint; }

	/**
	 * Index of the waypoint that was not reached, valid only after a timeout
	 * (HasTimedOut() == true); otherwise INDEX_NONE (Requirement 2.6).
	 */
	int32 GetUnreachedWaypointIndex() const { return UnreachedIndex; }

	/**
	 * Advance the machine one step given the rover's current planar position
	 * (meters) and the elapsed time since the last call (seconds).
	 *
	 * Pure decision logic, in this fixed order each tick:
	 *   1. If idle/finished/stopped, return the terminal action (no state change).
	 *   2. Accumulate elapsed time against the current waypoint.
	 *   3. If within arrival tolerance: either advance to the next waypoint, or,
	 *      if this was the final waypoint, latch "holding at final".
	 *   4. Otherwise, if the per-waypoint timeout elapsed before arrival, latch
	 *      "stopped, timed out" and record the unreached waypoint index.
	 *   5. Otherwise, keep driving toward the current target.
	 *
	 * Arrival is checked BEFORE timeout so a waypoint reached exactly as its
	 * timeout elapses still counts as reached.
	 */
	FWaypointStep Update(double RoverX, double RoverY, double DeltaSeconds);

	/** Planar Euclidean distance (meters) between two points. */
	static double PlanarDistance(double Ax, double Ay, double Bx, double By);

private:
	TArray<TPair<double, double>> WaypointsXY;
	double ArrivalToleranceM = 0.0;
	double WaypointTimeoutS = 0.0; // <= 0 means "no timeout"

	int32 CurrentIndex = 0;
	double TimeOnCurrentWaypoint = 0.0;

	bool bHoldingAtFinal = false;
	bool bStoppedTimedOut = false;
	int32 UnreachedIndex = INDEX_NONE;

	FWaypointStep MakeIdleStep() const;
};
