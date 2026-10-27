"""sensorsim_analysis — Python analysis package for Sensor Sim Sandbox.

Reads on-disk Run_Logs (JSONL) and ROS 2 bag recordings, computes per-sensor error
metrics (bias / std / RMSE), measured vs configured dropout and latency, and achieved
publish rate, then writes plots and ``results/summary.md`` consumed by the README.
``synthetic`` builds Run_Logs with known injected errors for tests and the quickstart.
"""

__all__ = ["loaders", "metrics", "plots", "summary", "synthetic"]
