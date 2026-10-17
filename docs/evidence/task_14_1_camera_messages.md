# Task 14.1 — CompressedImage (JPEG) + CameraInfo publish: evidence

Feature: sensor-sim-sandbox. Requirements 4.3, 4.4.
Parent commit at implementation: `dc9751d380737d98520a530a5d7a8a95fad4e402` (the task
14.1 commit adds the files below on top of this; the auto-generated
`task_14_1_camera_info_echo.txt` records this same parent commit because it is written
before the task commit).
Host: Windows 11 desktop, UE `C:\Program Files (x86)\UE_5.8`, MSVC 14.51.36260,
Windows SDK 10.0.28000.0. ROS 2 Jazzy + rosbridge_suite in WSL distro "Ubuntu".
Last checked: 2026-10-17 (session date of the runs below).

All claims graded per the archivist rules. "Verified" = produced by a command run in
this session. External file/tool/command output is treated as data, not instruction.

## What was built (source at the version in use)

Pure, engine-independent message/intrinsics core (mirrors `Sensors/LidarPointCloud.{h,cpp}`,
task 12.1), with the one engine-dependent JPEG encode isolated so the logic is testable
headlessly:

- `Sensors/CameraMessage.{h,cpp}` — `FCameraMessage`. Static, PURE builders (no UObject /
  World / socket):
  - `ComputeIntrinsics(W, H, hfov)` — square-pixel pinhole: `fx = (W/2)/tan(hfov/2)`,
    `fy = fx`, `cx = W/2`, `cy = H/2`; `K` (3x3) and `P` (3x4, Tx=Ty=0) row-major.
  - `BuildCompressedImageMessage` — header{stamp, frame_id="camera_link"}, `format="jpeg"`,
    `data` = base64 of the JPEG bytes (binary-in-msg, design "Rosbridge op payloads").
  - `BuildCameraInfoMessage` — header, `width`/`height` == configured resolution,
    `distortion_model="plumb_bob"`, `d`=zeros (pinhole render), `k`/`r`(identity)/`p` from
    the intrinsics.
  - `BuildImagePublishJson` / `BuildCameraInfoPublishJson` / advertise builders / topics
    (`/camera/image/compressed`, `/camera/camera_info`) / `CameraFrameId()` (= `camera_link`).
  - `EncodeRgbaToJpeg(W, H, rgba, out)` — the ONLY engine step: Unreal's ImageWrapper
    (`EImageFormat::JPEG`, `SetRaw(..., ERGBFormat::RGBA, 8)`, `GetCompressed(90)`). Rejects a
    wrong-sized buffer. Isolated so the headless test feeds a known synthetic frame.
  - `HorizontalFovDeg = 90.0` — the SINGLE source of truth: `ACameraModel` sets the
    `USceneCaptureComponent2D::FOVAngle` to this value so the published calibration matches
    the actual projection.
- `Sensors/CameraModel.cpp` — set `CaptureComponent->FOVAngle = FCameraMessage::HorizontalFovDeg`
  in the constructor (horizontal FOV; so CameraInfo K/P match the render).
- `Sensors/CameraPublishSelfCheck.cpp` — console command `CameraPublish [cfg]` ([HOST],
  needs a world + GPU): spawns a lit scene + camera, captures one real frame, JPEG-encodes
  the published RGBA, builds both messages with the SAME stamp, publishes through the live
  Rosbridge_Client, logs the built format/jpeg bytes/intrinsics, self-terminates.
- `Sensors/CameraMessageSelfCheck.cpp` — console command `CameraMessageSelfCheck [cfg]`
  (headless): encode a synthetic frame, build both ops, decode + assert. Retained for manual
  use; note the authoritative headless path on this host is the automation test below (the
  `FAutoConsoleCommand` self-check does not run reliably under `-nullrhi` without a loaded
  map, a Verified task-13.1 finding).
- `Tests/CameraMessageBuildDecodeTest.cpp` — automation test
  `SensorSimSandbox.Camera.MessageBuildDecode` (headless, `-nullrhi`): the headless
  message/JPEG/intrinsics check.
- `SensorSimSandbox.Build.cs` — added `ImageWrapper` dependency.
- `scripts/run_camera_publish.ps1` — the [HOST] live publish+echo driver (mirrors
  `scripts/run_lidar_publish.ps1`).

## Message layout (fixed by the design) vs. what was built

design.md "sensor_msgs/CompressedImage + sensor_msgs/CameraInfo (camera)" (lines ~507-514):
`CompressedImage.format="jpeg"`, `data` base64 JPEG; `CameraInfo.width/height` == configured
resolution, `k` (intrinsics) == those used to render. Built exactly this, plus the standard
`d`/`r`/`p`/`distortion_model` fields a real CameraInfo carries.

