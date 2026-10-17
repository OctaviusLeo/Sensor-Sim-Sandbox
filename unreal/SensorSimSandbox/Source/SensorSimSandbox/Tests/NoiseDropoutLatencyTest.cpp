// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Misc/AutomationTest.h"

#include "Pipeline/SensorPipeline.h"
#include "Pipeline/SensorRng.h"
#include "Pipeline/LatencyQueue.h"

#include <cmath>

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox
//
// Three statistical property tests over the pure, seeded pipeline stages
// (noise, dropout, latency). Each drives the PURE pipeline classes directly
// (FSensorPipeline / TLatencyQueue over a synthetic truth scan and a synthetic
// clock) with no world or rendering, consistent with how task 5.1 isolated
// the pipeline and how Properties 1 and 2 (tasks 5.2, 5.3) test it.
//
// Unreal's Automation framework has no native property generator, so a
// lightweight *seeded* generator loop drives >= 100 iterations per property
// (design "Testing Strategy" / "Property-based testing"). Every generated
// iteration is reproducible from a fixed base seed, so a failing iteration is
// a bug to investigate at a known case, NEVER a reason to lower the iteration
// count, lower the draw count, or widen a tolerance.
//
// Sample sizes and tolerances are fixed by the design ("Sample sizes that make
// the +/-2 pp and +/-10% tolerances achievable"): 20,000 draws per iteration
// for noise (Property 3) and dropout (Property 4), and >= 100 samples per
// iteration on a synthetic clock stepping L/50 for latency (Property 5).
// ---------------------------------------------------------------------------

namespace
{
	// All three statistical property tests run >= 100 iterations (design
	// "Testing Strategy"). Do not lower this; a failing iteration is a real
	// bug at a known, reproducible seed.
	constexpr int32 NumIterations = 100;

	// Noise (Property 3) and dropout (Property 4) draw 20,000 samples per
	// iteration (design "Sample sizes"). At these counts a correct model
	// passes the +/-10% / +/-2 pp tolerances with near certainty; do not
	// reduce this to make a test pass.
	constexpr int32 DrawsPerIteration = 20000;

	// Property 5: at least 100 samples per iteration released against the
	// synthetic clock.
	constexpr int32 LatencySamplesPerIteration = 128;

	// Fixed base seeds per property so each generator loop is reproducible
	// across runs and machines, independent of the pipeline's own RNG stream.
	constexpr uint64 NoiseGeneratorBaseSeed   = 0x3C9A7E12F640B5D3ull;
	constexpr uint64 DropoutGeneratorBaseSeed = 0x71E4B8D0A2C65F93ull;
	constexpr uint64 LatencyGeneratorBaseSeed = 0x58D3A6C1904E27BFull;
}

