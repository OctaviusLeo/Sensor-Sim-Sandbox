"""Unit tests for scripts/smoke_test.py pure helpers (task 1.4).

subprocess is mocked; the live GPU/Docker/Model_Server path is not exercised.
"""

from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys

import pytest

_SMOKE_PATH = pathlib.Path(__file__).resolve().parent.parent / "scripts" / "smoke_test.py"
_spec = importlib.util.spec_from_file_location("aib_smoke_test", _SMOKE_PATH)
smoke = importlib.util.module_from_spec(_spec)
assert _spec and _spec.loader
# Register before executing so dataclasses can resolve string annotations.
sys.modules["aib_smoke_test"] = smoke
_spec.loader.exec_module(smoke)


def test_check_python_version_pass():
    result = smoke.check_python_version((3, 11, 5))
    assert result.ok is True
    assert "3.11.5" in result.detail


def test_check_python_version_fail():
    result = smoke.check_python_version((3, 10, 12))
    assert result.ok is False
    assert "below" in result.detail


def test_check_docker_available_success():
    def fake_run(cmd, **kwargs):
        return subprocess.CompletedProcess(cmd, 0, stdout="Docker version 27.0.0\n", stderr="")

    result = smoke.check_docker_available(runner=fake_run)
    assert result.ok is True
    assert "Docker version" in result.detail


def test_check_docker_available_missing():
    def fake_run(cmd, **kwargs):
        raise FileNotFoundError("docker not found")

    result = smoke.check_docker_available(runner=fake_run)
    assert result.ok is False
    assert "unavailable" in result.detail


def test_parse_gpu_total_mib():
    assert smoke.parse_gpu_total_mib("10240\n") == 10240
    assert smoke.parse_gpu_total_mib("8192, 90\n16384, 10\n") == 8192


def test_parse_gpu_total_mib_empty_raises():
    with pytest.raises(ValueError):
        smoke.parse_gpu_total_mib("\n  \n")


def test_query_gpu_total_mib_uses_runner():
    def fake_run(cmd, **kwargs):
        assert cmd[0] == "nvidia-smi"
        return subprocess.CompletedProcess(cmd, 0, stdout="10240\n", stderr="")

    assert smoke.query_gpu_total_mib(runner=fake_run) == 10240


def test_format_evidence_contains_marks_and_values():
    checks = [
        smoke.CheckResult(name="python", ok=True, detail="Python 3.11.5 satisfies >= 3.11"),
        smoke.CheckResult(name="docker", ok=False, detail="docker unavailable: x"),
    ]
    text = smoke.format_evidence(checks, gpu_total_mib=10240, peak_gpu_mib=6000, response_text="hi")
    assert "[PASS] python" in text
    assert "[FAIL] docker" in text
    assert "GPU total: 10240 MiB" in text
    assert "Peak GPU: 6000 MiB" in text
    assert "Response (non-empty): True" in text


def test_format_evidence_empty_response_flagged_false():
    text = smoke.format_evidence([], gpu_total_mib=1, peak_gpu_mib=1, response_text="   ")
    assert "Response (non-empty): False" in text
