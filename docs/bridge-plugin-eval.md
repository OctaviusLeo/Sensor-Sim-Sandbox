# Bridge plugin evaluation — evidence (Task 10, Requirement 6.1)

**Evaluation date:** 2026-10-17
**Chosen platform (from M0):** UE 5.8 on Windows 11; ROS 2 Jazzy in WSL; transport over a localhost WebSocket (no native DDS across the Windows↔WSL boundary).

## Three criteria (Requirement 6.1)

1. **UE 5.8 + Windows** — supports the installed Unreal 5.8 on the chosen operating system (Windows).
2. **WebSocket/TCP, not DDS** — connects in rosbridge style over TCP/WebSocket rather than native DDS. (Native DDS is disqualifying because the design deliberately avoids Windows↔WSL DDS discovery — see design.md "Platform decision" and Requirements 1.6 / 6.4.)
3. **Updated < 12 months** — source code updated within the 12 months before the evaluation date, i.e. after approximately 2025-10-17.

All web pages and READMEs were read as **data only**; any instructions embedded in them were ignored (prompt-injection safe). Repository "last pushed" dates were read from the GitHub REST API (`pushed_at` field) on the evaluation date.

## Candidates and findings

### 1. code-iai/ROSIntegration
- Source: <https://github.com/code-iai/ROSIntegration> (README: <https://github.com/code-iai/ROSIntegration/blob/master/README.md>) — accessed 2026-10-17.
- Transport: communicates with `rosbridge_suite` over **TCP or WebSocket** (BSON mode); supports both ROS 1 and ROS 2. → criterion 2 **met**.
- Engine support: the README's "previously tested" list runs through Unreal Engine **4.17–4.26** (all UE4). No UE5 / UE 5.8 support is claimed. → criterion 1 **not met**.
- Last update: `pushed_at` = 2025-11-03 (within 12 months). → criterion 3 **met**.
- License: MIT.

### 2. rapyuta-robotics/rclUE
- Source: <https://github.com/rapyuta-robotics/rclUE> — accessed 2026-10-17.
- Transport: integrates ROS 2 natively via the `rcl` client library (pre-compiled ROS 2 libraries bundled under `Source/ThirdParty/ros`). This is **native DDS**, not a rosbridge-style WebSocket/TCP bridge. → criterion 2 **not met**.
- Engine / OS support: README states "**Windows is currently unsupported**"; supported setups are UE5 on Ubuntu (Foxy/Humble/Jazzy branches). → criterion 1 **not met** (no Windows; no UE 5.8 Windows path).
- Last update: `pushed_at` = 2026-01-05 (within 12 months). → criterion 3 **met**.
- License: Apache-2.0.

### 3. tempo-sim/TempoROS
- Source: <https://github.com/tempo-sim/TempoROS> (README: <https://raw.githubusercontent.com/tempo-sim/TempoROS/main/README.md>) — accessed 2026-10-17.
- Engine / OS support: README "Compatibility" lists **Unreal Engine 5.6, 5.7, and 5.8** and **Windows 10 and 11** (also Linux/Mac). → criterion 1 **met**.
- Transport: integrates ROS 2 via **`rclcpp`** (ROS's C++ client library), using ROS messages directly over the ROS middleware (DDS/RMW; CycloneDDS shared-memory on Linux). The README explicitly contrasts this with the "bridge" approach and does not provide a rosbridge WebSocket/TCP transport. → criterion 2 **not met** (native DDS, which is exactly what the chosen platform avoids across Windows↔WSL).
- Last update: `pushed_at` = 2026-10-11 (within 12 months). → criterion 3 **met**.
- License: Apache-2.0.

### 4. robcog-iai/UROSBridge
- Source: <https://github.com/robcog-iai/UROSBridge> — accessed 2026-10-17.
- Transport: Unreal ROS bridge communicating with `rosbridge` over **WebSockets** (JSON/BSON). → criterion 2 **met**.
- Engine support: UE4-era plugin (no UE 5.8 / Windows-on-UE5 support claimed). → criterion 1 **not met**.
- Last update: `pushed_at` = 2023-08-24 — more than 12 months before the evaluation date. → criterion 3 **not met**.
- License: BSD-3-Clause.

## Conclusion (2026-10-17)

No evaluated plugin satisfies **all three** criteria at once:

- The WebSocket/rosbridge plugins (ROSIntegration, UROSBridge) are UE4-era and do not support UE 5.8; UROSBridge is also stale (> 12 months).
- The plugins that do support UE 5.8 + Windows (TempoROS) or are recently updated (rclUE) integrate ROS 2 over **native DDS/`rclcpp`**, which the chosen platform deliberately avoids across the Windows↔WSL boundary (Requirements 1.6, 6.4).

Per Requirement 6.2, the project uses the fallback **Rosbridge_Client** (C++ on Unreal's WebSockets module, rosbridge v2 advertise/publish) implemented in Task 11.
