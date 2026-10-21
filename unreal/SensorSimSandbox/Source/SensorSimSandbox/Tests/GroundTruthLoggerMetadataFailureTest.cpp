// Copyright (c) 2026 OctaviusLeo. MIT License.

#include "Misc/AutomationTest.h"

#include "GroundTruth/GroundTruthLogger.h"
#include "Sensors/LidarConfig.h"

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 7.3 — Unreal Automation EXAMPLE tests for
// the GroundTruthLogger's metadata header and write-failure handling.
//
// These are concrete-scenario example tests (not property loops): each drives
// the REAL FGroundTruthLogger through an injected in-memory sink and asserts
// specific, exact outcomes from the design's "Ground-truth logging format".
// They complement the format test and the Property 8 monotonicity test already
// in Tests/ (GroundTruthLoggerTest.cpp, LoggingMonotonicCompleteTest.cpp),
// covering the parts of Requirement 5 those do not pin end to end:
//
//   SensorSimSandbox.Logging.HeaderMetadata          — seed + full config (5.4)
//   SensorSimSandbox.Logging.CameraSamplingInterval  — interval present / omitted (5.3)
//   SensorSimSandbox.Logging.WriteFailureHandling     — stop/retain/error (5.5)
//
// Validates: Requirements 5.3, 5.4, 5.5
//
// Every assertion is concrete and specific; nothing is weakened to pass. A
// failing assertion is a real header-content or write-failure contract bug to
// investigate.
// ---------------------------------------------------------------------------

namespace
{
	// -----------------------------------------------------------------------
	// In-memory sink that captures every line the logger emits (never fails),
	// so the header-content and camera-interval examples can read the exact
	// bytes the logger wrote back without touching disk.
	// -----------------------------------------------------------------------
	class FCapturingSink : public ILogRunSink
	{
	public:
		virtual bool WriteLine(const FString& Line) override
		{
			Lines.Add(Line);
			return true;
		}
		virtual bool Flush() override { return true; }
		virtual FString Describe() const override { return TEXT("<in-memory capture>"); }

		TArray<FString> Lines;
	};

	// -----------------------------------------------------------------------
	// A sink that fails on the Nth WriteLine (1-based), keeping every earlier
	// line in a buffer it NEVER discards — the same pattern the task 7.1
	// self-check uses — so the write-failure path (5.5) runs headlessly: we
	// assert the run stops, the error is emitted, and the already-written lines
	// are retained intact.
	// -----------------------------------------------------------------------
	class FFailingAtNSink : public ILogRunSink
	{
	public:
		explicit FFailingAtNSink(int32 InFailAtWrite) : FailAtWrite(InFailAtWrite) {}

		virtual bool WriteLine(const FString& Line) override
		{
			++WriteCount;
			if (WriteCount == FailAtWrite)
			{
				bFailed = true;
				return false; // retained lines in `Lines` are NOT discarded
			}
			Lines.Add(Line);
			return true;
		}
		virtual bool Flush() override { return !bFailed; }
		virtual FString Describe() const override { return TEXT("<in-memory failing sink>"); }

		int32 WriteCount = 0;
		int32 FailAtWrite = 0;
		bool bFailed = false;
		TArray<FString> Lines;
	};

	/** A simple one-ray lidar sample so write-failure tests can log records. */
	FLogSample MakeLidarSample(int64 Seq, double TCapture, double TPublish)
	{
		FLogSample S;
		S.Sensor = TEXT("lidar");
		S.Seq = Seq;
		S.TCapture = TCapture;
		S.TPublish = TPublish;
		S.bDropped = false;
		S.bHasRangeArrays = true;
		S.Truth = { FSensorValue(10.0), FSensorValue(20.0) };
		S.Published = { FSensorValue(10.02), FSensorValue(20.01) };
		return S;
	}
}

