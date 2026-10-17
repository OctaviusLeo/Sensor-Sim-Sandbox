# Task 6.1 — Lidar_Model raycast scanning (evidence)

**Requirements:** 3.1, 3.5, 3.6, 3.7
**Command (host, UE 5.8):**

```
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_lidar.ps1
```

which clears the UBT `Makefile.bin` cache, builds `SensorSimSandboxEditor Win64
Development` with `Build.bat ... -WaitMutex`, then runs the headless scan via a
`-game` launch of `/Game/Maps/RoverLevel`:

```
UnrealEditor.exe <uproject> /Game/Maps/RoverLevel -game -ResX=640 -ResY=480 -windowed ^
  -ExecCmds="LidarSelfCheck \"config\lidar.yaml\" 1000;Quit" -unattended -nopause -nosplash -abslog=<log>
```

A live world is required because the lidar casts real line traces. The console
command `LidarSelfCheck` (Sensors/LidarSelfCheck.cpp) loads the Gate A config,
spawns a 20 m enclosure (4 walls + floor + ceiling + 3 pillars, with collision)
500 m above the level so no level actor overlaps the sensor origin, then
performs raycast scans.

## What was verified

| Check | Result |
| --- | --- |
| Compiles cleanly (editor target) | `Result: Succeeded` (compile.log) |
| Gate A config loaded + validated via the shared bound set | channels=16, h_fov=360, v_fov=30, res=1.0 → **5760 rays/scan**, max_range=100 m, rate=10 Hz, sigma=0.02, p=0.10, latency=200 ms |
| Out-of-bound config rejected naming the parameter (shared contract) | `channels=999 rejected=1 offending='channels'` |
| ≥1000 returns with per-ray truth + published | **5149 returns** in one scan (5718 hits of 5760 rays) |
| Per-ray Ground_Truth captured BEFORE the pipeline | truth values are the pristine line-trace ranges; published = truth + N(0, 0.02²) |
| No-hit rays are `null` and excluded from stats | **42 no-hit** rays this scan; pure probe injected 800 no-hit of 4000 → all excluded, 0 marked dropped |
| Per-ray dropout over hit rays only | **569 dropped** of 5718 hits ≈ 9.9% (configured 10%) |
| Ranges are NOT clamped | no-clamp probe: truth all at max_range (100 m) → **2201 / 5000** noisy returns exceeded max_range |

## Key log lines (from `task_6_1_lidar_selfcheck.log`)

```
LogLidarSelfCheck: loaded Gate A config .../config/lidar.yaml: channels=16 h_fov=360.0 v_fov=30.0 res=1.00 -> 5760 rays/scan, max_range=100.0 m, sigma=0.0200, p=0.100, latency=200 ms
LogLidarSelfCheck: out-of-bound channels=999 rejected=1 offending='channels' msg='parameter 'channels' value 999 is out of bounds; expected within [1, 128]'
LogLidarSelfCheck: pure no-hit exclusion: injected 800 no-hit of 4000 rays; excluded-from-stats=1 (dropped among hits=312)
LogLidarSelfCheck: return seq=0 ray=1 ch=0 az=1.0 truth=10.25080 m published=10.26511 m
LogLidarSelfCheck: return seq=0 ray=14 ch=0 az=14.0 truth=10.56300 m published=10.53180 m
LogLidarSelfCheck: return seq=0 ray=19 ch=0 az=19.0 truth=4.76986 m published=4.76044 m
LogLidarSelfCheck: scan 0: 5760 rays, 5718 hits, 5149 returns, 569 dropped (cumulative returns=5149)
LogLidarSelfCheck: no-clamp probe: 2201 of 5000 returns at truth=max_range exceeded max_range (expect >0 when unclamped & sigma>0); observed-unclamped-in-scans=0
LogLidarSelfCheck: LidarSelfCheck PASS: config=.../config/lidar.yaml rays/scan=5760 scans=1 returns=5149 hits=5718 dropped=569 no-hit=42 | configValid=1 outOfBoundRejected=1 enough(>=1000)=1 noHitExcluded=1 noClamp=1
```

Full log: `docs/evidence/task_6_1_lidar_selfcheck.log`.

## Design conformance

- **Scan geometry** (Sensors/LidarScanGeometry, pure/testable): `channels` rows
  spaced evenly across `v_fov_deg` centered on the horizontal plane; azimuth
  columns sweep `h_fov_deg` at `angular_resolution_deg`; a full 360° sweep uses
  `round(h_fov/res)` columns without a duplicate wrap sample. 16 × 360 = 5,760
  rays, matching the design's Gate A note.
- **Capture-before-pipeline / no-hit→null→excluded / no-clamp** are enforced in
  `ALidarModel::PerformScan` → `FLidarPipelineRunner::ProcessCapturedScan`
  (`bClamp=false`), reusing the shared `FSensorPipeline` (seeded noise →
  per-element dropout) and the per-sensor RNG (`hash(run_seed, "lidar")`).
- **Config bounds** use the SHARED `FSensorBoundSet` (`FLidarConfig::MakeBoundSet`),
  the same bounds the Property 2 config-bounds test enumerates.

The pure core (`FLidarPipelineRunner`, `FLidarScanGeometry`, `FLidarConfig`) has
no UObject/World dependency, so the no-hit-exclusion and no-clamp rules are also
proven here without a running world (the two "pure probe" log lines above).
