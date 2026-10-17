// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Misc/AutomationTest.h"

#include "GroundTruth/GroundTruthLogger.h"
#include "Pipeline/SensorPipeline.h"
#include "Pipeline/SensorRng.h"
#include "Sensors/LidarConfig.h"

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, Property 8: Run_Log timestamps are monotonic
// and complete.
//
// *For any* sequence of logged samples, the recorded `t` values are
// monotonically non-decreasing in file order and every sample record contains
// the rover pose, its sensor's Ground_Truth, and the corresponding
// Published_Value (or a dropped marker).
//
// Validates: Requirements 5.1
//
// This drives the real FGroundTruthLogger end to end (BeginRun -> LogSample* ->
// EndRun) through an in-memory ILogRunSink that captures every emitted line,
// then parses the captured JSONL back and asserts the design's "Ground-truth
// logging format" invariants over the whole sequence:
//
//   MONOTONIC: the `t` field of every sample record is non-decreasing across
//   all records in file order (design: `t` monotonically non-decreasing in
//   file order, 5.1). The generator deliberately feeds samples whose requested
//   times are out of order -- including a dropped frame whose capture time
//   precedes an earlier published release -- so the logger's monotonic
//   enforcement path (`t = max(prev_t, requested_t)`) is exercised, not just
//   already-sorted input.
//
//   COMPLETE: exactly one header line plus exactly one sample record per
//   LogSample call that returned true, in order, with no loss or duplication;
//   each record's `sensor` and `seq` match the input it came from, and the
//   count of sample records equals the number of successful LogSample calls.
//
// Unreal's Automation framework has no native property generator, so a
// lightweight *seeded* generator loop drives >= 100 iterations (design
// "Testing Strategy" / "Property-based testing"). Each iteration's sequence is
// derived from a fixed base seed, so a failing iteration is a real logger bug
// to investigate at a known, reproducible case -- never a reason to lower the
// iteration count or weaken an assertion. A failing seed is a bug to
// root-cause (and could be a real logger bug) before touching the test.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxLoggingProperty8Test,
	"SensorSimSandbox.Logging.Property8_RunLogMonotonicAndComplete",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
{
	// Property-based tests in this project run >= 100 iterations (design
	// "Testing Strategy"). Do not lower this to make the test pass; a failing
	// iteration is a real monotonicity/completeness bug.
	constexpr int32 NumIterations = 128;

	// Fixed base seed for the generator loop so the generated sequences are
	// themselves reproducible across runs and machines.
	constexpr uint64 GeneratorBaseSeed = 0x5BD1E995C0FFEE42ull;

	// ------------------------------------------------------------------
	// In-memory sink: captures every line the logger emits so the test can
	// parse the Run_Log back without touching disk. Never fails (write-failure
	// is covered by task 7.3), so every LogSample that the logger accepts
	// produces exactly one captured line.
	// ------------------------------------------------------------------
	class FCapturingLogRunSink : public ILogRunSink
	{
	public:
		virtual bool WriteLine(const FString& Line) override
		{
			Lines.Add(Line);
			return true;
		}

		virtual bool Flush() override { return true; }

		virtual FString Describe() const override { return TEXT("<in-memory capture>"); }

		const TArray<FString>& GetLines() const { return Lines; }

	private:
		TArray<FString> Lines;
	};

	// One generated input sample paired with the sensor/seq we expect to read
	// back from its record, so completeness can be checked field by field.
	struct FExpectedRecord
	{
		FString Sensor;
		int64 Seq = 0;
	};

	// Extract the numeric value of a top-level JSON field like `"t":12.5,` from
	// a serialized sample record. Returns false if the field is absent. The
	// logger's serializer writes compact `"<name>":<number>` with no spaces,
	// which this parse relies on (and which the format test in task 7.1 pins).
	bool ParseNumberField(const FString& Record, const FString& FieldName, double& OutValue)
	{
		const FString Key = FString::Printf(TEXT("\"%s\":"), *FieldName);
		int32 KeyStart = Record.Find(Key);
		if (KeyStart == INDEX_NONE)
		{
			return false;
		}
		int32 ValStart = KeyStart + Key.Len();
		// Read until the next ',' or '}' that closes the number.
		int32 i = ValStart;
		while (i < Record.Len() && Record[i] != TEXT(',') && Record[i] != TEXT('}'))
		{
			++i;
		}
		const FString ValStr = Record.Mid(ValStart, i - ValStart).TrimStartAndEnd();
		if (ValStr.IsEmpty() || ValStr == TEXT("null"))
		{
			return false;
		}
		OutValue = FCString::Atod(*ValStr);
		return true;
	}

	// Extract a top-level JSON string field like `"sensor":"lidar"`.
	bool ParseStringField(const FString& Record, const FString& FieldName, FString& OutValue)
	{
		const FString Key = FString::Printf(TEXT("\"%s\":\""), *FieldName);
		int32 KeyStart = Record.Find(Key);
		if (KeyStart == INDEX_NONE)
		{
			return false;
		}
		int32 ValStart = KeyStart + Key.Len();
		int32 ValEnd = Record.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, ValStart);
		if (ValEnd == INDEX_NONE)
		{
			return false;
		}
		OutValue = Record.Mid(ValStart, ValEnd - ValStart);
		return true;
	}

	// Build one generated sample. Varies sensor, seq, capture/publish ordering
	// and dropped-ness so the logger's monotonic-enforcement path is exercised.
	// `BaseTime` grows across the sequence but each sample's requested time is
	// perturbed around it (sometimes backwards, sometimes a dropped frame whose
	// capture precedes an earlier publish) to force out-of-order requests.
	FLogSample GenerateSample(FSensorRng& Gen, int64 LidarSeq, int64 CameraSeq, double BaseTime, FExpectedRecord& OutExpected)
	{
		FLogSample S;

		const bool bLidar = (Gen.NextUInt64() % 2) == 0;
		S.Sensor = bLidar ? TEXT("lidar") : TEXT("camera");
		S.Seq = bLidar ? LidarSeq : CameraSeq;

		// Pose: arbitrary but present on every record (completeness checks the
		// pose is emitted). Values are irrelevant to monotonicity.
		S.Pose.PosX = Gen.NextUniform() * 10.0 - 5.0;
		S.Pose.PosY = Gen.NextUniform() * 10.0 - 5.0;
		S.Pose.PosZ = Gen.NextUniform() * 2.0;
		S.Pose.QuatW = 1.0;

		// Capture time wanders around BaseTime, sometimes BEFORE it, so the
		// requested record time is frequently out of order relative to the
		// previous record -- the case the logger must clamp.
		const double Jitter = (Gen.NextUniform() - 0.5) * 0.4; // [-0.2, 0.2]
		S.TCapture = FMath::Max(0.0, BaseTime + Jitter);

		// ~30% of samples are dropped: no publish time (null), record time
		// becomes the capture time. A dropped frame whose capture precedes an
		// earlier published release is exactly the ordering the monotonic
		// enforcement must absorb.
		const bool bDropped = (Gen.NextUInt64() % 10) < 3;
		if (bDropped)
		{
			S.bDropped = true;
			S.TPublish.Reset();
		}
		else
		{
			S.bDropped = false;
			// Publish after capture by a small latency, also jittered.
			const double Latency = Gen.NextUniform() * 0.3;
			S.TPublish = S.TCapture + Latency;
		}

		if (bLidar)
		{
			// A small per-ray range array with a mix of hits and no-hits so the
			// record carries the sensor's Ground_Truth / Published_Value.
			const int32 NumRays = 3 + static_cast<int32>(Gen.NextUInt64() % 6); // [3, 8]
			S.bHasRangeArrays = true;
			S.Truth.Reserve(NumRays);
			S.Published.Reserve(NumRays);
			for (int32 r = 0; r < NumRays; ++r)
			{
				const bool bHit = (Gen.NextUInt64() % 5) != 0;
				if (bHit)
				{
					const double Range = 0.5 + Gen.NextUniform() * 99.5;
					S.Truth.Add(FSensorValue(Range));
					S.Published.Add(S.bDropped ? FSensorValue() : FSensorValue(Range + (Gen.NextUniform() - 0.5) * 0.1));
				}
				else
				{
					S.Truth.Add(FSensorValue());
					S.Published.Add(FSensorValue());
				}
			}
		}
		else
		{
			// Camera frame: sampled frames carry image refs; dropped frames
			// carry none; unsampled published frames carry explicit null refs.
			S.bIsCameraFrame = true;
			if (!S.bDropped && (Gen.NextUInt64() % 2) == 0)
			{
				S.TruthRef = FString::Printf(TEXT("frames/cam_%06lld.png"), S.Seq);
				S.PublishedRef = FString::Printf(TEXT("frames/cam_%06lld_pub.jpg"), S.Seq);
			}
		}

		OutExpected.Sensor = S.Sensor;
		OutExpected.Seq = S.Seq;
		return S;
	}
}

