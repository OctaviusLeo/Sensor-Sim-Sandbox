// Copyright (c) 2026 OctaviusLeo. MIT License.

#pragma once

#include "CoreMinimal.h"

/**
 * Deterministic HEADLESS demo/verify for the camera logging path (task 15,
 * Requirement 5.3, 4.3).
 *
 * Task 15 WIRES the camera frames into the GroundTruthLogger end to end: one
 * Run_Log record per PRODUCED camera frame (including dropped frames), with
 * image refs ONLY on sampled, non-dropped frames, and the camera sampling
 * interval ratio in the header. The logger DATA MODEL already supports every
 * camera record shape (task 7.1 FLogSample + FGroundTruthLogFormat, verified by
 * SensorSimSandbox.Logging.GroundTruthLogFormat); this check proves the WIRING:
 * it drives a deterministic sequence of synthetic frames through the SHARED
 * camera pipeline (FCameraPipelineRunner) and the REAL FGroundTruthLogger,
 * writes a real `.jsonl` Run_Log under runs/<run_id>/ (gitignored), saves the
 * truth-PNG / published-JPEG image pair ONLY for sampled non-dropped frames
 * under the run folder, publishes every non-dropped frame's CompressedImage +
 * CameraInfo through a Rosbridge_Client, then re-reads the Run_Log and asserts
 * the whole camera record contract.
 *
 * It needs NO world and NO GPU RHI: FCameraPipelineRunner is the pure core
 * (noise / dropout / latency on synthetic frames) and the image encoders are
 * CPU-only (libjpeg / libpng via ImageWrapper), so it runs under the -nullrhi
 * automation harness (scripts/run_ue_tests.ps1). The live `-game` CameraPublish
 * command (task 14.1) exercises the real GPU render + live rosbridge; THIS
 * headless check on a deterministic frame sequence is the authoritative gate.
 *
 * Determinism of the mix: the sampling decision is deterministic from the
 * config's "N/M" interval (FCameraConfig::IsFrameSampled), and the dropout
 * draws are the seeded pipeline's, so a fixed (seed, config, frame count)
 * yields the SAME sampled / unsampled / dropped mix every run. The check
 * requires the mix to contain at least one of each category and FAILS loudly if
 * the chosen parameters ever stop producing all three (never silently passing).
 *
 * Assertions re-read from the written Run_Log (design "Ground-truth logging
 * format", camera rows):
 *   - header is a run_meta carrying camera_sampling_interval == the ratio (5.3);
 *   - exactly one record per PRODUCED frame, dropped frames included — the
 *     record count equals the number of frames produced;
 *   - every record is sensor "camera" and carries dropped, t_capture, t_publish;
 *   - a SAMPLED non-dropped frame carries BOTH truth_ref and published_ref, and
 *     the two referenced image files exist on disk;
 *   - an UNSAMPLED non-dropped frame carries explicit null refs;
 *   - a DROPPED frame has dropped=true, t_publish null, and no image refs;
 *   - t values are non-decreasing in file order (5.1);
 *   - every non-dropped frame was published through the Rosbridge_Client (the
 *     image + camera_info ops were built and sent; count equals non-dropped).
 *
 * Logs a single `CameraLoggerSelfCheck PASS`/`FAIL` line a verify script can
 * grep, plus the counts and the Run_Log path.
 */
class FCameraLoggerSelfCheck
{
public:
	struct FResult
	{
		bool bHeaderHasInterval = false;    // header camera_sampling_interval == ratio (5.3)
		bool bRecordPerProducedFrame = false; // one record per produced frame (dropped incl.)
		bool bSampledRefsPresent = false;   // sampled frames carry both refs + files exist (4.3, 5.3)
		bool bUnsampledRefsNull = false;    // unsampled non-dropped frames have null refs
		bool bDroppedShape = false;         // dropped: dropped=true, t_publish null, no refs
		bool bMonotonicT = false;           // t non-decreasing in file order (5.1)
		bool bPublishedNonDropped = false;  // every non-dropped frame published through the client

		int32 FramesProduced = 0;
		int32 SampledCount = 0;
		int32 UnsampledCount = 0;
		int32 DroppedCount = 0;
		int32 RecordsRead = 0;
		int32 PublishedCount = 0;
		int32 ImageFilesWritten = 0;
		FString RunLogPath;
		FString SamplingInterval;
		FString Detail;

		bool Passed() const
		{
			return bHeaderHasInterval && bRecordPerProducedFrame &&
				bSampledRefsPresent && bUnsampledRefsNull && bDroppedShape &&
				bMonotonicT && bPublishedNonDropped;
		}
	};

	/**
	 * Run the headless camera logging check. `CameraConfigPath` empty resolves
	 * config/camera.yaml. `RunDir` empty resolves runs/<timestamp>/. No world /
	 * RHI is required.
	 */
	static FResult Run(const FString& CameraConfigPath, const FString& RunDir);
};
