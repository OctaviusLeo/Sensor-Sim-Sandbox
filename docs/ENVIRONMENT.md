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

Captured during the M0 Smoke_Test (Requirement 1.2).
Generating command: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke_test.ps1 -EnginePath "C:\Program Files (x86)\UE_5.8" -WsUrl "ws://127.0.0.1:9090" -WslDistro "Ubuntu"`
Commit: `101d32bf25db9bf86fd06b5a4aa35c20855d982c`

- **Chosen platform option:** a
- **`nvidia-smi` output:**
  ```
Fri Oct 16 21:51:18 2026       
+-----------------------------------------------------------------------------------------+
| NVIDIA-SMI 617.42                 KMD Version: 617.42        CUDA UMD Version: 13.4     |
+-----------------------------------------+------------------------+----------------------+
| GPU  Name                  Driver-Model | Bus-Id          Disp.A | Volatile Uncorr. ECC |
| Fan  Temp   Perf          Pwr:Usage/Cap |           Memory-Usage | GPU-Util  Compute M. |
|                                         |                        |               MIG M. |
|=========================================+========================+======================|
|   0  NVIDIA GeForce RTX 3080      WDDM  |   00000000:2B:00.0  On |                  N/A |
| 61%   83C    P0            352W /  368W |    6746MiB /  10240MiB |    100%      Default |
|                                         |                        |                  N/A |
+-----------------------------------------+------------------------+----------------------+

+-----------------------------------------------------------------------------------------+
| Processes:                                                                              |
|  GPU   GI   CI              PID   Type   Process name                        GPU Memory |
|        ID   ID                                                               Usage      |
|=========================================================================================|
|    0   N/A  N/A            6004    C+G   ...Browser\Application\brave.exe      N/A      |
|    0   N/A  N/A            8180    C+G   ....0.4283.45\msedgewebview2.exe      N/A      |
|    0   N/A  N/A            8996    C+G   ...s\Win64\EpicGamesLauncher.exe      N/A      |
|    0   N/A  N/A           10272    C+G   ...2txyewy\CrossDeviceResume.exe      N/A      |
|    0   N/A  N/A           10836    C+G   ...yb3d8bbwe\WindowsTerminal.exe      N/A      |
|    0   N/A  N/A           11076    C+G   C:\Windows\explorer.exe               N/A      |
|    0   N/A  N/A           11180    C+G   ...indows\System32\ShellHost.exe      N/A      |
|    0   N/A  N/A           11820    C+G   ...5n1h2txyewy\TextInputHost.exe      N/A      |
|    0   N/A  N/A           12104    C+G   ..._cw5n1h2txyewy\SearchHost.exe      N/A      |
|    0   N/A  N/A           12128    C+G   ...y\StartMenuExperienceHost.exe      N/A      |
|    0   N/A  N/A           16504    C+G   ...aries\Win64\EpicWebHelper.exe      N/A      |
|    0   N/A  N/A           17296    C+G   ...Browser\Application\brave.exe      N/A      |
|    0   N/A  N/A           18764    C+G   ...ayRenderer-Win64-Shipping.exe      N/A      |
|    0   N/A  N/A           22792    C+G   ...\cef.win64\steamwebhelper.exe      N/A      |
|    0   N/A  N/A           67220    C+G   ...\Local\Programs\Kiro\Kiro.exe      N/A      |
|    0   N/A  N/A           67480    C+G   ...xyewy\ShellExperienceHost.exe      N/A      |
+-----------------------------------------------------------------------------------------+
  ```
- **`winver` output (Windows only; captured as command-line text):**
  ```
cmd /c ver:
Microsoft Windows [Version 10.0.26200.9711]

Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber:
Caption     : Microsoft Windows 11 Pro
Version     : 10.0.26200
BuildNumber : 26200

HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion:
  DisplayVersion = 25H2
  UBR            = 9711

