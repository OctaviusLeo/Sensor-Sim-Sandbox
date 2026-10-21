// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "Pipeline/SensorConfigValidation.h"
#include "Sensors/CameraConfig.h"
#include "Sensors/CameraPipelineRunner.h"

#include <cmath>

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 13.2 (EXAMPLE test, not a property loop).
//
// In-bound camera configuration is accepted (Requirement 4.1): valid camera
// configurations ACROSS THE FULL BOUND RANGES validate (bValid == true, no
// offending parameter named), including the exact edges each bound allows.
//
// Validates: Requirements 4.1
//
// This is the camera ACCEPT counterpart to the lidar in-bound test
// (InBoundConfigAcceptedTest.cpp, task 6.2) and the camera half of the Property
// 2 rejection test (ConfigBoundsEnforcedTest.cpp, task 5.3). Where Property 2
// enumerates OUT-of-bound values and asserts rejection, this test picks
// concrete representative IN-bound points — the lower edge, the upper edge, and
// a mid value for each parameter — and asserts acceptance.
//
// Every camera bound is INCLUSIVE at both ends (unlike the lidar's exclusive-min
// FOV bounds), so each edge below is the boundary value itself, exactly:
//
//   width/height       1..7680,   int   (both edges inclusive)
//   rate_hz            0.1..100.0, float (both inclusive)
//   pixel_noise_sigma  0.0..255.0, float (both inclusive; accepts exactly 0)
//   frame_dropout_prob 0.0..1.0,   float (both inclusive; accepts 0 and 1)
//   latency_ms         0..1000,    int   (both edges inclusive; accepts 0)
//
// The Gate A defaults (config/camera.yaml: 640x480, 10 Hz, sigma 5.0, dropout
// 0.05, latency 200) are also asserted accepted, and acceptance is driven end
// to end through FCameraPipelineRunner::Initialise / ApplyConfig (which is what
// ACameraModel::Initialise / ApplyConfig delegate to) to prove the accept path
// is wired, not just the bound set validated in isolation.
//
// This drives the SHARED validation contract via FCameraConfig::Validate (the
// same path a sensor uses), with no world or rendering — FCameraPipelineRunner
// is stack-constructible. Nothing here is weakened to pass: if an edge believed
// to be in-bound were rejected, that would be a bound-definition/config bug to
// investigate, not a test to relax.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxCameraInBoundConfigAcceptedTest,
	"SensorSimSandbox.Camera.InBoundConfigAccepted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
{
	// One in-bound candidate value for a single parameter, with a label so a
	// failure points at exactly which edge/point was wrongly rejected.
	struct FEdgeCase
	{
		FString Label;
		double Value = 0.0;
	};
}

