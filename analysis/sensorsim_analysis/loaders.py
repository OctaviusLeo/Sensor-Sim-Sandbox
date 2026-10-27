"""loaders.py — readers for JSONL Run_Logs and ``ros2 bag`` recordings.

On missing or unparsable input, the analysis terminates without writing a Summary_File
and emits an error naming the bad input (Requirement 8.1, 8.2).

Run_Logs are streamed line by line; each record's range arrays are converted to
``numpy.float64`` immediately (``null`` -> NaN). Bags are read with the pure-Python
``rosbags`` package (sqlite3 storage); no ROS Python module is imported, so the
analysis runs on Windows and in CI.
"""

from __future__ import annotations

import json
import math
from collections.abc import Iterable, Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np

REQUIRED_SAMPLE_FIELDS = ("sensor", "seq", "t", "t_capture", "t_publish", "dropped")


class AnalysisInputError(Exception):
    """Missing or unparsable analysis input; the message names the bad input."""


@dataclass
class Sample:
    """One Run_Log sample record. Range arrays use NaN for JSON ``null``."""

    sensor: str
    seq: int
    t: float
    t_capture: float
    t_publish: float | None
    dropped: bool
    rover_pose: dict | None = None
    truth: np.ndarray | None = None
    published: np.ndarray | None = None
    truth_ref: str | None = None
    published_ref: str | None = None


@dataclass
class RunLog:
    path: Path
    meta: dict
    samples: list[Sample] = field(default_factory=list)

    @property
    def folder(self) -> Path:
        return self.path.parent


def _to_array(values: Any, where: str) -> np.ndarray:
    if not isinstance(values, list):
        raise AnalysisInputError(f"{where}: expected a JSON array")
    return np.array([math.nan if v is None else float(v) for v in values], dtype=np.float64)


def _parse_sample(obj: dict, where: str) -> Sample:
    for key in REQUIRED_SAMPLE_FIELDS:
        if key not in obj:
            raise AnalysisInputError(f"{where}: sample record missing field '{key}'")
    t_publish = obj["t_publish"]
    s = Sample(
        sensor=str(obj["sensor"]),
        seq=int(obj["seq"]),
        t=float(obj["t"]),
        t_capture=float(obj["t_capture"]),
        t_publish=None if t_publish is None else float(t_publish),
        dropped=bool(obj["dropped"]),
        rover_pose=obj.get("rover_pose"),
        truth_ref=obj.get("truth_ref"),
        published_ref=obj.get("published_ref"),
    )
    if "truth" in obj:
        s.truth = _to_array(obj["truth"], f"{where} 'truth'")
    if "published" in obj:
        s.published = _to_array(obj["published"], f"{where} 'published'")
    if s.truth is not None and s.published is not None and s.truth.shape != s.published.shape:
        raise AnalysisInputError(f"{where}: 'truth' and 'published' lengths differ")
    return s


def iter_run_log(path: str | Path) -> Iterator[dict | Sample]:
    """Yield the header dict, then each Sample, streaming the file in order."""
    p = Path(path)
    if not p.is_file():
        raise AnalysisInputError(f"Run_Log not found: {p}")
    try:
        fh = p.open("r", encoding="utf-8")
    except OSError as exc:
        raise AnalysisInputError(f"Run_Log unreadable: {p} ({exc})") from exc
    with fh:
        header_seen = False
        for lineno, line in enumerate(fh, start=1):
            if not line.strip():
                continue
            where = f"Run_Log {p} line {lineno}"
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as exc:
                raise AnalysisInputError(f"{where}: not valid JSON ({exc.msg})") from exc
            if not isinstance(obj, dict):
                raise AnalysisInputError(f"{where}: record is not a JSON object")
            if not header_seen:
                if obj.get("type") != "run_meta":
                    raise AnalysisInputError(f"{where}: first line is not a run_meta header")
                header_seen = True
                yield obj
                continue
            if obj.get("type") != "sample":
                raise AnalysisInputError(f"{where}: unknown record type {obj.get('type')!r}")
            yield _parse_sample(obj, where)
        if not header_seen:
            raise AnalysisInputError(f"Run_Log {p}: empty (no run_meta header)")


def load_run_log(path: str | Path) -> RunLog:
    """Load a whole Run_Log (header + samples) in file order."""
    it = iter_run_log(path)
    meta = next(it)
    log = RunLog(path=Path(path), meta=meta)  # type: ignore[arg-type]
    log.samples.extend(it)  # type: ignore[arg-type]
    return log


def _json_value(arr: np.ndarray) -> list:
    return [None if math.isnan(v) else float(v) for v in arr.tolist()]


def write_run_log(path: str | Path, meta: dict, samples: Iterable[Sample]) -> None:
    """Write a Run_Log in the design's JSONL format (used for synthetic logs and tests)."""
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("w", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps({"type": "run_meta", **meta}, allow_nan=False) + "\n")
        for s in samples:
            rec: dict[str, Any] = {
                "type": "sample",
                "sensor": s.sensor,
                "seq": s.seq,
                "t": s.t,
                "t_capture": s.t_capture,
                "t_publish": s.t_publish,
                "dropped": s.dropped,
                "rover_pose": s.rover_pose
                if s.rover_pose is not None
                else {"pos": [0.0, 0.0, 0.0], "quat": [1.0, 0.0, 0.0, 0.0]},
            }
            if s.truth is not None:
                rec["truth"] = _json_value(s.truth)
            if s.published is not None:
                rec["published"] = _json_value(s.published)
            if s.sensor == "camera" and not s.dropped:
                rec["truth_ref"] = s.truth_ref
                rec["published_ref"] = s.published_ref
            fh.write(json.dumps(rec, allow_nan=False) + "\n")


@dataclass
class BagSummary:
    path: Path
    timestamps_ns: dict[str, list[int]]

    def count(self, topic: str) -> int:
        return len(self.timestamps_ns.get(topic, []))


def load_bag(path: str | Path) -> BagSummary:
    """Read per-topic message receive timestamps from a rosbag2 (sqlite3) folder."""
    from rosbags.rosbag2 import Reader  # pure Python; no ROS install needed

    p = Path(path)
    if not p.exists():
        raise AnalysisInputError(f"bag not found: {p}")
    try:
        stamps: dict[str, list[int]] = {}
        with Reader(p) as reader:
            for connection, timestamp, _raw in reader.messages():
                stamps.setdefault(connection.topic, []).append(int(timestamp))
    except AnalysisInputError:
        raise
    except Exception as exc:  # rosbags raises several reader error types
        raise AnalysisInputError(f"bag unparsable: {p} ({exc})") from exc
    for v in stamps.values():
        v.sort()
    return BagSummary(path=p, timestamps_ns=stamps)
