"""Unit tests for loaders, metrics edge cases, plots, and summary wiring (Requirement 9.3)."""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from sensorsim_analysis import synthetic
from sensorsim_analysis.loaders import AnalysisInputError, Sample, load_bag, load_run_log, write_run_log
from sensorsim_analysis.metrics import compute_metrics
from sensorsim_analysis.plots import make_plots
from sensorsim_analysis.summary import RateMeasurement, render_summary, write_summary

import analyze


def _lidar(seq, t_cap, lat, truth, pub):
    return Sample("lidar", seq, t_cap + lat, t_cap, t_cap + lat, False,
                  truth=np.array(truth, dtype=float), published=np.array(pub, dtype=float))


def test_lidar_known_answer(tmp_path):
    nan = math.nan
    samples = [
        _lidar(0, 0.0, 0.2, [1.0, 2.0, nan, 4.0], [1.1, nan, nan, 4.1]),
        _lidar(1, 0.1, 0.2, [1.0, 2.0, 3.0, nan], [0.9, 2.1, 3.1, nan]),
    ]
    write_run_log(tmp_path / "l.jsonl", {"seed": 1, "commit": "c",
                                         "sensor_config": {"lidar": {"dropout_prob": 0.1, "latency_ms": 200}}},
                  samples)
    m = compute_metrics(load_run_log(tmp_path / "l.jsonl"))["lidar"]
    diffs = np.array([0.1, 0.1, -0.1, 0.1, 0.1])
    assert m.error_samples == 5
    assert m.bias == pytest.approx(diffs.mean())
    assert m.std == pytest.approx(diffs.std())
    assert m.rmse == pytest.approx(math.sqrt((diffs**2).mean()))
    assert m.measured_dropout == pytest.approx(1 / 6)  # 1 dropped of 6 rays with a hit
    assert m.measured_latency_ms == pytest.approx(200.0)
    assert m.achieved_rate_hz == pytest.approx(1 / 0.1)


def test_camera_frames_and_images(tmp_path):
    (tmp_path / "frames").mkdir()
    truth = np.full((4, 6, 3), 100, dtype=np.uint8)
    Image.fromarray(truth).save(tmp_path / "frames/cam_000000.png")
    Image.fromarray(truth + 2).save(tmp_path / "frames/cam_000000_pub.png")  # lossless "published"
    samples = [
        Sample("camera", 0, 0.2, 0.0, 0.2, False, truth_ref="frames/cam_000000.png",
               published_ref="frames/cam_000000_pub.png"),
        Sample("camera", 1, 0.1, 0.1, None, True),
        Sample("camera", 2, 0.4, 0.2, 0.4, False),
        Sample("camera", 3, 0.5, 0.3, 0.5, False),
    ]
    write_run_log(tmp_path / "c.jsonl", {"seed": 1, "commit": "c", "sensor_config": {}}, samples)
    m = compute_metrics(load_run_log(tmp_path / "c.jsonl"))["camera"]
    assert m.error_samples == 4 * 6 * 3
    assert m.bias == pytest.approx(2.0) and m.std == pytest.approx(0.0)
    assert m.measured_dropout == pytest.approx(0.25)
    assert m.published_count == 3
    assert m.achieved_rate_hz == pytest.approx(2 / 0.3)


def test_missing_camera_image_is_an_input_error(tmp_path):
    samples = [Sample("camera", 0, 0.2, 0.0, 0.2, False, truth_ref="frames/x.png", published_ref="frames/y.jpg")]
    write_run_log(tmp_path / "c.jsonl", {"seed": 1, "commit": "c", "sensor_config": {}}, samples)
    with pytest.raises(AnalysisInputError, match="x.png"):
        compute_metrics(load_run_log(tmp_path / "c.jsonl"))


def test_header_only_log_has_no_sensors(tmp_path):
    write_run_log(tmp_path / "h.jsonl", {"seed": 1, "commit": "c", "sensor_config": {}}, [])
    assert compute_metrics(load_run_log(tmp_path / "h.jsonl")) == {}