bool FSensorSimSandboxCameraInBoundConfigAcceptedTest::RunTest(const FString& Parameters)
{
	// -----------------------------------------------------------------------
	// Part 1: Each parameter accepted at its lower edge, upper edge, and a mid
	// value, holding every OTHER parameter at a known in-bound value. Poisoning
	// exactly one parameter at a time keeps any failure attributable.
	// -----------------------------------------------------------------------

	// A known fully in-bound baseline (the Gate A defaults). Each sweep below
	// overrides exactly one field of a COPY of this baseline.
	const FCameraConfig Baseline; // defaults are the Gate A values

	// Per-parameter in-bound edge cases: {lower edge, upper edge, a mid value}.
	// All camera bounds are inclusive at both ends, so each edge is the
	// boundary value itself. The setter applies the value onto a copy of the
	// baseline.
	struct FParamSweep
	{
		FString Name;
		TArray<FEdgeCase> Cases;
		TFunction<void(FCameraConfig&, double)> Apply;
	};

	const TArray<FParamSweep> Sweeps = {
		{
			TEXT("width"),
			// int, 1..7680 inclusive both ends.
			{ { TEXT("min=1"), 1.0 }, { TEXT("max=7680"), 7680.0 }, { TEXT("mid=640"), 640.0 } },
			[](FCameraConfig& C, double V) { C.Width = FMath::RoundToInt(V); }
		},
		{
			TEXT("height"),
			// int, 1..7680 inclusive both ends.
			{ { TEXT("min=1"), 1.0 }, { TEXT("max=7680"), 7680.0 }, { TEXT("mid=480"), 480.0 } },
			[](FCameraConfig& C, double V) { C.Height = FMath::RoundToInt(V); }
		},
		{
			TEXT("rate_hz"),
			// float, 0.1..100.0 inclusive both ends.
			{ { TEXT("min=0.1"), 0.1 }, { TEXT("max=100.0"), 100.0 }, { TEXT("mid=50.0"), 50.0 } },
			[](FCameraConfig& C, double V) { C.RateHz = V; }
		},
		{
			TEXT("pixel_noise_sigma"),
			// float, 0.0..255.0 inclusive both ends; accepts EXACTLY 0.
			{ { TEXT("min=0.0"), 0.0 }, { TEXT("max=255.0"), 255.0 }, { TEXT("mid=127.5"), 127.5 } },
			[](FCameraConfig& C, double V) { C.PixelNoiseSigma = V; }
		},
		{
			TEXT("frame_dropout_prob"),
			// float, 0.0..1.0 inclusive both ends; accepts EXACTLY 0 and EXACTLY 1.
			{ { TEXT("min=0.0"), 0.0 }, { TEXT("max=1.0"), 1.0 }, { TEXT("mid=0.5"), 0.5 } },
			[](FCameraConfig& C, double V) { C.FrameDropoutProb = V; }
		},
		{
			TEXT("latency_ms"),
			// int, 0..1000 inclusive both ends; accepts EXACTLY 0.
			{ { TEXT("min=0"), 0.0 }, { TEXT("max=1000"), 1000.0 }, { TEXT("mid=500"), 500.0 } },
			[](FCameraConfig& C, double V) { C.LatencyMs = FMath::RoundToInt(V); }
		},
	};

	for (const FParamSweep& Sweep : Sweeps)
	{
		for (const FEdgeCase& Case : Sweep.Cases)
		{
			FCameraConfig Config = Baseline;
			Sweep.Apply(Config, Case.Value);

			const FSensorConfigValidationResult Result = Config.Validate();

			if (!Result.bValid)
			{
				AddError(FString::Printf(
					TEXT("task 13.2 FAILED [%s @ %s]: an in-bound value was REJECTED ")
					TEXT("(offending='%s', msg='%s'). Values on/within the stated bound ")
					TEXT("must be accepted (Requirement 4.1)."),
					*Sweep.Name, *Case.Label, *Result.OffendingParameter, *Result.ErrorMessage));
				return false;
			}

			// An accepted result must name no offending parameter.
			TestEqual(
				FString::Printf(TEXT("accepted config names no offending parameter [%s @ %s]"),
					*Sweep.Name, *Case.Label),
				Result.OffendingParameter, FString());
		}
	}

	// -----------------------------------------------------------------------
	// Part 2: An "all edges simultaneously" extreme — every parameter at a
	// bound edge at once — is still a valid configuration. This proves the
	// accept region is the full product of per-parameter bounds, not just one
	// axis at a time.
	// -----------------------------------------------------------------------
	{
		FCameraConfig AllMin;
		AllMin.Width = 1;
		AllMin.Height = 1;
		AllMin.RateHz = 0.1;
		AllMin.PixelNoiseSigma = 0.0;
		AllMin.FrameDropoutProb = 0.0;
		AllMin.LatencyMs = 0;
		const FSensorConfigValidationResult MinResult = AllMin.Validate();
		if (!MinResult.bValid)
		{
			AddError(FString::Printf(
				TEXT("task 13.2 FAILED [all-min edges]: the all-lower-edge config was ")
				TEXT("rejected (offending='%s', msg='%s'); every parameter is in-bound."),
				*MinResult.OffendingParameter, *MinResult.ErrorMessage));
			return false;
		}

		FCameraConfig AllMax;
		AllMax.Width = 7680;
		AllMax.Height = 7680;
		AllMax.RateHz = 100.0;
		AllMax.PixelNoiseSigma = 255.0;
		AllMax.FrameDropoutProb = 1.0;
		AllMax.LatencyMs = 1000;
		const FSensorConfigValidationResult MaxResult = AllMax.Validate();
		if (!MaxResult.bValid)
		{
			AddError(FString::Printf(
				TEXT("task 13.2 FAILED [all-max edges]: the all-upper-edge config was ")
				TEXT("rejected (offending='%s', msg='%s'); every parameter is in-bound."),
				*MaxResult.OffendingParameter, *MaxResult.ErrorMessage));
			return false;
		}
	}

	// -----------------------------------------------------------------------
	// Part 3: The Gate A config (config/camera.yaml defaults) is accepted.
	// These are the values the real run ships with (640x480, 10 Hz, sigma 5.0,
	// dropout 0.05, latency 200), so acceptance here guards the shipped
	// configuration directly.
	// -----------------------------------------------------------------------
	{
		const FCameraConfig GateA; // defaults mirror config/camera.yaml
		const FSensorConfigValidationResult Result = GateA.Validate();
		if (!Result.bValid)
		{
			AddError(FString::Printf(
				TEXT("task 13.2 FAILED [Gate A defaults]: the shipped camera.yaml defaults ")
				TEXT("were rejected (offending='%s', msg='%s')."),
				*Result.OffendingParameter, *Result.ErrorMessage));
			return false;
		}

		// Sanity: the baseline defaults really are the documented Gate A values.
		TestEqual(TEXT("Gate A default width is 640"), GateA.Width, 640);
		TestEqual(TEXT("Gate A default height is 480"), GateA.Height, 480);
		TestEqual(TEXT("Gate A default rate_hz is 10.0"), GateA.RateHz, 10.0);
		TestEqual(TEXT("Gate A default latency_ms is 200"), GateA.LatencyMs, 200);
	}

	// -----------------------------------------------------------------------
	// Part 4: Acceptance drives through the pure runner end to end. A valid
	// config passed to Initialise is adopted and the runner becomes ready; a
	// later valid ApplyConfig (an edge config) is adopted and becomes the
	// current config. FCameraPipelineRunner is exactly what ACameraModel's
	// Initialise / ApplyConfig delegate to, so this proves the accept path is
	// wired, not just the bound set validated in isolation.
	// -----------------------------------------------------------------------
	{
		FCameraPipelineRunner Runner;

		const FCameraConfig GateA; // valid
		const FSensorConfigValidationResult Init = Runner.Initialise(/*seed*/ 12345, GateA, TEXT("camera"));
		TestTrue(TEXT("Initialise with a valid config returns bValid"), Init.bValid);
		TestTrue(TEXT("runner is ready after a valid Initialise"), Runner.IsReady());
		TestEqual(TEXT("ready runner adopted the width of the valid config"),
			Runner.GetConfig().Width, GateA.Width);
		TestEqual(TEXT("ready runner adopted the height of the valid config"),
			Runner.GetConfig().Height, GateA.Height);

		// An edge-of-bound but valid runtime update is adopted.
		FCameraConfig EdgeUpdate = GateA;
		EdgeUpdate.Width = 7680;             // upper edge, inclusive
		EdgeUpdate.Height = 1;               // lower edge, inclusive
		EdgeUpdate.FrameDropoutProb = 1.0;   // upper edge, inclusive
		EdgeUpdate.PixelNoiseSigma = 0.0;    // lower edge, inclusive (exactly 0)
		EdgeUpdate.LatencyMs = 0;            // lower edge, inclusive (exactly 0)
		EdgeUpdate.RateHz = 100.0;           // upper edge, inclusive
		const FSensorConfigValidationResult Applied = Runner.ApplyConfig(EdgeUpdate);
		TestTrue(TEXT("ApplyConfig with a valid edge config returns bValid"), Applied.bValid);
		TestTrue(TEXT("runner stays ready after a valid ApplyConfig"), Runner.IsReady());
		TestEqual(TEXT("runner adopted the edge config's width"),
			Runner.GetConfig().Width, 7680);
		TestEqual(TEXT("runner adopted the edge config's height"),
			Runner.GetConfig().Height, 1);
		TestEqual(TEXT("runner adopted the edge config's frame_dropout_prob"),
			Runner.GetConfig().FrameDropoutProb, 1.0);
		TestEqual(TEXT("runner adopted the edge config's pixel_noise_sigma"),
			Runner.GetConfig().PixelNoiseSigma, 0.0);
		TestEqual(TEXT("runner adopted the edge config's latency_ms"),
			Runner.GetConfig().LatencyMs, 0);
	}

	return true;
}
