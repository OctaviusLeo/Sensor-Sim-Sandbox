// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Sensors/LidarConfig.h"
#include "Sensors/CameraConfig.h"
#include "Pipeline/SensorPipeline.h"

/**
 * GroundTruthLogger (design "GroundTruthLogger" + "Ground-truth logging
 * format", Requirement 5.1-5.5).
 *
 * Writes a Run_Log as a `.jsonl` file: the first line is a metadata header and
 * every subsequent line is one sample record (one per sensor sample, so the
 * truth and the published value of the same capture always share a record).
 *
 * Interface (design):
 *   - BeginRun(seed, config)  writes the metadata header line.
 *   - LogSample(...)          appends one sample record.
 *   - EndRun()                flushes to disk.
 *
 * Contract (design / Requirement 5):
 *   - `t` is monotonically non-decreasing seconds since run start (5.1). The
 *     logger ENFORCES this: it carries each record's `t` forward so a record
 *     whose supplied `t` is less than the previous record's `t` is clamped up
 *     to the previous value (a captured dropped frame released out of order
 *     never regresses file order). The run start is captured in BeginRun.
 *   - The format is plain JSONL the analysis tool reads without conversion (5.2).
 *   - The header stores the seed, the full commit SHA, the full per-sensor
 *     config, and, when camera frames are sampled, the camera sampling interval
 *     ratio (5.3, 5.4). The camera field is plumbed here so M3 only has to set
 *     it; it is omitted from the header when empty.
 *   - A write failure stops the run, RETAINS all records already flushed to
 *     disk, and emits a write-failure error identifying the failure (5.5).
 *
 * Design for testability: all engine-light logic (record/header serialization,
 * `t` monotonicity, write-failure handling) runs against an injectable sink
 * (ILogRunSink) rather than a hard-coded file path, so the Property 8 test
 * (7.2) and the metadata / write-failure example tests (7.3) drive the logger
 * headlessly with no world. The default sink is a buffered file sink writing
 * under runs/<run_id>/ (gitignored). A failing sink is injected to exercise 5.5.
 */

// ---------------------------------------------------------------------------
// Record payload types
// ---------------------------------------------------------------------------

/**
 * The rover pose carried in each record, already in REP 103 (meters) per the
 * design record schema: `pos = [x, y, z]`, `quat = [w, x, y, z]`.
 */
struct FLogRoverPose
{
	double PosX = 0.0;
	double PosY = 0.0;
	double PosZ = 0.0;
	double QuatW = 1.0;
	double QuatX = 0.0;
	double QuatY = 0.0;
	double QuatZ = 0.0;
};

/**
 * One sensor sample to log. A single struct serves every sensor shape the
 * design's record format allows:
 *   - Lidar: set `Sensor="lidar"`, fill `Truth`/`Published` per-ray arrays
 *     (empty TOptional = null for no-hit / dropped), leave the image refs unset.
 *   - Camera: set `Sensor="camera"`, leave the ranges empty, set the image refs
 *     on SAMPLED frames (TruthRef/PublishedRef present) or leave both unset so
 *     they serialize as null (unsampled published frame). A dropped frame sets
 *     bDropped=true, TPublish unset (null), and carries no refs.
 *
 * `t_publish` is a TOptional: empty => null (a dropped sample).
 */
struct FLogSample
{
	/** `"lidar"`, `"camera"` (M5: `"imu"`, `"gps"`). */
	FString Sensor;

	/** Capture counter per sensor, from 0. */
	int64 Seq = 0;

	/** Capture time, seconds since run start. */
	double TCapture = 0.0;

	/** Release time, seconds since run start; empty => null (dropped). */
	TOptional<double> TPublish;

	/** Whole-sample dropout (camera frames; always false for lidar). */
	bool bDropped = false;

	/** Rover pose in REP 103 at capture. */
	FLogRoverPose Pose;

	// --- Lidar payload (per-ray ranges; empty entry => null) ---------------
	/** True when this is a lidar sample carrying per-ray truth/published arrays. */
	bool bHasRangeArrays = false;
	TArray<FSensorValue> Truth;
	TArray<FSensorValue> Published;

	// --- Camera payload (image refs; unset => field omitted as null) -------
	/** True when this is a camera sample (emits truth_ref/published_ref). */
	bool bIsCameraFrame = false;
	TOptional<FString> TruthRef;
	TOptional<FString> PublishedRef;
};

// ---------------------------------------------------------------------------
// Serialization (pure; no sink, no engine) — the single source of the exact
// on-disk format from design.md "Ground-truth logging format".
// ---------------------------------------------------------------------------

/**
 * Pure JSONL serialization of the header and sample records. Kept free of any
 * sink or file so the exact byte format can be unit-tested directly and reused
 * by the logger and the demo self-check.
 */
