// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "Pipeline/SensorPipeline.h"
#include "Pipeline/SensorRng.h"
#include "Pipeline/LatencyQueue.h"

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, Property 1: Pipeline determinism under fixed seed
//
// *For any* run seed and sensor configuration, running the seeded sensor
// pipeline twice produces byte-for-byte identical output, excluding timestamps
// and absolute file paths.
//
// Validates: Requirements 0.3
//
// This drives the PURE pipeline classes directly (FSensorPipeline over a
// synthetic truth scan, then TLatencyQueue on a synthetic clock) with no
// world or rendering, consistent with how task 5.1 isolated the pipeline.
//
// Unreal's Automation framework has no native property generator, so a
// lightweight *seeded* generator loop drives >= 100 iterations (design
// "Testing Strategy" / "Property-based testing"). The generator loop is itself
// reproducible: iteration i's (seed, config, scan) are derived from a fixed
// base seed, so a failing iteration is a bug to investigate at a known case,
// never a reason to lower the iteration count or change the seeds.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxPipelineDeterminismTest,
	"SensorSimSandbox.Pipeline.Property1_DeterminismUnderFixedSeed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
{
	// Number of generated cases. Property-based tests in this project run
	// >= 100 iterations (design "Testing Strategy"). Do not lower this to make
	// the test pass; a failing case is a real determinism bug.
	constexpr int32 NumIterations = 128;

	// Fixed base seed for the generator loop so the generated cases are
	// themselves reproducible across runs and machines.
	constexpr uint64 GeneratorBaseSeed = 0xA17C9D3F2B6E5041ull;

	// One generated test case: a run seed, a sensor config, and a synthetic
	// truth scan (a mix of present ranges and no-return gaps).
	struct FGeneratedCase
	{
		int64 RunSeed = 0;
		FString SensorName;
		double Sigma = 0.0;
		double DropoutProb = 0.0;
		double LatencySeconds = 0.0;
		TArray<FSensorValue> Truth;
	};

	// Generate case `Index` deterministically from the base seed. A dedicated
	// FSensorRng seeded per iteration keeps the generated inputs reproducible
	// and independent of the pipeline's own RNG stream.
	FGeneratedCase GenerateCase(int32 Index)
	{
		FGeneratedCase Case;

		FSensorRng Gen(static_cast<uint64>(GeneratorBaseSeed) ^ (static_cast<uint64>(Index) * 0x9E3779B97F4A7C15ull));

		// Run seed spans the full int64 range (including negatives) so the
		// determinism claim is exercised over arbitrary seeds, not just small ones.
		Case.RunSeed = static_cast<int64>(Gen.NextUInt64());

		// Vary the sensor name so the derived per-sensor seed path varies too.
		Case.SensorName = FString::Printf(TEXT("sensor_%d"), static_cast<int32>(Gen.NextUInt64() % 7));

		// Config within the pipeline's bounds. Include sigma = 0 (exact
		// passthrough, no RNG draw) and dropout extremes 0 and 1 some of the
		// time so those branches are covered by the determinism check.
		switch (static_cast<int32>(Gen.NextUInt64() % 4))
		{
			case 0:  Case.Sigma = 0.0; break;                       // noiseless passthrough
			default: Case.Sigma = Gen.NextUniform() * 2.0; break;   // [0, 2) m
		}

		switch (static_cast<int32>(Gen.NextUInt64() % 5))
		{
			case 0:  Case.DropoutProb = 0.0; break;                 // never drop
			case 1:  Case.DropoutProb = 1.0; break;                 // always drop
			default: Case.DropoutProb = Gen.NextUniform(); break;   // [0, 1)
		}

		// Latency in [0, 0.5] s, including L = 0 (release in capture step).
		Case.LatencySeconds = (Gen.NextUInt64() % 3 == 0) ? 0.0 : Gen.NextUniform() * 0.5;

		// Synthetic truth scan: variable length, with periodic no-return gaps.
		const int32 NumRays = 64 + static_cast<int32>(Gen.NextUInt64() % 512); // [64, 575]
		const int32 GapStride = 11 + static_cast<int32>(Gen.NextUInt64() % 23); // [11, 33]
		Case.Truth.Reserve(NumRays);
		for (int32 i = 0; i < NumRays; ++i)
		{
			if (i % GapStride == 0)
			{
				Case.Truth.Add(FSensorValue()); // no return
			}
			else
			{
				const double Range = 0.5 + Gen.NextUniform() * 99.5; // [0.5, 100) m
				Case.Truth.Add(FSensorValue(Range));
			}
		}

		return Case;
	}

	// Serialize published values + dropped flags into a stable string with NO
	// timestamps or paths, so the comparison is byte-identical on the published
	// output only (excluding timestamps/paths per Property 1). Doubles are
	// serialized as their raw bits so equality is exact, not rounded.
	FString SerializeStageOutput(const FSensorStageOutput& Output)
	{
		FString S;
		S.Reserve(Output.Published.Num() * 24);
		for (int32 i = 0; i < Output.Published.Num(); ++i)
		{
			const bool bDropped = Output.bDropped.IsValidIndex(i) ? Output.bDropped[i] : false;
			if (Output.Published[i].IsSet())
			{
				const double V = Output.Published[i].GetValue();
				uint64 Bits;
				FMemory::Memcpy(&Bits, &V, sizeof(double));
				S += FString::Printf(TEXT("%d:v=%016llx:d=%d|"), i, Bits, bDropped ? 1 : 0);
			}
			else
			{
				S += FString::Printf(TEXT("%d:null:d=%d|"), i, bDropped ? 1 : 0);
			}
		}
		return S;
	}

	// Run the full pure pipeline for one case: capture -> seeded noise ->
	// dropout -> latency queue, draining the queue on a synthetic clock
	// stepping L/50 (the design's synthetic-clock convention). Returns a stable
	// serialization of the PUBLISHED stream (seq + per-ray values + dropped
	// flags, in release order) excluding the publish/capture timestamps.
	FString RunPipelineOnce(const FGeneratedCase& Case)
	{
		FSensorPipeline Pipeline(Case.RunSeed, Case.SensorName);
		const FSensorStageOutput Scan = Pipeline.ProcessScan(Case.Truth, Case.Sigma, Case.DropoutProb);

		// Push the processed scan through the latency queue as a single payload
		// and drain it on a synthetic clock. This confirms determinism of the
		// published stream, not just the noise/dropout stages.
		TLatencyQueue<FSensorStageOutput> Queue(Case.LatencySeconds);
		const double CaptureTime = 0.0;
		Queue.Enqueue(/*Seq*/ 0, CaptureTime, Scan);

		// Synthetic clock: step L/50 (>= some minimum so L = 0 still advances).
		const double Step = (Case.LatencySeconds > 0.0) ? (Case.LatencySeconds / 50.0) : 0.001;

		FString Published;
		double Now = CaptureTime;
		// Bounded number of steps: L/Step = 50 plus margin; guarantees release.
		for (int32 StepIdx = 0; StepIdx <= 64 && !Queue.IsEmpty(); ++StepIdx)
		{
			const TArray<TReleasedSample<FSensorStageOutput>> Released = Queue.ReleaseDue(Now);
			for (const TReleasedSample<FSensorStageOutput>& Sample : Released)
			{
				// Record seq + the stage output. Exclude PublishTime/CaptureTime
				// (timestamps) from the comparison per Property 1.
				Published += FString::Printf(TEXT("seq=%lld{"), Sample.Seq);
				Published += SerializeStageOutput(Sample.Payload);
				Published += TEXT("}");
			}
			Now += Step;
		}

		return Published;
	}
}