// ---------------------------------------------------------------------------
// HeaderMetadata (5.4): BeginRun with a known seed, full commit, and a specific
// NON-DEFAULT lidar config; capture the header the logger wrote and assert it
// carries the exact seed, the full commit SHA, and the full lidar sensor_config
// with the exact field values set here — proving the REAL config is serialized,
// not the Gate A defaults.
//
// Validates: Requirements 5.4
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxLoggingHeaderMetadataTest,
	"SensorSimSandbox.Logging.HeaderMetadata",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxLoggingHeaderMetadataTest::RunTest(const FString& Parameters)
{
	// A config whose every field is DELIBERATELY different from the Gate A
	// defaults (Channels 16, HFov 360, VFov 30, AngRes 1.0, MaxRange 100,
	// Rate 10, Sigma 0.02, Dropout 0.10, Latency 200). Values are chosen to be
	// exactly representable so %.17g serialization round-trips to the literal
	// text asserted below (all are integers or exact binary fractions).
	FLidarConfig Cfg;
	Cfg.Channels = 32;
	Cfg.HFovDeg = 180.0;
	Cfg.VFovDeg = 45.0;
	Cfg.AngularResolutionDeg = 0.5;   // exact in binary
	Cfg.MaxRangeM = 250.0;
	Cfg.RateHz = 20.0;
	Cfg.RangeNoiseSigmaM = 0.25;      // exact in binary
	Cfg.DropoutProb = 0.125;          // exact in binary
	Cfg.LatencyMs = 150;

	const int64 Seed = 987654321;
	const FString Commit = TEXT("0123456789abcdef0123456789abcdef01234567"); // full 40-char SHA

	TUniquePtr<FCapturingSink> OwnedSink = MakeUnique<FCapturingSink>();
	FCapturingSink* Sink = OwnedSink.Get();

	FGroundTruthLogger Logger;
	const bool bBegan = Logger.BeginRun(Seed, Cfg, Commit, MoveTemp(OwnedSink), /*CameraSamplingInterval*/ FString());
	TestTrue(TEXT("BeginRun succeeded with a healthy sink"), bBegan);
	TestTrue(TEXT("run is active after BeginRun"), Logger.IsRunning());

	// Exactly one line was written by BeginRun: the header.
	if (!TestEqual(TEXT("BeginRun wrote exactly one line (the header)"), Sink->Lines.Num(), 1))
	{
		return false;
	}
	const FString& Header = Sink->Lines[0];

	// Header type.
	TestTrue(TEXT("header type is run_meta"),
		Header.Contains(TEXT("\"type\":\"run_meta\"")));

	// EXACT seed (not a default).
	TestTrue(TEXT("header carries the exact seed"),
		Header.Contains(TEXT("\"seed\":987654321")));

	// FULL commit SHA, verbatim.
	TestTrue(TEXT("header carries the full commit SHA"),
		Header.Contains(TEXT("\"commit\":\"0123456789abcdef0123456789abcdef01234567\"")));

	// The sensor_config.lidar object is present and nested as the format states.
	TestTrue(TEXT("header nests sensor_config.lidar"),
		Header.Contains(TEXT("\"sensor_config\":{\"lidar\":{")));

	// EVERY lidar field carries the EXACT non-default value set above — proving
	// the real config is serialized, not Gate A defaults.
	TestTrue(TEXT("lidar channels is the configured 32"),
		Header.Contains(TEXT("\"channels\":32")));
	TestTrue(TEXT("lidar h_fov_deg is the configured 180"),
		Header.Contains(TEXT("\"h_fov_deg\":180")));
	TestTrue(TEXT("lidar v_fov_deg is the configured 45"),
		Header.Contains(TEXT("\"v_fov_deg\":45")));
	TestTrue(TEXT("lidar angular_resolution_deg is the configured 0.5"),
		Header.Contains(TEXT("\"angular_resolution_deg\":0.5")));
	TestTrue(TEXT("lidar max_range_m is the configured 250"),
		Header.Contains(TEXT("\"max_range_m\":250")));
	TestTrue(TEXT("lidar rate_hz is the configured 20"),
		Header.Contains(TEXT("\"rate_hz\":20")));
	TestTrue(TEXT("lidar range_noise_sigma_m is the configured 0.25"),
		Header.Contains(TEXT("\"range_noise_sigma_m\":0.25")));
	TestTrue(TEXT("lidar dropout_prob is the configured 0.125"),
		Header.Contains(TEXT("\"dropout_prob\":0.125")));
	TestTrue(TEXT("lidar latency_ms is the configured 150"),
		Header.Contains(TEXT("\"latency_ms\":150")));

	// Guard against accidentally serializing a Gate A default: these literal
	// field:value pairs must NOT appear, so a regression to defaults is caught.
	TestFalse(TEXT("header did not fall back to default channels 16"),
		Header.Contains(TEXT("\"channels\":16")));
	TestFalse(TEXT("header did not fall back to default latency_ms 200"),
		Header.Contains(TEXT("\"latency_ms\":200")));
	TestFalse(TEXT("header did not fall back to default max_range_m 100"),
		Header.Contains(TEXT("\"max_range_m\":100")));

	return true;
}

