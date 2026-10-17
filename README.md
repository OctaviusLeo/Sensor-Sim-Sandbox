# Sensor Sim Sandbox

> **Status:** scaffolding (M0). Placeholders below are filled in as milestones complete.
> This repository is **private** — no public license, no packaging, no public releases.

## Description

_TBD — one-paragraph description of the project: a drivable UE5 rover whose simulated
lidar and camera stream to ROS 2 Jazzy over a rosbridge-style WebSocket bridge, with a
Python tool that measures each sensor's error against ground truth._

## Demo

_TBD — demo GIF (Requirement 10.1)._

![demo](docs/demo.gif)

## Architecture

_TBD — architecture diagram (Requirement 10.1). See `.kiro/specs/sensor-sim-sandbox/design.md`
for the current block diagram._

## Run instructions

_TBD — exact steps to run the simulation, the bridge, RViz2, and recording (Requirement 10.1)._

```bash
# TBD
```

## Measured results

_TBD — this table is sourced from the generated `results/summary.md` (never hand-edited).
Every number is produced by a repository script and recorded with its command, seed, and
full commit hash (Requirement 0.1, 10.1, 10.2, 10.3)._

| Sensor | Bias | Std | RMSE | Measured dropout | Configured dropout | Measured latency (ms) | Configured latency (ms) | Achieved rate (Hz) | Target rate (Hz) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| lidar  | _TBD_ | _TBD_ | _TBD_ | _TBD_ | _TBD_ | _TBD_ | _TBD_ | _TBD_ | 10 |
| camera | _TBD_ | _TBD_ | _TBD_ | _TBD_ | _TBD_ | _TBD_ | _TBD_ | _TBD_ | 10 |

- Lidar rate measured with `ros2 topic hz /lidar/points` over 60 s — _command / seed / commit TBD_.
- Camera rate measured with `ros2 topic hz /camera/image/compressed` over 60 s at 640×480 — _command / seed / commit TBD_.

## Test commands

The exact single command for each test suite (Requirement 9.6).

**Unreal Automation tests (headless).** `scripts/run_ue_tests.ps1` builds the
editor target, then runs every project test (all registered under the
`SensorSimSandbox.` prefix) headless and exits 0 only when every test passed and
at least one ran:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_ue_tests.ps1
```

Under the hood it runs the engine's headless automation command (the report is
written to `runs/logs/ue_tests/report/index.json` and parsed; the script exits 0
only if `failed == 0`, `succeeded >= 1`, and `notRun == 0`):

```text
UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests SensorSimSandbox;Quit" -unattended -nopause -nosplash -nullrhi -testexit="Automation Test Queue Empty" -ReportExportPath=runs/logs/ue_tests/report -log
```

Pass `-EnginePath` to point at a different UE 5.8 install (default
`C:\Program Files (x86)\UE_5.8`).

**Python analysis tests (pytest).**

```powershell
.\.venv\Scripts\python -m pytest analysis
```

## Known limitations

_TBD — known-limitations section (Requirement 10.1)._

## Reproducibility & governance

- Versions of every tool/plugin/API are proven on the machine and recorded in
  [`docs/ENVIRONMENT.md`](docs/ENVIRONMENT.md).
- Third-party licenses are recorded in [`THIRD_PARTY.md`](THIRD_PARTY.md).
- No secrets are committed; secrets are read from environment variables and every
  secret-bearing file is listed in [`.gitignore`](.gitignore).
