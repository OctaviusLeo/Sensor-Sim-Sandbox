"""sensorsim_analysis — Python analysis package for Sensor Sim Sandbox.

Reads on-disk Run_Logs (JSONL) and ROS 2 bag recordings, computes per-sensor error
metrics (bias / std / RMSE), measured vs configured dropout and latency, and achieved
publish rate, then writes plots and ``results/summary.md`` consumed by the README.

Scaffolding placeholder — implementations arrive in milestone M4 (tasks 16–18).
"""

__all__ = ["loaders", "metrics", "plots", "summary"]
