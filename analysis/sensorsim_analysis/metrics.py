"""metrics.py — per-sensor error and timing metrics.

Bias / std / RMSE of Published_Values vs Ground_Truth paired within each record
(``sensor``, ``seq``), measured dropout and latency beside their Configured_Values, and
achieved rate ``(n - 1) / T`` over ``t_publish``. Records are accumulated in file order
with a fixed merge formula, so two runs on identical inputs give bitwise-identical
results (Requirement 8.3, 8.4, 8.5, 8.8).
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from .loaders import AnalysisInputError, RunLog, Sample


@dataclass
class ErrorAccumulator:
    """Streaming mean/variance of (published - truth) using Chan's parallel merge."""

    n: int = 0
    mean: float = 0.0
    m2: float = 0.0
    sum_sq: float = 0.0
    hist_edges: np.ndarray | None = None
    hist_counts: np.ndarray | None = None

    def add(self, diffs: np.ndarray) -> None:
        if diffs.size == 0:
            return
        nb = int(diffs.size)
        mb = float(np.mean(diffs))
        m2b = float(np.sum((diffs - mb) ** 2))
        delta = mb - self.mean
        n = self.n + nb
        self.mean = self.mean + delta * nb / n
        self.m2 = self.m2 + m2b + delta * delta * self.n * nb / n
        self.n = n
        self.sum_sq += float(np.sum(diffs * diffs))
        if self.hist_edges is not None:
            counts, _ = np.histogram(diffs, bins=self.hist_edges)
            self.hist_counts = counts if self.hist_counts is None else self.hist_counts + counts

    @property
    def bias(self) -> float:
        return self.mean if self.n else math.nan

    @property
    def std(self) -> float:
        return math.sqrt(self.m2 / self.n) if self.n else math.nan

    @property
    def rmse(self) -> float:
        return math.sqrt(self.sum_sq / self.n) if self.n else math.nan


@dataclass
class SensorMetrics:
    sensor: str
    bias: float
    std: float
    rmse: float
    error_samples: int
    measured_dropout: float
    configured_dropout: float | None
    dropout_population: int
    measured_latency_ms: float
    configured_latency_ms: float | None
    achieved_rate_hz: float
    published_count: int
    configured_rate_hz: float | None
    configured_noise_sigma: float | None
    configured_bias: float | None = None
    hist_edges: np.ndarray | None = field(default=None, repr=False)
    hist_counts: np.ndarray | None = field(default=None, repr=False)


def achieved_rate_hz(t_publish: list[float]) -> float:
    """``(n - 1) / T`` for n publish times spanning T seconds (NaN when undefined)."""
    n = len(t_publish)
    if n < 2:
        return math.nan
    span = max(t_publish) - min(t_publish)
    return (n - 1) / span if span > 0 else math.nan


# Configured noise / dropout keys per sensor in the Run_Log header's sensor_config.
_NOISE_KEY = {"lidar": "range_noise_sigma_m", "camera": "pixel_noise_sigma"}
_DROPOUT_KEY = {"lidar": "dropout_prob", "camera": "frame_dropout_prob"}


def _load_image(folder: Path, ref: str, what: str) -> np.ndarray:
    from PIL import Image

    p = folder / ref
    if not p.is_file():
        raise AnalysisInputError(f"camera {what} image not found: {p}")
    try:
        with Image.open(p) as im:
            return np.asarray(im.convert("RGB"), dtype=np.float64)
    except Exception as exc:
        raise AnalysisInputError(f"camera {what} image unparsable: {p} ({exc})") from exc


def _hist_edges(sigma: float | None) -> np.ndarray:
    half = 6.0 * sigma if sigma and sigma > 0 else 1.0
    return np.linspace(-half, half, 81)


def compute_metrics(log: RunLog, sensor_config: dict | None = None) -> dict[str, SensorMetrics]:
    """Compute SensorMetrics for every sensor present in the Run_Log (sorted by name)."""
    cfg_all = sensor_config if sensor_config is not None else log.meta.get("sensor_config", {}) or {}

    per_sensor: dict[str, list[Sample]] = {}
    for s in log.samples:  # file order preserved per sensor
        per_sensor.setdefault(s.sensor, []).append(s)

    out: dict[str, SensorMetrics] = {}
    for sensor in sorted(per_sensor):
        samples = per_sensor[sensor]
        cfg = cfg_all.get(sensor, {}) or {}
        sigma_cfg = cfg.get(_NOISE_KEY.get(sensor, "noise_sigma"))
        acc = ErrorAccumulator(hist_edges=_hist_edges(sigma_cfg))
        dropped = 0
        population = 0
        latencies: list[float] = []
        publish_times: list[float] = []

        for s in samples:
            if s.truth is not None and s.published is not None:
                # Per-element dropout over elements with a truth value (lidar rays).
                has_truth = ~np.isnan(s.truth)
                has_pub = ~np.isnan(s.published)
                population += int(np.count_nonzero(has_truth))
                dropped += int(np.count_nonzero(has_truth & ~has_pub))
                both = has_truth & has_pub
                acc.add(s.published[both] - s.truth[both])
            else:
                # Whole-sample dropout (camera frames, M5 sensors).
                population += 1
                dropped += 1 if s.dropped else 0
                if not s.dropped and s.truth_ref and s.published_ref:
                    truth = _load_image(log.folder, s.truth_ref, "truth")
                    pub = _load_image(log.folder, s.published_ref, "published")
                    if truth.shape != pub.shape:
                        raise AnalysisInputError(
                            f"camera seq {s.seq}: truth/published image sizes differ"
                        )
                    acc.add((pub - truth).ravel())
            if not s.dropped and s.t_publish is not None:
                latencies.append(s.t_publish - s.t_capture)
                publish_times.append(s.t_publish)

        out[sensor] = SensorMetrics(
            sensor=sensor,
            bias=acc.bias,
            std=acc.std,
            rmse=acc.rmse,
            error_samples=acc.n,
            measured_dropout=dropped / population if population else math.nan,
            configured_dropout=cfg.get(_DROPOUT_KEY.get(sensor, "dropout_prob")),
            dropout_population=population,
            measured_latency_ms=1000.0 * math.fsum(latencies) / len(latencies) if latencies else math.nan,
            configured_latency_ms=cfg.get("latency_ms"),
            achieved_rate_hz=achieved_rate_hz(publish_times),
            published_count=len(publish_times),
            configured_rate_hz=cfg.get("rate_hz"),
            configured_noise_sigma=sigma_cfg,
            configured_bias=cfg.get("bias"),
            hist_edges=acc.hist_edges,
            hist_counts=acc.hist_counts,
        )
    return out