bool FSensorSimSandboxLoggingProperty8Test::RunTest(const FString& Parameters)
{
	int32 CasesChecked = 0;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FSensorRng Gen(static_cast<uint64>(GeneratorBaseSeed) ^ (static_cast<uint64>(Iteration) * 0x9E3779B97F4A7C15ull));

		// Variable-length sequence per iteration, always non-trivial.
		const int32 NumSamples = 8 + static_cast<int32>(Gen.NextUInt64() % 40); // [8, 47]

		// Drive the REAL logger through the capturing sink.
		TUniquePtr<FCapturingLogRunSink> OwnedSink = MakeUnique<FCapturingLogRunSink>();
		FCapturingLogRunSink* Sink = OwnedSink.Get();

		FGroundTruthLogger Logger;
		FLidarConfig Cfg; // Gate A defaults; header content is covered by task 7.1
		const bool bBegan = Logger.BeginRun(
			/*Seed*/ static_cast<int64>(Gen.NextUInt64()),
			Cfg,
			/*Commit*/ TEXT("deadbeefcafef00d0000000000000000deadbeef"),
			MoveTemp(OwnedSink),
			/*CameraSamplingInterval*/ TEXT("1/10"));

		if (!bBegan)
		{
			AddError(FString::Printf(TEXT("Property 8 iteration %d: BeginRun failed unexpectedly."), Iteration));
			return false;
		}

		// Log the generated sequence, recording the expected (sensor, seq) of
		// every sample the logger ACCEPTED, in the order it accepted them.
		TArray<FExpectedRecord> Expected;
		Expected.Reserve(NumSamples);
		int64 LidarSeq = 0;
		int64 CameraSeq = 0;
		double BaseTime = 0.0;
		int64 AcceptedCount = 0;
		for (int32 s = 0; s < NumSamples; ++s)
		{
			FExpectedRecord Exp;
			const FLogSample Sample = GenerateSample(Gen, LidarSeq, CameraSeq, BaseTime, Exp);
			if (Sample.Sensor == TEXT("lidar")) { ++LidarSeq; } else { ++CameraSeq; }

			// BaseTime generally advances so the run spans real time, while the
			// per-sample jitter in GenerateSample still produces out-of-order
			// requested times to exercise the clamp.
			BaseTime += Gen.NextUniform() * 0.3;

			const bool bLogged = Logger.LogSample(Sample);
			if (bLogged)
			{
				Expected.Add(Exp);
				++AcceptedCount;
			}
			else
			{
				// The capturing sink never fails, so a healthy logger accepts
				// every sample while the run is active. A rejection here is a
				// real bug.
				AddError(FString::Printf(
					TEXT("Property 8 iteration %d: LogSample unexpectedly returned false at sample %d ")
					TEXT("(sensor=%s seq=%lld)."),
					Iteration, s, *Sample.Sensor, Sample.Seq));
				return false;
			}
		}

		Logger.EndRun();

		// ----- Completeness: the logger's own counter matches acceptances ----
		if (Logger.GetSamplesWritten() != AcceptedCount)
		{
			AddError(FString::Printf(
				TEXT("Property 8 iteration %d: logger SamplesWritten=%lld but %lld LogSample calls were accepted."),
				Iteration, Logger.GetSamplesWritten(), AcceptedCount));
			return false;
		}

		const TArray<FString>& Lines = Sink->GetLines();

		// ----- Completeness: exactly one header + one record per accepted sample
		const int32 ExpectedLineCount = 1 /*header*/ + static_cast<int32>(AcceptedCount);
		if (Lines.Num() != ExpectedLineCount)
		{
			AddError(FString::Printf(
				TEXT("Property 8 iteration %d: captured %d lines, expected %d (1 header + %lld records). ")
				TEXT("Records were lost or duplicated."),
				Iteration, Lines.Num(), ExpectedLineCount, AcceptedCount));
			return false;
		}

		// First line is the run_meta header, not a sample record.
		if (!Lines[0].Contains(TEXT("\"type\":\"run_meta\"")))
		{
			AddError(FString::Printf(
				TEXT("Property 8 iteration %d: first captured line is not the run_meta header."), Iteration));
			return false;
		}

		// ----- Parse records and check monotonic `t` + per-record completeness
		double PrevT = 0.0;
		bool bHasPrevT = false;
		for (int32 RecIdx = 1; RecIdx < Lines.Num(); ++RecIdx)
		{
			const FString& Rec = Lines[RecIdx];
			const int32 SampleIdx = RecIdx - 1; // index into Expected

			// Every sample record is of type "sample".
			if (!Rec.Contains(TEXT("\"type\":\"sample\"")))
			{
				AddError(FString::Printf(
					TEXT("Property 8 iteration %d: record %d is not type \"sample\": %s"),
					Iteration, SampleIdx, *Rec));
				return false;
			}

			// MONOTONIC: `t` is present and non-decreasing in file order.
			double T = 0.0;
			if (!ParseNumberField(Rec, TEXT("t"), T))
			{
				AddError(FString::Printf(
					TEXT("Property 8 iteration %d: record %d has no numeric `t` field: %s"),
					Iteration, SampleIdx, *Rec));
				return false;
			}
			if (bHasPrevT && T < PrevT)
			{
				AddError(FString::Printf(
					TEXT("Property 8 iteration %d: NON-MONOTONIC `t` at record %d: t=%.17g < previous t=%.17g. ")
					TEXT("The logger must carry each record's `t` forward (t = max(prev_t, requested_t))."),
					Iteration, SampleIdx, T, PrevT));
				return false;
			}
			PrevT = T;
			bHasPrevT = true;

			// COMPLETENESS: sensor + seq match the input this record came from,
			// in order (no loss, no duplication, no reordering).
			FString Sensor;
			if (!ParseStringField(Rec, TEXT("sensor"), Sensor) || Sensor != Expected[SampleIdx].Sensor)
			{
				AddError(FString::Printf(
					TEXT("Property 8 iteration %d: record %d sensor mismatch (got '%s', expected '%s'): %s"),
					Iteration, SampleIdx, *Sensor, *Expected[SampleIdx].Sensor, *Rec));
				return false;
			}
			double SeqD = 0.0;
			if (!ParseNumberField(Rec, TEXT("seq"), SeqD) || static_cast<int64>(SeqD) != Expected[SampleIdx].Seq)
			{
				AddError(FString::Printf(
					TEXT("Property 8 iteration %d: record %d seq mismatch (got %lld, expected %lld): %s"),
					Iteration, SampleIdx, static_cast<int64>(SeqD), Expected[SampleIdx].Seq, *Rec));
				return false;
			}

			// COMPLETENESS: every record carries the rover pose and either the
			// sensor's Ground_Truth/Published payload or a dropped marker.
			if (!Rec.Contains(TEXT("\"rover_pose\":{\"pos\":[")))
			{
				AddError(FString::Printf(
					TEXT("Property 8 iteration %d: record %d is missing the rover pose: %s"),
					Iteration, SampleIdx, *Rec));
				return false;
			}
			const bool bHasTruthPayload =
				Rec.Contains(TEXT("\"truth\":"))      // lidar ground truth
				|| Rec.Contains(TEXT("\"truth_ref\":")); // camera ground-truth ref
			const bool bDroppedMarker = Rec.Contains(TEXT("\"dropped\":true"));
			if (!bHasTruthPayload && !bDroppedMarker)
			{
				AddError(FString::Printf(
					TEXT("Property 8 iteration %d: record %d carries neither a Ground_Truth payload nor a dropped marker: %s"),
					Iteration, SampleIdx, *Rec));
				return false;
			}
		}

		++CasesChecked;
	}

	// Guard against a loop that silently ran zero cases.
	TestTrue(TEXT("Property 8 checked at least 100 generated sequences"), CasesChecked >= 100);

	return true;
}
