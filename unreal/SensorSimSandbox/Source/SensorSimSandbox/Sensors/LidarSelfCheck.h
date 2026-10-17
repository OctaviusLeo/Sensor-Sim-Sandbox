// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * Deterministic headless demo/verify for the Lidar_Model (task 6.1 demo/verify,
 * Requirement 3.1, 3.5, 3.6, 3.7).
 *
 * Exposed as the console command `LidarSelfCheck` so a one-command headless run
 * (UnrealEditor-Cmd -ExecCmds) can perform real raycast scans against a world
 * and log >= 1000 returns, each with its per-ray Ground_Truth and published
 * value. The check:
 *   - loads config/lidar.yaml (the Gate A values) and validates it through the
 *     shared bound set,
 *   - spawns a small enclosing scene (floor + walls + obstacles) with collision
 *     around the lidar so rays reliably hit, so the demo does not depend on a
 *     particular level being loaded,
 *   - performs scans, capturing per-ray truth BEFORE the pipeline, until it has
 *     logged >= 1000 returns,
 *   - confirms that no-hit rays are null and excluded from the noise and dropout
 *     statistics, and that published ranges are NOT clamped (noisy ranges may
 *     exceed max_range_m or go below 0),
 *   - also runs a pure, world-free check (ProcessTruthRanges) with injected
 *     no-hit rays to prove the exclusion rule directly, and an out-of-bound
 *     config rejection to prove the shared validation contract is wired in.
 *
 * Logs a single `LidarSelfCheck PASS`/`FAIL` line a verify script can grep,
 * plus the first N per-ray records and the aggregate return count.
 */
class FLidarSelfCheck
{
public:
	struct FResult
	{
		bool bConfigValid = false;
		bool bOutOfBoundRejected = false;
		bool bEnoughReturns = false;
		bool bNoHitExcluded = false;
		bool bNoClamping = false;
		int32 TotalReturnsLogged = 0;
		int32 TotalHits = 0;
		int32 TotalDropped = 0;
		int32 TotalNoHit = 0;
		FString Detail;

		bool Passed() const
		{
			return bConfigValid && bOutOfBoundRejected && bEnoughReturns &&
				bNoHitExcluded && bNoClamping;
		}
	};

	/**
	 * Run the check in `World`. If World is null the raycast portion is skipped
	 * and only the pure checks run (and the result reports the missing world).
	 * `LidarConfigPath` empty resolves config/lidar.yaml.
	 * `MinReturns` is the number of logged returns to require (default 1000).
	 */
	static FResult Run(UWorld* World, const FString& LidarConfigPath, int32 MinReturns = 1000);
};
