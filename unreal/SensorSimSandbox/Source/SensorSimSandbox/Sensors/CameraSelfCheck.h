// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"

/**
 * Deterministic HEADLESS demo/verify for the Camera_Model pipeline (task 13.1
 * demo/verify, Requirement 4.1, 4.2, 4.5, 4.6).
 *
 * The engine scene-capture + GPU readback lives in ACameraModel; this check
 * exercises the PURE pipeline (FCameraPipelineRunner) on SYNTHETIC frames, so
 * it runs with no world and no RHI (safe under -nullrhi / CI). It is the hard
 * part the task requires to pass headlessly: it proves the per-pixel noise,
 * whole-frame dropout, latency and config-bounds behaviour directly, decoupled
 * from the live GPU capture.
 *
 * Exposed as the console command `CameraSelfCheck` so a one-command run can
 * drive it, and callable directly from an automation test.
 *
 * Checks (each with saved numbers):
 *   - config: config/camera.yaml loads and validates (Gate A values in-bound).
 *   - out-of-bound rejection: a width of 99999 is rejected naming "width".
 *   - determinism: two runners with the same (seed, config) over the same
 *     synthetic frames produce byte-identical published pixels and identical
 *     dropped flags.
 *   - per-pixel noise: over a large flat frame the sample std of
 *     (published - truth) on UNCLAMPED mid-grey pixels recovers pixel_noise_sigma
 *     within +-10%; sigma = 0 is an exact passthrough.
 *   - frame dropout: measured dropped fraction over N frames (sized by the
 *     design's n >= ceil(40000*p*(1-p)) rule) is within +-2 pp of
 *     frame_dropout_prob.
 *   - latency: on a synthetic clock stepping L/50, the mean released delay is
 *     within +-20% of L; L = 0 releases in the capture step (delay exactly 0).
 *   - clamp: with a bright frame (255) and sigma>0, published pixels never
 *     exceed 255 and never go below 0 (clamp to [0,255]).
 *
 * Logs a single `CameraSelfCheck PASS`/`FAIL` line a verify script can grep,
 * plus the measured numbers.
 */
class FCameraSelfCheck
{
public:
	struct FResult
	{
		bool bConfigValid = false;
		bool bOutOfBoundRejected = false;
		bool bDeterministic = false;
		bool bNoiseRecovered = false;
		bool bSigmaZeroPassthrough = false;
		bool bDropoutWithinTolerance = false;
		bool bLatencyWithinTolerance = false;
		bool bZeroLatencyImmediate = false;
		bool bClamped = false;

		double RecoveredSigma = 0.0;
		double ConfiguredSigma = 0.0;
		double MeasuredDropout = 0.0;
		double ConfiguredDropout = 0.0;
		int32 DropoutSamples = 0;
		double MeasuredLatencyMs = 0.0;
		double ConfiguredLatencyMs = 0.0;
		FString Detail;

		bool Passed() const
		{
			return bConfigValid && bOutOfBoundRejected && bDeterministic &&
				bNoiseRecovered && bSigmaZeroPassthrough &&
				bDropoutWithinTolerance && bLatencyWithinTolerance &&
				bZeroLatencyImmediate && bClamped;
		}
	};

	/**
	 * Run the headless pipeline check. `CameraConfigPath` empty resolves
	 * config/camera.yaml. No world / RHI is required.
	 */
	static FResult Run(const FString& CameraConfigPath);
};
