"""Hypothesis property tests for the analysis core (Properties 9, 14, 15, 16, 17).

Each property runs ≥100 examples (design "Testing Strategy").
"""

from __future__ import annotations

import json
import math
import random
import shutil
import tempfile
from pathlib import Path

import numpy as np
import pytest
from hypothesis import HealthCheck, given, settings
from hypothesis import strategies as st

from sensorsim_analysis.loaders import AnalysisInputError, Sample, load_run_log, write_run_log
from sensorsim_analysis.metrics import achieved_rate_hz, compute_metrics
from sensorsim_analysis.summary import render_summary
from sensorsim_analysis.synthetic import Injected, lidar_meta, make_lidar_samples

import analyze

PROPS = settings(
    max_examples=100,
    deadline=None,
    suppress_health_check=[HealthCheck.too_slow, HealthCheck.function_scoped_fixture],
)


def _tmpdir() -> Path:
    return Path(tempfile.mkdtemp(prefix="ssa_"))


finite = st.floats(min_value=-1e6, max_value=1e6, allow_nan=False, allow_infinity=False)
range_or_null = st.one_of(st.none(), st.floats(min_value=0.0, max_value=1e3, allow_nan=False))


@st.composite
def samples_st(draw):
    n = draw(st.integers(min_value=0, max_value=12))
    out = []
    t = 0.0
    for seq in range(n):
        sensor = draw(st.sampled_from(["lidar", "camera"]))
        t_cap = t
        dropped = sensor == "camera" and draw(st.booleans())
        t_pub = None if dropped else t_cap + draw(st.floats(min_value=0.0, max_value=1.0))
        t = t_cap if dropped else t_pub
        if sensor == "lidar":
            rays = draw(st.integers(min_value=0, max_value=20))
            truth = [draw(range_or_null) for _ in range(rays)]
            pub = [None if v is None else draw(range_or_null) for v in truth]
            out.append(Sample("lidar", seq, t, t_cap, t_pub, False,
                              truth=np.array([math.nan if v is None else v for v in truth]),
                              published=np.array([math.nan if v is None else v for v in pub])))
        else:
            ref = None if dropped or draw(st.booleans()) else f"frames/cam_{seq:06d}"
            out.append(Sample("camera", seq, t, t_cap, t_pub, dropped,
                              truth_ref=None if ref is None else ref + ".png",
                              published_ref=None if ref is None else ref + "_pub.jpg"))
    return out


# Feature: sensor-sim-sandbox, Property 9: Run_Log round-trip preserves data
@PROPS
@given(samples=samples_st(), seed=st.integers(min_value=0, max_value=2**31))
def test_property9_run_log_round_trip(samples, seed):
    d = _tmpdir()
    try:
        meta = {"seed": seed, "commit": "abc", "sensor_config": {"lidar": {"latency_ms": 200}}}
        write_run_log(d / "log.jsonl", meta, samples)
        log = load_run_log(d / "log.jsonl")
        assert log.meta == {"type": "run_meta", **meta}
        assert len(log.samples) == len(samples)
        for a, b in zip(samples, log.samples):
            assert (a.sensor, a.seq, a.t, a.t_capture, a.t_publish, a.dropped) == (
                b.sensor, b.seq, b.t, b.t_capture, b.t_publish, b.dropped)
            for x, y in ((a.truth, b.truth), (a.published, b.published)):
                if x is None:
                    assert y is None
                else:
                    np.testing.assert_array_equal(x, y)
            assert (a.truth_ref, a.published_ref) == (b.truth_ref, b.published_ref)
    finally:
        shutil.rmtree(d, ignore_errors=True)


# Feature: sensor-sim-sandbox, Property 14: Analysis recovers injected error parameters
@PROPS
@given(
    seed=st.integers(min_value=0, max_value=2**31),
    bias=st.floats(min_value=-0.5, max_value=0.5),
    sigma=st.floats(min_value=0.005, max_value=0.5),
    dropout=st.floats(min_value=0.0, max_value=0.9),
    latency_ms=st.integers(min_value=0, max_value=1000),
)
def test_property14_recovers_injected_parameters(seed, bias, sigma, dropout, latency_ms):
    # 20,000 rays: dropout ±2 pp and sigma ±10% are > 4 standard errors (design "Sample sizes").
    inj = Injected(bias=bias, sigma=sigma, dropout=dropout, latency_s=latency_ms / 1000.0,
                   rays=2000, scans=10)
    d = _tmpdir()
    try:
        write_run_log(d / "log.jsonl", lidar_meta(inj, seed), make_lidar_samples(inj, seed))
        m = compute_metrics(load_run_log(d / "log.jsonl"))["lidar"]
    finally:
        shutil.rmtree(d, ignore_errors=True)
    n = m.error_samples
    assert abs(m.bias - bias) <= 5 * sigma / math.sqrt(n)
    assert abs(m.std - sigma) <= 0.10 * sigma
    assert m.rmse == pytest.approx(math.sqrt(m.bias**2 + m.std**2), rel=1e-9)
    assert abs(m.measured_dropout - dropout) <= 0.02
    assert m.measured_latency_ms == pytest.approx(latency_ms, abs=1e-6)
    assert m.configured_dropout == dropout
    assert m.configured_noise_sigma == sigma
    assert m.configured_latency_ms == pytest.approx(latency_ms)


