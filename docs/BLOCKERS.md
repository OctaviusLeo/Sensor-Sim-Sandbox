# Blockers

Blockers recorded per the execution-environment rules: when a gate or verification
cannot pass after the allowed fix attempts for the same cause, record it here (task,
command, exit code, log path, suspected cause) and stop that task. Targets, tolerances,
and tests are never loosened to make something pass.

---

## Task 2.2 — [HOST] Execute `scripts/smoke_test.ps1` and commit the evidence

- **Date (UTC):** 2026-10-17
- **Commit:** `101d32bf25db9bf86fd06b5a4aa35c20855d982c`
- **Command:** `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke_test.ps1`
  (run in the background via `scripts/smoke_test.ps1`; log at `runs/logs/smoke/run.log`,
  exit-code marker at `runs/logs/smoke/exitcode.txt`).
- **Script exit code:** 0 (but this is a *false pass* — see suspected cause below).

### What the smoke test actually verified on this host

| Condition (Requirement 1.2) | Result | Evidence |
| --- | --- | --- |
| `nvidia-smi` captured | PASS | `docs/ENVIRONMENT.md` (live capture), RTX 3080, driver 617.42 |
| Windows version captured | PASS | `docs/ENVIRONMENT.md` (Windows 11 Pro, 10.0.26200, 25H2, UBR 9711) |
| rosbridge answering on `localhost:9090` | PASS | WSL rosbridge forwarded to Windows localhost; `Test-NetConnection localhost -Port 9090` succeeded |
| ROS 2 Jazzy present in WSL Ubuntu-24.04 | PASS | `ros2 doctor --report` → distribution `jazzy`, active; Ubuntu 24.04.5 LTS; Python 3.12.3 |
| **C++ project compiles with zero errors** | **FAIL** | `docs/evidence/compile.log`: `Build.bat not found at C:\Program Files\Epic Games\UE_5.8\...` |
| **Editor opens with no error dialog** | **FAIL (not attempted)** | no `docs/evidence/editor_open.png`, no `editor_startup.log` |
| **Unreal publishes `/smoke_test`; `ros2 topic echo` receives it ≤10 s** | **FAIL (not attempted)** | no `docs/evidence/ros2_echo_smoke.txt` |

### Suspected cause (root cause)

**Unreal Engine 5.8 is not installed on this host.** The environment steering file
(`environment.md`) states UE 5.8 lives at `C:\Program Files\Epic Games\UE_5.8`, but that
directory contains only `config.ini` — no engine. Neither `UnrealEditor.exe` nor
`Build.bat` exists there, and no `UnrealEditor.exe` was found anywhere on `C:` or `D:`
(the Epic Games Launcher is installed, but the engine itself is not). Verified:

```
Test-Path 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'  -> False
Test-Path 'C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat'       -> False
Get-ChildItem 'C:\Program Files\Epic Games'                                            -> config.ini only
```

Because the engine is absent, the three Unreal-dependent Smoke_Test conditions that
Requirement 1.2 demands (C++ compiles, editor opens, Unreal→rosbridge→`ros2 topic echo`
round-trip) **cannot be satisfied on this host as it currently stands**. Option (a)
correctly fails at `cpp_compiles_zero_errors`.

### Second defect uncovered: option (b) reports a false pass

`scripts/smoke_test.ps1` (task 2.1) declares **option (b) PASS** after checking only two
conditions — `no_windows_wsl_dds` and `rosbridge_listening_windows` — and never checks the
Unreal compile / editor-opens / `ros2 topic echo` conditions that Requirement 1.2 requires
of a passing platform option. On this host, `rosbridge_listening_windows` is `true` only
because the WSL rosbridge is reachable on Windows `localhost:9090`, so the script exits 0
and writes `chosen_option = "b"` even though Unreal is not installed at all. This is a
specification/implementation gap in task 2.1's option-(b) placeholder, not a genuine pass.
The exit-0 result must **not** be treated as task 2.2 succeeding.

### Resolution needed (owner action — not agent-fixable without new deps)

One of the following is required before task 2.2 can produce honest evidence:

1. Install Unreal Engine 5.8 at `C:\Program Files\Epic Games\UE_5.8` (or correct
   `environment.md` and `scripts/smoke_test.ps1`'s `-EnginePath` to the real install path),
   then re-run the smoke test; **or**
2. Fix task 2.1's option-(b) logic to require the full Requirement-1.2 condition set
   (Unreal compile, editor opens, `/smoke_test` round-trip) before it can report a pass,
   so the smoke test fails honestly on a host without the engine.

Installing a multi-gigabyte engine or editing a sibling-project steering file is outside
the scope of an agent fix attempt (no `sudo`; do not modify files outside the repo except
`.venv`; do not loosen tests), so this task is **stopped** pending owner input. No evidence
was committed as "passing", and the false-positive `results/smoke_result.json` is left in
place as the raw record of what the current script produced.

### RESOLVED — 2026-10-17

The engine turned out to be installed at `C:\Program Files (x86)\UE_5.8` (note the
"Program Files (x86)" location, not the Epic default). Re-running the smoke test with
`-EnginePath "C:\Program Files (x86)\UE_5.8"` and fixing a chain of real defects produced
a **genuine pass on option (a)** — all five Requirement 1.2 conditions true. The blocker is
closed. What was fixed (none of it loosened a target or test):

1. **Option (b) false pass (the second defect above):** `New-OptionBPlaceholder` reported
   PASS after checking only `no_windows_wsl_dds` + `rosbridge_listening_windows`. It is now
   `Invoke-OptionB`, which exercises the full Requirement-1.2 set (compile, editor opens,
   `/smoke_test` round trip) exactly like option (a) and can never pass without them.
2. **Build-environment conflict:** both target files used `BuildSettingsVersion.V5`, whose
   warning-level defaults conflicted with the shared `UnrealEditor` build environment on
   UE 5.8 (`OtherCompilationError`). Set to `V7` (the 5.8 defaults).
3. **Missing include path:** `SmokePublisher.cpp` includes `"Bridge/SmokePublisher.h"`, but
   the flat module layout left the module root off the include path under 5.8
   (`bLegacyPublicIncludePaths = false`). Added `PublicIncludePaths.Add(ModuleDirectory)`.
4. **rosbridge WebSocket 404:** UE's bundled libwebsockets sends `GET //` (double slash) for
   a root `ws://host:port` URL; rosbridge's default `/` route 404s it. Served rosbridge at
   `url_path=//` so Tornado's route matches, and hardened `Ensure-Rosbridge` to require a
   real HTTP-101 upgrade (not just an open TCP port) at that path.
5. **Publisher closed too early:** the SmokePublisher closed the socket the instant it sent,
   so rosbridge tore the publisher down before delivery. Added a short grace period before
   disconnect.
6. **Echo ordering + WSL session teardown:** the echo subscriber was started after the
   one-shot publish (missing it), and a `& disown` subscriber inside a short `wsl bash -lc`
   was reaped when that call returned. The subscriber is now started BEFORE the publish and
   launched as a detached Windows-side `wsl.exe` process whose lifetime the script controls.

Evidence (option a, all conditions pass): `docs/evidence/compile.log` (`Result: Succeeded`),
`docs/evidence/editor_open.png`, `docs/evidence/editor_startup.log` (0 `Error:` lines),
`docs/evidence/ros2_echo_smoke.txt` (`data: sensor-sim-sandbox`), and
`results/smoke_result.json` (`chosen_option: a`, every condition `true`). Reproducible
command recorded in `results/smoke_result.json` and the M0 block of `docs/ENVIRONMENT.md`.
