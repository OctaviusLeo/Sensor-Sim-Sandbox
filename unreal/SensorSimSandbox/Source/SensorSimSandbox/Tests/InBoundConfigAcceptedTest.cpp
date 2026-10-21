// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "Pipeline/SensorConfigValidation.h"
#include "Sensors/LidarConfig.h"
#include "Sensors/LidarPipelineRunner.h"

#include <cmath>

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 6.2 (EXAMPLE test, not a property loop).
//
// In-bound lidar configuration is accepted (Requirement 3.1): valid lidar
// configurations ACROSS THE FULL BOUND RANGES validate (bValid == true, no
// offending parameter named), including the exact edges each bound allows.
//
// Validates: Requirements 3.1
//
// This is the ACCEPT counterpart to the Property 2 rejection test
// (ConfigBoundsEnforcedTest.cpp, task 5.3). Where Property 2 enumerates
// OUT-of-bound values and asserts rejection, this test picks concrete
// representative IN-bound points — the lower edge, the upper edge, and a mid
// value for each parameter — and asserts acceptance, honouring whether each
// edge is inclusive or exclusive:
//
//   channels               1..128,    int   (both edges inclusive)
//   h_fov_deg              (0..360],   float (min EXCLUSIVE, max inclusive)
//   v_fov_deg              (0..180],   float (min EXCLUSIVE, max inclusive)
//   angular_resolution_deg 0.01..10.0, float (both inclusive)
//   max_range_m            0.1..1000.0,float (both inclusive)
//   rate_hz                0.1..100.0, float (both inclusive)
//   range_noise_sigma_m    0.0..10.0,  float (both inclusive; accepts exactly 0)
//   dropout_prob           0.0..1.0,   float (both inclusive; accepts 0 and 1)
//   latency_ms             0..1000,    int   (both edges inclusive)
//
// For the exclusive-min floats the smallest ACCEPTED value is just above the
// boundary (not the boundary itself, which Property 2 proves is rejected); the
// inclusive maxima (360, 180) are accepted exactly. The Gate A defaults
// (config/lidar.yaml) are also asserted accepted, and acceptance is driven end
// to end through FLidarPipelineRunner::Initialise / ApplyConfig to prove the
// accept path is wired, not just the bound set in isolation.
//
// This drives the SHARED validation contract via FLidarConfig::Validate (the
// same path a sensor uses), with no world or rendering — FLidarPipelineRunner
// is stack-constructible. Nothing here is weakened to pass: if an edge believed
// to be in-bound were rejected, that would be a bound-definition/config bug to
// investigate, not a test to relax.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxLidarInBoundConfigAcceptedTest,
	"SensorSimSandbox.Lidar.InBoundConfigAccepted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
{
	// A tiny step used to land JUST ABOVE an exclusive lower bound. Large
	// enough to be unambiguously inside the open interval, small enough that
	// the value is still "at" the lower edge for the purposes of this test.
	constexpr double ExclusiveMinEpsilon = 1e-6;

	// One in-bound candidate value for a single parameter, with a label so a
	// failure points at exactly which edge/point was wrongly rejected.
	struct FEdgeCase
	{
		FString Label;
		double Value = 0.0;
	};
}

