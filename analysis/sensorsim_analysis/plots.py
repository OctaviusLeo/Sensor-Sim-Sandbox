"""plots.py — matplotlib plots saved to ``results/`` (Requirement 8.6)."""

from __future__ import annotations

import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

from .metrics import SensorMetrics  # noqa: E402

_SAVE_KW = {"dpi": 110, "metadata": {"Software": None}}


def _nan0(x: float | None) -> float:
    return 0.0 if x is None or math.isnan(x) else float(x)


def plot_error_histograms(metrics: dict[str, SensorMetrics], out_dir: Path) -> list[Path]:
    paths = []
    for name in sorted(metrics):
        m = metrics[name]
        if m.hist_counts is None or m.hist_edges is None or m.error_samples == 0:
            continue
        fig, ax = plt.subplots(figsize=(6, 3.5))
        centers = 0.5 * (m.hist_edges[:-1] + m.hist_edges[1:])
        ax.bar(centers, m.hist_counts, width=np.diff(m.hist_edges), color="#4c72b0")
        ax.set_title(f"{name}: published − truth (bias {m.bias:.4g}, std {m.std:.4g})")
        ax.set_xlabel("error")
        ax.set_ylabel("count")
        fig.tight_layout()
        p = out_dir / f"{name}_error_hist.png"
        fig.savefig(p, **_SAVE_KW)
        plt.close(fig)
        paths.append(p)
    return paths


def plot_configured_vs_measured(metrics: dict[str, SensorMetrics], out_dir: Path) -> Path:
    names = sorted(metrics)
    x = np.arange(len(names))
    fig, axes = plt.subplots(1, 3, figsize=(11, 3.5))
    pairs = [
        ("dropout", [_nan0(metrics[n].configured_dropout) for n in names],
         [_nan0(metrics[n].measured_dropout) for n in names]),
        ("latency (ms)", [_nan0(metrics[n].configured_latency_ms) for n in names],
         [_nan0(metrics[n].measured_latency_ms) for n in names]),
        ("rate (Hz)", [_nan0(metrics[n].configured_rate_hz) for n in names],
         [_nan0(metrics[n].achieved_rate_hz) for n in names]),
    ]
    for ax, (title, cfg, meas) in zip(axes, pairs, strict=True):
        ax.bar(x - 0.2, cfg, 0.4, label="configured", color="#999999")
        ax.bar(x + 0.2, meas, 0.4, label="measured", color="#4c72b0")
        ax.set_xticks(x, names)
        ax.set_title(title)
    axes[0].legend(fontsize=8)
    fig.tight_layout()
    p = out_dir / "configured_vs_measured.png"
    fig.savefig(p, **_SAVE_KW)
    plt.close(fig)
    return p


def make_plots(metrics: dict[str, SensorMetrics], out_dir: str | Path) -> list[Path]:
    d = Path(out_dir)
    d.mkdir(parents=True, exist_ok=True)
    paths = plot_error_histograms(metrics, d)
    if metrics:
        paths.append(plot_configured_vs_measured(metrics, d))
    return paths