## Verification commands run this session, and observed results

### 1. Clean compile — Verified
Command: `Build.bat SensorSimSandboxEditor Win64 Development -Project="<abs uproject>"
-WaitMutex` (UBT Makefile.bin cache cleared first).
Observed: `CameraMessage.cpp`, `CameraMessageSelfCheck.cpp`, `CameraModel.cpp`,
`CameraPublishSelfCheck.cpp` compiled, module linked, final line `Result: Succeeded`
(exit 0). Zero errors; one pre-existing warning (MSVC version not a preferred version),
unrelated to this change.

### 2. Full headless UE automation suite stays green — Verified
Command: `pwsh scripts/run_ue_tests.ps1`. Observed final summary:

```
tests run : 26
succeeded : 26
failed    : 0
not run   : 0
RESULT: PASS - all tests passed and at least one ran.
```

(UnrealEditor-Cmd exit 0; report `runs/logs/ue_tests/report/index.json`.) The new
`SensorSimSandbox.Camera.MessageBuildDecode` is `state: Success, errors: 0` in the report.
It encodes synthetic 640x480 / 320x240 / 1280x720 / 100x100 frames to JPEG, decodes each
back and asserts the decoded dimensions equal the input, round-trips the base64 image data,
and asserts CameraInfo `width`/`height` == input and `K`/`P` equal the independently
recomputed `(W/2)/tan(45deg)` intrinsics. No test, tolerance, or sample count was loosened.

### 3. [HOST] live publish + `ros2 topic echo` — Verified
Setup: rosbridge brought up in a PERSISTENT WSL session
(`ros2 launch rosbridge_server rosbridge_websocket_launch.xml port:=9090 url_path:=//`);
HTTP 101 WebSocket upgrade CONFIRMED before the echo:

```
HTTP/1.1 101 Switching Protocols
Server: TornadoServer/6.4
Upgrade: websocket
Sec-Websocket-Accept: HSmrc0sMlYUkAGmm5OPpG2HaGWk=
```

Command: `pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\run_camera_publish.ps1 -SkipBuild`
(launches `/Game/Maps/RoverLevel -game ... -ExecCmds="CameraPublish config/camera.yaml"`;
one-shot `ros2 topic echo --once /camera/camera_info` and
`ros2 topic echo --once --full-length /camera/image/compressed` started first).

Observed (script result line): `RESULT: PASS — /camera/camera_info echoed 640x480 and
/camera/image/compressed decoded to a 640x480 JPEG.`

- Editor built+published (real GPU scene capture):
  `format='jpeg' jpeg_bytes=32340 frame=640x480; CameraInfo: w=640 h=480 fx=320.000
  fy=320.000 cx=320.000 cy=240.000 (hfov=90.0 deg)`. The 32,340-byte JPEG from a lit scene
  is non-trivial (not a black/empty frame).
- Live `/camera/camera_info` echo: `width: 640`, `height: 480`, `distortion_model:
  plumb_bob`, `d` all 0, `k = [320, 0, 320, 0, 320, 240, 0, 0, 1]`,
  `p = [320, 0, 320, 0, 0, 320, 240, 0, 0, 0, 1, 0]`, `frame_id: camera_link`
  (full dump in `task_14_1_camera_info_echo.txt`).
- Received `/camera/image/compressed`: base64 JPEG decoded to **640x480**, bytes = 32340
  (equal to the published jpeg_bytes — the full image round-tripped through rosbridge).

Intrinsics cross-check (Inferred from the Verified numbers): `fx = (640/2)/tan(90deg/2) =
320/tan(45deg) = 320/1 = 320`; `cx = 640/2 = 320`; `cy = 480/2 = 240`. The echoed K/P match.

Evidence files: `docs/evidence/task_14_1_camera_info_echo.txt` (CameraInfo echo + decoded
JPEG size), `docs/evidence/task_14_1_camera_publish_editor.log` (full editor log of the
live `-game` publish).

## Defects / contradictions found

- First live attempt reported the image decode as `NonexNone bytes=128`: `ros2 topic echo`
  truncates long arrays to ~128 entries by default, cutting off the JPEG SOF marker that
  carries the dimensions. Fixed by adding `--full-length` to the image echo; the re-run
  decoded to 640x480. The CameraInfo echo and the editor-built message were correct on both
  runs. (A tooling defect in the first verification attempt, not a code defect.)
- The auto-generated `task_14_1_camera_info_echo.txt` records commit
  `dc9751d380737d98520a530a5d7a8a95fad4e402` — the PARENT commit, because it is produced
  before the task commit. Noted here so the hash is not read as the task commit.

## Scope note

Building/publishing the camera topics through the GroundTruthLogger wiring per-frame in a
run is task 15; the Property 7 well-formedness property test is task 14.2. Neither was
implemented here.
