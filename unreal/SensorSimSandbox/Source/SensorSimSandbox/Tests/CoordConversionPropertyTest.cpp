// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "Coords/Rep103.h"
#include "Pipeline/SensorRng.h"

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 9.2 (PROPERTY tests).
//
// Property 12: Position conversion matches REP 103
//   *For any* Unreal position (x, y, z) in centimeters, the converted REP 103
//   position equals (x/100, -y/100, z/100) to within 1e-6 meters.
//   Validates: Requirements 7.1, 7.2
//
// Property 13: Rotation conversion matches REP 103
//   *For any* Unreal rotation, the converted REP 103 orientation matches the
//   expected right-handed orientation (w, -x, y, -z) to within 1e-6 radians,
//   and converting forward then back is the identity.
//   Validates: Requirements 7.3
//
// These drive the PURE converter (FRep103) directly with NO world, rendering,
// or live WebSocket, consistent with how the other pure cores are isolated.
// Task 9.1's CoordConversionTfTest pins the fixed mapping on hand-worked
// values and exercises the TF publish path; this file provides the exhaustive
// >= 100-iteration property coverage of the 1e-6 conversion tolerance.
//
// Unreal's Automation framework has no native property generator, so a
// lightweight *seeded* generator loop drives >= 100 iterations (design
// "Testing Strategy" / "Property-based testing"). The generator loop is itself
// reproducible: iteration i's inputs are derived from a fixed base seed, so a
// failing iteration is a bug to investigate at a known case in FRep103, never
// a reason to lower the iteration count or weaken the tolerance.
// ---------------------------------------------------------------------------

namespace
{
	// Number of generated cases. Property-based tests in this project run
	// >= 100 iterations (design "Testing Strategy"). Do not lower this to make
	// a test pass; a failing case is a real conversion bug in FRep103.
	constexpr int32 NumIterations = 128;

	// Tolerances fixed by the design / Requirement 7.2, 7.3. Never widen these
	// to pass; the double-precision converter round-trips far inside them.
	constexpr double PositionToleranceM = 1e-6;    // meters
	constexpr double RotationToleranceRad = 1e-6;  // radians (angular error)

	// Fixed base seeds for the generator loops so the generated cases are
	// themselves reproducible across runs and machines. Distinct per property
	// so the two tests exercise independent input sequences.
	constexpr uint64 PositionGeneratorBaseSeed = 0x5F1CA7D2903BE641ull;
	constexpr uint64 RotationGeneratorBaseSeed = 0xB3E9206C41A87D5Full;

	// Per-iteration seed mix (SplitMix64 odd constant) so each iteration gets
	// an independent, reproducible sub-stream from the base seed.
	FSensorRng MakeGenerator(uint64 BaseSeed, int32 Index)
	{
		return FSensorRng(BaseSeed ^ (static_cast<uint64>(Index) * 0x9E3779B97F4A7C15ull));
	}

	// Generate an Unreal position (centimeters) spanning a wide range,
	// including negatives, small magnitudes, and large magnitudes, so the
	// mapping is exercised well beyond the hand-worked spot checks.
	FVector GeneratePositionCm(FSensorRng& Gen)
	{
		// Choose a per-component magnitude scale so some cases are small
		// (sub-millimeter in cm) and some are large (hundreds of kilometers in
		// cm), keeping the /100 rescale honest across magnitudes.
		auto Component = [&Gen]() -> double
		{
			// Scale decade in [1e-3, 1e7] cm, signed.
			const double Exponent = -3.0 + Gen.NextUniform() * 10.0; // [-3, 7)
			const double Magnitude = FMath::Pow(10.0, Exponent);
			const double Signed = (Gen.NextUniform() < 0.5) ? -1.0 : 1.0;
			// A uniform multiplier in [0, 1) within the chosen decade.
			return Signed * Magnitude * Gen.NextUniform();
		};

		// Occasionally force an exact zero component so (0 -> -0) handedness
		// flips are covered too.
		const double X = (Gen.NextUInt64() % 9 == 0) ? 0.0 : Component();
		const double Y = (Gen.NextUInt64() % 9 == 0) ? 0.0 : Component();
		const double Z = (Gen.NextUInt64() % 9 == 0) ? 0.0 : Component();
		return FVector(X, Y, Z);
	}