# Feature: sensor-sim-sandbox, Property 15: Achieved rate matches the sample timeline
@PROPS
@given(st.lists(st.floats(min_value=0.0, max_value=1e4, allow_nan=False), min_size=2, max_size=200))
def test_property15_rate_matches_timeline(times):
    span = max(times) - min(times)
    rate = achieved_rate_hz(times)
    if span > 0:
        assert rate == (len(times) - 1) / span
    else:
        assert math.isnan(rate)


# Feature: sensor-sim-sandbox, Property 16: Analysis is deterministic on identical inputs
@PROPS
@given(samples=samples_st(), seed=st.integers(min_value=0, max_value=2**31))
def test_property16_deterministic(samples, seed):
    samples = [s for s in samples if s.sensor == "lidar" or s.truth_ref is None]  # no image files
    d = _tmpdir()
    try:
        write_run_log(d / "log.jsonl", {"seed": seed, "commit": "c", "sensor_config": {}}, samples)
        texts = []
        for _ in range(2):
            log = load_run_log(d / "log.jsonl")
            texts.append(render_summary(compute_metrics(log), command="x", seed=seed,
                                        commit="c", run_log="log.jsonl"))
        assert texts[0] == texts[1]
    finally:
        shutil.rmtree(d, ignore_errors=True)


CORRUPTIONS = ["missing", "empty", "garbage", "no_header", "bad_field", "truncated"]


# Feature: sensor-sim-sandbox, Property 17: Missing or unparsable input is rejected
@PROPS
@given(kind=st.sampled_from(CORRUPTIONS), seed=st.integers(min_value=0, max_value=2**31))
def test_property17_bad_input_rejected(kind, seed, capsys):
    d = _tmpdir()
    try:
        rnd = random.Random(seed)
        log = d / f"run_{seed}.jsonl"
        good_header = json.dumps({"type": "run_meta", "seed": seed, "commit": "c", "sensor_config": {}})
        good = json.dumps({"type": "sample", "sensor": "lidar", "seq": 0, "t": 1.0, "t_capture": 0.8,
                           "t_publish": 1.0, "dropped": False, "truth": [1.0], "published": [1.1]})
        if kind == "empty":
            log.write_text("")
        elif kind == "garbage":
            log.write_text("".join(chr(rnd.randint(33, 126)) for _ in range(40)) + "{\n")
        elif kind == "no_header":
            log.write_text(good + "\n")
        elif kind == "bad_field":
            rec = json.loads(good)
            del rec[rnd.choice(["sensor", "seq", "t", "t_capture", "t_publish", "dropped"])]
            log.write_text(good_header + "\n" + json.dumps(rec) + "\n")
        elif kind == "truncated":
            log.write_text(good_header + "\n" + good[: rnd.randint(1, len(good) - 2)] + "\n")
        run_yaml = d / "run.yaml"
        run_yaml.write_text(f"run_log: {log.as_posix()}\noutput_dir: {(d / 'out').as_posix()}\n")
        rc = analyze.run(["--run", str(run_yaml)])
        err = capsys.readouterr().err
        assert rc == 2
        assert log.name in err
        assert not (d / "out" / "summary.md").exists()
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_property17_rejects_missing_bag(tmp_path, capsys):
    inj = Injected(0.0, 0.02, 0.1, 0.2, rays=50, scans=3)
    write_run_log(tmp_path / "log.jsonl", lidar_meta(inj, 1), make_lidar_samples(inj, 1))
    run_yaml = tmp_path / "run.yaml"
    run_yaml.write_text(f"run_log: {(tmp_path / 'log.jsonl').as_posix()}\n"
                        f"output_dir: {(tmp_path / 'out').as_posix()}\n")
    rc = analyze.run(["--run", str(run_yaml), "--bag", str(tmp_path / "missing_bag")])
    assert rc == 2
    assert "missing_bag" in capsys.readouterr().err
    assert not (tmp_path / "out" / "summary.md").exists()
