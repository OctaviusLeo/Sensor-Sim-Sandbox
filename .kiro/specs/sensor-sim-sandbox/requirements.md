# Requirements Document

## Introduction

Sensor Sim Sandbox is an Unreal Engine 5 (UE5) C++ project that provides a small drivable rover level whose simulated lidar and camera stream to ROS 2 Jazzy over a rosbridge-style WebSocket connection, together with a Python analysis tool that measures each sensor's error against ground truth. The project is portfolio evidence for sensor-simulation and modeling-and-simulation roles, so every reported number must be reproducible from a script in this repository, and every third-party dependency must be proven on the target machine and license-checked.

The work is organized into milestones M0 through M5. M0 selects a runnable platform via a smoke test. M1 delivers the rover, the lidar, the shared noise, dropout, and latency models with tests, and logging to disk. M2 adds the bridge, coordinate frames, and TF. M3 adds the camera on top of the same models. M4 delivers the analysis tool, results table, and README, and is the primary acceptance gate (Gate A). M5 is optional and adds IMU and GPS models.

This document defines the requirements only. Platform choice, bridge-plugin evaluation, and implementation details are deferred to the design phase.

## Glossary

- **System**: The complete Sensor Sim Sandbox project, comprising the UE5 C++ simulation, the bridge, and the Python analysis tool.
- **Simulation**: The UE5 C++ application that renders the rover level and runs the sensor models.
- **Rover**: The drivable simulated ground vehicle in the level.
- **Level**: The small outdoor UE5 scene containing the Rover, static obstacles, and landmarks.
- **Lidar_Model**: The C++ raycast lidar sensor component.
- **Camera_Model**: The C++ camera sensor component.
- **IMU_Model**: The optional C++ inertial measurement unit sensor component (M5).
- **GPS_Model**: The optional C++ global positioning sensor component (M5).
- **Bridge**: The component that transports messages from the Simulation to ROS 2 over a rosbridge-style WebSocket connection.
- **Rosbridge_Client**: The minimal rosbridge v2 protocol client, implemented in C++ with Unreal's WebSockets module, used if no existing plugin qualifies.
- **Ground_Truth**: The noise-free values the Simulation knows exactly: Rover pose and each sensor's ideal readings before noise, dropout, and latency are applied.
- **Published_Value**: The value actually sent to ROS 2 after noise, dropout, and latency models are applied.
- **Run_Log**: The on-disk, machine-readable record written for every run containing Rover pose, each sensor's Ground_Truth values, Published_Values, and timestamps.
- **Analysis_Tool**: The Python tool that reads Run_Logs and ROS 2 bag recordings and reports per-sensor error metrics.
- **ROS_Bag**: A ROS 2 bag recording of the published topics.
- **REP_103**: ROS Enhancement Proposal 103, defining a right-handed coordinate frame in meters with x forward, y left, z up.
- **TF**: The ROS 2 transform tree published via tf2.
- **Smoke_Test**: The M0 procedure that selects and validates a runnable platform.
- **Gate_A**: The M4 acceptance gate: the System runs end to end, publishes to ROS 2 at a stated measured rate, and unit tests pass.
- **Measured_Value**: A number produced by a script in this repository, recorded with its command, seed, and commit hash.
- **Configured_Value**: A parameter value set in a sensor configuration before a run.
- **ENVIRONMENT_Doc**: The file `docs/ENVIRONMENT.md` recording exact tool and dependency versions proven on the machine.
- **THIRD_PARTY_Doc**: The file `THIRD_PARTY.md` recording each third-party dependency and its license.
- **Summary_File**: The generated file `results/summary.md` containing the measured results table.

## Requirements

### Requirement 0: Governing non-functional requirements

**User Story:** As the repository owner, I want every result reproducible and every dependency proven and license-checked, so that I can defend every line of the project in an interview.

#### Acceptance Criteria

1. WHERE a number is reported in the README, in `docs/`, or in the Summary_File, THE System SHALL produce that number from a script in this repository and SHALL record, in the same document and within the same section as the number, the generating command, the seed, and the full commit hash.
2. WHERE a System component uses randomness, THE System SHALL expose a configurable seed through a documented configuration parameter.
3. WHEN two runs of a System component execute with the same seed and the same configuration, THE System SHALL produce byte-for-byte identical output, excluding timestamps and absolute file paths.
4. IF a run fails or misses a stated target, THEN THE System SHALL report, in the run output, the actual result and the identified cause, and SHALL leave the stated target and the test unchanged.
5. THE System SHALL record, in the ENVIRONMENT_Doc, the exact version string of every tool, plugin, API, and dependency it relies on.
6. WHEN a dependency version is recorded in the ENVIRONMENT_Doc, THE System SHALL include, alongside that version, at least one of the following verifiable artifacts obtained on the target machine: a package-manager query result showing that exact version, a documentation reference for that exact version, or a minimal test that passes against that exact version.
7. WHERE a third-party package or asset is added to the repository, THE System SHALL record, in the THIRD_PARTY_Doc, the package or asset name, its version, and its license identifier.
8. THE System SHALL read every secret from an environment variable and SHALL list every secret-bearing file in `.gitignore` so that no such file is tracked in version control.
9. WHERE a module contains more than one public function or exceeds 50 source lines, THE System SHALL provide unit tests for that module, and THE README SHALL state the exact single command that runs those tests.

