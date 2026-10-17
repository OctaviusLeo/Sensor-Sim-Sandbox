# Gate A (task 19) — 2026-10-17

| Check | Command | Result |
| --- | --- | --- |
| pytest (fresh clone) | `.venv\Scripts\python -m pytest analysis/tests` | 15 passed |
| Unreal Automation | `pwsh -NoProfile -File scripts\run_ue_tests.ps1` | total=28 succeeded=28 failed=0 notRun=0, exit 0 |
| End-to-end | `pwsh -NoProfile -File scripts\run_e2e.ps1` (seed 1234, commit 6486781250115858cf3b39f4d61ca8bdbaa8363f) | RESULT: PASS, run e2e_20261017_180743, 2,000 camera frames, 223.8 s |
| Analysis | `.venv\Scripts\python analysis\analyze.py --run config/run.yaml` | exit 0, results/summary.md with both topic rates |
| Demo GIF | docs/media/demo.gif | 7,005,870 bytes (< 10 MB), 100 in-engine viewport frames |

Measured rates (ros2 topic hz, 60 s): /lidar/points 9.775 Hz, /camera/image/compressed 9.420 Hz — target 10 Hz not met; causes in README "Measured results".
Earlier full runs on the same code path: commit aa625bf lidar 9.997 / camera 9.640 Hz (avg frame 13.0 ms); commit 50d5b6f lidar 9.455 / camera 9.040 Hz (demo screenshots overlapped the hz window — fixed in 6486781).
