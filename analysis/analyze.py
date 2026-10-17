"""analyze.py — single-command entry point for the analysis tool.

Usage:
    python analysis/analyze.py --run config/run.yaml [--log RUN_LOG] [--bag BAG] [--out DIR]

Reads the Run_Log (and optionally a ROS 2 bag and raw ``ros2 topic hz`` captures),
computes per-sensor metrics, saves plots to the output directory, and writes
``summary.md`` there (Requirement 8.1-8.8). On missing or unparsable input it exits
with status 2, prints an error naming the bad input, and writes no Summary_File.
"""

from __future__ import annotations

import argparse
import math
import re
import shlex
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from sensorsim_analysis.loaders import AnalysisInputError, load_bag, load_run_log  # noqa: E402
from sensorsim_analysis.metrics import achieved_rate_hz, compute_metrics  # noqa: E402
from sensorsim_analysis.plots import make_plots  # noqa: E402
from sensorsim_analysis.summary import RateMeasurement, render_summary, write_summary  # noqa: E402

TOPICS = ("/lidar/points", "/camera/image/compressed")


def _load_yaml(path: Path) -> dict:
    import yaml

    if not path.is_file():
        raise AnalysisInputError(f"run config not found: {path}")
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except yaml.YAMLError as exc:
        raise AnalysisInputError(f"run config unparsable: {path} ({exc})") from exc
    if not isinstance(data, dict):
        raise AnalysisInputError(f"run config unparsable: {path} (not a mapping)")
    return data


def parse_topic_hz(path: Path) -> float:
    """Last ``average rate: X`` reported in a raw ``ros2 topic hz`` capture."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise AnalysisInputError(f"topic hz capture unreadable: {path} ({exc})") from exc
    rates = re.findall(r"average rate:\s*([0-9.]+)", text)
    if not rates:
        raise AnalysisInputError(f"topic hz capture has no 'average rate' line: {path}")
    return float(rates[-1])


def _topic_file(hz_dir: Path, topic: str) -> Path:
    return hz_dir / (topic.strip("/").replace("/", "_") + ".txt")


def run(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--run", required=True, help="run config YAML (config/run.yaml)")
    ap.add_argument("--log", help="Run_Log path (overrides run_log in the run config)")
    ap.add_argument("--bag", help="rosbag2 folder (overrides bag in the run config)")
    ap.add_argument("--hz-dir", help="folder of raw `ros2 topic hz` captures")
    ap.add_argument("--out", help="output folder (overrides output_dir)")
    args = ap.parse_args(argv)

    command = "python analysis/analyze.py " + " ".join(
        shlex.quote(a) for a in (argv if argv is not None else sys.argv[1:])
    )
    try:
        run_path = Path(args.run)
        cfg = _load_yaml(run_path)
        root = Path.cwd()
        log_path = Path(args.log or cfg.get("run_log") or "")
        if not str(log_path) or str(log_path) == ".":
            raise AnalysisInputError(f"no run_log given (in {run_path} or --log)")
        out_dir = Path(args.out or cfg.get("output_dir") or "results")

        log = load_run_log(root / log_path if not log_path.is_absolute() else log_path)
        metrics = compute_metrics(log)

        rates: list[RateMeasurement] = []
        notes: list[str] = []
        hz_dir_s = args.hz_dir or cfg.get("topic_hz_dir")
        if hz_dir_s:
            hz_dir = Path(hz_dir_s)
            explicit = args.hz_dir is not None
            for topic in TOPICS:
                f = _topic_file(hz_dir, topic)
                if f.is_file():
                    rates.append(RateMeasurement(topic, parse_topic_hz(f), "ros2 topic hz (60 s window)"))
                elif explicit:
                    raise AnalysisInputError(f"topic hz capture not found: {f}")
        bag_s = args.bag or cfg.get("bag")
        if bag_s:
            bag_path = Path(bag_s)
            if bag_path.exists() or args.bag:
                bag = load_bag(bag_path)
                for topic in TOPICS:
                    ts = bag.timestamps_ns.get(topic, [])
                    rate = achieved_rate_hz([t * 1e-9 for t in ts])
                    rates.append(RateMeasurement(topic, rate, f"bag receive timestamps ({len(ts)} msgs)"))
            else:
                notes.append(f"No bag at `{bag_path}`; bag rate cross-check skipped.")

        text = render_summary(
            metrics,
            command=command,
            seed=log.meta.get("seed"),
            commit=log.meta.get("commit"),
            run_log=str(log_path).replace("\\", "/"),
            rates=rates,
            notes=notes,
        )
        make_plots(metrics, out_dir)
        summary_path = write_summary(out_dir / "summary.md", text)
    except AnalysisInputError as exc:
        print(f"analyze.py: error: {exc}", file=sys.stderr)
        print("analyze.py: no Summary_File written.", file=sys.stderr)
        return 2

    print(f"analyze.py: wrote {summary_path}")
    for name in sorted(metrics):
        m = metrics[name]
        print(
            f"  {name}: bias={m.bias:.6g} std={m.std:.6g} rmse={m.rmse:.6g} "
            f"dropout={m.measured_dropout:.4f} (cfg {m.configured_dropout}) "
            f"latency={m.measured_latency_ms:.2f} ms (cfg {m.configured_latency_ms}) "
            f"rate={m.achieved_rate_hz if not math.isnan(m.achieved_rate_hz) else 'n/a'}"
        )
    return 0


def main() -> None:
    sys.exit(run())


if __name__ == "__main__":
    main()