// ===========================================================================
// Property 3: Range-noise standard deviation is recovered
//
// *For any* configured range-noise standard deviation sigma within bounds,
// estimating the standard deviation of (published - truth) over at least 1000
// generated returns (the test uses 20,000 per iteration) recovers sigma within
// +/-10%; for sigma = 0 the published values equal the truth exactly.
//
// Validates: Requirements 3.5
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxRangeNoiseSigmaRecoveredTest,
	"SensorSimSandbox.Pipeline.Property3_RangeNoiseSigmaRecovered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxRangeNoiseSigmaRecoveredTest::RunTest(const FString& Parameters)
{
	int32 IterationsChecked = 0;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		// A reproducible generator for this iteration's sigma, truth values,
		// and sensor name. Independent of the pipeline's own RNG stream.
		FSensorRng Gen(
			NoiseGeneratorBaseSeed,
			FString::Printf(TEXT("noise|%d"), Iteration));

		// Pick a sigma spanning the valid range for range_noise_sigma_m
		// (design bounds 0.0 .. 10.0). Iteration 0 pins sigma = 0 (the exact
		// passthrough case); every other iteration draws sigma in (0, 10].
		double Sigma;
		if (Iteration == 0)
		{
			Sigma = 0.0;
		}
		else
		{
			// Strictly positive, up to the 10.0 upper bound.
			Sigma = 0.05 + Gen.NextUniform() * (10.0 - 0.05);
		}

		// A sensor name per iteration so the derived per-sensor RNG path varies.
		const FString SensorName = FString::Printf(TEXT("lidar_noise_%d"), Iteration);

		// Build 20,000 known-truth returns. The truth values themselves are
		// arbitrary (noise is additive and independent of the truth); use a
		// spread of ranges in a plausible lidar interval so (noisy - truth) is
		// what we measure, not any truth structure.
		TArray<FSensorValue> Truth;
		Truth.Reserve(DrawsPerIteration);
		for (int32 i = 0; i < DrawsPerIteration; ++i)
		{
			const double Range = 1.0 + Gen.NextUniform() * 199.0; // [1, 200) m
			Truth.Add(FSensorValue(Range));
		}

		FSensorPipeline Pipeline(static_cast<int64>(Iteration) + 1, SensorName);
		const TArray<FSensorValue> Noisy = Pipeline.AddGaussianNoise(Truth, Sigma);

		if (Noisy.Num() != Truth.Num())
		{
			AddError(FString::Printf(
				TEXT("Property 3 FAILED iter %d: AddGaussianNoise returned %d values for %d truth values."),
				Iteration, Noisy.Num(), Truth.Num()));
			return false;
		}

		if (Sigma == 0.0)
		{
			// sigma = 0 must be an EXACT, bit-identical passthrough: no RNG
			// draw, no perturbation.
			for (int32 i = 0; i < Truth.Num(); ++i)
			{
				if (!Noisy[i].IsSet())
				{
					AddError(FString::Printf(
						TEXT("Property 3 FAILED iter %d (sigma=0): element %d lost its value."),
						Iteration, i));
					return false;
				}

				const double T = Truth[i].GetValue();
				const double N = Noisy[i].GetValue();
				uint64 TBits;
				uint64 NBits;
				FMemory::Memcpy(&TBits, &T, sizeof(double));
				FMemory::Memcpy(&NBits, &N, sizeof(double));
				if (TBits != NBits)
				{
					AddError(FString::Printf(
						TEXT("Property 3 FAILED iter %d (sigma=0): element %d changed from %.17g to %.17g; ")
						TEXT("sigma = 0 must be an exact passthrough (bit-identical)."),
						Iteration, i, T, N));
					return false;
				}
			}

			++IterationsChecked;
			continue;
		}

		// sigma > 0: estimate the sample standard deviation of the error
		// e[i] = noisy[i] - truth[i] over all 20,000 draws, and assert it is
		// within +/-10% of the configured sigma. Use the two-pass (unbiased,
		// n-1) estimator for numerical stability.
		double Mean = 0.0;
		for (int32 i = 0; i < Truth.Num(); ++i)
		{
			if (!Noisy[i].IsSet())
			{
				AddError(FString::Printf(
					TEXT("Property 3 FAILED iter %d: element %d has no value after noise (sigma=%.17g)."),
					Iteration, i, Sigma));
				return false;
			}
			Mean += (Noisy[i].GetValue() - Truth[i].GetValue());
		}
		Mean /= static_cast<double>(Truth.Num());

		double SumSq = 0.0;
		for (int32 i = 0; i < Truth.Num(); ++i)
		{
			const double Err = (Noisy[i].GetValue() - Truth[i].GetValue()) - Mean;
			SumSq += Err * Err;
		}
		const double SampleStdDev = std::sqrt(SumSq / static_cast<double>(Truth.Num() - 1));

		const double LowerBound = Sigma * 0.90;
		const double UpperBound = Sigma * 1.10;
		if (SampleStdDev < LowerBound || SampleStdDev > UpperBound)
		{
			AddError(FString::Printf(
				TEXT("Property 3 FAILED iter %d: recovered sample std dev %.6f is outside +/-10%% of ")
				TEXT("configured sigma %.6f (allowed [%.6f, %.6f]) over %d draws. ")
				TEXT("Investigate this fixed-seed case as a bug; do not widen the tolerance."),
				Iteration, SampleStdDev, Sigma, LowerBound, UpperBound, DrawsPerIteration));
			return false;
		}

		++IterationsChecked;
	}

	TestTrue(
		TEXT("Property 3 ran >= 100 iterations"),
		IterationsChecked >= 100);

	return true;
}