	// Generate a NORMALIZED Unreal quaternion. Draw four standard normals and
	// normalize (uniform-ish over the unit sphere / SO(3) double cover); reject
	// the (near-)zero vector so normalization is well-defined.
	FQuat GenerateUnitQuat(FSensorRng& Gen)
	{
		for (int32 Attempt = 0; Attempt < 16; ++Attempt)
		{
			const double A = Gen.NextGaussian();
			const double B = Gen.NextGaussian();
			const double C = Gen.NextGaussian();
			const double D = Gen.NextGaussian();
			const double NormSq = A * A + B * B + C * C + D * D;
			if (NormSq > 1e-12)
			{
				const double InvNorm = 1.0 / FMath::Sqrt(NormSq);
				// FQuat ctor is (X, Y, Z, W).
				FQuat Q(A * InvNorm, B * InvNorm, C * InvNorm, D * InvNorm);
				return Q;
			}
		}
		// Degenerate fallback (effectively never hit): identity rotation.
		return FQuat::Identity;
	}

	// Angular distance (radians) between two quaternions, accounting for the
	// double cover (q and -q are the same rotation). Returns a value in
	// [0, pi]. Used to assert the forward-then-back round trip is the identity
	// to within the 1e-6 rad tolerance Property 13 requires.
	double AngleBetweenRad(const FQuat& A, const FQuat& B)
	{
		const double Dot = static_cast<double>(A.X) * B.X
			+ static_cast<double>(A.Y) * B.Y
			+ static_cast<double>(A.Z) * B.Z
			+ static_cast<double>(A.W) * B.W;
		// |dot| in [0, 1]; angle = 2 * acos(|dot|). Clamp for numerical safety.
		const double AbsDot = FMath::Clamp(FMath::Abs(Dot), 0.0, 1.0);
		return 2.0 * FMath::Acos(AbsDot);
	}
}

// ---------------------------------------------------------------------------
// Property 12: Position conversion matches REP 103 (within 1e-6 m).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxCoordProperty12Test,
	"SensorSimSandbox.Coords.Property12_PositionConversion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxCoordProperty12Test::RunTest(const FString& Parameters)
{
	int32 CasesChecked = 0;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FSensorRng Gen = MakeGenerator(PositionGeneratorBaseSeed, Iteration);
		const FVector UeCm = GeneratePositionCm(Gen);

		const FRep103Position Converted = FRep103::ConvertPosition(UeCm);

		// Expected fixed REP 103 mapping (design "Coordinate conversion"):
		// (x/100, -y/100, z/100).
		const double ExpectedX = static_cast<double>(UeCm.X) / 100.0;
		const double ExpectedY = -static_cast<double>(UeCm.Y) / 100.0;
		const double ExpectedZ = static_cast<double>(UeCm.Z) / 100.0;

		const double ErrX = FMath::Abs(Converted.X - ExpectedX);
		const double ErrY = FMath::Abs(Converted.Y - ExpectedY);
		const double ErrZ = FMath::Abs(Converted.Z - ExpectedZ);

		if (ErrX > PositionToleranceM || ErrY > PositionToleranceM || ErrZ > PositionToleranceM)
		{
			AddError(FString::Printf(
				TEXT("Property 12 FAILED at iteration %d: position conversion off by more than %.1e m ")
				TEXT("(ue_cm=(%.17g, %.17g, %.17g); got=(%.17g, %.17g, %.17g); ")
				TEXT("expected=(%.17g, %.17g, %.17g); err=(%.3e, %.3e, %.3e))."),
				Iteration, PositionToleranceM,
				UeCm.X, UeCm.Y, UeCm.Z,
				Converted.X, Converted.Y, Converted.Z,
				ExpectedX, ExpectedY, ExpectedZ,
				ErrX, ErrY, ErrZ));
			return false;
		}

		// The inverse must round-trip back to the original Unreal position
		// within 1e-6 m (design: convert-then-inverse identity check).
		const FVector RoundTrip = FRep103::InvertPosition(Converted);
		const double RtErrX = FMath::Abs(static_cast<double>(RoundTrip.X) - static_cast<double>(UeCm.X));
		const double RtErrY = FMath::Abs(static_cast<double>(RoundTrip.Y) - static_cast<double>(UeCm.Y));
		const double RtErrZ = FMath::Abs(static_cast<double>(RoundTrip.Z) - static_cast<double>(UeCm.Z));

		// The round-trip tolerance scales with magnitude (the forward step
		// divides by 100 and the inverse multiplies back), so compare in the
		// same meter frame the property states by converting the cm error to m.
		const double RtErrXm = RtErrX / 100.0;
		const double RtErrYm = RtErrY / 100.0;
		const double RtErrZm = RtErrZ / 100.0;

		if (RtErrXm > PositionToleranceM || RtErrYm > PositionToleranceM || RtErrZm > PositionToleranceM)
		{
			AddError(FString::Printf(
				TEXT("Property 12 FAILED at iteration %d: position round-trip off by more than %.1e m ")
				TEXT("(ue_cm=(%.17g, %.17g, %.17g); round_trip=(%.17g, %.17g, %.17g); ")
				TEXT("err_m=(%.3e, %.3e, %.3e))."),
				Iteration, PositionToleranceM,
				UeCm.X, UeCm.Y, UeCm.Z,
				RoundTrip.X, RoundTrip.Y, RoundTrip.Z,
				RtErrXm, RtErrYm, RtErrZm));
			return false;
		}

		++CasesChecked;
	}

	// Guard against a loop that silently ran zero cases.
	TestTrue(TEXT("position conversion checked at least 100 generated cases"), CasesChecked >= 100);

	return true;
}

