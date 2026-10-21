// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"

/**
 * Deterministic headless self-check for the shared SensorModel pipeline
 * (task 5.1 demo/verify, Requirement 0.3).
 *
 * Runs the pure pipeline (seeded noise -> per-ray dropout -> latency release)
 * twice with the same seed and configuration over a fixed synthetic scan, on a
 * synthetic clock, and reports whether the two runs produce byte-identical
 * output (excluding timestamps and paths — this check compares only the
 * published values and dropped flags, which carry no timestamps). It also
 * confirms the config-bounds validation contract rejects an out-of-bound value
 * while retaining the last valid configuration.
 *
 * Exposed as the console command `PipelineSelfCheck` so a one-command headless
 * run (via UnrealEditor-Cmd -ExecCmds) can verify determinism. The later
 * property tests (tasks 5.2-5.4) build on the same pure logic exercised here.
 */
class FPipelineSelfCheck
{
public:
	struct FResult
	{
		bool bDeterministic = false;
		bool bValidationContractHolds = false;
		FString Detail;

		bool Passed() const { return bDeterministic && bValidationContractHolds; }
	};

	/**
	 * Run the self-check with the given run seed. Pure: no engine state is
	 * touched, so it is safe to call from a commandlet, a console command, or a
	 * unit test.
	 */
	static FResult Run(int64 RunSeed);
};