// ===========================================================================
// Property 4: Dropout fraction matches configured probability
//
// *For any* configured dropout probability p in [0, 1], over 20,000 draws the
// measured dropped fraction is within +/-2 percentage points of p; p = 0 drops
// none and p = 1 drops all. Only eligible (present) entries are counted,
// consistent with the pipeline contract (no-return entries are not drawn for).
//
// Validates: Requirements 3.6, 4.5
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxDropoutFractionMatchesPTest,
	"SensorSimSandbox.Pipeline.Property4_DropoutFractionMatchesP",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxDropoutFractionMatchesPTest::RunTest(const FString& Parameters)
{
	int32 IterationsChecked = 0;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FSensorRng Gen(
			DropoutGeneratorBaseSeed,
			FString::Printf(TEXT("dropout|%d"), Iteration));

		// p is drawn over [0, 1] INCLUDING both endpoints. Pin iteration 0 to
		// p = 0 (drop none) and iteration 1 to p = 1 (drop all); every other
		// iteration draws p in [0, 1].
		double P;
		if (Iteration == 0)
		{
			P = 0.0;
		}
		else if (Iteration == 1)
		{
			P = 1.0;
		}
		else
		{
			P = Gen.NextUniform(); // [0, 1)
		}

		const FString SensorName = FString::Printf(TEXT("lidar_dropout_%d"), Iteration);

		// 20,000 present (eligible) returns: every entry is drawn for, so the
		// eligible count equals the draw count and the measured fraction is
		// (dropped / 20,000). Values are arbitrary; dropout does not read them.
		TArray<FSensorValue> Values;
		Values.Reserve(DrawsPerIteration);
		for (int32 i = 0; i < DrawsPerIteration; ++i)
		{
			const double Range = 1.0 + Gen.NextUniform() * 199.0; // [1, 200) m
			Values.Add(FSensorValue(Range));
		}

		FSensorPipeline Pipeline(static_cast<int64>(Iteration) + 1, SensorName);
		const FSensorStageOutput Out = Pipeline.ApplyPerElementDropout(Values, P);

		if (Out.Published.Num() != Values.Num() || Out.bDropped.Num() != Values.Num())
		{
			AddError(FString::Printf(
				TEXT("Property 4 FAILED iter %d: dropout output size mismatch (published=%d, dropped=%d, in=%d)."),
				Iteration, Out.Published.Num(), Out.bDropped.Num(), Values.Num()));
			return false;
		}

		// Count dropped entries over the eligible (present) entries only. Here
		// every input entry is present, so Eligible == DrawsPerIteration; the
		// check is written generally so the pipeline contract (gaps not drawn
		// for) is respected if the inputs ever change.
		int32 Eligible = 0;
		int32 Dropped = 0;
		for (int32 i = 0; i < Values.Num(); ++i)
		{
			if (!Values[i].IsSet())
			{
				// No-return: must not be counted and must not be marked dropped.
				if (Out.bDropped[i])
				{
					AddError(FString::Printf(
						TEXT("Property 4 FAILED iter %d: a no-return entry %d was marked dropped."),
						Iteration, i));
					return false;
				}
				continue;
			}

			++Eligible;
			if (Out.bDropped[i])
			{
				++Dropped;
				// A dropped present entry must become empty in the output.
				if (Out.Published[i].IsSet())
				{
					AddError(FString::Printf(
						TEXT("Property 4 FAILED iter %d: entry %d is marked dropped but still has a published value."),
						Iteration, i));
					return false;
				}
			}
			else
			{
				// A surviving present entry must keep its value.
				if (!Out.Published[i].IsSet())
				{
					AddError(FString::Printf(
						TEXT("Property 4 FAILED iter %d: entry %d is not dropped but lost its published value."),
						Iteration, i));
					return false;
				}
			}
		}

		if (Eligible == 0)
		{
			AddError(FString::Printf(
				TEXT("Property 4 FAILED iter %d: no eligible entries to measure dropout over."), Iteration));
			return false;
		}

		const double MeasuredFraction = static_cast<double>(Dropped) / static_cast<double>(Eligible);

		// Endpoint exactness: p = 0 drops exactly none, p = 1 drops exactly all.
		if (P <= 0.0)
		{
			if (Dropped != 0)
			{
				AddError(FString::Printf(
					TEXT("Property 4 FAILED iter %d (p=0): %d of %d eligible entries were dropped; must be 0."),
					Iteration, Dropped, Eligible));
				return false;
			}
			++IterationsChecked;
			continue;
		}
		if (P >= 1.0)
		{
			if (Dropped != Eligible)
			{
				AddError(FString::Printf(
					TEXT("Property 4 FAILED iter %d (p=1): %d of %d eligible entries were dropped; must be all."),
					Iteration, Dropped, Eligible));
				return false;
			}
			++IterationsChecked;
			continue;
		}

		// Interior p: measured dropped fraction within +/-2 percentage points.
		const double Lower = P - 0.02;
		const double Upper = P + 0.02;
		if (MeasuredFraction < Lower || MeasuredFraction > Upper)
		{
			AddError(FString::Printf(
				TEXT("Property 4 FAILED iter %d: measured dropped fraction %.5f is outside +/-2 pp of ")
				TEXT("p=%.5f (allowed [%.5f, %.5f]) over %d eligible draws (%d dropped). ")
				TEXT("Investigate this fixed-seed case as a bug; do not widen the tolerance."),
				Iteration, MeasuredFraction, P, Lower, Upper, Eligible, Dropped));
			return false;
		}

		++IterationsChecked;
	}

	TestTrue(
		TEXT("Property 4 ran >= 100 iterations"),
		IterationsChecked >= 100);

	return true;
}