### Requirement 1: Platform smoke test (M0)

**User Story:** As the repository owner, I want a validated platform selected by a smoke test, so that Unreal and ROS 2 run together on one machine with a documented environment.

#### Acceptance Criteria

1. WHEN the Smoke_Test runs, THE System SHALL attempt the platform options in this order and SHALL keep the first option that passes: (a) Unreal on Windows with ROS 2 Jazzy in WSL2 and a rosbridge WebSocket server inside WSL reached from Unreal at localhost; (b) Unreal and ROS 2 Jazzy (Windows binary archive) both on Windows; (c) Unreal and ROS 2 Jazzy both on Ubuntu 24.04 with NVIDIA driver version 570 or newer.
2. THE Smoke_Test SHALL pass for a platform option only when all of the following hold: the output of `nvidia-smi` is saved in the ENVIRONMENT_Doc; on Windows the Windows edition, version, and OS build that `winver` displays are also saved in the ENVIRONMENT_Doc, captured as command-line text because `winver` only opens a dialog; the Unreal editor opens to its main window without an error dialog; a C++ project compiles with zero compiler errors; and a test message sent from Unreal is received on a ROS 2 topic as observed with `ros2 topic echo` within 10 seconds of being sent.
3. WHEN the Smoke_Test selects a passing platform option, THE System SHALL record the chosen platform option in `design.md`.
4. IF option (c) is attempted on Ubuntu 24.04, THEN THE System SHALL record in the ENVIRONMENT_Doc each problem encountered that arises from Epic documenting Ubuntu 22.04 rather than 24.04, including the step at which it occurred and its observed effect.
5. IF all three platform options (a), (b), and (c) fail the Smoke_Test, THEN THE System SHALL report that no platform option passed and SHALL record in the ENVIRONMENT_Doc, for each option, the Smoke_Test condition that first failed.
6. THE System SHALL NOT depend on DDS discovery between Windows and WSL for any platform option.

### Requirement 2: Rover and level

**User Story:** As a user, I want a drivable rover in a small outdoor level, so that I can generate sensor data along controlled trajectories.

#### Acceptance Criteria

1. WHEN a user provides keyboard input, THE Rover SHALL begin applying the corresponding throttle, brake, or steering motion within 100 milliseconds of the input.
2. THE Level SHALL contain at least three static obstacles and at least three landmarks, where each landmark differs from every other landmark in at least one of shape, color, or geometry so that a viewer can tell them apart.
3. WHILE scripted waypoint mode is active, THE Rover SHALL drive to each waypoint in the configured sequence using the configured sequence order and SHALL ignore keyboard input.
4. WHEN the Rover reaches a position within the configured arrival tolerance (in meters) of the current waypoint, THE Rover SHALL advance to the next waypoint in the sequence.
5. WHEN the Rover reaches the final waypoint within the configured arrival tolerance, THE Rover SHALL stop and SHALL hold its position with zero commanded throttle and steering.
6. IF the Rover does not reach the current waypoint within the configured waypoint timeout, THEN THE Rover SHALL stop and SHALL report that the waypoint was not reached, identifying the unreached waypoint.

### Requirement 3: Lidar model

**User Story:** As a user, I want a configurable raycast lidar that publishes point clouds, so that I can simulate and validate lidar behavior.

#### Acceptance Criteria

