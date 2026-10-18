# Sensor Sim Sandbox

> This repository is **private** — no public license, no packaging, no public releases.

## Description

A drivable Unreal Engine 5.8 rover whose simulated **lidar** (raycast, 16 × 360 rays per
scan) and **camera** (scene capture, 640×480 JPEG) run through a shared, seeded sensor
pipeline (Gaussian noise → dropout → latency queue) and stream to **ROS 2 Jazzy** (plus an **IMU** and **GPS** reusing the same pipeline) in WSL
over a rosbridge WebSocket (no DDS between Windows and WSL). Every capture is logged with
its ground truth to a JSONL Run_Log, and a Python tool (`analysis/analyze.py`) measures
each sensor's bias / std / RMSE, dropout, latency and publish rate against the configured
values.

## Demo

![demo](docs/media/demo.gif)

The rover follows its waypoint route in `RoverLevel` (chase camera; 10 s of the
end-to-end run, captured from the game's own viewport by `scripts/run_e2e.ps1`).

## Architecture

```mermaid
flowchart LR
  subgraph Windows["Windows — Unreal Engine 5.8 (-game)"]
    Rover["ARover<br/>waypoint controller"] --> Mgr["ASensorRunManager"]
    Lidar["ALidarModel<br/>(line traces)"] --> Pipe
    Cam["ACameraModel<br/>(scene capture)"] --> Pipe
    Pipe["Seeded pipeline<br/>noise → dropout → latency"] --> Mgr
    Mgr --> Log["GroundTruthLogger<br/>runs/&lt;id&gt;/run_log.jsonl + frames/"]
    Mgr --> Msg["PointCloud2 / CompressedImage /<br/>CameraInfo / TF (REP 103)"]
    Msg --> RB["Rosbridge_Client<br/>(WebSocket, reconnect ≤ 5 s)"]
  end
  subgraph WSL["WSL Ubuntu 24.04 — ROS 2 Jazzy"]
    RBS["rosbridge_server :9090"] --> Topics["/lidar/points<br/>/camera/image/compressed<br/>/camera/camera_info  /tf"]
    Topics --> Bag["ros2 bag record (sqlite3)"]
    Topics --> Hz["ros2 topic hz"]
  end
  RB -- "ws://127.0.0.1:9090" --> RBS
  Log --> Ana["analysis/analyze.py"]
  Bag --> Ana
  Hz --> Ana
  Ana --> Out["results/summary.md + plots"]
```

## Run instructions

Prerequisites (versions in [`docs/ENVIRONMENT.md`](docs/ENVIRONMENT.md)): UE 5.8 at
`C:\Program Files (x86)\UE_5.8`, WSL distro `Ubuntu` with ROS 2 Jazzy and
`ros-jazzy-rosbridge-suite`, FFmpeg on `PATH`, and the analysis venv:

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python -m pip install -r analysis\requirements.txt
```

**End-to-end run (one command).** Checks the tracked tree is clean, starts rosbridge in
WSL if needed, builds the editor target, records a bag, launches `RoverLevel -game` in
waypoint mode with the sensor run, measures `ros2 topic hz` for 60 s, writes the demo GIF,
stops at ≥ 2,000 camera frames and ≥ 120 s, and runs the analysis:

```powershell
pwsh -NoProfile -File scripts\run_e2e.ps1
```

**Analysis only** (on the latest run, `runs/latest` → the run folder):

```powershell
.\.venv\Scripts\python analysis\analyze.py --run config/run.yaml
```

**Drive manually / view in RViz2.** Launch the game in manual mode (W/S throttle, A/D
steer) and watch the topics from WSL:

```powershell
& "C:\Program Files (x86)\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" `
  "$PWD\unreal\SensorSimSandbox\SensorSimSandbox.uproject" /Game/Maps/RoverLevel -game -windowed -E2ERun -RoverMode=Manual
wsl -d Ubuntu -- bash -lc "source /opt/ros/jazzy/setup.bash && rviz2"   # fixed frame: odom
```

## Measured results

Copied from the generated [`results/summary.md`](results/summary.md) (never hand-edited).
Command `pwsh -NoProfile -File scriptsun_e2e.ps1 -SkipBuild`, seed **1234**, commit
**`f92afce57feadb4e9ae74804bd2850f8119701ca`**, run `e2e_20261017_182940` (210.0 s;
2,000 lidar scans; 2,000 camera frames, 97 dropped; IMU and GPS enabled; average frame
time 26.223 ms).

| Sensor | Bias | Configured bias | Std | Configured σ | RMSE | Measured dropout | Configured dropout | Measured latency (ms) | Configured latency (ms) | Achieved rate (Hz) | Target rate (Hz) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| lidar (m) | -0.000008 | — | 0.019992 | 0.02 | 0.019992 | 0.099984 | 0.10 | 231.020 | 200 | 9.5698 | 10 |
| camera (intensity) | 1.013167 | — | 3.360541 | 5 | 3.509949 | 0.048573 | 0.05 | 230.786 | 200 | 9.1048 | 10 |
| imu_accel (m/s²) | 0.020007 | 0.02 | 0.050033 | 0.05 | 0.053885 | 0.019275 | 0.02 | 48.559 | 20 | 30.6763 | 50 (configured) |
| imu_gyro (rad/s) | 0.000950 | 0.001 | 0.005010 | 0.005 | 0.005099 | 0.019275 | 0.02 | 48.559 | 20 | 30.6763 | 50 (configured) |
| gps (m) | 0.305687 | 0.3 | 0.497535 | 0.5 | 0.583939 | 0.048048 | 0.05 | 126.837 | 100 | 9.1144 | 10 (configured) |

The camera error is published JPEG vs truth PNG on sampled frames, so JPEG compression
smooths part of the injected σ = 5 noise and adds its own error. Achieved rate is
`(n − 1) / T` over the whole run's `t_publish`.

**ROS 2 publish rates (target 10 Hz, not met in this run):**

- `/lidar/points`: **9.330 Hz** — `ros2 topic hz /lidar/points`, 60 s window (raw output in `runs/<id>/hz/`); seed 1234, commit `f92afce…`.
- `/camera/image/compressed` at 640×480: **8.963 Hz** — `ros2 topic hz /camera/image/compressed`, 60 s window; seed 1234, commit `f92afce…`.
- Bag cross-check over the whole run: `/lidar/points` 9.569 Hz, `/camera/image/compressed` 9.104 Hz, `/imu` 30.698 Hz, `/gps/fix` 9.113 Hz.

Causes, reported rather than tuned away (Requirement 0.4):
- **Dropout:** the camera and GPS publish only the samples that survive their dropout (5 %), so at a 10 Hz capture rate their expected topic rate is 9.5 Hz.
- **Game-tick capture:** every sensor captures and releases on the game tick, at most once per tick. Frame-time hitches (scene-capture readback, demo screenshots after t = 100 s, host load) therefore delay captures. This run averaged 26 ms per frame, so the IMU (configured 50 Hz) got about 31 Hz.
- **Host load:** the same code measured higher on a less loaded host. Commit `aa625bf` (13 ms average frame time) got lidar 9.997 Hz / camera 9.640 Hz; commit `724ce26` (18 ms) got lidar 10.044 Hz / camera 9.669 Hz. See [`docs/evidence/gate_a.md`](docs/evidence/gate_a.md) and [`docs/evidence/m5.md`](docs/evidence/m5.md).

Latency runs 15–30 ms (one to two game ticks) above the configured value. The queue is checked once per tick, so the release error lies in `[0, dt)`; at a 26 ms average tick the lidar and camera are 15 % over their 200 ms target, inside ±20 %. The IMU (20 ms target) and GPS (100 ms) are well outside ±20 % because one tick is a large fraction of their latency.

Plots: [`results/lidar_error_hist.png`](results/lidar_error_hist.png),
[`results/camera_error_hist.png`](results/camera_error_hist.png),
[`results/imu_accel_error_hist.png`](results/imu_accel_error_hist.png),
[`results/imu_gyro_error_hist.png`](results/imu_gyro_error_hist.png),
[`results/gps_error_hist.png`](results/gps_error_hist.png),
[`results/configured_vs_measured.png`](results/configured_vs_measured.png).

## Test commands

The exact single command for each test suite (Requirement 9.6).

**Unreal Automation tests (headless).** `scripts/run_ue_tests.ps1` builds the
editor target, then runs every project test (all registered under the
`SensorSimSandbox.` prefix) headless and exits 0 only when every test passed and
at least one ran (33 tests at the time of writing):

```powershell
pwsh -NoProfile -File scripts\run_ue_tests.ps1
```

Under the hood it runs the engine's headless automation command (the report is
written to `runs/logs/ue_tests/report/index.json` and parsed; the script exits 0
only if `failed == 0`, `succeeded >= 1`, and `notRun == 0`):

