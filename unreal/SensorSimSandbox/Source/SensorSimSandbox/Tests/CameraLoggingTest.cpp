// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "Sensors/CameraLoggerSelfCheck.h"
#include "GroundTruth/GroundTruthLoggerSelfCheck.h" // ResolveRunsRoot

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 15 — extend the logger and bridge for the
// camera. Headless automation test for the camera LOGGING path.
//
// Task 15 wires the camera frames into the GroundTruthLogger end to end: one
// Run_Log record per PRODUCED camera frame (including dropped frames), with
// image refs ONLY on sampled non-dropped frames, and the camera sampling
// interval ratio in the header; non-dropped frames publish their
// CompressedImage + CameraInfo through the Rosbridge_Client.
//
// This test drives the REAL path through FCameraLoggerSelfCheck: it runs a
// deterministic mix of sampled / unsampled / dropped frames through the shared
// camera pipeline and the real FGroundTruthLogger, writes a real `.jsonl`
// Run_Log under runs/<run_id>/ (gitignored), saves the sampled image pairs, and
// publishes the non-dropped frames; it then re-reads the Run_Log and asserts
// the full camera record contract from the design's "Ground-truth logging
// format" (camera rows).
//
//   SensorSimSandbox.Logging.CameraLogging
//
// Validates: Requirements 5.3, 4.3
//
// It runs with no world and no GPU RHI (the pipeline is pure and the image
// encoders are CPU-only), so it executes reliably under the nullrhi automation
// harness (scripts/run_ue_tests.ps1). Nothing is weakened to pass: a failing
// assertion is a real camera-logging wiring bug to investigate. The self-check
// itself FAILS LOUDLY if the deterministic parameters ever stop producing all
// three frame categories, so this test can never pass by vacuously skipping a
// case.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxLoggingCameraLoggingTest,
	"SensorSimSandbox.Logging.CameraLogging",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxLoggingCameraLoggingTest::RunTest(const FString& Parameters)
{
	// Write to a unique folder under runs/ so repeated test runs do not collide
	// and so the output stays within the gitignored runs/ tree.
	const FString RunId = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S_%s"));
	const FString RunDir = FPaths::Combine(
		FGroundTruthLoggerSelfCheck::ResolveRunsRoot(),
		FString::Printf(TEXT("camlog_test_%s"), *RunId));

	const FCameraLoggerSelfCheck::FResult Result =
		FCameraLoggerSelfCheck::Run(/*CameraConfigPath*/ FString(), RunDir);

	// A `.jsonl` Run_Log was actually written to disk.
	TestTrue(
		FString::Printf(TEXT("Run_Log file exists on disk (%s)"), *Result.RunLogPath),
		FPaths::FileExists(Result.RunLogPath));

	// The deterministic mix covered all three categories (the self-check FAILS
	// if not, so a positive count here also proves it did not short-circuit).
	TestTrue(TEXT("at least one SAMPLED frame was produced"), Result.SampledCount > 0);
	TestTrue(TEXT("at least one UNSAMPLED non-dropped frame was produced"), Result.UnsampledCount > 0);
	TestTrue(TEXT("at least one DROPPED frame was produced"), Result.DroppedCount > 0);

	// Header records camera_sampling_interval == the config ratio (5.3).
	TestTrue(TEXT("header records camera_sampling_interval == the ratio (5.3)"),
		Result.bHeaderHasInterval);

	// Exactly one record per produced frame, dropped included.
	TestTrue(
		FString::Printf(TEXT("one record per produced frame incl. dropped (records=%d produced=%d)"),
			Result.RecordsRead - 1, Result.FramesProduced),
		Result.bRecordPerProducedFrame);

	// Sampled frames carry both refs AND the referenced image files exist (4.3, 5.3).
	TestTrue(TEXT("sampled frames carry both refs and the image files exist on disk (4.3)"),
		Result.bSampledRefsPresent);

	// Unsampled non-dropped frames carry explicit null refs.
	TestTrue(TEXT("unsampled non-dropped frames carry null refs"),
		Result.bUnsampledRefsNull);

	// Dropped frames: dropped=true, t_publish null, no refs.
	TestTrue(TEXT("dropped frames have dropped=true, t_publish null, no image refs"),
		Result.bDroppedShape);

	// t non-decreasing in file order (5.1).
	TestTrue(TEXT("Run_Log t values are non-decreasing in file order (5.1)"),
		Result.bMonotonicT);

	// Every non-dropped frame published its CompressedImage + CameraInfo through
	// the Rosbridge_Client (count equals sampled + unsampled).
	TestTrue(
		FString::Printf(TEXT("every non-dropped frame published through the client (published=%d non-dropped=%d)"),
			Result.PublishedCount, Result.SampledCount + Result.UnsampledCount),
		Result.bPublishedNonDropped);

	if (!Result.Passed())
	{
		AddError(FString::Printf(TEXT("CameraLoggerSelfCheck did not pass: %s"), *Result.Detail));
	}

	return Result.Passed();
}
