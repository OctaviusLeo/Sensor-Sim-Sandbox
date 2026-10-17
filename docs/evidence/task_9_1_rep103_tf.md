# Task 9.1 — REP 103 converter + TfPublisher evidence

Requirements: 7.1, 7.4

## What was built

- `unreal/SensorSimSandbox/Source/SensorSimSandbox/Coords/Rep103.h`
  - `FRep103` — the PURE REP 103 converter (no UObject/World/Engine), fixing the
    design's mapping exactly:
    - `ConvertPosition(FVector)` → `(x/100, -y/100, z/100)` (Unreal cm,
      left-handed → ROS meters, right-handed).
    - `ConvertRotation(FQuat)` → `q_ros = (w, -x, y, -z)`.
    - `InvertPosition` / `InvertRotation` for the forward-then-back identity
      check (the component mapping is its own inverse).
  - `FRep103Position` (x,y,z meters) and `FRep103Quat` (stored w,x,y,z to match
    the Run_Log `quat` ordering; reordered to x,y,z,w at ROS serialization).
- `Coords/TfPublisher.{h,cpp}`
  - `FTfPublisher` — builds a `tf2_msgs/msg/TFMessage` rosbridge `publish` op on
    `/tf` for the rover body + each sensor frame, converting every frame's Unreal
    transform through `FRep103` first. Publishes at **≥ 10 Hz** via a
    deadline-anchored scheduler (`Tick(now, frames)`); the requested rate is
    clamped UP to the 10 Hz floor so Requirement 7.4 always holds.
  - `ITfTransport` — injectable transport sink. The real Rosbridge_Client
    (task 11) implements it; `FCapturingTfTransport` is a logging/capturing stub
    usable now and in tests, mirroring how `FSmokePublisher` separates rosbridge
    framing from the live WebSocket.
  - Frame tree: `odom → base_link` (rover body); `base_link → {lidar_link,
    camera_link, imu_link, gps_link}` (sensor frames). `lidar_link` matches the
    PointCloud2 `header.frame_id` of task 12. Ops are serialized condensed
    (single-line) for deterministic framing.
- `Coords/TfSelfCheck.{h,cpp}` — the one-command headless demo/verify
  (`FTfSelfCheck::Run()`, console command `TfSelfCheck`): converter correctness,
  round-trip identity, TFMessage build, and the ≥ 10 Hz publish scheduler.
- `Tests/CoordConversionTfTest.cpp`
  - `SensorSimSandbox.Coords.ConversionAndTfPublish` — drives the pure cores
    directly (no world/rendering/WebSocket) and asserts every check PASSes.

## Coordinate conversion (design.md "Coordinate conversion")

```
position:  x_ros =  x_ue / 100
           y_ros = -y_ue / 100
           z_ros =  z_ue / 100

rotation:  q_ros = (w, -x, y, -z)   for a UE quaternion (w, x, y, z)
```

Verified against hand-worked values within 1e-6 (e.g. UE `(100,200,300)` cm →
`(1,-2,3)` m; UE quat `(x,y,z,w)=(0.3,0.4,0.5,0.7)` → `(w,x,y,z)=(0.7,-0.3,0.4,-0.5)`),
plus a forward-then-back identity over several normalized rotations.

## TF message (tf2_msgs/msg/TFMessage, topic `/tf`)

The publish op carries `msg.transforms[]`, each a `geometry_msgs/TransformStamped`
with `header.frame_id` = parent frame, `child_frame_id` = child frame,
`transform.translation` (REP 103 meters) and `transform.rotation` (REP 103
x,y,z,w). The rover body transform under `odom` with UE pose `(250,-100,0)` cm
serializes to REP 103 translation `(2.5, 1.0, 0.0)` m — confirmed by parsing the
op back and checking the numeric fields.

## Build (zero errors)

```
Build.bat SensorSimSandboxEditor Win64 Development -Project="...\SensorSimSandbox.uproject" -WaitMutex -NoHotReloadFromIDE
Result: Succeeded
```

(UBT Makefile.bin cache cleared first so the new `Coords/*.cpp` and test file are
picked up.)

## Headless verification (one command)

```
UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests \
  SensorSimSandbox.Coords.ConversionAndTfPublish;Quit" \
  -unattended -nopause -nosplash -nullrhi -testexit="Automation Test Queue Empty" -log
```

Result: `succeeded=1 failed=0 notRun=0`, `Result={Success}`, `EXIT CODE: 0`
(report `runs/logs/task9_1/report2/index.json`, log `runs/logs/task9_1/test2.log`).

The self-check verifies, headlessly:
- position mapping `(x/100,-y/100,z/100)` within 1e-6 m — PASS;
- rotation mapping `q_ros=(w,-x,y,-z)` + forward-then-back identity within 1e-6 — PASS;
- `tf2_msgs/msg/TFMessage` well-formed on `/tf` (parent/child frames, REP 103
  translation `(2.5,1,0)` m for the rover body) — PASS;
- scheduler publishes ≥ 10 Hz on a synthetic clock, with the largest
  inter-publish gap ≤ one period — PASS (measured 10.0 Hz).

Full project suite after this change: `succeeded=17 failed=0 notRun=0`
(`scripts/run_ue_tests.ps1`, exit 0) — no existing test regressed.

## Deferred: live `ros2 topic hz /tf` ([HOST])

The task's demo/verify asks for a live `ros2 topic hz /tf` ≥ 10 Hz measurement in
WSL with the game running. That path publishes TF over the rosbridge WebSocket,
which is the **Rosbridge_Client (task 11)** — it does not exist yet, so TF cannot
actually reach `rosbridge_server`/`/tf` on the ROS 2 side at this point in the
plan (M2 orders 10 → 11 → 12, and TF ties in with 11). This is recorded honestly
rather than faked:

- **Done now (headless, deterministic):** the REP 103 conversion, the TFMessage
  construction, and the ≥ 10 Hz publish scheduling are all verified above.
- **Deferred to M2 bridge work (task 11/12):** the live
  `ros2 topic hz /tf` ≥ 10 Hz capture in WSL. When `FTfPublisher` is wired to the
  real Rosbridge_Client transport, run with the game up:
  `wsl -d Ubuntu -- bash -lc "source /opt/ros/jazzy/setup.bash && ros2 topic hz /tf"`
  (rosbridge on `localhost:9090`, launched `url_path:=//` per the M0 finding) and
  save the output here as `task_9_1_tf_hz.txt`.

## Notes / provenance

- Command: see the headless verification command above (no seed parameter — the
  self-check is deterministic). Commit: recorded in the task 9.1 commit.
- `FTfPublisher` takes an injectable `ITfTransport`, so task 11's Rosbridge_Client
  drops in with no change to the conversion or message-build logic.
- The property coverage of the 1e-6 tolerance (Property 12 position, Property 13
  rotation, ≥ 100 iterations) is task 9.2, built on the same `FRep103` core.
