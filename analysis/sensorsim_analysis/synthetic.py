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


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--seed", type=int, default=1234)
    a = ap.parse_args()
    # Gate A lidar values, 3,601+ rays per dropout check (design "Sample sizes").
    inj = Injected(bias=0.0, sigma=0.02, dropout=0.10, latency_s=0.2, rays=5760, scans=20)
    write_run_log(Path(a.out), lidar_meta(inj, a.seed), make_lidar_samples(inj, a.seed))
    print(f"wrote {a.out}: {inj}")


if __name__ == "__main__":
    main()