def test_reads_engine_written_header_format(tmp_path):
    p = tmp_path / "e.jsonl"
    p.write_text(
        '{"type":"run_meta","seed":1234,"commit":"fe40","sensor_config":{"lidar":{"dropout_prob":0.10000000000000001}},'
        '"camera_sampling_interval":"1/10"}\n'
        '{"type":"sample","sensor":"lidar","seq":0,"t":0.20000000000000001,"t_capture":0,"t_publish":0.20000000000000001,'
        '"dropped":false,"rover_pose":{"pos":[0,0,0],"quat":[1,0,0,0]},"truth":[1.5,null],"published":[1.52,null]}\n'
    )
    log = load_run_log(p)
    assert log.meta["camera_sampling_interval"] == "1/10"
    assert np.isnan(log.samples[0].truth[1])


def _write_bag(path: Path, topic: str, stamps_ns: list[int]):
    from rosbags.rosbag2 import StoragePlugin, Writer
    from rosbags.typesys import Stores, get_typestore

    ts = get_typestore(Stores.ROS2_JAZZY)
    String = ts.types["std_msgs/msg/String"]
    with Writer(path, version=8, storage_plugin=StoragePlugin.SQLITE3) as w:
        conn = w.add_connection(topic, String.__msgtype__, typestore=ts)
        for t in stamps_ns:
            w.write(conn, t, ts.serialize_cdr(String(data="x"), String.__msgtype__))


def test_bag_rates_and_summary(tmp_path):
    stamps = [int(i * 0.1 * 1e9) for i in range(21)]
    _write_bag(tmp_path / "bag", "/lidar/points", stamps)
    bag = load_bag(tmp_path / "bag")
    assert bag.count("/lidar/points") == 21

    samples = [_lidar(i, i * 0.1, 0.2, [1.0], [1.0]) for i in range(5)]
    write_run_log(tmp_path / "l.jsonl", {"seed": 7, "commit": "deadbeef", "sensor_config": {}}, samples)
    hz = tmp_path / "hz"
    hz.mkdir()
    (hz / "lidar_points.txt").write_text("average rate: 9.500\n\tmin: 0.1s\naverage rate: 9.998\n")
    # The camera hz capture is absent, and --hz-dir is explicit -> rejected:
    rc = analyze.run(["--run", str(_run_yaml(tmp_path)), "--hz-dir", str(hz), "--out", str(tmp_path / "o1")])
    assert rc == 2 and not (tmp_path / "o1" / "summary.md").exists()
    (hz / "camera_image_compressed.txt").write_text("average rate: 10.001\n")
    rc = analyze.run(["--run", str(_run_yaml(tmp_path)), "--bag", str(tmp_path / "bag"),
                      "--hz-dir", str(hz), "--out", str(tmp_path / "out")])
    assert rc == 0
    text = (tmp_path / "out" / "summary.md").read_text(encoding="utf-8")
    assert "Seed: 7" in text and "`deadbeef`" in text
    assert "| `/lidar/points` | 9.998 | 10 | no | ros2 topic hz" in text
    assert "| `/camera/image/compressed` | 10.001 | 10 | yes |" in text
    assert "| `/lidar/points` | 10.000 | 10 | yes | bag receive timestamps (21 msgs) |" in text
    assert (tmp_path / "out" / "lidar_error_hist.png").is_file()
    assert (tmp_path / "out" / "configured_vs_measured.png").is_file()


def _run_yaml(tmp_path: Path) -> Path:
    p = tmp_path / "run.yaml"
    p.write_text(f"run_log: {(tmp_path / 'l.jsonl').as_posix()}\n")
    return p


def test_unparsable_bag_rejected(tmp_path):
    (tmp_path / "bag").mkdir()
    (tmp_path / "bag" / "metadata.yaml").write_text("not: [valid")
    with pytest.raises(AnalysisInputError, match="bag"):
        load_bag(tmp_path / "bag")


