# unreal/SensorSimSandbox

UE5 C++ project for Sensor Sim Sandbox. The `.uproject` and `Source/` tree are populated
as milestones M0–M5 proceed.

Layout (see `.kiro/specs/sensor-sim-sandbox/design.md`):

- `SensorSimSandbox.uproject` — project file (added during M0 smoke test, task 2 [ME]).
- `Config/` — engine/project ini + sensor config loader.
- `Source/SensorSimSandbox/`
  - `Rover/` — ARover pawn, WaypointController
  - `Sensors/` — SensorModel base, Lidar_Model, Camera_Model (M5: Imu, Gps)
  - `Bridge/` — Rosbridge_Client (WebSockets), plugin-eval notes
  - `Coords/` — REP 103 converter, TfPublisher
  - `GroundTruth/` — GroundTruthLogger (JSONL writer)
  - `Tests/` — Unreal Automation tests (headless-runnable)
