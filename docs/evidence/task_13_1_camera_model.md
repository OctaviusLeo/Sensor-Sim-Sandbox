# Task 13.1 — Camera_Model (scene-capture) + shared pipeline: evidence

Feature: sensor-sim-sandbox. Requirements 4.1, 4.2, 4.5, 4.6.
Parent commit at implementation: `cd47afbb18c5a402d19b9995d817f5a72849a3ff` (the task 13.1
commit adds the files below on top of this).
Host: Windows 11 desktop, UE `C:\Program Files (x86)\UE_5.8`, MSVC 14.51.36260,
Windows SDK 10.0.28000.0. Last checked: 2026-10-17 (session date of the runs below).

All claims graded per the archivist rules. "Verified" = produced by a command run
in this session. Evidence is the command and its observed output; file paths cite
the source at the version in use.

## What was built (source at the version in use)

Pure, engine-independent core (stack-constructible, headlessly testable), mirroring the
lidar pattern (`Sensors/LidarConfig.*`, `Sensors/LidarPipelineRunner.*`, `Sensors/LidarModel.*`):

- `config/camera.yaml` — Gate A values: `width` 640, `height` 480, `rate_hz` 10,
  `pixel_noise_sigma` 5.0, `frame_dropout_prob` 0.05, `latency_ms` 200,
  `camera_sampling_interval` "1/10" (design.md "Gate A run configuration").
- `Sensors/CameraConfig.{h,cpp}` — parses the flat YAML; `MakeBoundSet()` builds the
  SHARED `FSensorBoundSet` with the exact bounds the Property 2 test enumerates for the
  camera (`Tests/ConfigBoundsEnforcedTest.cpp`): width/height (1–7680, int),
  rate_hz (0.1–100.0), pixel_noise_sigma (0.0–255.0), frame_dropout_prob (0.0–1.0),
  latency_ms (0–1000, int). `Validate()` delegates to the shared contract; no bespoke
  checker. Carries `SamplingInterval` ("1/10") + `IsFrameSampled(seq)` helper.
- `Sensors/CameraPipelineRunner.{h,cpp}` — PURE core: capture Ground_Truth (caller
  supplies RGBA) → per-pixel Gaussian noise clamped to [0,255] via
  `FSensorPipeline::AddGaussianNoise(..., bClamp=true, 0, 255)` → whole-frame dropout
  via `FSensorPipeline::ApplyFrameDropout` → shared `TLatencyQueue<int64>`. Fixed draw
  order (all RGB noise draws first, then the one frame-dropout draw; alpha carried
  verbatim), so the per-sensor RNG stream advances identically regardless of the dropout
  outcome. No engine/RHI dependency.
- `Sensors/CameraModel.{h,cpp}` — `ACameraModel` AActor: `USceneCaptureComponent2D` +
  `UTextureRenderTarget2D` (RTF_RGBA8, `SCS_FinalColorLDR`), reads back the pristine
  truth pixels (`CaptureScene` → `ReadPixels`) BEFORE handing them to the runner. The
  only engine/RHI-dependent step. Degrades gracefully (returns a dropped frame) with no
  RHI (e.g. `-nullrhi`).
- `Sensors/CameraSelfCheck.{h,cpp}` + console command `CameraSelfCheck` — headless
  demo/verify on SYNTHETIC frames (no world/RHI).
- `Tests/CameraPipelineHeadlessTest.cpp` — automation test
  `SensorSimSandbox.Camera.PipelineHeadless` (headless, `-nullrhi`).
- `SensorSimSandbox.Build.cs` — added `RenderCore`, `RHI` for the render-target readback.

Scope note: building `sensor_msgs/CompressedImage` + `CameraInfo` is task 14, not 13.1;
the task-13.2 in-bound example test (`[~]`) is a separate task and was not written here.

## Verification commands run this session, and observed results

### 1. Clean compile — Verified
Command: `Build.bat SensorSimSandboxEditor Win64 Development -Project="<abs uproject>" -WaitMutex -NoHotReloadFromIDE`
(UBT Makefile.bin cache cleared first). Observed: all five new .cpp files compiled
(`CameraConfig.cpp`, `CameraModel.cpp`, `CameraPipelineRunner.cpp`, `CameraSelfCheck.cpp`,
`CameraPipelineHeadlessTest.cpp`), module linked, final line `Result: Succeeded`
(log: `runs/logs/ue_tests/compile_task13_1.log`). Zero errors, zero warnings from the
new code (one pre-existing warning: MSVC version not a preferred version).