// ---------------------------------------------------------------------------
// CameraSamplingInterval (5.3): two concrete cases — the header carries
// `camera_sampling_interval` with the exact ratio when one is provided to
// BeginRun, and the field is OMITTED entirely when the interval is empty (the
// design records it only when camera frames are sampled).
//
// Validates: Requirements 5.3
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxLoggingCameraSamplingIntervalTest,
	"SensorSimSandbox.Logging.CameraSamplingInterval",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxLoggingCameraSamplingIntervalTest::RunTest(const FString& Parameters)
{
	FLidarConfig Cfg; // Gate A defaults; this test is about the camera field only.
	const int64 Seed = 1234;
	const FString Commit = TEXT("deadbeefcafef00d0000000000000000deadbeef");

	// --- Case 1: a sampling interval IS provided -> header carries it -------
	{
		TUniquePtr<FCapturingSink> OwnedSink = MakeUnique<FCapturingSink>();
		FCapturingSink* Sink = OwnedSink.Get();

		FGroundTruthLogger Logger;
		const bool bBegan = Logger.BeginRun(
			Seed, Cfg, Commit, MoveTemp(OwnedSink), /*CameraSamplingInterval*/ TEXT("1/5"));
		TestTrue(TEXT("BeginRun succeeded (interval provided)"), bBegan);

		if (!TestEqual(TEXT("header is the only line written (interval provided)"), Sink->Lines.Num(), 1))
		{
			return false;
		}
		const FString& Header = Sink->Lines[0];
		TestTrue(TEXT("header carries camera_sampling_interval with the exact ratio (5.3)"),
			Header.Contains(TEXT("\"camera_sampling_interval\":\"1/5\"")));
	}

	// --- Case 2: NO sampling interval -> field is OMITTED -------------------
	{
		TUniquePtr<FCapturingSink> OwnedSink = MakeUnique<FCapturingSink>();
		FCapturingSink* Sink = OwnedSink.Get();

		FGroundTruthLogger Logger;
		const bool bBegan = Logger.BeginRun(
			Seed, Cfg, Commit, MoveTemp(OwnedSink), /*CameraSamplingInterval*/ FString());
		TestTrue(TEXT("BeginRun succeeded (no interval)"), bBegan);

		if (!TestEqual(TEXT("header is the only line written (no interval)"), Sink->Lines.Num(), 1))
		{
			return false;
		}
		const FString& Header = Sink->Lines[0];
		// The field must not appear at all — neither the key nor any value.
		TestFalse(TEXT("camera_sampling_interval is omitted when not sampling (5.3)"),
			Header.Contains(TEXT("camera_sampling_interval")));
		// The rest of the header is still well-formed (seed + config present).
		TestTrue(TEXT("header still carries seed when interval omitted"),
			Header.Contains(TEXT("\"seed\":1234")));
		TestTrue(TEXT("header still nests sensor_config.lidar when interval omitted"),
			Header.Contains(TEXT("\"sensor_config\":{\"lidar\":{")));
	}

	return true;
}