bool FSensorSimSandboxLidarInBoundConfigAcceptedTest::RunTest(const FString& Parameters)
{
	// -----------------------------------------------------------------------
	// Part 1: Each parameter accepted at its lower edge, upper edge, and a mid
	// value, holding every OTHER parameter at a known in-bound value. Poisoning
	// exactly one parameter at a time keeps any failure attributable.
	// -----------------------------------------------------------------------

	// A known fully in-bound baseline (the Gate A defaults). Each sweep below
	// overrides exactly one field of a COPY of this baseline.
	const FLidarConfig Baseline; // defaults are the Gate A values

	// Per-parameter in-bound edge cases: {lower edge (honouring exclusivity),
	// upper edge (inclusive), a mid value}. The setter applies the value onto a
	// copy of the baseline.
	struct FParamSweep
	{
		FString Name;
		TArray<FEdgeCase> Cases;
		TFunction<void(FLidarConfig&, double)> Apply;
	};

	const TArray<FParamSweep> Sweeps = {
		{
			TEXT("channels"),
			// int, 1..128 inclusive both ends.
			{ { TEXT("min=1"), 1.0 }, { TEXT("max=128"), 128.0 }, { TEXT("mid=64"), 64.0 } },
			[](FLidarConfig& C, double V) { C.Channels = FMath::RoundToInt(V); }
		},
		{
			TEXT("h_fov_deg"),
			// float, (0..360]: min exclusive -> just above 0; max 360 inclusive.
			{ { TEXT("min~0+eps"), ExclusiveMinEpsilon }, { TEXT("max=360"), 360.0 }, { TEXT("mid=180"), 180.0 } },
			[](FLidarConfig& C, double V) { C.HFovDeg = V; }
		},
		{
			TEXT("v_fov_deg"),
			// float, (0..180]: min exclusive -> just above 0; max 180 inclusive.
			{ { TEXT("min~0+eps"), ExclusiveMinEpsilon }, { TEXT("max=180"), 180.0 }, { TEXT("mid=90"), 90.0 } },
			[](FLidarConfig& C, double V) { C.VFovDeg = V; }
		},
		{
			TEXT("angular_resolution_deg"),
			// float, 0.01..10.0 inclusive both ends.
			{ { TEXT("min=0.01"), 0.01 }, { TEXT("max=10.0"), 10.0 }, { TEXT("mid=5.0"), 5.0 } },
			[](FLidarConfig& C, double V) { C.AngularResolutionDeg = V; }
		},
		{
			TEXT("max_range_m"),
			// float, 0.1..1000.0 inclusive both ends.
			{ { TEXT("min=0.1"), 0.1 }, { TEXT("max=1000.0"), 1000.0 }, { TEXT("mid=500.0"), 500.0 } },
			[](FLidarConfig& C, double V) { C.MaxRangeM = V; }
		},
		{
			TEXT("rate_hz"),
			// float, 0.1..100.0 inclusive both ends.
			{ { TEXT("min=0.1"), 0.1 }, { TEXT("max=100.0"), 100.0 }, { TEXT("mid=50.0"), 50.0 } },
			[](FLidarConfig& C, double V) { C.RateHz = V; }
		},
		{
			TEXT("range_noise_sigma_m"),
			// float, 0.0..10.0 inclusive both ends; accepts EXACTLY 0.
			{ { TEXT("min=0.0"), 0.0 }, { TEXT("max=10.0"), 10.0 }, { TEXT("mid=5.0"), 5.0 } },
			[](FLidarConfig& C, double V) { C.RangeNoiseSigmaM = V; }
		},
		{
			TEXT("dropout_prob"),
			// float, 0.0..1.0 inclusive both ends; accepts EXACTLY 0 and EXACTLY 1.
			{ { TEXT("min=0.0"), 0.0 }, { TEXT("max=1.0"), 1.0 }, { TEXT("mid=0.5"), 0.5 } },
			[](FLidarConfig& C, double V) { C.DropoutProb = V; }
		},
		{
			TEXT("latency_ms"),
			// int, 0..1000 inclusive both ends; accepts EXACTLY 0.
			{ { TEXT("min=0"), 0.0 }, { TEXT("max=1000"), 1000.0 }, { TEXT("mid=500"), 500.0 } },
			[](FLidarConfig& C, double V) { C.LatencyMs = FMath::RoundToInt(V); }
		},
	};

	for (const FParamSweep& Sweep : Sweeps)
	{
		for (const FEdgeCase& Case : Sweep.Cases)
		{
			FLidarConfig Config = Baseline;
			Sweep.Apply(Config, Case.Value);

			const FSensorConfigValidationResult Result = Config.Validate();

			if (!Result.bValid)
			{
				AddError(FString::Printf(
					TEXT("task 6.2 FAILED [%s @ %s]: an in-bound value was REJECTED ")
					TEXT("(offending='%s', msg='%s'). Values on/within the stated bound ")
					TEXT("must be accepted (Requirement 3.1)."),
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
		FLidarConfig AllMin;
		AllMin.Channels = 1;
		AllMin.HFovDeg = ExclusiveMinEpsilon; // (0..360], min exclusive
		AllMin.VFovDeg = ExclusiveMinEpsilon; // (0..180], min exclusive
		AllMin.AngularResolutionDeg = 0.01;
		AllMin.MaxRangeM = 0.1;
		AllMin.RateHz = 0.1;
		AllMin.RangeNoiseSigmaM = 0.0;
		AllMin.DropoutProb = 0.0;
		AllMin.LatencyMs = 0;
		const FSensorConfigValidationResult MinResult = AllMin.Validate();
		if (!MinResult.bValid)
		{
			AddError(FString::Printf(
				TEXT("task 6.2 FAILED [all-min edges]: the all-lower-edge config was ")
				TEXT("rejected (offending='%s', msg='%s'); every parameter is in-bound."),
				*MinResult.OffendingParameter, *MinResult.ErrorMessage));
			return false;
		}

		FLidarConfig AllMax;
		AllMax.Channels = 128;
		AllMax.HFovDeg = 360.0;
		AllMax.VFovDeg = 180.0;
		AllMax.AngularResolutionDeg = 10.0;
		AllMax.MaxRangeM = 1000.0;
		AllMax.RateHz = 100.0;
		AllMax.RangeNoiseSigmaM = 10.0;
		AllMax.DropoutProb = 1.0;
		AllMax.LatencyMs = 1000;
		const FSensorConfigValidationResult MaxResult = AllMax.Validate();
		if (!MaxResult.bValid)
		{
			AddError(FString::Printf(
				TEXT("task 6.2 FAILED [all-max edges]: the all-upper-edge config was ")
				TEXT("rejected (offending='%s', msg='%s'); every parameter is in-bound."),
				*MaxResult.OffendingParameter, *MaxResult.ErrorMessage));
			return false;
		}
	}

	// -----------------------------------------------------------------------
	// Part 3: The Gate A config (config/lidar.yaml defaults) is accepted. These
	// are the values the real run ships with, so acceptance here guards the
	// shipped configuration directly.
	// -----------------------------------------------------------------------
	{
		const FLidarConfig GateA; // defaults mirror config/lidar.yaml
		const FSensorConfigValidationResult Result = GateA.Validate();
		if (!Result.bValid)
		{
			AddError(FString::Printf(
				TEXT("task 6.2 FAILED [Gate A defaults]: the shipped lidar.yaml defaults ")
				TEXT("were rejected (offending='%s', msg='%s')."),
				*Result.OffendingParameter, *Result.ErrorMessage));
			return false;
		}
	}

	// -----------------------------------------------------------------------
	// Part 4: Acceptance drives through the pure runner end to end. A valid
	// config passed to Initialise is adopted and the runner becomes ready; a
	// later valid ApplyConfig (an edge config) is adopted and becomes the
	// current config. This proves the accept path is wired, not just the bound
	// set validated in isolation.
	// -----------------------------------------------------------------------
	{
		FLidarPipelineRunner Runner;

		const FLidarConfig GateA; // valid
		const FSensorConfigValidationResult Init = Runner.Initialise(/*seed*/ 12345, GateA, TEXT("lidar"));
		TestTrue(TEXT("Initialise with a valid config returns bValid"), Init.bValid);
		TestTrue(TEXT("runner is ready after a valid Initialise"), Runner.IsReady());
		TestEqual(TEXT("ready runner adopted the channels of the valid config"),
			Runner.GetConfig().Channels, GateA.Channels);

		// An edge-of-bound but valid runtime update is adopted.
		FLidarConfig EdgeUpdate = GateA;
		EdgeUpdate.Channels = 128;        // upper edge, inclusive
		EdgeUpdate.DropoutProb = 1.0;     // upper edge, inclusive
		EdgeUpdate.RangeNoiseSigmaM = 0.0; // lower edge, inclusive (exactly 0)
		EdgeUpdate.HFovDeg = 360.0;       // upper edge, inclusive
		const FSensorConfigValidationResult Applied = Runner.ApplyConfig(EdgeUpdate);
		TestTrue(TEXT("ApplyConfig with a valid edge config returns bValid"), Applied.bValid);
		TestTrue(TEXT("runner stays ready after a valid ApplyConfig"), Runner.IsReady());
		TestEqual(TEXT("runner adopted the edge config's channels"),
			Runner.GetConfig().Channels, 128);
		TestEqual(TEXT("runner adopted the edge config's dropout_prob"),
			Runner.GetConfig().DropoutProb, 1.0);
		TestEqual(TEXT("runner adopted the edge config's range_noise_sigma_m"),
			Runner.GetConfig().RangeNoiseSigmaM, 0.0);
	}

	return true;
}