1. THE Lidar_Model SHALL expose configurable channels (integer, 1 to 128), horizontal field of view (degrees, 0 exclusive to 360 inclusive), vertical field of view (degrees, 0 exclusive to 180 inclusive), angular resolution (degrees, 0.01 to 10.0), maximum range (meters, 0.1 to 1000.0), publish rate (hertz, 0.1 to 100.0), Gaussian range-noise standard deviation (meters, 0.0 to 10.0), per-ray dropout probability (0.0 to 1.0), and latency (milliseconds, 0 to 1000).
2. IF the Lidar_Model is configured with any parameter outside its stated bound, THEN THE Lidar_Model SHALL reject the configuration, emit an error indication identifying the out-of-bound parameter, and retain the last valid configuration.
3. WHEN the Lidar_Model produces a scan, THE Lidar_Model SHALL publish a `sensor_msgs/PointCloud2` message.
4. WHEN the Lidar_Model publishes a point cloud, THE Lidar_Model SHALL set the message `frame_id` to the lidar sensor frame and set the message timestamp to the scan acquisition time.
5. WHEN range noise is estimated from a logged run of at least 1000 ray returns against Ground_Truth, THE recovered noise standard deviation SHALL match the Configured_Value within plus or minus 10 percent.
6. WHEN per-ray dropout is estimated from a logged run of at least 1000 rays, THE measured dropout fraction SHALL match the configured per-ray dropout probability within plus or minus 2 percentage points.
7. WHEN latency is estimated from a logged run of at least 100 scans, THE measured latency SHALL match the Configured_Value within plus or minus 20 percent.

### Requirement 4: Camera model

**User Story:** As a user, I want a configurable camera that publishes compressed images with calibration, so that I can simulate and validate camera behavior.

#### Acceptance Criteria

1. THE Camera_Model SHALL expose configurable resolution (width and height in pixels, each 1 to 7680), publish rate (hertz, 0.1 to 100.0), pixel noise standard deviation (intensity levels, 0.0 to 255.0), frame dropout probability (0.0 to 1.0), and latency (milliseconds, 0 to 1000).
2. IF the Camera_Model is configured with any parameter outside its stated bound, THEN THE Camera_Model SHALL reject the configuration, emit an error indication identifying the out-of-bound parameter, and retain the last valid configuration.
3. WHEN the Camera_Model produces a frame, THE Camera_Model SHALL publish a `sensor_msgs/CompressedImage` message in JPEG format together with a `sensor_msgs/CameraInfo` message.
4. WHEN the Camera_Model publishes a frame, THE Camera_Model SHALL set the published `sensor_msgs/CameraInfo` width and height equal to the configured resolution and set the published intrinsic parameters equal to the intrinsic parameters used to render the frame.
5. WHEN frame dropout is estimated from a logged run of at least 100 frames, THE measured frame dropout fraction SHALL match the configured frame dropout probability within plus or minus 2 percentage points.
6. WHEN latency is estimated from a logged run of at least 100 frames, THE measured latency SHALL match the Configured_Value within plus or minus 20 percent.

### Requirement 5: Ground truth logging

**User Story:** As a user, I want every run logged with ground truth and published values, so that the analysis tool can measure sensor error.

#### Acceptance Criteria

1. WHEN a run executes, THE System SHALL write a Run_Log to disk containing the Rover pose, each sensor's Ground_Truth values, and the corresponding Published_Values, each tagged with a monotonically non-decreasing timestamp expressed in seconds.
2. THE Run_Log SHALL be written in a machine-readable format that the Analysis_Tool reads without manual conversion.
3. WHERE camera frames are sampled rather than logged in full, THE System SHALL record in the Run_Log the sampling interval applied, expressed as the number of frames logged per total frames produced.
4. WHEN a run executes, THE System SHALL record the seed and the complete sensor configuration used for that run in the Run_Log.
5. IF the System cannot write the Run_Log to disk during a run, THEN THE System SHALL stop the run, retain any Run_Log data already written, and produce an error indication identifying the write failure.

### Requirement 6: Bridge to ROS 2

**User Story:** As a user, I want a WebSocket-based bridge to ROS 2, so that the simulation publishes topics without relying on native DDS.

#### Acceptance Criteria

1. THE System SHALL record in `design.md` an evaluation of each existing Unreal-to-ROS 2 bridge plugin considered, scored against these three criteria: support for the installed Unreal 5.x version and the chosen operating system; connection over TCP or WebSocket in rosbridge style rather than native DDS; and a source-code update within the 12 months preceding the evaluation date.
2. IF no evaluated plugin satisfies all three criteria in Acceptance Criterion 1, THEN THE System SHALL provide a Rosbridge_Client implementing the rosbridge v2 protocol in C++ using Unreal's WebSockets module, supporting advertise and publish operations.
3. THE Bridge SHALL expose a host and a port that are both configurable without recompilation.
4. THE Bridge SHALL NOT rely on DDS discovery between Windows and WSL.
5. IF the Bridge connection is lost, THEN THE Bridge SHALL retry the connection at intervals of no more than 5 seconds until it reconnects, and SHALL keep the Unreal editor running without crashing throughout the loss and reconnection.

### Requirement 7: Coordinate frames and transforms

**User Story:** As a user, I want correct coordinate conversion and TF, so that published data aligns with ROS conventions.

#### Acceptance Criteria

