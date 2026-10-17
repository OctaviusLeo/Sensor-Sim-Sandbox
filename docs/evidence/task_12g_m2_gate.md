# Task 12g — Automated gate (M2) evidence

- Generated: 2026-10-17 (UTC-04:00; run timestamps below are host-local -04:00)
- Commit: `556f171efa7c17bf68eebc801866663e1335d663`
- Host: Windows 11 desktop, RTX 3080; UE 5.8 at `C:\Program Files (x86)\UE_5.8`
- WSL distro: `Ubuntu` (ROS 2 Jazzy + rosbridge_suite)
- Gate type: automated, no manual review stop.

## Verdict

**GATE PASS.** Both legs exited 0:

| Leg | Command | Exit | Result |
| --- | --- | --- | --- |
| (a) headless UE automation suite | `pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\run_ue_tests.ps1` | 0 | 23/23 tests passed, 0 failed, 0 not-run |
| (b) advertise+publish round-trip (stub + real rosbridge + reconnect) | `pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\run_bridge_roundtrip.ps1` | 0 | A (stub) PASS, B (real rosbridge) PASS, C (reconnect ≤5s) PASS |

Evidence grade: **Verified** (both gate commands executed this session; exit codes and
report counts observed directly). The only load-bearing claim below the top grade is the
run-1 reconnect failure diagnosis, graded **Supported** (one editor log).

## rosbridge-up confirmation (done FIRST, per the 12.3 finding)

Task 12.3 had found rosbridge not answering at run time because a one-shot
`nohup ... & disown` inside a short `wsl bash -lc` was reaped. This gate started
rosbridge in a **persistent, tracked WSL session** that stayed alive for the whole gate.

Commands and observed output (this session, 2026-10-17):

- ROS 2 Jazzy present: `ros2 pkg executables rosbridge_server` →
  `rosbridge_server rosbridge_websocket` ; `ROS_DISTRO=jazzy` (after sourcing
  `/opt/ros/jazzy/setup.bash`).
- Launched (persistent background session, not disowned):
  `wsl.exe -d Ubuntu -- bash -lc 'source /opt/ros/jazzy/setup.bash && exec ros2 run rosbridge_server rosbridge_websocket --ros-args -p url_path:=//'`
  Process log: `Rosbridge WebSocket server started on port 9090`.
- Listener confirmed in WSL: `ss -ltnp` →
  `LISTEN 0 128 0.0.0.0:9090 ... users:(("python3",pid=32717,...))`.
- WebSocket upgrade confirmed from Windows at path `//` (the `Test-WsUpgrade` the
  scripts use): GET `//` HTTP/1.1 with Upgrade: websocket →
  `HTTP/1.1 101 Switching Protocols`.

`url_path:=//` is used per the M0 libwebsockets GET-`//` finding; the UE client requests
path `/` which libwebsockets matches to the `//` url_path.

## Leg (a) — scripts/run_ue_tests.ps1

Command: `pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\run_ue_tests.ps1`

Observed console output:

```
[run_ue_tests] Build: Ok=True Build.bat Result: Succeeded (exit 0)
[run_ue_tests] Automation run: UnrealEditor-Cmd exit 0
[run_ue_tests] Report: total=23 succeeded=23 failed=0 notRun=0
==== SensorSimSandbox Unreal Automation summary ====
  tests run : 23
  succeeded : 23
  failed    : 0
  not run   : 0
RESULT: PASS - all tests passed and at least one ran.
```

Recorded exit code (`runs\logs\ue_tests\gate12g_exit.txt`): `GATE_UE_EXIT=0`.
Report: `runs\logs\ue_tests\report\index.json` (exists). Pass condition
(failed==0 AND succeeded>=1 AND notRun==0) satisfied.

## Leg (b) — scripts/run_bridge_roundtrip.ps1

Command: `pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\run_bridge_roundtrip.ps1`

Final passing run (run 2) console output:

