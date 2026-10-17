// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Misc/AutomationTest.h"

#include "Sensors/CameraConfig.h"
#include "Sensors/CameraPipelineRunner.h"
#include "Sensors/CameraSelfCheck.h"
#include "Pipeline/SensorConfigValidation.h"

#include <cmath>

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 13.1 — HEADLESS camera pipeline verify.
//
// The engine scene-capture + GPU readback lives in ACameraModel; this test
// drives the PURE camera pipeline (FCameraPipelineRunner) on SYNTHETIC frames
// with no world and no RHI, so it runs under -nullrhi in the headless suite
// (scripts/run_ue_tests.ps1). It is the "pipeline and message logic verified
// headless on synthetic frames" half of the task 13.1 demo/verify.
//
// Validates: Requirements 4.1, 4.2, 4.5, 4.6
//
// It asserts the end-to-end FCameraSelfCheck passes (determinism, per-pixel
// noise sigma recovery within +-10%, sigma=0 passthrough, frame dropout within
// +-2 pp, latency within +-20% and L=0 immediate, clamp to [0,255], config
// bounds). It then adds direct unit assertions for the capture-before-pipeline
// rule, the shared validation contract (reject + retain-last-valid), and the
// sampling-interval helper. Nothing here is weakened to pass; a failing case is
// a real bug to investigate.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxCameraPipelineHeadlessTest,
	"SensorSimSandbox.Camera.PipelineHeadless",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
{
	FCameraFrameBuffer MakeFlat(int32 W, int32 H, uint8 Level)
	{
		FCameraFrameBuffer Buf;
		Buf.Width = W;
		Buf.Height = H;
		const int64 N = static_cast<int64>(W) * H;
		Buf.Pixels.SetNumUninitialized(N * 4);
		for (int64 i = 0; i < N; ++i)
		{
			const int64 B = i * 4;
			Buf.Pixels[B + 0] = Level;
			Buf.Pixels[B + 1] = Level;
			Buf.Pixels[B + 2] = Level;
			Buf.Pixels[B + 3] = 255;
		}
		return Buf;
	}
}

