// Copyright (c) Sensor Sim Sandbox. Private portfolio project.

#include "Misc/AutomationTest.h"

#include "GroundTruth/GroundTruthLogger.h"
#include "GroundTruth/GroundTruthLoggerSelfCheck.h"
#include "Sensors/LidarConfig.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"

// ---------------------------------------------------------------------------
// Feature: sensor-sim-sandbox, task 7.1 (demo/verify of the GroundTruthLogger).
//
// This is the headless, one-command verification for task 7.1's demo/verify:
// it drives the GroundTruthLogger end to end (BeginRun -> LogSample* -> EndRun)
// through the shared FGroundTruthLoggerSelfCheck, writing a REAL `.jsonl`
// Run_Log under runs/<run_id>/ (gitignored), then re-reads it and asserts the
// design's "Ground-truth logging format" invariants:
//
//   - the first line is a run_meta header carrying the seed, the full sensor
//     config, and (when sampling) the camera sampling interval (5.3, 5.4);
//   - every sample record's `t` is monotonically non-decreasing in file order
//     (5.1);
//   - a write failure stops the run, retains already-written data, and emits a
//     write-failure error (5.5).
//
// Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5
//
// It runs with no world (FGroundTruthLoggerSelfCheck falls back to synthetic
// lidar samples when World is null), so it executes reliably under the
// nullrhi automation harness (scripts/run_ue_tests.ps1) — the one command that
// writes a .jsonl Run_Log whose header carries the seed/config and whose `t`
// values are non-decreasing. The live `-game` console command
// `GroundTruthLoggerSelfCheck` performs the same check with real raycast scans.
//
// Nothing here is weakened to pass: a failing assertion is a real format or
// contract bug to investigate.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxGroundTruthLoggerSelfCheckTest,
	"SensorSimSandbox.Logging.GroundTruthLoggerSelfCheck",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxGroundTruthLoggerSelfCheckTest::RunTest(const FString& Parameters)
{
	// The self-check DELIBERATELY triggers the write-failure path (5.5), which
	// emits a write-failure error by design. Register that expected error so the
	// automation framework does not treat the intentional error log as a test
	// failure. The pattern matches the logger's emitted message; it is a
	// regex-less substring expectation (the default), repeated once.
	AddExpectedErrorPlain(TEXT("Run_Log write failure"), EAutomationExpectedErrorFlags::Contains, 1);

	// Write to a unique folder under runs/ so repeated test runs do not collide
	// and so the output stays within the gitignored runs/ tree.
	const FString RunId = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S_%s"));
	const FString RunDir = FPaths::Combine(
		FGroundTruthLoggerSelfCheck::ResolveRunsRoot(),
		FString::Printf(TEXT("gtlog_test_%s"), *RunId));

	const FGroundTruthLoggerSelfCheck::FResult Result =
		FGroundTruthLoggerSelfCheck::Run(/*World*/ nullptr, /*LidarConfigPath*/ FString(), RunDir);

	// A `.jsonl` Run_Log was actually written to disk.
	TestTrue(
		FString::Printf(TEXT("Run_Log file exists on disk (%s)"), *Result.RunLogPath),
		FPaths::FileExists(Result.RunLogPath));

	// Header carries the seed and full sensor config (5.3, 5.4).
	TestTrue(TEXT("header carries seed + full sensor config (5.4) and camera interval (5.3)"),
		Result.bHeaderOk);

	// Sample records were written (5.1, 5.2).
	TestTrue(TEXT("sample records were written (5.1)"), Result.bRecordsWritten);

	// `t` is monotonically non-decreasing in file order (5.1).
	TestTrue(TEXT("Run_Log timestamps are monotonically non-decreasing (5.1)"),
		Result.bMonotonicT);

	// Write failure stops the run, retains data, emits an error (5.5).
	TestTrue(TEXT("write failure stops run, retains flushed data, emits error (5.5)"),
		Result.bWriteFailureHandled);

	if (!Result.Passed())
	{
		AddError(FString::Printf(TEXT("GroundTruthLoggerSelfCheck did not pass: %s"), *Result.Detail));
	}

	return Result.Passed();
}