// ---------------------------------------------------------------------------
// WriteFailureHandling (5.5): inject a sink that fails on a chosen write and
// assert the full contract across two concrete scenarios:
//
//   (A) MID-STREAM sample-write failure: header + one sample succeed, the next
//       sample write fails. LogSample returns false on the failing write; the
//       run stops (IsRunning()==false, HasWriteFailure()==true); a non-empty
//       write-failure error is emitted; the already-written lines (header +
//       first sample) are RETAINED intact in the sink buffer; a subsequent
//       LogSample is a no-op returning false and writes nothing more.
//
//   (B) HEADER-write failure: the very first write (the header) fails. BeginRun
//       returns false, the run never becomes active, the error is emitted, the
//       sink retains nothing, and a later LogSample is still a no-op.
//
// The failing path emits a "Run_Log write failure" error BY DESIGN; register it
// as an expected error (once per deliberate failure) so the automation harness
// does not fail the test on the intentional error log.
//
// Validates: Requirements 5.5
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxLoggingWriteFailureHandlingTest,
	"SensorSimSandbox.Logging.WriteFailureHandling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxLoggingWriteFailureHandlingTest::RunTest(const FString& Parameters)
{
	// Two deliberate write-failure errors are emitted (one per scenario). Each
	// matches the logger's "Run_Log write failure" message; register the
	// expected error twice so neither intentional error fails the test.
	AddExpectedErrorPlain(TEXT("Run_Log write failure"), EAutomationExpectedErrorFlags::Contains, 2);

	FLidarConfig Cfg; // Gate A defaults.
	const int64 Seed = 1234;
	const FString Commit = TEXT("deadbeefcafef00d0000000000000000deadbeef");

	// --- (A) MID-STREAM sample-write failure --------------------------------
	// Writes: #1 header (ok), #2 first sample (ok), #3 second sample (FAILS).
	{
		FFailingAtNSink* Sink = new FFailingAtNSink(/*FailAtWrite*/ 3);
		FGroundTruthLogger Logger;

		const bool bBegan = Logger.BeginRun(Seed, Cfg, Commit, TUniquePtr<ILogRunSink>(Sink), FString());
		TestTrue(TEXT("(A) BeginRun succeeded (header write #1 ok)"), bBegan);
		TestTrue(TEXT("(A) run active after a successful header write"), Logger.IsRunning());

		const bool bFirst = Logger.LogSample(MakeLidarSample(/*Seq*/ 0, 0.0, 0.2));  // write #2
		TestTrue(TEXT("(A) first sample write succeeds and returns true"), bFirst);
		TestTrue(TEXT("(A) run still active after the first sample"), Logger.IsRunning());
		TestEqual(TEXT("(A) one sample recorded after the first write"), Logger.GetSamplesWritten(), (int64)1);

		const bool bSecond = Logger.LogSample(MakeLidarSample(/*Seq*/ 1, 0.1, 0.3)); // write #3, FAILS
		TestFalse(TEXT("(A) LogSample returns false on the failing write"), bSecond);

		// Run stopped by the write failure.
		TestFalse(TEXT("(A) IsRunning() is false after the write failure (5.5)"), Logger.IsRunning());
		TestTrue(TEXT("(A) HasWriteFailure() is true after the write failure (5.5)"), Logger.HasWriteFailure());

		// A non-empty, identifying write-failure error was emitted (5.5).
		const FString& Err = Logger.GetWriteFailureError();
		TestTrue(TEXT("(A) a non-empty write-failure error is emitted (5.5)"), !Err.IsEmpty());
		TestTrue(TEXT("(A) the error identifies it as a Run_Log write failure"),
			Err.Contains(TEXT("Run_Log write failure")));

		// The samples counter did not advance on the failed write.
		TestEqual(TEXT("(A) sample counter unchanged by the failed write"), Logger.GetSamplesWritten(), (int64)1);

		// RETAINED data: the two successful writes (header + first sample) are
		// still in the sink buffer, untouched, in order.
		if (TestEqual(TEXT("(A) sink retains exactly the two successful lines (5.5)"), Sink->Lines.Num(), 2))
		{
			TestTrue(TEXT("(A) retained line 0 is the run_meta header"),
				Sink->Lines[0].Contains(TEXT("\"type\":\"run_meta\"")));
			TestTrue(TEXT("(A) retained line 1 is the first sample (seq 0), intact"),
				Sink->Lines[1].Contains(TEXT("\"type\":\"sample\"")) &&
				Sink->Lines[1].Contains(TEXT("\"seq\":0")));
		}

		// A subsequent LogSample is a no-op returning false and writes nothing.
		const int32 WriteCountBefore = Sink->WriteCount;
		const bool bAfter = Logger.LogSample(MakeLidarSample(/*Seq*/ 2, 0.2, 0.4));
		TestFalse(TEXT("(A) a LogSample after the failure is a no-op returning false"), bAfter);
		TestEqual(TEXT("(A) no further line count change after the no-op"), Sink->Lines.Num(), 2);
		TestEqual(TEXT("(A) the sink's WriteLine is not even called again after stop"),
			Sink->WriteCount, WriteCountBefore);
	}

	// --- (B) HEADER-write failure -------------------------------------------
	// Writes: #1 header (FAILS). The run never starts.
	{
		FFailingAtNSink* Sink = new FFailingAtNSink(/*FailAtWrite*/ 1);
		FGroundTruthLogger Logger;

		const bool bBegan = Logger.BeginRun(Seed, Cfg, Commit, TUniquePtr<ILogRunSink>(Sink), FString());
		TestFalse(TEXT("(B) BeginRun returns false when the header write fails (5.5)"), bBegan);
		TestFalse(TEXT("(B) run never becomes active on a header-write failure"), Logger.IsRunning());
		TestTrue(TEXT("(B) HasWriteFailure() is true after the header-write failure (5.5)"),
			Logger.HasWriteFailure());

		const FString& Err = Logger.GetWriteFailureError();
		TestTrue(TEXT("(B) a non-empty write-failure error is emitted (5.5)"), !Err.IsEmpty());
		TestTrue(TEXT("(B) the error identifies it as a Run_Log write failure"),
			Err.Contains(TEXT("Run_Log write failure")));

		// Nothing was retained: the failing header write kept no line.
		TestEqual(TEXT("(B) sink retains nothing when the header write fails"), Sink->Lines.Num(), 0);
		TestEqual(TEXT("(B) no sample records were written"), (int32)Logger.GetSamplesWritten(), 0);

		// A LogSample is still a no-op (the run never started).
		const bool bAfter = Logger.LogSample(MakeLidarSample(/*Seq*/ 0, 0.0, 0.2));
		TestFalse(TEXT("(B) LogSample after a failed BeginRun is a no-op returning false"), bAfter);
		TestEqual(TEXT("(B) still nothing retained after the no-op LogSample"), Sink->Lines.Num(), 0);
	}

	return true;
}
