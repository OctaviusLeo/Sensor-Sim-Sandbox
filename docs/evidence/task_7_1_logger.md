# Task 7.1 — GroundTruthLogger (JSONL Run_Log) evidence

Requirements: 5.1, 5.2, 5.3, 5.4, 5.5

## What was built

- `unreal/SensorSimSandbox/Source/SensorSimSandbox/GroundTruth/GroundTruthLogger.{h,cpp}`
  - `FGroundTruthLogger` — `BeginRun(seed, config)` writes the metadata header,
    `LogSample(...)` appends one record per sensor sample, `EndRun()` flushes.
  - `FGroundTruthLogFormat` — the pure serializer producing the exact JSONL in
    design.md's "Ground-truth logging format" (header + sample record), reused by
    the logger and the demo so there is a single source of the on-disk format.
  - `ILogRunSink` / `FFileLogRunSink` — an injectable sink. The file sink appends
    each line immediately (`FILEWRITE_Append`), so a later write failure never
    loses already-flushed records. A failing sink is injectable so the
    write-failure path (5.5) is testable without touching the disk.
- `GroundTruth/GroundTruthLoggerSelfCheck.{h,cpp}` — the one-command demo/verify,
  exposed as the console command `GroundTruthLoggerSelfCheck` (live `-game` run
  with real raycast scans) and driven headlessly by the automation test below.
- `Tests/GroundTruthLoggerTest.cpp`
  - `SensorSimSandbox.Logging.GroundTruthLoggerSelfCheck` — end-to-end: writes a
    real `.jsonl` Run_Log under `runs/<run_id>/`, re-reads it, and asserts the
    header carries the seed/config/camera-interval, `t` is non-decreasing, and the
    write-failure path stops the run while retaining flushed data.
  - `SensorSimSandbox.Logging.GroundTruthLogFormat` — byte-level format checks of
    the header and lidar/camera (sampled, unsampled, dropped) records.

## Record format (design.md "Ground-truth logging format")

- Header: `{"type":"run_meta","seed":...,"commit":"<full-sha>","sensor_config":{"lidar":{...}}[,"camera_sampling_interval":"1/10"]}`
  (`camera_sampling_interval` present only when camera frames are sampled — 5.3).
- Sample: `{"type":"sample","sensor":...,"seq":...,"t":...,"t_capture":...,"t_publish":...|null,"dropped":bool,"rover_pose":{"pos":[x,y,z],"quat":[w,x,y,z]}, ...payload}`
  - lidar payload: `"truth":[...]`, `"published":[...]` (empty rays serialize as `null`);
  - camera payload: `"truth_ref"`/`"published_ref"` (image refs on sampled frames,
    `null` on unsampled published frames, omitted on dropped frames).
- `t` = record write time, seconds since run start, monotonically non-decreasing
  in file order (5.1); enforced by carrying the previous `t` forward.

## Build (zero errors)

```
Build.bat SensorSimSandboxEditor Win64 Development -Project="...\SensorSimSandbox.uproject" -WaitMutex
Result: Succeeded
```

## Headless verification (one command)

```
UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests \
  SensorSimSandbox.Logging.GroundTruthLoggerSelfCheck+SensorSimSandbox.Logging.GroundTruthLogFormat;Quit" \
  -unattended -nopause -nosplash -nullrhi -testexit="Automation Test Queue Empty" -log
```

Result: `succeeded=2 failed=0 notRun=0` (report `runs/logs/task7_1/report2/index.json`).

The run wrote `runs/<run_id>/run_log.jsonl`. A copy of one such Run_Log is saved
alongside this file as `task_7_1_run_log_sample.jsonl`.

### Header (seed + full sensor config + camera sampling interval)

```json
{"type":"run_meta","seed":1234,"commit":"33e130928f42eada8c9428acc236beffca5a1d97","sensor_config":{"lidar":{"channels":16,"h_fov_deg":360,"v_fov_deg":30,"angular_resolution_deg":1,"max_range_m":100,"rate_hz":10,"range_noise_sigma_m":0.02,"dropout_prob":0.10000000000000001,"latency_ms":200}},"camera_sampling_interval":"1/10"}
```

### `t` is non-decreasing across the sample records

```
line 1 t=0.2
line 2 t=0.3
line 3 t=0.4
line 4 t=0.5
line 5 t=0.6
```

### Write-failure handling (5.5)

From the self-check log (an injected sink that fails on the 3rd write):

```
LogGroundTruthLogger: Error: Run_Log write failure: could not write sample record (sensor=lidar seq=1) (sink: <in-memory failing sink>). Run stopped; 1 record(s) already written are retained.
LogGtLoggerSelfCheck: write-failure check: began=1 first=1 second(expect fail)=0 stopped=1 error=1 retained=1(lines=2) after(expect noop)=0
```

The run stopped on the failed write, the two already-written lines (header + first
sample) were retained intact, the error indication was emitted, and a subsequent
`LogSample` was a no-op.

## Notes / provenance

- Command: see the headless verification command above. Seed: `1234`
  (Gate A run seed). Commit: `33e130928f42eada8c9428acc236beffca5a1d97`.
- Run_Logs are written under `runs/<run_id>/` which is gitignored (not committed).
- The REP 103 rover pose in the demo uses the design's fixed mapping inline; the
  shared converter arrives in task 9 and the demo will switch to it then.