def test_summary_formatting_and_atomic_write(tmp_path):
    samples = [_lidar(i, i * 0.1, 0.2, [1.0, 2.0], [1.0, 2.5]) for i in range(3)]
    write_run_log(tmp_path / "l.jsonl", {"seed": 3, "commit": "c", "sensor_config": {}}, samples)
    metrics = compute_metrics(load_run_log(tmp_path / "l.jsonl"))
    text = render_summary(metrics, command="cmd", seed=3, commit="c", run_log="l.jsonl",
                          rates=[RateMeasurement("/lidar/points", math.nan, "bag")])
    assert "| lidar | 0.250000 | n/a | 0.250000 | 0.353553 | 6 |" in text
    assert "| `/lidar/points` | n/a | 10 | n/a | bag |" in text
    p = write_summary(tmp_path / "out" / "summary.md", text)
    assert p.read_text(encoding="utf-8") == text
    assert not (tmp_path / "out" / "summary.md.tmp").exists()
    assert len(make_plots(metrics, tmp_path / "plots")) == 2


def test_summary_rate_target_is_each_topics_configured_rate(tmp_path):
    imu = [Sample("imu_accel", i, i * 0.02 + 0.02, i * 0.02, i * 0.02 + 0.02, False,
                  truth=np.zeros(3), published=np.zeros(3)) for i in range(3)]
    samples = [_lidar(i, i * 0.1, 0.2, [1.0], [1.0]) for i in range(3)] + imu
    write_run_log(tmp_path / "l.jsonl", {"seed": 3, "commit": "c", "sensor_config": {
        "lidar": {"rate_hz": 10.0}, "imu_accel": {"rate_hz": 50}}}, samples)
    metrics = compute_metrics(load_run_log(tmp_path / "l.jsonl"))
    text = render_summary(metrics, command="cmd", seed=3, commit="c", run_log="l.jsonl",
                          rates=[RateMeasurement("/lidar/points", 10.2, "bag"),
                                 RateMeasurement("/imu", 30.698, "bag"),
                                 RateMeasurement("/gps/fix", 9.5, "bag")])
    assert "| `/lidar/points` | 10.200 | 10 | yes | bag |" in text
    assert "| `/imu` | 30.698 | 50 | no | bag |" in text
    assert "| `/gps/fix` | 9.500 | 10 | no | bag |" in text  # no gps config: 10 Hz default


def test_reads_jazzy_sqlite3_bag_recorded_on_host():
    """Proves the pinned rosbags reads a bag written by `ros2 bag record -s sqlite3` (Jazzy, WSL)."""
    bag = load_bag(Path(__file__).parent / "data" / "jazzy_bag")
    assert bag.count("/fixture") >= 10
    stamps = bag.timestamps_ns["/fixture"]
    assert stamps == sorted(stamps)


def test_cli_help_describes_the_tool(capsys):
    with pytest.raises(SystemExit) as exc:
        analyze.run(["--help"])
    assert exc.value.code == 0
    out = capsys.readouterr().out
    assert "bias, std, RMSE" in out and "Exit status" in out


def test_synthetic_quickstart_recovers_injected_lidar_errors(tmp_path):
    """The README's CPU-only quickstart: synthetic Run_Log -> analyze.py -> summary.md."""
    log = tmp_path / "run_log.jsonl"
    synthetic.main([str(log)])
    run_yaml = tmp_path / "run.yaml"
    run_yaml.write_text("seed: 1234\n")  # hermetic: no bag or topic hz inputs
    assert analyze.run(["--run", str(run_yaml), "--log", str(log), "--out", str(tmp_path / "out")]) == 0
    assert "| lidar |" in (tmp_path / "out" / "summary.md").read_text(encoding="utf-8")
    m = compute_metrics(load_run_log(log))["lidar"]
    assert m.std == pytest.approx(0.02, rel=0.05)
    assert m.measured_dropout == pytest.approx(0.10, abs=0.01)
    assert m.measured_latency_ms == pytest.approx(200.0)
