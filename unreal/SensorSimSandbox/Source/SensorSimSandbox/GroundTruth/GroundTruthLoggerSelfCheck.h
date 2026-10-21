// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * Deterministic headless demo/verify for the GroundTruthLogger (task 7.1
 * demo/verify, Requirement 5.1-5.5).
 *
 * Exposed as the console command `GroundTruthLoggerSelfCheck` so a one-command
 * headless run (UnrealEditor-Cmd -ExecCmds or a short `-game` launch) writes a
 * real `.jsonl` Run_Log under runs/<run_id>/ (gitignored) and verifies it. The
 * check:
 *   - loads config/lidar.yaml (the Gate A values) and validates it,
 *   - resolves the full commit SHA the way the design's provenance does
 *     (git rev-parse HEAD),
 *   - BeginRun writes the metadata header carrying the seed, commit, full
 *     sensor config, and (optionally) the camera sampling interval,
 *   - builds a small enclosing scene, performs real raycast lidar scans, and
 *     LogSample-appends one record per scan with the rover pose (REP 103),
 *     per-ray truth/published arrays, t_capture, t_publish and dropped,
 *   - EndRun flushes, then re-reads the file and asserts: the first line is a
 *     run_meta header with the seed/config present; every subsequent line is a
 *     sample whose `t` is non-decreasing in file order; and a write failure is
 *     detected and reported without corrupting already-written data.
 *
 * Logs a single `GroundTruthLoggerSelfCheck PASS`/`FAIL` line a verify script
 * can grep, plus the Run_Log path and a short head of the file.
 */
class FGroundTruthLoggerSelfCheck
{
public:
	struct FResult
	{
		bool bHeaderOk = false;          // first line is run_meta with seed + config
		bool bRecordsWritten = false;    // >= 1 sample record written
		bool bMonotonicT = false;        // `t` non-decreasing across all records
		bool bWriteFailureHandled = false; // injected failure stopped run + retained data
		FString RunLogPath;
		int32 RecordsRead = 0;
		FString Detail;

		bool Passed() const
		{
			return bHeaderOk && bRecordsWritten && bMonotonicT && bWriteFailureHandled;
		}
	};

	/**
	 * Run the check in `World`. If World is null the raycast portion is skipped
	 * and the logger is driven with synthetic truth so the format/monotonic/
	 * write-failure assertions still run. `LidarConfigPath` empty resolves
	 * config/lidar.yaml. `RunDir` empty resolves runs/<timestamp>/.
	 */
	static FResult Run(UWorld* World, const FString& LidarConfigPath, const FString& RunDir);

	/** Resolve the full commit SHA (git rev-parse HEAD), empty on failure. */
	static FString ResolveCommitSha();

	/** Resolve the repo-root runs/ directory (runs under <repo>/runs/). */
	static FString ResolveRunsRoot();
};
