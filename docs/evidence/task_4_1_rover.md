# Task 4.1 evidence — Rover pawn, waypoint controller, RoverLevel

Verifies Requirements 2.1, 2.2, 2.3, 2.4, 2.5, 2.6 on the GPU/Unreal host
(engine `C:\Program Files (x86)\UE_5.8`, UE 5.8). Reproduce with:

```
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_rover.ps1
```

## 1. Clean compile

`Build.bat SensorSimSandboxEditor Win64 Development -Project="…\SensorSimSandbox.uproject" -WaitMutex -NoHotReloadFromIDE`

```
[1/4] Compile [x64] Rover.cpp
[2/4] Link [x64] UnrealEditor-SensorSimSandbox.dll
[4/4] WriteMetadata SensorSimSandboxEditor.target [NoUba]
Result: Succeeded
```

## 2. Headless waypoint state-machine self-check

`UnrealEditor-Cmd.exe <uproject> -ExecCmds="RoverSelfCheck <config/level.yaml>;Quit" -unattended -nopause -nosplash -nullrhi`

Drives the PURE `FWaypointStateMachine` (no rendering/engine) exactly as the
later automation tests 4.2/4.3 will:

```
LogRoverSelfCheck: RoverSelfCheck PASS: config=C:/dev/Projects/config/level.yaml;
(route from config); order=PASS advance=PASS hold=PASS;
timed out and reported unreached waypoint 0; levelContent=PASS
```

- `order=PASS` — waypoints visited in configured order (2.3).
- `advance=PASS` — index advances only within the arrival tolerance (2.4).
- `hold=PASS` — final waypoint latches a zero-command hold (2.5).
- `timed out and reported unreached waypoint 0` — a never-reached waypoint
  stops the machine and reports the unreached index (2.6).
- `levelContent=PASS` — config/level.yaml has ≥3 obstacles and ≥3
  distinguishable landmarks (2.2).

## 3. RoverLevel built without the editor UI + short -game launch

The map `/Game/Maps/RoverLevel` package was created headlessly by the editor
Python script `scripts/build_rover_level.py` (no editor UI). The scene itself is
spawned at BeginPlay by the `ARoverLevelBuilder` game mode from
`config/level.yaml` — ground plane, directional + sky light, obstacles, and
landmarks from engine basic shapes only.

`UnrealEditor.exe <uproject> /Game/Maps/RoverLevel -game -windowed -ResX=1280 -ResY=720 -RoverMode=Waypoint -RoverConfig=<config/level.yaml> -RunDir=<runs/…>`

```
LogLoad: Game class is 'RoverLevelBuilder'
LogRoverLevel: RoverLevelBuilder building from …\config\level.yaml: 3 obstacles, 3 landmarks (3 distinguishable), 5 waypoints
LogRover: Rover mode -> Waypoint
LogRover: Rover loaded 5 waypoints from …\config\level.yaml (tol=1.50 m, timeout=30.0 s)
LogRoverLevel: Spawned ARover at origin with 5 waypoints.
LogRover: Rover advanced to waypoint 1
LogRover: Rover advanced to waypoint 2
LogRover: Rover advanced to waypoint 3
LogRover: Rover advanced to waypoint 4
LogRover: Rover reached final waypoint 4; holding.
```

Command-line args (`-RoverMode`, `-RoverConfig`, `-RunDir`) selected waypoint
mode, the run config, and the run folder, so the run needed no keyboard. The
rover drove the configured route in order and held at the final waypoint.
```
```