class FGroundTruthLogFormat
{
public:
	/**
	 * Serialize the metadata header line (no trailing newline). `CommitSha` is
	 * the full commit SHA. `CameraSamplingInterval` is written only when
	 * non-empty (present only when camera frames are sampled, design 5.3).
	 */
	static FString SerializeHeader(
		int64 Seed,
		const FString& CommitSha,
		const FLidarConfig& LidarConfig,
		const FString& CameraSamplingInterval,
		const FCameraConfig* CameraConfig = nullptr,
		const TArray<TPair<FString, FString>>* ExtraSensorConfigs = nullptr);

	/** Serialize one sample record line (no trailing newline). */
	static FString SerializeSample(const FLogSample& Sample, double TWritten);

	/** Serialize a per-ray range array ("[...]"), empty entries as `null`. */
	static FString SerializeRangeArray(const TArray<FSensorValue>& Values);
};

// ---------------------------------------------------------------------------
// Sink abstraction (injectable so write-failure is testable, 5.5)
// ---------------------------------------------------------------------------

/**
 * Where the logger writes its lines. The default sink appends to a `.jsonl`
 * file under runs/<run_id>/; tests inject a sink that captures lines in memory
 * or fails on demand to exercise Requirement 5.5. A line is logically one
 * record (header or sample); the sink appends the trailing newline.
 */
class ILogRunSink
{
public:
	virtual ~ILogRunSink() = default;

	/**
	 * Append one line (the logger passes the line WITHOUT a trailing newline;
	 * the sink appends it). Returns false on a write failure; the logger then
	 * stops the run and emits the error (5.5). On failure the sink must retain
	 * everything already successfully written (5.5).
	 */
	virtual bool WriteLine(const FString& Line) = 0;

	/** Flush buffered data to its backing store. Returns false on failure. */
	virtual bool Flush() = 0;

	/** A human-readable identifier of the backing store, for error messages. */
	virtual FString Describe() const = 0;
};

/**
 * The default file sink: appends UTF-8 lines to a `.jsonl` file, flushing on
 * demand. Each successful WriteLine is persisted (append + the data is retained
 * on any later failure), satisfying the "retain already-written data" rule.
 */
class FFileLogRunSink : public ILogRunSink
{
public:
	explicit FFileLogRunSink(const FString& InFilePath);

	virtual bool WriteLine(const FString& Line) override;
	virtual bool Flush() override;
	virtual FString Describe() const override { return FilePath; }

	const FString& GetFilePath() const { return FilePath; }

private:
	FString FilePath;
	bool bEverWritten = false;
};

// ---------------------------------------------------------------------------
// The logger
// ---------------------------------------------------------------------------

class FGroundTruthLogger
{
public:
	FGroundTruthLogger() = default;

	/**
	 * BeginRun: adopt `Sink`, capture the run start, and write the metadata
	 * header (seed, commit, full sensor config, and camera sampling interval
	 * when sampling). Returns false (and stops the run) if the header write
	 * fails (5.5). `CommitSha` is the full SHA; `CameraSamplingInterval` is the
	 * frames-logged-per-frames-produced ratio (e.g. "1/10"), empty when camera
	 * frames are logged in full (then the header omits the field).
	 */
	bool BeginRun(
		int64 Seed,
		const FLidarConfig& LidarConfig,
		const FString& CommitSha,
		TUniquePtr<ILogRunSink> Sink,
		const FString& CameraSamplingInterval = FString(),
		const FCameraConfig* CameraConfig = nullptr);

	/**
	 * LogSample: append one sample record. `t` is derived here as the record's
	 * write time, seconds since run start, and ENFORCED monotonically
	 * non-decreasing: `t = max(prev_t, requested_t)` where requested_t is the
	 * release time for a published sample or the capture time for a dropped
	 * one. Returns false (and stops the run) on a write failure (5.5).
	 */
	bool LogSample(const FLogSample& Sample);

	/** EndRun: flush the sink. Returns false on a flush failure (5.5). */
	bool EndRun();

	/**
	 * Add `"Name": <ConfigJson>` to the header's sensor_config (IMU / GPS
	 * channels, M5). Call before BeginRun.
	 */
	void AddSensorConfigJson(const FString& Name, const FString& ConfigJson) { ExtraSensorConfigs.Emplace(Name, ConfigJson); }

	/** True while the run is active (BeginRun succeeded, not stopped/ended). */
	bool IsRunning() const { return bRunning; }

	/** True if the run was stopped by a write failure (5.5). */
	bool HasWriteFailure() const { return bWriteFailed; }

	/** The error emitted on a write failure (empty when none). */
	const FString& GetWriteFailureError() const { return WriteFailureError; }

	/** Count of sample records successfully written so far. */
	int64 GetSamplesWritten() const { return SamplesWritten; }

	/** The last `t` written (seconds); the monotonic floor for the next record. */
	double GetLastT() const { return LastT; }

private:
	TUniquePtr<ILogRunSink> Sink;

	bool bRunning = false;
	bool bWriteFailed = false;
	FString WriteFailureError;

	double LastT = 0.0;
	bool bHasLastT = false;
	int64 SamplesWritten = 0;
	TArray<TPair<FString, FString>> ExtraSensorConfigs;

	/** Emit the write-failure error (log + stored) and stop the run (5.5). */
	void FailRun(const FString& Context);
};
