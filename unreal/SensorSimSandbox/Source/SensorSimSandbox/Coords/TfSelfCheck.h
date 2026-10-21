// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"

/**
 * Deterministic headless self-check for the REP 103 converter and TfPublisher
 * (task 9.1 demo/verify, Requirement 7.1, 7.4).
 *
 * Runs with NO world, rendering, or live WebSocket, so it is driveable from a
 * commandlet / console command / unit test. It verifies four things:
 *
 *   1. Position conversion matches the fixed REP 103 mapping (x/100, -y/100,
 *      z/100) against hand-worked expected values, within 1e-6 m.
 *   2. Rotation conversion matches q_ros = (w, -x, y, -z), and the
 *      forward-then-back round trip is the identity within 1e-6.
 *   3. The TfPublisher builds a well-formed `tf2_msgs/msg/TFMessage` publish op
 *      for the rover body + each sensor frame, with the REP 103-converted
 *      translation/rotation in it.
 *   4. The >= 10 Hz scheduler (Requirement 7.4) emits publishes at a measured
 *      rate >= 10 Hz over a synthetic clock.
 *
 * The live `ros2 topic hz /tf` [HOST] measurement requires the Rosbridge_Client
 * transport (task 11), which does not exist yet; this self-check verifies every
 * part that is reachable headlessly now (conversion, message build, scheduling)
 * so the deferred live step is the ONLY thing left for M2's bridge work.
 */
class FTfSelfCheck
{
public:
	struct FResult
	{
		bool bPositionConversionCorrect = false;
		bool bRotationConversionCorrect = false;
		bool bRoundTripIdentity = false;
		bool bTfMessageWellFormed = false;
		bool bRateAtLeast10Hz = false;
		double MeasuredRateHz = 0.0;
		FString Detail;

		bool Passed() const
		{
			return bPositionConversionCorrect
				&& bRotationConversionCorrect
				&& bRoundTripIdentity
				&& bTfMessageWellFormed
				&& bRateAtLeast10Hz;
		}
	};

	/** Run every headless check and report the combined result. */
	static FResult Run();
};
