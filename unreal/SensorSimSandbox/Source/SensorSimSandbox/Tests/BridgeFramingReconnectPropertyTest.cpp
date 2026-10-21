// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "Bridge/RosbridgeClient.h"
#include "Pipeline/SensorRng.h"

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 11.1 (PROPERTY tests).
//
// Property 10: Rosbridge framing round-trips binary payloads
//   *For any* binary payload, the publish op the Rosbridge_Client builds carries
//   the bytes base64-encoded inside `msg`, and base64-decoding that field
//   reproduces the ORIGINAL bytes exactly. >= 100 seeded iterations over varied
//   payloads (empty, tiny, large, every byte value), with NO live socket.
//   Validates: Requirements 6.2
//
// Property 11: Reconnect interval is bounded
//   *For any* loss sequence, every reconnect attempt the policy schedules is
//   <= 5 s apart, and the client's loop stays alive throughout the outage.
//   >= 100 seeded iterations over loss sequences of varied length, with NO live
//   socket.
//   Validates: Requirements 6.5
//
// Both drive the PURE cores (FRosbridgeFraming, FReconnectPolicy) directly,
// consistent with how SmokePublisher/TfPublisher separate framing from
// transport and how the other property tests isolate pure logic from the
// engine. The reconnect bound (5 s) and the base64 round-trip are exact; a
// failing iteration is a real bug at a known, reproducible seed, NEVER a reason
// to lower the iteration count or widen the <= 5 s bound.
// ---------------------------------------------------------------------------

namespace
{
	// Property-based tests in this project run >= 100 iterations (design
	// "Testing Strategy"). Do not lower these.
	constexpr int32 NumIterations = 128;

	// The reconnect bound fixed by Requirement 6.5. Never widened to pass.
	constexpr double MaxReconnectIntervalSeconds = 5.0;

	// Fixed base seeds so each generator loop is reproducible across runs and
	// machines, distinct per property so they exercise independent sequences.
	constexpr uint64 FramingGeneratorBaseSeed   = 0x9C2F7A41D038E65Bull;
	constexpr uint64 ReconnectGeneratorBaseSeed = 0x41B7E9032C6AF58Dull;

	FSensorRng MakeGenerator(uint64 BaseSeed, int32 Index)
	{
		return FSensorRng(BaseSeed ^ (static_cast<uint64>(Index) * 0x9E3779B97F4A7C15ull));
	}

	// Generate a binary payload spanning sizes and byte values. Iteration 0
	// pins the empty payload (edge case); iteration 1 pins a payload that
	// contains every byte value 0..255 so the base64 alphabet is fully
	// exercised; other iterations draw a random length and random bytes.
	TArray<uint8> GeneratePayload(FSensorRng& Gen, int32 Iteration)
	{
		TArray<uint8> Bytes;

		if (Iteration == 0)
		{
			return Bytes; // empty payload
		}
		if (Iteration == 1)
		{
			Bytes.Reserve(256);
			for (int32 i = 0; i < 256; ++i)
			{
				Bytes.Add(static_cast<uint8>(i));
			}
			return Bytes;
		}

		// Length spans 1 .. ~4 KB so single-byte, non-multiple-of-3 (base64
		// padding), and large payloads are all covered.
		const int32 Length = 1 + static_cast<int32>(Gen.NextUInt64() % 4096);
		Bytes.Reserve(Length);
		for (int32 i = 0; i < Length; ++i)
		{
			Bytes.Add(static_cast<uint8>(Gen.NextUInt64() & 0xFF));
		}
		return Bytes;
	}
}

// ===========================================================================
// Property 10: Rosbridge framing round-trips binary payloads (base64).
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxBridgeProperty10Test,
	"SensorSimSandbox.Bridge.Property10_FramingBinaryRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxBridgeProperty10Test::RunTest(const FString& Parameters)
{
	int32 CasesChecked = 0;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FSensorRng Gen = MakeGenerator(FramingGeneratorBaseSeed, Iteration);
		const TArray<uint8> Original = GeneratePayload(Gen, Iteration);

		// Build the exact publish op the live client would send: the binary
		// field base64-encoded inside `msg`. PointCloud2.data / image data use
		// this shape (field name "data").
		const FString Topic = TEXT("/lidar/points");
		const FString FieldName = TEXT("data");
		const FString PublishOp = FRosbridgeFraming::BuildBinaryPublishJson(Topic, FieldName, Original);

		// It must be a well-formed rosbridge v2 publish op.
		if (!PublishOp.Contains(TEXT("\"op\":\"publish\"")) || !PublishOp.Contains(TEXT("\"msg\":")))
		{
			AddError(FString::Printf(
				TEXT("Property 10 FAILED at iteration %d: built op is not a rosbridge publish op: %s"),
				Iteration, *PublishOp));
			return false;
		}

		// Extract and base64-decode msg.data from the SERIALIZED op (not a side
		// channel) and require it to reproduce the original bytes exactly.
		TArray<uint8> RoundTrip;
		if (!FRosbridgeFraming::ExtractBinaryField(PublishOp, FieldName, RoundTrip))
		{
			AddError(FString::Printf(
				TEXT("Property 10 FAILED at iteration %d: could not extract/decode msg.%s from op (len=%d)."),
				Iteration, *FieldName, Original.Num()));
			return false;
		}

		if (RoundTrip != Original)
		{
			AddError(FString::Printf(
				TEXT("Property 10 FAILED at iteration %d: base64 round-trip changed the bytes ")
				TEXT("(original len=%d, decoded len=%d)."),
				Iteration, Original.Num(), RoundTrip.Num()));
			return false;
		}

		++CasesChecked;
	}

	TestTrue(TEXT("framing round-trip checked at least 100 generated payloads"), CasesChecked >= 100);
	return true;
}