(winver itself only opens a dialog and prints nothing; the text above is the
equivalent command-line capture.)
  ```
- **Editor opens with no error dialog:** see `docs/evidence/editor_startup.log` (the screenshot `editor_open.png` caught the engine splash screen rather than the editor window, so it was removed; `results/smoke_result.json` still names it)
- **C++ project compiles with zero errors:** see `docs/evidence/compile.log`
- **`ros2 topic echo` receives test message within 10 s:** see `docs/evidence/ros2_echo_smoke.txt`
- **Per-option first-failing conditions (Requirement 1.5):**
  (none â€” an option passed)
- **Ubuntu 24.04 vs 22.04 problems (only if option (c) attempted â€” Requirement 1.4):**
  Option (c) is not attemptable on this host (no native Ubuntu 24.04 install).

---


## Tool, plugin, and API versions

> Only versions with a verifiable artifact obtained on this host are filled in below.
> The Unreal / MSVC / Visual Studio / Windows-SDK rows are now filled in: UE 5.8 is
> installed at `C:\Program Files (x86)\UE_5.8` (note: "Program Files (x86)", not the
> Epic default `C:\Program Files\Epic Games\UE_5.8`), and task 2.2's smoke test built
> and launched the project on it. ROS runs in the WSL distro named `Ubuntu` (also
> Ubuntu 24.04.5 LTS with ROS 2 Jazzy), which is the distro that hosts the rosbridge
> on `localhost:9090`. The Python-analysis rows come from the `.venv` created in M4
> (task 16, `py -3.12 -m venv .venv`; pins in `analysis/requirements.txt`); do not
> record a version without its verifiable artifact.

| Component | Exact version | Verifiable artifact (query / docs ref / passing test) |
| --- | --- | --- |
| OS | Windows 11 Pro, 10.0.26200 (25H2, UBR 9711) | `cmd /c ver` + `Get-CimInstance Win32_OperatingSystem` + registry `DisplayVersion`/`UBR`, captured in the M0 block above |
| NVIDIA driver | 617.42 (RTX 3080, CUDA UMD 13.4) | live `nvidia-smi` capture in the M0 block above |
| Unreal Engine | 5.8.3 (CL 58210709, `++UE5+Release-5.8`) | `C:\Program Files (x86)\UE_5.8\Engine\Build\Build.version` (MajorVersion 5, MinorVersion 8, PatchVersion 3); the smoke test built `SensorSimSandboxEditor` against it (`docs/evidence/compile.log` → `Result: Succeeded`) |
| Visual Studio | Community 2026, product version 18.11.0 | `vswhere -latest -property catalog_productDisplayVersion` → `18.11.0`; `-property displayName` → `Visual Studio Community 2026` |
| MSVC toolchain / compiler | toolset 14.51.36231 (cl 14.51.36260) | smoke-test build log (`runs/logs/smoke/build_check.log`): `Using Visual Studio 14.51.36260 toolchain (…\VC\Tools\MSVC\14.51.36231)` |
| Windows 10 SDK | 10.0.28000.0 | smoke-test build log: `… and Windows 10.0.28000.0 SDK (C:\Program Files (x86)\Windows Kits\10).` |
| Unreal WebSockets module (libwebsockets) | bundled with UE 5.8.3 | `docs/evidence/editor_startup.log`: `LogModuleManager: InternalLoadLibrary: 'WebSockets' ('…\UnrealEditor-WebSockets.dll')`; the SmokePublisher used it to advertise+publish `/smoke_test` (log → `SmokePublish succeeded`) |
| Bridge plugin (if used) | none (fallback Rosbridge_Client path) | smoke test uses the in-project SmokePublisher over the Unreal WebSockets module; no third-party bridge plugin installed |
| WSL | 3.0.3.0 (kernel 6.18.54.1-1, WSLg 1.0.79) | `wsl --version` → `WSL version: 3.0.3.0`, `Kernel version: 6.18.54.1-1`, `WSLg version: 1.0.79` |
| Ubuntu (WSL distro `Ubuntu`) | 24.04.5 LTS | `wsl -d Ubuntu -- lsb_release -ds` → `Ubuntu 24.04.5 LTS` |
| ROS 2 distribution | Jazzy Jalisco (ros2cli 0.32.13) | `wsl -d Ubuntu -- ros2 doctor --report` → `distribution name: jazzy`, `distribution type: ros2`; `python3 -c "import importlib.metadata as m; print(m.version('ros2cli'))"` → `0.32.13` |
| rosbridge_suite | 2.7.1-1noble.20261006.035549 | `wsl -d Ubuntu -- dpkg-query -W -f='${Version}' ros-jazzy-rosbridge-suite` → `2.7.1-1noble.20261006.035549` |
| rosbridge_server | 2.7.1-1noble.20261006.035306 | `wsl -d Ubuntu -- dpkg-query -W -f='${Version}' ros-jazzy-rosbridge-server` → `2.7.1-1noble.20261006.035306`; served the `/smoke_test` round trip at `url_path=//` (see M0 block; `docs/evidence/ros2_echo_smoke.txt`) |
| Python (WSL / ROS) | 3.12.3 | `wsl -d Ubuntu -- python3 --version` → `Python 3.12.3` |
| GitHub CLI | 2.102.0 (2026-09-30) | `gh --version` → `gh version 2.102.0 (2026-09-30)` |
| FFmpeg | N-118205-gd3aa99a4f4-20250101 | `ffmpeg -version` → `ffmpeg version N-118205-gd3aa99a4f4-20250101` |
| pytest | 9.1.1 | `.\.venv\Scripts\python -m pip freeze` → `pytest==9.1.1` |
| Hypothesis | 6.170.1 | `.\.venv\Scripts\python -m pip freeze` → `hypothesis==6.170.1` |
| NumPy | 2.5.4 | `.\.venv\Scripts\python -m pip freeze` → `numpy==2.5.4` |
| matplotlib | 3.11.2 | `.\.venv\Scripts\python -m pip freeze` → `matplotlib==3.11.2` |
| PyYAML | 6.0.3 | `.\.venv\Scripts\python -m pip freeze` → `PyYAML==6.0.3` |
| rosbags / ros2 bag reader | 0.11.8 | `.\.venv\Scripts\python -m pip freeze` → `rosbags==0.11.8` |
| Pillow | 13.0.0 | `.\.venv\Scripts\python -m pip freeze` → `pillow==13.0.0` |
| Python (Windows .venv) | 3.12.10 | `py -3.12 --version` → `Python 3.12.10` |

---

## Reproducibility note

Every number reported anywhere in this repository comes from a repository script and
is recorded with its **command**, **seed**, and **full commit hash** in the same
section as the number (Requirement 0.1).