// ---------------------------------------------------------------------------
// Property 13: Rotation conversion matches REP 103 (within 1e-6 rad,
// forward-then-back identity).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxCoordProperty13Test,
	"SensorSimSandbox.Coords.Property13_RotationConversion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxCoordProperty13Test::RunTest(const FString& Parameters)
{
	int32 CasesChecked = 0;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FSensorRng Gen = MakeGenerator(RotationGeneratorBaseSeed, Iteration);
		const FQuat UeQuat = GenerateUnitQuat(Gen);

		const FRep103Quat Converted = FRep103::ConvertRotation(UeQuat);

		// Expected fixed REP 103 mapping (design "Coordinate conversion"):
		// q_ros = (w, -x, y, -z) for a UE quaternion (w, x, y, z).
		const double ExpectedW = static_cast<double>(UeQuat.W);
		const double ExpectedX = -static_cast<double>(UeQuat.X);
		const double ExpectedY = static_cast<double>(UeQuat.Y);
		const double ExpectedZ = -static_cast<double>(UeQuat.Z);

		const double ErrW = FMath::Abs(Converted.W - ExpectedW);
		const double ErrX = FMath::Abs(Converted.X - ExpectedX);
		const double ErrY = FMath::Abs(Converted.Y - ExpectedY);
		const double ErrZ = FMath::Abs(Converted.Z - ExpectedZ);

		// Component mapping is exact copies/negations, so it holds far inside
		// the 1e-6 tolerance; a miss here is a mapping bug in FRep103.
		if (ErrW > RotationToleranceRad || ErrX > RotationToleranceRad
			|| ErrY > RotationToleranceRad || ErrZ > RotationToleranceRad)
		{
			AddError(FString::Printf(
				TEXT("Property 13 FAILED at iteration %d: rotation mapping off by more than %.1e ")
				TEXT("(ue_quat (w,x,y,z)=(%.17g, %.17g, %.17g, %.17g); ")
				TEXT("got (w,x,y,z)=(%.17g, %.17g, %.17g, %.17g); ")
				TEXT("expected=(%.17g, %.17g, %.17g, %.17g))."),
				Iteration, RotationToleranceRad,
				UeQuat.W, UeQuat.X, UeQuat.Y, UeQuat.Z,
				Converted.W, Converted.X, Converted.Y, Converted.Z,
				ExpectedW, ExpectedX, ExpectedY, ExpectedZ));
			return false;
		}

		// Forward-then-back identity: InvertRotation(ConvertRotation(q)) == q,
		// measured as angular error (radians) and accepting the quaternion
		// double cover (q and -q are the same rotation).
		const FQuat RoundTrip = FRep103::InvertRotation(Converted);
		const double AngleErr = AngleBetweenRad(RoundTrip, UeQuat);

		if (AngleErr > RotationToleranceRad)
		{
			AddError(FString::Printf(
				TEXT("Property 13 FAILED at iteration %d: rotation round-trip angular error %.3e rad ")
				TEXT("exceeds %.1e rad ")
				TEXT("(ue_quat (x,y,z,w)=(%.17g, %.17g, %.17g, %.17g); ")
				TEXT("round_trip (x,y,z,w)=(%.17g, %.17g, %.17g, %.17g))."),
				Iteration, AngleErr, RotationToleranceRad,
				UeQuat.X, UeQuat.Y, UeQuat.Z, UeQuat.W,
				RoundTrip.X, RoundTrip.Y, RoundTrip.Z, RoundTrip.W));
			return false;
		}

		++CasesChecked;
	}

	// Guard against a loop that silently ran zero cases.
	TestTrue(TEXT("rotation conversion checked at least 100 generated cases"), CasesChecked >= 100);

	return true;
}
