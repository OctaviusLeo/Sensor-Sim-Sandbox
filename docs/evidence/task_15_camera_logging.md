# Task 15 / 15g evidence — camera logging + M3 gate

Command: `pwsh -File scripts/run_ue_tests.ps1` (seed 1234); parent commit `fe400f6805b7c1460bc1610a7d473eae5d247b9a`, date 2026-10-17.

Result: total=28 succeeded=28 failed=0 notRun=0

Includes `SensorSimSandbox.Logging.CameraLogging` (Run_Log written by CameraLoggerSelfCheck) and `SensorSimSandbox.Camera.MessageBuildDecode` (JPEG decodes to CameraInfo width/height) — the M3 gate's headless build+decode check.

Run_Log excerpt (runs/camlog_test_20261017_214756_303//run_log.jsonl, 41 camera records, 2 dropped, 4 with image refs):

```
{"type":"run_meta","seed":1234,"commit":"fe400f6805b7c1460bc1610a7d473eae5d247b9a","sensor_config":{"lidar":{"channels":16,"h_fov_deg":360,"v_fov_deg":30,"angular_resolution_deg":1,"max_range_m":100,"rate_hz":10,"range_noise_sigma_m":0.02,"dropout_prob":0.10000000000000001,"latency_ms":200},"camera":{"width":640,"height":480,"rate_hz":10,"pixel_noise_sigma":5,"frame_dropout_prob":0.050000000000000003,"latency_ms":200}},"camera_sampling_interval":"1/10"}
{"type":"sample","sensor":"camera","seq":0,"t":0.20000000000000001,"t_capture":0,"t_publish":0.20000000000000001,"dropped":false,"rover_pose":{"pos":[0,0,0],"quat":[1,0,0,0]},"truth_ref":"frames/cam_000000.png","published_ref":"frames/cam_000000_pub.jpg"}
{"type":"sample","sensor":"camera","seq":1,"t":0.30000000000000004,"t_capture":0.10000000000000001,"t_publish":0.30000000000000004,"dropped":false,"rover_pose":{"pos":[0,0,0],"quat":[1,0,0,0]},"truth_ref":null,"published_ref":null}
{"type":"sample","sensor":"camera","seq":4,"t":0.5,"t_capture":0.40000000000000002,"t_publish":null,"dropped":true,"rover_pose":{"pos":[0,0,0],"quat":[1,0,0,0]}}
```
