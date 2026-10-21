// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "Coords/Rep103.h"
#include "Coords/TfPublisher.h"
#include "Coords/TfSelfCheck.h"

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 9.1 (EXAMPLE/integration test).
//
// Headless verification that the REP 103 converter and the TfPublisher behave
// as the design fixes them (Requirement 7.1, 7.4):
//
//   * ConvertPosition applies (x/100, -y/100, z/100) to within 1e-6 m,
//   * ConvertRotation applies q_ros = (w, -x, y, -z) and round-trips to the
//     identity,
//   * TfPublisher builds a well-formed tf2_msgs/msg/TFMessage publish op for
//     the rover body + each sensor frame with the REP 103-converted transform,
//   * the publish scheduler sustains >= 10 Hz on a synthetic clock.
//
// Validates: Requirements 7.1, 7.4
//
// It drives the PURE cores (FRep103, FTfPublisher, FTfSelfCheck) directly with
// NO world, rendering, or live WebSocket, consistent with how the other pure
// cores are isolated. The exhaustive >=100-iteration property coverage of the
// 1e-6 conversion tolerance (Property 12, 13) lives in task 9.2; this test
// pins the fixed mapping on hand-worked values and exercises the TF publish
// path end to end through a capturing transport.
//
// The live `ros2 topic hz /tf` [HOST] measurement needs the Rosbridge_Client
// transport (task 11), which does not exist yet, so the >= 10 Hz check here is
// the scheduler's achieved publish rate against a capturing sink. The live
// measurement is recorded as deferred to M2's bridge work.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxCoordConversionTfTest,
	"SensorSimSandbox.Coords.ConversionAndTfPublish",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxCoordConversionTfTest::RunTest(const FString& Parameters)
{
	// --- Direct converter spot checks (hand-worked expected values) --------
	{
		const FRep103Position P = FRep103::ConvertPosition(FVector(100.0, 200.0, 300.0));
		TestEqual(TEXT("position x = x/100"), P.X, 1.0);
		TestEqual(TEXT("position y = -y/100"), P.Y, -2.0);
		TestEqual(TEXT("position z = z/100"), P.Z, 3.0);
	}
	{
		// FQuat ctor is (X, Y, Z, W).
		const FQuat Ue(0.3, 0.4, 0.5, 0.7);
		const FRep103Quat Q = FRep103::ConvertRotation(Ue);
		TestEqual(TEXT("rotation w unchanged"), Q.W, 0.7);
		TestEqual(TEXT("rotation x negated"), Q.X, -0.3);
		TestEqual(TEXT("rotation y unchanged"), Q.Y, 0.4);
		TestEqual(TEXT("rotation z negated"), Q.Z, -0.5);
	}

	// --- Full self-check (position, rotation, round trip, TF build, rate) --
	const FTfSelfCheck::FResult Result = FTfSelfCheck::Run();

	TestTrue(FString::Printf(TEXT("position conversion correct [%s]"), *Result.Detail),
		Result.bPositionConversionCorrect);
	TestTrue(FString::Printf(TEXT("rotation conversion correct [%s]"), *Result.Detail),
		Result.bRotationConversionCorrect);
	TestTrue(FString::Printf(TEXT("rotation round-trip identity [%s]"), *Result.Detail),
		Result.bRoundTripIdentity);
	TestTrue(FString::Printf(TEXT("TFMessage well-formed [%s]"), *Result.Detail),
		Result.bTfMessageWellFormed);
	TestTrue(FString::Printf(TEXT("TF publish rate >= 10 Hz (measured %.3f Hz) [%s]"),
		Result.MeasuredRateHz, *Result.Detail),
		Result.bRateAtLeast10Hz);

	// The measured scheduler rate must be at least the 10 Hz floor.
	TestTrue(TEXT("measured scheduler rate >= 10 Hz"), Result.MeasuredRateHz + 1e-9 >= FTfPublisher::MinRateHz);

	return Result.Passed();
}