// ===========================================================================
// Property 5: Latency queue releases within tolerance
//
// *For any* configured latency L, the latency queue releases each sample only
// after capture_time + L has elapsed, and over at least 100 samples on a
// synthetic clock stepping L/50 the mean release delay matches L within +/-20%;
// for L = 0 every sample is released in the step it was captured (delay = 0).
//
// Validates: Requirements 3.7, 4.6
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxLatencyReleaseWithinToleranceTest,
	"SensorSimSandbox.Pipeline.Property5_LatencyReleaseWithinTolerance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxLatencyReleaseWithinToleranceTest::RunTest(const FString& Parameters)
{
	int32 IterationsChecked = 0;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FSensorRng Gen(
			LatencyGeneratorBaseSeed,
			FString::Printf(TEXT("latency|%d"), Iteration));

		// L spans the valid latency range (design latency_ms bounds 0..1000,
		// i.e. 0..1.0 s). Iteration 0 pins L = 0 (same-step release); every
		// other iteration draws L in (0, 1] s.
		double L;
		if (Iteration == 0)
		{
			L = 0.0;
		}
		else
		{
			L = 0.01 + Gen.NextUniform() * (1.0 - 0.01); // (0.01, 1.0) s
		}

		// The synthetic clock steps L/50 (design convention). For L = 0 the
		// step is a small positive constant so the clock still advances. The
		// clock time at tick t is computed as `t * Step` by MULTIPLICATION, not
		// by repeated accumulation, so a given tick's `Now` is bit-exact and
		// reproducible; this matters for the L = 0 same-step contract, where a
		// sample captured exactly at a tick must be released at a `Now` equal to
		// its capture time (delay exactly 0).
		const double Step = (L > 0.0) ? (L / 50.0) : 0.001;

		TLatencyQueue<int64> Queue(L);

		// Capture times sit on tick boundaries: sample k is captured at tick
		// (k * CaptureTickStride), so its capture time equals that tick's
		// `Now` bit-for-bit. Spacing the captures a few ticks apart lets each
		// sample sit in the queue across several checks (for L > 0) while still
		// being "checked at its capture time" on the tick it is enqueued (so
		// L = 0 releases in that same step with delay exactly 0).
		const int32 CaptureTickStride = 3;
		TArray<int32> CaptureTick;
		TArray<double> CaptureTimes;
		CaptureTick.Reserve(LatencySamplesPerIteration);
		CaptureTimes.Reserve(LatencySamplesPerIteration);
		for (int32 k = 0; k < LatencySamplesPerIteration; ++k)
		{
			const int32 Tick = k * CaptureTickStride;
			CaptureTick.Add(Tick);
			CaptureTimes.Add(static_cast<double>(Tick) * Step);
		}

		// Track realized delay per seq. -1 marks "not yet released".
		TArray<double> RealizedDelay;
		RealizedDelay.Init(-1.0, LatencySamplesPerIteration);

		// Drive the synthetic clock by integer tick index. At each tick:
		//   Now = Tick * Step  (bit-exact; same formula as the capture times)
		//   1) enqueue any sample whose capture tick == this tick, then
		//   2) release everything due at `Now`.
		// Enqueuing before releasing on the SAME tick is what gives the L = 0
		// sample its same-step release at exactly its capture time.
		const int32 LastCaptureTick = CaptureTick[LatencySamplesPerIteration - 1];
		// Ticks needed to clear the last sample's latency, plus margin.
		const int32 LatencyTicks = (L > 0.0) ? (static_cast<int32>(std::ceil(L / Step)) + 2) : 1;
		const int32 MaxTick = LastCaptureTick + LatencyTicks + 4;

		int32 NextToEnqueue = 0;
		int32 NumReleased = 0;

		for (int32 Tick = 0; Tick <= MaxTick && NumReleased < LatencySamplesPerIteration; ++Tick)
		{
			const double Now = static_cast<double>(Tick) * Step;

			// Enqueue every sample captured on this exact tick.
			while (NextToEnqueue < LatencySamplesPerIteration &&
				   CaptureTick[NextToEnqueue] == Tick)
			{
				Queue.Enqueue(
					static_cast<int64>(NextToEnqueue),
					CaptureTimes[NextToEnqueue],
					static_cast<int64>(NextToEnqueue));
				++NextToEnqueue;
			}

			const TArray<TReleasedSample<int64>> Released = Queue.ReleaseDue(Now);
			for (const TReleasedSample<int64>& Sample : Released)
			{
				const int32 Idx = static_cast<int32>(Sample.Seq);
				if (RealizedDelay.IsValidIndex(Idx) && RealizedDelay[Idx] < 0.0)
				{
					RealizedDelay[Idx] = Sample.Delay();
					++NumReleased;
				}

				// A sample must never be released early: capture + L <= publish.
				if (Sample.CaptureTime + L > Sample.PublishTime + 1e-9)
				{
					AddError(FString::Printf(
						TEXT("Property 5 FAILED iter %d: sample %lld released early ")
						TEXT("(capture=%.9f, L=%.9f, publish=%.9f)."),
						Iteration, Sample.Seq, Sample.CaptureTime, L, Sample.PublishTime));
					return false;
				}
			}
		}

		if (NumReleased != LatencySamplesPerIteration)
		{
			AddError(FString::Printf(
				TEXT("Property 5 FAILED iter %d: only %d of %d samples were released (L=%.9f, step=%.9f)."),
				Iteration, NumReleased, LatencySamplesPerIteration, L, Step));
			return false;
		}

		if (L == 0.0)
		{
			// L = 0: every sample released in the step it was captured, so the
			// realized delay is exactly 0.
			for (int32 k = 0; k < LatencySamplesPerIteration; ++k)
			{
				if (RealizedDelay[k] != 0.0)
				{
					AddError(FString::Printf(
						TEXT("Property 5 FAILED iter %d (L=0): sample %d had delay %.9f; must be exactly 0."),
						Iteration, k, RealizedDelay[k]));
					return false;
				}
			}
			++IterationsChecked;
			continue;
		}

		// L > 0: mean realized delay within +/-20% of L. With a clock stepping
		// L/50 the release error lies in [0, L/50), i.e. under 2% of L, so a
		// correct queue is comfortably inside +/-20%.
		double SumDelay = 0.0;
		for (int32 k = 0; k < LatencySamplesPerIteration; ++k)
		{
			SumDelay += RealizedDelay[k];
		}
		const double MeanDelay = SumDelay / static_cast<double>(LatencySamplesPerIteration);

		const double Lower = L * 0.80;
		const double Upper = L * 1.20;
		if (MeanDelay < Lower || MeanDelay > Upper)
		{
			AddError(FString::Printf(
				TEXT("Property 5 FAILED iter %d: mean release delay %.9f is outside +/-20%% of ")
				TEXT("L=%.9f (allowed [%.9f, %.9f]) over %d samples (step=%.9f). ")
				TEXT("Investigate this fixed-seed case as a bug; do not widen the tolerance."),
				Iteration, MeanDelay, L, Lower, Upper, LatencySamplesPerIteration, Step));
			return false;
		}

		++IterationsChecked;
	}

	TestTrue(
		TEXT("Property 5 ran >= 100 iterations"),
		IterationsChecked >= 100);

	return true;
}
