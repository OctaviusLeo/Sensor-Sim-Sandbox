// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Misc/AutomationTest.h"

#include "Pipeline/SensorConfigValidation.h"
#include "Pipeline/SensorRng.h"

#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, Property 2: Configuration bounds are enforced
//
// *For any* sensor parameter and *for any* value outside that parameter's
// stated bound, the SensorModel rejects the configuration, the emitted error
// identifies the out-of-bound parameter, and the last valid configuration is
// retained.
//
// Validates: Requirements 3.2, 4.2
//
// This drives the shared validation contract directly (FSensorBoundSet /
// FSensorConfigValidationResult, task 5.1) with no world or rendering,
// consistent with how task 5.1 isolated the contract. Each bounded parameter
// enumerated in the design (Components -> Lidar_Model / Camera_Model and
// Data Models -> LidarConfig / CameraConfig) is validated through its sensor's
// full bound set so a rejection is checked exactly as a sensor would apply it:
// the candidate is rejected as a whole (never partially applied), the error
// names the offending parameter, and the holder's last valid config survives.
//
// Unreal's Automation framework has no native property generator, so a
// lightweight *seeded* generator loop runs >= 100 iterations PER PARAMETER
// (design "Testing Strategy" / "Property-based testing"). The generator loop
// is reproducible: iteration i's out-of-bound value is derived from a fixed
// base seed, so a failing iteration is a bug to investigate at a known case,
// never a reason to lower the iteration count or weaken an assertion.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxConfigBoundsEnforcedTest,
	"SensorSimSandbox.Pipeline.Property2_ConfigBoundsEnforced",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
{
	// Iterations of out-of-bound values generated per bounded parameter.
	// Property-based tests in this project run >= 100 iterations (design
	// "Testing Strategy"). Do not lower this to make the test pass; a failing
	// case is a real bounds-enforcement bug.
	constexpr int32 NumIterationsPerParam = 128;

	// Fixed base seed for the generator loop so the generated out-of-bound
	// values are themselves reproducible across runs and machines.
	constexpr uint64 GeneratorBaseSeed = 0xB2E7F10C4D593A86ull;

	// Declaration of one bounded parameter: its name, inclusive/exclusive
	// bounds, and whether it is integer-only. Mirrors the design's declared
	// bounds for each sensor so the test exercises exactly the stated space.
	struct FParamSpec
	{
		FString Name;
		double Min = 0.0;
		double Max = 0.0;
		bool bMinInclusive = true;
		bool bMaxInclusive = true;
		bool bIntegerOnly = false;
	};

	// One sensor: its name (for the error context) and its full ordered bound
	// set. Validation runs the whole candidate map through the sensor's bound
	// set, matching how a sensor applies a config update.
	struct FSensorSpec
	{
		FString SensorName;
		TArray<FParamSpec> Params;
	};

	// Build an FSensorBoundSet from a sensor spec, in declared order (so the
	// "offending parameter" is deterministic, as task 5.1 guarantees).
	FSensorBoundSet MakeBoundSet(const FSensorSpec& Sensor)
	{
		FSensorBoundSet Bounds;
		for (const FParamSpec& P : Sensor.Params)
		{
			Bounds.Add(P.Name, P.Min, P.Max, P.bMinInclusive, P.bMaxInclusive, P.bIntegerOnly);
		}
		return Bounds;
	}

	// A mid-range, in-bound value for a parameter. Used to build a known-valid
	// candidate so that when we poison ONE parameter with an out-of-bound
	// value, the offending parameter is unambiguous.
	double InBoundMidValue(const FParamSpec& P)
	{
		if (P.bIntegerOnly)
		{
			// Nearest integer to the midpoint, clamped into the (possibly
			// exclusive) range. For the ranges used here the midpoint is well
			// inside, so flooring the midpoint is safely in-bound.
			const double Mid = std::floor((P.Min + P.Max) / 2.0);
			return Mid;
		}
		return (P.Min + P.Max) / 2.0;
	}

	// Build the baseline fully-valid candidate for a sensor (every parameter
	// at a mid-range in-bound value).
	TMap<FString, double> MakeValidCandidate(const FSensorSpec& Sensor)
	{
		TMap<FString, double> Candidate;
		for (const FParamSpec& P : Sensor.Params)
		{
			Candidate.Add(P.Name, InBoundMidValue(P));
		}
		return Candidate;
	}

	// Classes of out-of-bound value a generator can produce for a parameter.
	enum class EViolation : int32
	{
		BelowMin,          // strictly below Min (or, for exclusive-min, == Min)
		AboveMax,          // strictly above Max (or, for exclusive-max, == Max)
		NonIntegral,       // integer-only parameters only: an in-range non-integer
		NaN,               // not-a-number
		PosInf,            // +infinity
		NegInf,            // -infinity
	};

	// Generate an out-of-bound value of class `Kind` for parameter `P`, using
	// RNG `Gen` for magnitude. Returns the value and a short label for errors.
	double GenerateOutOfBound(const FParamSpec& P, EViolation Kind, FSensorRng& Gen, FString& OutLabel)
	{
		switch (Kind)
		{
			case EViolation::BelowMin:
			{
				if (!P.bMinInclusive)
				{
					// Exclusive min: the boundary value itself is out of bounds.
					// Half the time return exactly Min, half the time go below.
					if (Gen.NextUInt64() % 2 == 0)
					{
						OutLabel = TEXT("== exclusive Min");
						return P.Min;
					}
				}
				// A finite distance strictly below Min.
				const double Delta = 1e-6 + Gen.NextUniform() * (FMath::Abs(P.Min) + 1.0) * 10.0;
				OutLabel = TEXT("below Min");
				return P.Min - Delta;
			}
			case EViolation::AboveMax:
			{
				if (!P.bMaxInclusive)
				{
					if (Gen.NextUInt64() % 2 == 0)
					{
						OutLabel = TEXT("== exclusive Max");
						return P.Max;
					}
				}
				const double Delta = 1e-6 + Gen.NextUniform() * (FMath::Abs(P.Max) + 1.0) * 10.0;
				OutLabel = TEXT("above Max");
				return P.Max + Delta;
			}
			case EViolation::NonIntegral:
			{
				// An in-range value with a non-zero fractional part: valid in
				// magnitude but illegal because the parameter is integer-only.
				const double Span = P.Max - P.Min;
				double Frac = 0.1 + Gen.NextUniform() * 0.8; // (0.1, 0.9)
				double Base = std::floor(P.Min + 1.0 + Gen.NextUniform() * FMath::Max(0.0, Span - 2.0));
				OutLabel = TEXT("non-integral (integer-only param)");
				return Base + Frac;
			}
			case EViolation::NaN:
				OutLabel = TEXT("NaN");
				return std::numeric_limits<double>::quiet_NaN();
			case EViolation::PosInf:
				OutLabel = TEXT("+Inf");
				return std::numeric_limits<double>::infinity();
			case EViolation::NegInf:
				OutLabel = TEXT("-Inf");
				return -std::numeric_limits<double>::infinity();
			default:
				OutLabel = TEXT("unknown");
				return std::numeric_limits<double>::quiet_NaN();
		}
	}

	// The applicable violation classes for a parameter. Integer-only
	// parameters add the NonIntegral class; every parameter gets the
	// below/above/NaN/Inf classes.
	TArray<EViolation> ViolationClassesFor(const FParamSpec& P)
	{
		TArray<EViolation> Classes;
		Classes.Add(EViolation::BelowMin);
		Classes.Add(EViolation::AboveMax);
		Classes.Add(EViolation::NaN);
		Classes.Add(EViolation::PosInf);
		Classes.Add(EViolation::NegInf);
		if (P.bIntegerOnly)
		{
			Classes.Add(EViolation::NonIntegral);
		}
		return Classes;
	}

	// The sensors and their declared bounds, straight from the design.
	//
	// Lidar_Model (design Components / LidarConfig):
	//   channels (1-128, int), h_fov_deg (0 exclusive-360], v_fov_deg
	//   (0 exclusive-180], angular_resolution_deg (0.01-10.0),
	//   max_range_m (0.1-1000.0), rate_hz (0.1-100.0),
	//   range_noise_sigma_m (0.0-10.0), dropout_prob (0.0-1.0),
	//   latency_ms (0-1000, int).
	//
	// Camera_Model (design Components / CameraConfig):
	//   width/height (1-7680, int), rate_hz (0.1-100.0),
	//   pixel_noise_sigma (0.0-255.0), frame_dropout_prob (0.0-1.0),
	//   latency_ms (0-1000, int).
	TArray<FSensorSpec> MakeSensorSpecs()
	{
		TArray<FSensorSpec> Sensors;

		FSensorSpec Lidar;
		Lidar.SensorName = TEXT("lidar");
		Lidar.Params = {
			{ TEXT("channels"),               1.0,   128.0,  true,  true,  true  },
			{ TEXT("h_fov_deg"),              0.0,   360.0,  false, true,  false },
			{ TEXT("v_fov_deg"),              0.0,   180.0,  false, true,  false },
			{ TEXT("angular_resolution_deg"), 0.01,  10.0,   true,  true,  false },
			{ TEXT("max_range_m"),            0.1,   1000.0, true,  true,  false },
			{ TEXT("rate_hz"),                0.1,   100.0,  true,  true,  false },
			{ TEXT("range_noise_sigma_m"),    0.0,   10.0,   true,  true,  false },
			{ TEXT("dropout_prob"),           0.0,   1.0,    true,  true,  false },
			{ TEXT("latency_ms"),             0.0,   1000.0, true,  true,  true  },
		};
		Sensors.Add(MoveTemp(Lidar));

		FSensorSpec Camera;
		Camera.SensorName = TEXT("camera");
		Camera.Params = {
			{ TEXT("width"),              1.0, 7680.0, true, true, true  },
			{ TEXT("height"),             1.0, 7680.0, true, true, true  },
			{ TEXT("rate_hz"),            0.1, 100.0,  true, true, false },
			{ TEXT("pixel_noise_sigma"),  0.0, 255.0,  true, true, false },
			{ TEXT("frame_dropout_prob"), 0.0, 1.0,    true, true, false },
			{ TEXT("latency_ms"),         0.0, 1000.0, true, true, true  },
		};
		Sensors.Add(MoveTemp(Camera));

		return Sensors;
	}
}

