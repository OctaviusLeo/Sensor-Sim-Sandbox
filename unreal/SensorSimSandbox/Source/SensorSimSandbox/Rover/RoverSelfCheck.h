// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"

/**
 * Deterministic headless self-check for the pure waypoint state machine
 * (task 4.1 demo/verify, Requirement 2.3-2.6).
 *
 * It drives FWaypointStateMachine with no engine/rendering — exactly as the
 * later automation tests (4.2/4.3) will — and verifies the controller's
 * contract:
 *   - waypoints are visited in configured order,
 *   - the index advances within the arrival tolerance,
 *   - the final waypoint latches a zero-command hold,
 *   - a waypoint that is never reached times out and reports its index,
 *   - and it also loads config/level.yaml and confirms the level meets the
 *     minimum content (>=3 obstacles, >=3 distinguishable landmarks).
 *
 * Exposed as the console command `RoverSelfCheck` so a one-command headless run
 * (UnrealEditor-Cmd -ExecCmds) can validate the state machine without opening a
 * window.
 */
class FRoverSelfCheck
{
public:
	struct FResult
	{
		bool bOrderPreserved = false;
		bool bAdvancesWithinTolerance = false;
		bool bHoldsAtFinal = false;
		bool bTimeoutReported = false;
		bool bLevelContentOk = false;
		FString Detail;

		bool Passed() const
		{
			return bOrderPreserved && bAdvancesWithinTolerance &&
				bHoldsAtFinal && bTimeoutReported && bLevelContentOk;
		}
	};

	/**
	 * Run the self-check. If LevelConfigPath is empty the repo default
	 * (config/level.yaml) is resolved. Pure except for reading the config file.
	 */
	static FResult Run(const FString& LevelConfigPath);
};