bool FSensorSimSandboxCameraPipelineHeadlessTest::RunTest(const FString& Parameters)
{
	// The runner logs an Error line when it rejects an out-of-bound config
	// (that error IS the "emit an error identifying the parameter" behaviour
	// Requirement 4.2 demands). This test deliberately drives that reject path
	// (the self-check's width=99999 probe and Part 3's height=0 update), so
	// declare those log lines as EXPECTED — otherwise the automation framework
	// would count our own intended error output as a test failure. We assert
	// the rejection's effect (bValid false, offending named, last valid config
	// retained) below; expecting the log here does not weaken those assertions.
	AddExpectedError(TEXT("Camera ApplyConfig rejected"), EAutomationExpectedErrorFlags::Contains);

	// -----------------------------------------------------------------------
	// Part 1: the full headless self-check passes. This uses the SHIPPED
	// config/camera.yaml (Gate A), so it guards the real configuration.
	// -----------------------------------------------------------------------
	const FCameraSelfCheck::FResult SC = FCameraSelfCheck::Run(FString());
	if (!SC.Passed())
	{
		AddError(FString::Printf(TEXT("CameraSelfCheck FAILED: %s"), *SC.Detail));
	}
	TestTrue(TEXT("camera config loads and validates (Gate A in-bound)"), SC.bConfigValid);
	TestTrue(TEXT("out-of-bound width rejected by shared contract (4.2)"), SC.bOutOfBoundRejected);
	TestTrue(TEXT("pipeline is deterministic under fixed seed"), SC.bDeterministic);
	TestTrue(TEXT("per-pixel noise sigma recovered within +-10% (4.5-adjacent)"), SC.bNoiseRecovered);
	TestTrue(TEXT("sigma=0 is an exact passthrough"), SC.bSigmaZeroPassthrough);
	TestTrue(TEXT("frame dropout within +-2 pp of configured (4.5)"), SC.bDropoutWithinTolerance);
	TestTrue(TEXT("latency mean within +-20% of L (4.6)"), SC.bLatencyWithinTolerance);
	TestTrue(TEXT("L=0 releases in the capture step"), SC.bZeroLatencyImmediate);
	TestTrue(TEXT("per-pixel values clamped to [0,255]"), SC.bClamped);

	// -----------------------------------------------------------------------
	// Part 2: capture-before-pipeline — the truth carried out of the runner is
	// byte-identical to the truth passed in (never noised in place).
	// -----------------------------------------------------------------------
	{
		FCameraConfig Cfg; // Gate A defaults
		FCameraPipelineRunner Runner;
		const FSensorConfigValidationResult R = Runner.Initialise(1234, Cfg, TEXT("camera"));
		TestTrue(TEXT("runner initialises with the Gate A config"), R.bValid);
		TestTrue(TEXT("runner is ready after a valid Initialise"), Runner.IsReady());

		const FCameraFrameBuffer Truth = MakeFlat(8, 8, 123);
		const FCameraFrame F = Runner.ProcessCapturedFrame(0, 0.0, Truth);
		TestEqual(TEXT("captured truth is preserved verbatim (captured first)"),
			F.Truth.Pixels, Truth.Pixels);
	}

	// -----------------------------------------------------------------------
	// Part 3: shared validation contract — an out-of-bound ApplyConfig is
	// rejected as a whole, names the parameter, and the last valid config is
	// retained (Requirement 4.2).
	// -----------------------------------------------------------------------
	{
		FCameraConfig Good; // Gate A defaults, valid
		FCameraPipelineRunner Runner;
		Runner.Initialise(1234, Good, TEXT("camera"));
		const int32 KeptWidth = Runner.GetConfig().Width;

		FCameraConfig Bad = Good;
		Bad.Height = 0; // 1..7680 -> out of bounds
		const FSensorConfigValidationResult Rej = Runner.ApplyConfig(Bad);
		TestFalse(TEXT("out-of-bound height is rejected"), Rej.bValid);
		TestEqual(TEXT("rejection names the offending parameter"), Rej.OffendingParameter, FString(TEXT("height")));
		TestEqual(TEXT("last valid config is retained after a rejected update"),
			Runner.GetConfig().Width, KeptWidth);
		TestEqual(TEXT("rejected height was NOT partially applied"),
			Runner.GetConfig().Height, Good.Height);
	}

	// -----------------------------------------------------------------------
	// Part 4: sampling-interval helper — "1/10" samples seq 0,10,20,...; a
	// malformed interval fails open (samples every frame).
	// -----------------------------------------------------------------------
	{
		FCameraConfig Cfg;
		Cfg.SamplingInterval = TEXT("1/10");
		TestTrue(TEXT("seq 0 sampled under 1/10"), Cfg.IsFrameSampled(0));
		TestFalse(TEXT("seq 5 not sampled under 1/10"), Cfg.IsFrameSampled(5));
		TestTrue(TEXT("seq 10 sampled under 1/10"), Cfg.IsFrameSampled(10));
		TestTrue(TEXT("seq 20 sampled under 1/10"), Cfg.IsFrameSampled(20));

		int32 N = 0, M = 0;
		TestTrue(TEXT("1/10 parses"), FCameraConfig::ParseSamplingInterval(TEXT("1/10"), N, M));
		TestEqual(TEXT("1/10 N"), N, 1);
		TestEqual(TEXT("1/10 M"), M, 10);
		TestFalse(TEXT("garbage ratio rejected"), FCameraConfig::ParseSamplingInterval(TEXT("abc"), N, M));

		FCameraConfig Bad;
		Bad.SamplingInterval = TEXT("not-a-ratio");
		TestTrue(TEXT("malformed interval fails open (samples every frame)"), Bad.IsFrameSampled(3));
	}

	return !HasAnyErrors();
}
