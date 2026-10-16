# Environment

This file records the **exact version string** of every tool, plugin, API, and
dependency the project relies on (Requirement 0.5). Each recorded version must be
accompanied by **at least one verifiable artifact obtained on the target machine**
(Requirement 0.6):

- a package-manager query result showing that exact version, **or**
- a documentation reference for that exact version, **or**
- a minimal test that passes against that exact version.

> Placeholders below are filled in as each dependency is proven on the machine.
> Do not record a version without its verifiable artifact.

---

## Platform smoke-test evidence (M0)

Captured during the M0 Smoke_Test (Requirement 1.2). Filled in by Task 2.

- **Chosen platform option:** _TBD (a / b / c)_
- **`nvidia-smi` output:**
  ```
  _TBD — paste captured output_
  ```
- **`winver` output (Windows only):**
  ```
  _TBD — paste captured output_
  ```
- **Editor opens with no error dialog:** _TBD (evidence path)_
- **C++ project compiles with zero errors:** _TBD (compile log path)_
- **`ros2 topic echo` receives test message within 10 s:** _TBD (capture path)_
- **Per-option first-failing conditions (only if all options failed — Requirement 1.5):**
  _TBD_
- **Ubuntu 24.04 vs 22.04 problems (only if option (c) attempted — Requirement 1.4):**
  _TBD — problem / step where it occurred / observed effect_

---

## Tool, plugin, and API versions

| Component | Exact version | Verifiable artifact (query / docs ref / passing test) |
| --- | --- | --- |
| OS | _TBD_ | _TBD_ |
| NVIDIA driver | _TBD_ | _TBD (nvidia-smi above)_ |
| Unreal Engine | _TBD_ | _TBD_ |
| C++ toolchain / compiler | _TBD_ | _TBD_ |
| ROS 2 distribution | _TBD (target: Jazzy Jalisco)_ | _TBD_ |
| rosbridge_suite / rosbridge_server | _TBD_ | _TBD_ |
| Unreal WebSockets module | _TBD_ | _TBD_ |
| Bridge plugin (if used) | _TBD_ | _TBD_ |
| Python | _TBD_ | _TBD_ |
| pytest | _TBD_ | _TBD_ |
| Hypothesis | _TBD_ | _TBD_ |
| NumPy | _TBD_ | _TBD_ |
| matplotlib | _TBD_ | _TBD_ |
| PyYAML | _TBD_ | _TBD_ |
| rosbags / ros2 bag reader | _TBD_ | _TBD_ |

---

## Reproducibility note

Every number reported anywhere in this repository comes from a repository script and
is recorded with its **command**, **seed**, and **full commit hash** in the same
section as the number (Requirement 0.1).