```text
UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests SensorSimSandbox;Quit" -unattended -nopause -nosplash -nullrhi -testexit="Automation Test Queue Empty" -ReportExportPath=runs/logs/ue_tests/report -log
```

Pass `-EnginePath` to point at a different UE 5.8 install (default
`C:\Program Files (x86)\UE_5.8`).

**Python analysis tests (pytest + Hypothesis).** Runs locally and in GitHub Actions:

```powershell
.\.venv\Scripts\python -m pytest analysis/tests
```

## Known limitations

- Publish rates fall short of 10 Hz (see above); captures run on the game tick, so the
  rate depends on frame time and host load.
- Latency resolution is one game tick, so short configured latencies (IMU 20 ms, GPS
  100 ms) come out well above target, outside ±20 %.
- The IMU truth comes from finite differences of the rover pose per tick. GPS uses a
  spherical local-tangent-plane conversion about `config/gps.yaml`'s origin, with odom
  x = east and y = north.
- The scene has no sky atmosphere (black sky); lighting is a directional light and a sky
  light spawned from `config/level.yaml`.
- rosbridge must be served with `url_path:=//` because UE's libwebsockets requests `GET //`.
- Unreal tests need a local UE 5.8 install and GPU; CI runs only the Python suite.
- The repository is shared with sibling projects; scripts stage and check only tracked files.

## Reproducibility & governance

- Versions of every tool/plugin/API are proven on the machine and recorded in
  [`docs/ENVIRONMENT.md`](docs/ENVIRONMENT.md).
- Third-party licenses are recorded in [`THIRD_PARTY.md`](THIRD_PARTY.md).
- No secrets are committed; secrets are read from environment variables and every
  secret-bearing file is listed in [`.gitignore`](.gitignore).

## How this was built

This project was built with Claude Code (Anthropic's agentic coding tool) as a pair programmer, under a spec-driven workflow: written requirements, then a design, then a task list with milestone gates, each gate reviewed before the next milestone started. The commit history shows this openly; commits carry a `Co-Authored-By` trailer naming the model that helped write them. Every reported number comes from a script in this repository and records the command, seed and commit that produced it, so nothing was typed, estimated or borrowed. I own the design decisions, the verification and the results, and I can explain and change any part of this code.