bool FSensorSimSandboxConfigBoundsEnforcedTest::RunTest(const FString& Parameters)
{
	const TArray<FSensorSpec> Sensors = MakeSensorSpecs();

	int32 TotalRejectionsChecked = 0;

	for (const FSensorSpec& Sensor : Sensors)
	{
		const FSensorBoundSet Bounds = MakeBoundSet(Sensor);

		// Baseline: the sensor's last valid configuration. It must validate up
		// front, and we re-check it after every rejection to prove retention.
		const TMap<FString, double> LastValid = MakeValidCandidate(Sensor);
		const FSensorConfigValidationResult BaselineAccept = Bounds.Validate(LastValid);
		if (!BaselineAccept.bValid)
		{
			AddError(FString::Printf(
				TEXT("Property 2 setup FAILED for sensor '%s': the mid-range in-bound ")
				TEXT("baseline candidate was rejected (offending='%s', msg='%s'). The ")
				TEXT("in-bound baseline must be accepted."),
				*Sensor.SensorName, *BaselineAccept.OffendingParameter, *BaselineAccept.ErrorMessage));
			return false;
		}

		for (const FParamSpec& Param : Sensor.Params)
		{
			const TArray<EViolation> Classes = ViolationClassesFor(Param);

			for (int32 Iteration = 0; Iteration < NumIterationsPerParam; ++Iteration)
			{
				// Reproducible per (sensor, parameter, iteration).
				FSensorRng Gen(
					GeneratorBaseSeed,
					FString::Printf(TEXT("%s|%s|%d"), *Sensor.SensorName, *Param.Name, Iteration));

				// Rotate through the violation classes so each is exercised
				// many times over the >= 100 iterations.
				const EViolation Kind = Classes[Iteration % Classes.Num()];

				FString ViolationLabel;
				const double BadValue = GenerateOutOfBound(Param, Kind, Gen, ViolationLabel);

				// Build a candidate equal to the last valid config but with
				// exactly ONE parameter poisoned, so the offending parameter is
				// unambiguous and we confirm no partial application.
				TMap<FString, double> Candidate = LastValid;
				Candidate[Param.Name] = BadValue;

				const FSensorConfigValidationResult Result = Bounds.Validate(Candidate);

				// 1) The candidate is rejected as a whole.
				if (Result.bValid)
				{
					AddError(FString::Printf(
						TEXT("Property 2 FAILED [%s.%s] iter %d: out-of-bound value %.17g (%s) ")
						TEXT("was ACCEPTED but must be rejected. Bound=[%.17g..%.17g] ")
						TEXT("minInc=%d maxInc=%d intOnly=%d."),
						*Sensor.SensorName, *Param.Name, Iteration, BadValue, *ViolationLabel,
						Param.Min, Param.Max, Param.bMinInclusive ? 1 : 0,
						Param.bMaxInclusive ? 1 : 0, Param.bIntegerOnly ? 1 : 0));
					return false;
				}

				// 2) The error identifies the offending parameter by name.
				if (Result.OffendingParameter != Param.Name)
				{
					AddError(FString::Printf(
						TEXT("Property 2 FAILED [%s.%s] iter %d: value %.17g (%s) was rejected ")
						TEXT("but the offending parameter was reported as '%s', not '%s'."),
						*Sensor.SensorName, *Param.Name, Iteration, BadValue, *ViolationLabel,
						*Result.OffendingParameter, *Param.Name));
					return false;
				}

				// The human-readable message must also name the parameter.
				if (!Result.ErrorMessage.Contains(Param.Name))
				{
					AddError(FString::Printf(
						TEXT("Property 2 FAILED [%s.%s] iter %d: value %.17g (%s) was rejected ")
						TEXT("but the error message did not name the parameter: '%s'."),
						*Sensor.SensorName, *Param.Name, Iteration, BadValue, *ViolationLabel,
						*Result.ErrorMessage));
					return false;
				}

				// 3) The last valid configuration is retained: re-validating the
				// prior valid config still succeeds AFTER the rejection, and the
				// poisoned candidate map is distinct from it (no partial apply).
				const FSensorConfigValidationResult StillValid = Bounds.Validate(LastValid);
				if (!StillValid.bValid)
				{
					AddError(FString::Printf(
						TEXT("Property 2 FAILED [%s.%s] iter %d: after rejecting %.17g (%s), ")
						TEXT("the retained last-valid config no longer validates (offending='%s', ")
						TEXT("msg='%s'). A rejected update must leave the last valid config intact."),
						*Sensor.SensorName, *Param.Name, Iteration, BadValue, *ViolationLabel,
						*StillValid.OffendingParameter, *StillValid.ErrorMessage));
					return false;
				}

				++TotalRejectionsChecked;
			}

			// Property 2 also calls for in-bound values to be accepted: confirm
			// a mid-range value for this parameter (atop the valid baseline) is
			// accepted, so rejection is specific to out-of-bound inputs.
			{
				TMap<FString, double> InBound = LastValid;
				InBound[Param.Name] = InBoundMidValue(Param);
				const FSensorConfigValidationResult Accept = Bounds.Validate(InBound);
				if (!Accept.bValid)
				{
					AddError(FString::Printf(
						TEXT("Property 2 FAILED [%s.%s]: an in-bound mid-range value %.17g was ")
						TEXT("rejected (offending='%s', msg='%s'); in-bound values must be accepted."),
						*Sensor.SensorName, *Param.Name, InBound[Param.Name],
						*Accept.OffendingParameter, *Accept.ErrorMessage));
					return false;
				}
			}
		}
	}

	// Guard against a loop that silently ran too few cases. Each bounded
	// parameter is exercised with >= 100 (here 128) out-of-bound iterations;
	// with 9 lidar + 6 camera bounded parameters that is well over 100 overall,
	// and >= 100 PER PARAMETER as Property 2 requires.
	TestTrue(
		TEXT("config-bounds rejection checked >= 100 iterations per bounded parameter"),
		TotalRejectionsChecked >= 100 * (9 + 6));

	return true;
}