1. WHEN the System converts a value from Unreal coordinates, THE System SHALL transform from the left-handed centimeter frame to the REP_103 right-handed meter frame with x forward, y left, and z up.
2. THE System SHALL provide unit tests that verify position conversion from Unreal coordinates to REP_103 agrees with the expected position to within 1e-6 meters.
3. THE System SHALL provide unit tests that verify rotation conversion from Unreal coordinates to REP_103 agrees with the expected orientation to within 1e-6 radians.
4. WHEN the Simulation runs, THE System SHALL publish TF for the Rover body frame and for each sensor frame at a rate of at least 10 Hz.

### Requirement 8: Analysis tool

**User Story:** As a user, I want a one-command analysis tool, so that I can measure each sensor's accuracy and timing against ground truth.

#### Acceptance Criteria

1. WHEN the Analysis_Tool runs, THE Analysis_Tool SHALL read the on-disk Run_Logs and the ROS 2 bag recordings specified for the run.
2. IF a specified Run_Log or ROS 2 bag recording is missing or cannot be parsed, THEN THE Analysis_Tool SHALL terminate without writing a Summary_File and SHALL emit an error indicating the missing or unparsable input.
3. WHEN the Analysis_Tool processes a sensor, THE Analysis_Tool SHALL report the bias, standard deviation, and RMSE of the Published_Values against the time-aligned Ground_Truth over all matched samples in the run.
4. WHEN the Analysis_Tool processes a sensor, THE Analysis_Tool SHALL report the measured dropout rate (expressed as a fraction from 0.0 to 1.0) and the measured latency (expressed in milliseconds) alongside their Configured_Values.
5. WHEN the Analysis_Tool processes a sensor, THE Analysis_Tool SHALL report the achieved publish rate expressed in hertz, computed over the full duration of the run.
6. WHEN the Analysis_Tool runs, THE Analysis_Tool SHALL save plots to disk and SHALL write the Summary_File that the README uses.
7. THE Analysis_Tool SHALL be invocable from a single command.
8. WHEN the Analysis_Tool runs twice on identical inputs, THE Analysis_Tool SHALL produce numeric results in the Summary_File that are bitwise identical across both runs.

### Requirement 9: Tests and continuous integration

**User Story:** As the repository owner, I want automated tests and CI, so that correctness is verified on every change.

#### Acceptance Criteria

1. THE System SHALL provide Unreal Automation tests covering the noise model, the dropout model, the latency model, the coordinate conversion, and the ROS message building.
2. THE System SHALL provide a script that runs the Unreal Automation tests headless and exits with a zero status code only when all tests pass.
3. THE System SHALL provide pytest tests for the Analysis_Tool.
4. WHEN a change is pushed to the repository, THE continuous-integration pipeline SHALL run the pytest tests.
5. IF any pytest test fails during a continuous-integration run, THEN THE continuous-integration pipeline SHALL report a failed status for that run.
6. THE README SHALL state the exact command for each test suite.

### Requirement 10: README and measured targets

**User Story:** As a reader, I want a README with measured results, so that I can understand and reproduce the project.

#### Acceptance Criteria

1. THE README SHALL contain a description of the project, a demo GIF, an architecture diagram, run instructions, a measured results table, and a known-limitations section.
2. WHEN the lidar publish rate is measured with `ros2 topic hz` over a 60-second window, THE System SHALL report the Measured_Value in hertz against the 10 Hz target.
3. WHEN the camera publish rate is measured with `ros2 topic hz` over a 60-second window at 640x480 resolution, THE System SHALL report the Measured_Value in hertz against the 10 Hz target.
4. WHEN M4 completes, THE System SHALL satisfy Gate_A by running end to end from rover start to Summary_File output without unhandled failure, publishing to ROS 2 at a stated Measured_Value rate in hertz, and passing all unit tests.

### Requirement 11: Optional IMU and GPS models (M5)

**User Story:** As a user, I want optional IMU and GPS models, so that I can extend the sensor suite and its analysis.

#### Acceptance Criteria

1. WHERE M5 is implemented, THE IMU_Model SHALL expose configurable noise and bias as Configured_Values and SHALL publish `sensor_msgs/Imu`.
2. WHERE M5 is implemented, THE GPS_Model SHALL expose configurable noise and bias as Configured_Values and SHALL publish `sensor_msgs/NavSatFix`.
3. WHERE M5 is implemented, THE Analysis_Tool SHALL report, for the IMU_Model and the GPS_Model, the same bias, standard deviation, RMSE, dropout rate, latency, and achieved publish rate metrics it reports for the lidar and camera.

## Out of Scope

The following are explicitly out of scope: drones, photorealism, multiplayer, packaged builds, and Isaac Sim.