### 2. Full headless UE automation suite stays green — Verified
Command: `pwsh scripts/run_ue_tests.ps1`. Observed final summary:

```
tests run : 24
succeeded : 24
failed    : 0
not run   : 0
RESULT: PASS - all tests passed and at least one ran.
```

(UnrealEditor-Cmd exit 0; report `runs/logs/ue_tests/report/index.json`.) The suite was
23/24 before adding `AddExpectedError` for the intentional reject-path log line (the
reject log IS the Requirement 4.2 "emit an error naming the parameter" behaviour); the
reject's effect — `bValid` false, offending parameter named, last valid config retained —
is still asserted. No test, tolerance, or sample count was loosened.

### 3. Headless camera pipeline numbers (from the passing automation run) — Verified
From `runs/logs/ue_tests/automation.log` (`LogCameraSelfCheck`), on the shipped
`config/camera.yaml` (640×480, sigma 5.0, frame_dropout 0.05, latency 200 ms, 1/10):

| Check | Observed | Target | Pass |
| --- | --- | --- | --- |
| Determinism (two runners, 8 frames) | byte-identical published + dropped flags | identical | yes |
| Per-pixel noise sigma recovery (24,000 RGB draws, mid-grey) | recovered **5.0180** | 5.0 ±10% (±0.5) | yes |
| sigma = 0 passthrough | published == truth (byte-identical) | exact | yes |
| Frame dropout (n = 1900 = ceil(40000·p·(1−p))) | 91/1900 = **0.0479** | 0.05 ±0.02 pp | yes |
| Latency, synthetic clock stepping L/50 (200 released) | mean **200.84 ms** | 200 ms ±20% | yes |
| L = 0 immediate release | released=1, delay = 0 | exact 0 | yes |
| Clamp [0,255] (bright 255 / dark 0 frame, sigma>0) | bright mean **252.99** (<255), dark mean **2.01** (>0) | within [0,255] | yes |
| Config bounds (shared contract) | width=99999 → rejected, offending `width`; height=0 → rejected, offending `height`, last valid retained | reject+name+retain | yes |

The dropout n follows the design's `n ≥ ceil(40000·p·(1−p))` rule
(p=0.05 → 1900), never below the Requirement 4.5 minimum of 100 frames.

## Deferred / not verified this session (labelled honestly)

- **[HOST] live `-game` GPU capture (sampled PNG/JPEG pairs that are not black):**
  **Unverified — deferred.** The engine-dependent `ACameraModel` render-target readback
  (`CaptureScene` → `ReadPixels`) compiled and links, but a live windowed `-game` run that
  renders RoverLevel and writes sampled image pairs to disk was NOT executed in this
  session. A standalone `UnrealEditor-Cmd ... -ExecCmds="CameraSelfCheck;Quit" -nullrhi`
  invocation registered the console command (`Cmd: CameraSelfCheck;Quit` in
  `runs/logs/ue_tests/camera_selfcheck.log`) but did not execute it to completion without a
  loaded map and timed out at 300 s; it was terminated. Reason for deferral: the live GPU
  capture + on-disk PNG/JPEG writing path belongs with task 14 (CompressedImage/CameraInfo
  encoding) and the task 15/18 end-to-end run, and a reliable windowed `-game` launch on
  this host has prior shader/DDC contention. No non-black frames are claimed. The headless
  pipeline + message logic — the hard part the task requires — is Verified above via the
  automation suite on synthetic frames.
  Requires: a windowed `-game` RoverLevel run on the RTX 3080 host writing sampled
  Ground_Truth/published image pairs, to verify end to end.

## Defects / contradictions found

None in the shared pipeline or validation contract. One process-level observation (not a
code defect): the `CameraSelfCheck` console command does not execute under
`UnrealEditor-Cmd -nullrhi` without a loaded map/world before `Quit`; the automation test
is the authoritative headless path and covers the identical logic.