bool FSensorSimSandboxPipelineDeterminismTest::RunTest(const FString& Parameters)
{
	int32 CasesChecked = 0;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		const FGeneratedCase Case = GenerateCase(Iteration);

		// Run the pipeline twice with the SAME seed/config over the SAME scan.
		const FString First = RunPipelineOnce(Case);
		const FString Second = RunPipelineOnce(Case);

		if (First != Second)
		{
			AddError(FString::Printf(
				TEXT("Property 1 FAILED at iteration %d: pipeline output not deterministic ")
				TEXT("(run_seed=%lld, sensor=%s, sigma=%.17g, dropout=%.17g, latency_s=%.17g, rays=%d). ")
				TEXT("First len=%d, Second len=%d."),
				Iteration, Case.RunSeed, *Case.SensorName, Case.Sigma, Case.DropoutProb,
				Case.LatencySeconds, Case.Truth.Num(), First.Len(), Second.Len()));
			// Report the first point of divergence to aid root-cause analysis.
			const int32 MinLen = FMath::Min(First.Len(), Second.Len());
			int32 DivergeAt = 0;
			while (DivergeAt < MinLen && First[DivergeAt] == Second[DivergeAt])
			{
				++DivergeAt;
			}
			AddError(FString::Printf(TEXT("First divergence at char %d."), DivergeAt));
			return false;
		}

		++CasesChecked;
	}

	// Guard against a loop that silently ran zero cases.
	TestTrue(TEXT("determinism checked at least 100 generated cases"), CasesChecked >= 100);

	return true;
}