// ===========================================================================
// Property 11: Reconnect interval is bounded (<= 5 s), client stays alive.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxBridgeProperty11Test,
	"SensorSimSandbox.Bridge.Property11_ReconnectBounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxBridgeProperty11Test::RunTest(const FString& Parameters)
{
	int32 CasesChecked = 0;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FSensorRng Gen = MakeGenerator(ReconnectGeneratorBaseSeed, Iteration);

		// A fresh policy per loss sequence, with a randomized initial interval
		// and growth multiplier (including aggressive multipliers that would
		// blow past 5 s if the cap were not enforced). The policy must clamp
		// every interval to <= 5 s regardless.
		const double Initial = 0.01 + Gen.NextUniform() * 6.0;   // [0.01, 6.01) — some already over cap
		const double Multiplier = 1.0 + Gen.NextUniform() * 8.0; // [1, 9)
		FReconnectPolicy Policy(Initial, Multiplier);

		// A loss sequence of varied length: a prolonged outage with many failed
		// attempts. 1 .. 500 attempts so long outages (where naive backoff would
		// exceed 5 s) are covered.
		const int32 NumAttempts = 1 + static_cast<int32>(Gen.NextUInt64() % 500);

		for (int32 Attempt = 0; Attempt < NumAttempts; ++Attempt)
		{
			// Peek must agree with the value actually returned, and both must be
			// within the bound.
			const double Peek = Policy.PeekNextIntervalSeconds();
			const double Interval = Policy.NextIntervalSeconds();

			if (Interval <= 0.0)
			{
				AddError(FString::Printf(
					TEXT("Property 11 FAILED at iteration %d attempt %d: non-positive reconnect interval %.6f ")
					TEXT("(initial=%.4f, mult=%.4f)."),
					Iteration, Attempt, Interval, Initial, Multiplier));
				return false;
			}

			if (Interval > MaxReconnectIntervalSeconds)
			{
				AddError(FString::Printf(
					TEXT("Property 11 FAILED at iteration %d attempt %d: reconnect interval %.6f s exceeds the ")
					TEXT("%.1f s bound (initial=%.4f, mult=%.4f)."),
					Iteration, Attempt, Interval, MaxReconnectIntervalSeconds, Initial, Multiplier));
				return false;
			}

			if (FMath::Abs(Peek - Interval) > 1e-9)
			{
				AddError(FString::Printf(
					TEXT("Property 11 FAILED at iteration %d attempt %d: peek %.9f disagrees with returned %.9f."),
					Iteration, Attempt, Peek, Interval));
				return false;
			}

			// The client's loop (and the editor) must stay alive throughout the
			// entire outage, no matter how many attempts have failed.
			if (!Policy.IsAlive())
			{
				AddError(FString::Printf(
					TEXT("Property 11 FAILED at iteration %d attempt %d: client reported not-alive mid-outage."),
					Iteration, Attempt));
				return false;
			}
		}

		// A successful reconnect resets the policy; it must still be alive and
		// its next interval must again be within the bound.
		Policy.Reset();
		if (!Policy.IsAlive() || Policy.PeekNextIntervalSeconds() > MaxReconnectIntervalSeconds)
		{
			AddError(FString::Printf(
				TEXT("Property 11 FAILED at iteration %d: post-reset state invalid (alive=%d, next=%.6f)."),
				Iteration, Policy.IsAlive() ? 1 : 0, Policy.PeekNextIntervalSeconds()));
			return false;
		}

		++CasesChecked;
	}

	TestTrue(TEXT("reconnect bound checked at least 100 generated loss sequences"), CasesChecked >= 100);
	return true;
}
