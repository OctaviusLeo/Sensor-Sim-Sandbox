"""synthetic.py — synthetic Run_Logs with injected, known error parameters.

Used by the property tests (Property 14) and the demo:
    python -m sensorsim_analysis.synthetic OUT.jsonl [--seed N]
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .loaders import Sample, write_run_log


@dataclass
class Injected:
    bias: float
    sigma: float
    dropout: float
    latency_s: float
    rate_hz: float = 10.0
    rays: int = 1000
    scans: int = 20
    truth_range: tuple[float, float] = (1.0, 50.0)


def make_lidar_samples(inj: Injected, seed: int) -> list[Sample]:
    """Lidar scans with published = truth + bias + N(0, sigma²) and per-ray dropout."""
    rng = np.random.default_rng(seed)
    samples = []
    period = 1.0 / inj.rate_hz
    for k in range(inj.scans):
        truth = rng.uniform(*inj.truth_range, size=inj.rays)
        noise = rng.normal(0.0, inj.sigma, size=inj.rays) if inj.sigma > 0 else np.zeros(inj.rays)
        published = truth + inj.bias + noise
        published[rng.random(inj.rays) < inj.dropout] = np.nan
        t_capture = k * period
        t_publish = t_capture + inj.latency_s
        samples.append(
            Sample("lidar", k, t_publish, t_capture, t_publish, False, truth=truth, published=published)
        )
    return samples


def make_vector_samples(sensor: str, inj: Injected, axes: int, seed: int) -> list[Sample]:
    """IMU/GPS-style channel: `axes` values per sample, whole-sample dropout."""
    rng = np.random.default_rng(seed)
    samples = []
    period = 1.0 / inj.rate_hz
    for k in range(inj.scans):
        truth = rng.normal(0.0, 10.0, size=axes)
        noise = rng.normal(0.0, inj.sigma, size=axes) if inj.sigma > 0 else np.zeros(axes)
        dropped = bool(rng.random() < inj.dropout)
        published = np.full(axes, np.nan) if dropped else truth + inj.bias + noise
        t_capture = k * period
        t_publish = None if dropped else t_capture + inj.latency_s
        samples.append(Sample(sensor, k, t_capture if dropped else t_publish, t_capture, t_publish,
                              dropped, truth=truth, published=published))
    return samples


def vector_meta(sensor: str, inj: Injected, seed: int) -> dict:
    return {
        "seed": seed,
        "commit": "synthetic",
        "sensor_config": {
            sensor: {
                "rate_hz": inj.rate_hz,
                "noise_sigma": inj.sigma,
                "bias": inj.bias,
                "dropout_prob": inj.dropout,
                "latency_ms": inj.latency_s * 1000.0,
            }
        },
    }


def lidar_meta(inj: Injected, seed: int) -> dict:
    return {
        "seed": seed,
        "commit": "synthetic",
        "sensor_config": {
            "lidar": {
                "rate_hz": inj.rate_hz,
                "range_noise_sigma_m": inj.sigma,
                "dropout_prob": inj.dropout,
                "latency_ms": inj.latency_s * 1000.0,
            }
        },
    }


def main(argv: list[str] | None = None) -> None:
    # Gate A lidar values, 3,601+ rays per dropout check (design "Sample sizes").
    inj = Injected(bias=0.0, sigma=0.02, dropout=0.10, latency_s=0.2, rays=5760, scans=20)
    ap = argparse.ArgumentParser(
        prog="python -m sensorsim_analysis.synthetic",
        description=f"Write a synthetic lidar Run_Log with known injected errors "
        f"(sigma {inj.sigma} m, dropout {inj.dropout}, latency {inj.latency_s * 1000:g} ms; "
        f"{inj.scans} scans of {inj.rays} rays) to check that analyze.py recovers them.",
    )
    ap.add_argument("out", help="Run_Log JSONL path to write (parent folders are created)")
    ap.add_argument("--seed", type=int, default=1234, help="RNG seed (default: %(default)s)")
    a = ap.parse_args(argv)
    write_run_log(Path(a.out), lidar_meta(inj, a.seed), make_lidar_samples(inj, a.seed))
    print(f"wrote {a.out}: {inj}")


if __name__ == "__main__":
    main()