```
A (stub): Ok=True advertise=True publish=True
C (reconnect): Ok=True scheduleLines=2 maxInterval=1s reconnects=2 opsAfterRestart=6 editorAlive=True
B (real): Ok=True Deferred=False
RESULT: PASS
GATE_BRIDGE_EXIT=0
```

Recorded exit code (`runs\logs\bridge\gate12g_exit_run2.txt`): `GATE_BRIDGE_EXIT=0`.

### Scenario A — stub round-trip (stub received both ops)

```
{"op": "advertise", "topic": "/bridge_test", "type":"std_msgs/msg/String"}
{"op": "publish",   "topic": "/bridge_test", "msg":{"data":"stub_round-trip_2026-10-17T11:13:49..."}}
```

### Scenario B — REAL rosbridge round-trip in WSL

`ros2 topic echo --once /bridge_test std_msgs/msg/String` received the editor's publish:

```
data: real_round-trip_2026-10-17T11:14:48.4190350-04:00
---
```

This proves the live `rosbridge_server` (the persistent WSL session confirmed above)
received the advertise+publish from the UE Rosbridge_Client.

### Scenario C — reconnect within 5 s (timing)

From `runs\logs\bridge\editor_C.log` (run 2):

```
11:14:22.963  Connected to ws://127.0.0.1:9091/            (first connect, pre-bounce)
11:14:23.902  WebSocket closed (1006)                      (harness killed stub1; downAt=23.892)
11:14:23.903  Scheduling reconnect attempt 1 in 0.50s (<= 5s)
11:14:26.452  Scheduling reconnect attempt 2 in 1.00s (<= 5s)
11:14:28.485  Connected to ws://127.0.0.1:9091/            (reconnect; stub2 upAt=27.925 -> gap ~0.56s)
11:14:30..38  publish #3..#7 (connected=1)                 (opsAfterRestart=6)
```

Every scheduled reconnect interval ≤ 5 s (max 1.00 s); the client reconnected after the
server bounce and the editor stayed alive the whole time (Requirement 6.5).

## Note: run-1 reconnect flake (diagnosed, not a client defect) — Supported

The first run of leg (b) returned `C: Ok=False reconnects=1 opsAfterRestart=7 editorAlive=True`.
Root cause (from `editor_C.log`, run 1): the editor reached its first connect at
`11:11:23.049`, but the harness's fixed `Start-Sleep -Seconds 12` kill of stub1 landed at
`downAt=11:11:23.16` — ~0.1 s after the editor's first connect attempt — so the first
connection closed with code 1006 **before** ever logging a clean `Connected` line. The
script counts `Connected to ws://` lines and requires ≥ 2 (initial + post-bounce); run 1
produced only the post-restart `Connected`, so the count was 1 even though the client
genuinely reconnected (`opsAfterRestart=7`, `editorAlive=True`). This is a nondeterministic
editor-boot-vs-fixed-kill-timer race in the **test harness**, not a defect in the
Rosbridge_Client; on run 2 the editor booted and connected cleanly before the bounce and
the count was 2. No script, test, or target was loosened. The scripts and config were not
modified for this gate.

## M0/M1 gate preconditions (cheap re-verification, all PASS)

- Repo layout: `unreal/`, `analysis/`, `config/`, `results/`, `docs/`, `scripts/`,
  `.github/workflows/` all exist (Verified via Test-Path).
- `.gitignore` covers secrets (`.env`, `*.key`, `*.pem`, `*.p12`, `*.pfx`, `credentials.*`,
  `*.token`, `.netrc`, …) plus Unreal outputs (`Binaries/`, `Intermediate/`, `Saved/`,
  `DerivedDataCache/`, `*.sln`, `.vs/`) and Python outputs (`__pycache__/`, `.venv/`,
  `runs/`) (Verified).
- `git remote -v`: `origin https://github.com/OctaviusLeo/Sensor-Sim-Sandbox.git` (fetch+push) (Verified).
- `results/smoke_result.json`: `chosen_option: "a"`, `all_failed: false`, option a
  `passed: true` with all 5 conditions true — a genuine passing option (Verified).