// ---------------------------------------------------------------------------
// A focused format test: the serialized header and sample records match the
// design's "Ground-truth logging format" byte layout exactly (field names,
// nesting, null handling), independent of file I/O. This guards the format the
// Python analysis loaders (task 16) parse.
//
// Validates: Requirements 5.1, 5.2, 5.3, 5.4
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSensorSimSandboxGroundTruthLogFormatTest,
	"SensorSimSandbox.Logging.GroundTruthLogFormat",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSensorSimSandboxGroundTruthLogFormatTest::RunTest(const FString& Parameters)
{
	// --- Header -------------------------------------------------------------
	FLidarConfig Cfg; // Gate A defaults
	const FString Header = FGroundTruthLogFormat::SerializeHeader(
		/*Seed*/ 1234, /*Commit*/ TEXT("abc123"), Cfg, /*CameraSamplingInterval*/ TEXT("1/10"));

	TestTrue(TEXT("header type is run_meta"), Header.Contains(TEXT("\"type\":\"run_meta\"")));
	TestTrue(TEXT("header carries seed"), Header.Contains(TEXT("\"seed\":1234")));
	TestTrue(TEXT("header carries full commit"), Header.Contains(TEXT("\"commit\":\"abc123\"")));
	TestTrue(TEXT("header nests sensor_config.lidar"),
		Header.Contains(TEXT("\"sensor_config\":{\"lidar\":{")));
	TestTrue(TEXT("header lidar config carries channels"), Header.Contains(TEXT("\"channels\":16")));
	TestTrue(TEXT("header lidar config carries dropout_prob"),
		Header.Contains(TEXT("\"dropout_prob\":0.1")));
	TestTrue(TEXT("header carries camera_sampling_interval when sampling (5.3)"),
		Header.Contains(TEXT("\"camera_sampling_interval\":\"1/10\"")));

	// camera_sampling_interval is OMITTED when empty (frames logged in full).
	const FString HeaderNoCam = FGroundTruthLogFormat::SerializeHeader(
		1234, TEXT("abc123"), Cfg, /*CameraSamplingInterval*/ FString());
	TestFalse(TEXT("camera_sampling_interval omitted when not sampling"),
		HeaderNoCam.Contains(TEXT("camera_sampling_interval")));

	// --- Lidar sample record: null handling in truth/published -------------
	{
		FLogSample S;
		S.Sensor = TEXT("lidar");
		S.Seq = 42;
		S.TCapture = 12.345;
		S.TPublish = 12.545;
		S.bDropped = false;
		S.Pose.PosX = 1.0; S.Pose.PosY = 2.0; S.Pose.PosZ = 3.0;
		S.Pose.QuatW = 1.0; S.Pose.QuatX = 0.0; S.Pose.QuatY = 0.0; S.Pose.QuatZ = 0.0;
		S.bHasRangeArrays = true;
		S.Truth = { FSensorValue(10.0), FSensorValue(), FSensorValue(20.0) };     // middle = no hit
		S.Published = { FSensorValue(10.02), FSensorValue(), FSensorValue() };    // dropped/no-hit = null

		const FString Rec = FGroundTruthLogFormat::SerializeSample(S, /*TWritten*/ 12.545);
		TestTrue(TEXT("lidar record type is sample"), Rec.Contains(TEXT("\"type\":\"sample\"")));
		TestTrue(TEXT("lidar record sensor"), Rec.Contains(TEXT("\"sensor\":\"lidar\"")));
		TestTrue(TEXT("lidar record seq"), Rec.Contains(TEXT("\"seq\":42")));
		TestTrue(TEXT("lidar record t"), Rec.Contains(TEXT("\"t\":12.545")));
		TestTrue(TEXT("lidar record t_capture"), Rec.Contains(TEXT("\"t_capture\":12.345")));
		TestTrue(TEXT("lidar record t_publish"), Rec.Contains(TEXT("\"t_publish\":12.545")));
		TestTrue(TEXT("lidar record dropped=false"), Rec.Contains(TEXT("\"dropped\":false")));
		TestTrue(TEXT("lidar record rover_pose.pos"), Rec.Contains(TEXT("\"rover_pose\":{\"pos\":[1,2,3]")));
		TestTrue(TEXT("lidar record rover_pose.quat"), Rec.Contains(TEXT("\"quat\":[1,0,0,0]}")));
		// truth has a null in the middle, published has two nulls.
		TestTrue(TEXT("lidar truth array with null"), Rec.Contains(TEXT("\"truth\":[10,null,20]")));
		TestTrue(TEXT("lidar published array with nulls"),
			Rec.Contains(TEXT("\"published\":[10.02,null,null]")));
	}

	// --- Dropped camera frame: null t_publish, no image refs ---------------
	{
		FLogSample S;
		S.Sensor = TEXT("camera");
		S.Seq = 18;
		S.TCapture = 12.451;
		S.TPublish.Reset(); // dropped -> null
		S.bDropped = true;
		S.bIsCameraFrame = true;

		const FString Rec = FGroundTruthLogFormat::SerializeSample(S, /*TWritten*/ 12.451);
		TestTrue(TEXT("dropped camera sensor"), Rec.Contains(TEXT("\"sensor\":\"camera\"")));
		TestTrue(TEXT("dropped camera t_publish null"), Rec.Contains(TEXT("\"t_publish\":null")));
		TestTrue(TEXT("dropped camera dropped=true"), Rec.Contains(TEXT("\"dropped\":true")));
		TestFalse(TEXT("dropped camera carries no image refs"), Rec.Contains(TEXT("truth_ref")));
	}

	// --- Sampled camera frame: image refs present -------------------------
	{
		FLogSample S;
		S.Sensor = TEXT("camera");
		S.Seq = 17;
		S.TCapture = 12.350;
		S.TPublish = 12.550;
		S.bDropped = false;
		S.bIsCameraFrame = true;
		S.TruthRef = TEXT("frames/cam_000017.png");
		S.PublishedRef = TEXT("frames/cam_000017_pub.jpg");

		const FString Rec = FGroundTruthLogFormat::SerializeSample(S, /*TWritten*/ 12.550);
		TestTrue(TEXT("sampled camera truth_ref"),
			Rec.Contains(TEXT("\"truth_ref\":\"frames/cam_000017.png\"")));
		TestTrue(TEXT("sampled camera published_ref"),
			Rec.Contains(TEXT("\"published_ref\":\"frames/cam_000017_pub.jpg\"")));
	}

	// --- Unsampled published camera frame: explicit null refs --------------
	{
		FLogSample S;
		S.Sensor = TEXT("camera");
		S.Seq = 19;
		S.TCapture = 12.6;
		S.TPublish = 12.8;
		S.bDropped = false;
		S.bIsCameraFrame = true;
		// TruthRef / PublishedRef left unset -> serialize as null.

		const FString Rec = FGroundTruthLogFormat::SerializeSample(S, /*TWritten*/ 12.8);
		TestTrue(TEXT("unsampled camera null truth_ref"), Rec.Contains(TEXT("\"truth_ref\":null")));
		TestTrue(TEXT("unsampled camera null published_ref"),
			Rec.Contains(TEXT("\"published_ref\":null")));
	}

	return true;
}
