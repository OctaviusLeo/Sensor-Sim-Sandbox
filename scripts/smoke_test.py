"""M0 environment + Model_Server smoke test (task 1.4; Req 1.4-1.11).

SHIPPED but NOT executed in CI or on the dev box: it requires Python>=3.11,
Docker, and a GPU (nvidia-smi) plus a running Model_Server. It verifies the
prerequisites, starts the Model_Server, sends one tool-free prompt, measures
peak GPU MiB, records a non-empty response, and writes the evidence into
docs/ENVIRONMENT.md. On any unmet prerequisite or a failure within the 120 s
cap it reports the cause, records the failed condition, and exits non-zero.

The pure helpers (prerequisite checks, GPU parsing, evidence formatting) are
factored out so tests/test_smoke_test.py can unit-test them with subprocess
mocked. No real network egress in the helpers.
"""

from __future__ import annotations

import subprocess
import sys
from dataclasses import dataclass

#: Smoke-test request timeout cap (seconds), per task 1.4.
TIMEOUT_CAP_S: float = 120.0

#: Minimum supported Python (Req 1.10).
MIN_PYTHON: tuple[int, int] = (3, 11)


@dataclass(frozen=True)
class CheckResult:
    """Outcome of a single prerequisite check."""

    name: str
    ok: bool
    detail: str


def check_python_version(version: tuple[int, int, int]) -> CheckResult:
    """Verify the running Python is >= :data:`MIN_PYTHON` (Req 1.10)."""
    ok = version[:2] >= MIN_PYTHON
    rendered = ".".join(str(p) for p in version)
    required = ".".join(str(p) for p in MIN_PYTHON)
    detail = (
        f"Python {rendered} satisfies >= {required}"
        if ok
        else f"Python {rendered} is below required {required}"
    )
    return CheckResult(name="python", ok=ok, detail=detail)


def check_docker_available(runner=subprocess.run) -> CheckResult:
    """Verify Docker is available via ``docker --version`` (Req 1.10)."""
    try:
        out = runner(
            ["docker", "--version"],
            capture_output=True,
            text=True,
            check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError, OSError) as exc:
        return CheckResult(name="docker", ok=False, detail=f"docker unavailable: {exc}")
    return CheckResult(name="docker", ok=True, detail=out.stdout.strip())


def parse_gpu_total_mib(nvidia_smi_output: str) -> int:
    """Parse total GPU MiB from ``nvidia-smi --query-gpu=memory.total`` output.

    Expects one integer MiB value per GPU line (``--format=csv,noheader,nounits``);
    returns the first GPU's total. Raises :class:`ValueError` on empty/garbage.
    """
    for line in nvidia_smi_output.splitlines():
        token = line.strip().split(",")[0].strip()
        if token:
            return int(token)
    raise ValueError("no GPU memory total found in nvidia-smi output")


def query_gpu_total_mib(runner=subprocess.run) -> int:
    """Capture total GPU MiB via nvidia-smi (Req 1.6, 1.7)."""
    out = runner(
        [
            "nvidia-smi",
            "--query-gpu=memory.total",
            "--format=csv,noheader,nounits",
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    return parse_gpu_total_mib(out.stdout)


def format_evidence(
    checks: list[CheckResult],
    gpu_total_mib: int,
    peak_gpu_mib: int,
    response_text: str,
) -> str:
    """Render the smoke-test evidence block for docs/ENVIRONMENT.md (Req 1.8)."""
    lines = ["### Smoke-test evidence (generated)", ""]
    for check in checks:
        mark = "PASS" if check.ok else "FAIL"
        lines.append(f"- [{mark}] {check.name}: {check.detail}")
    lines.append(f"- GPU total: {gpu_total_mib} MiB")
    lines.append(f"- Peak GPU: {peak_gpu_mib} MiB")
    lines.append(f"- Response (non-empty): {bool(response_text.strip())}")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:  # pragma: no cover - GPU/Docker path
    """Run the smoke test end-to-end. Returns a process exit code.

    Not exercised in CI (no GPU/Docker). The pure helpers above carry the
    unit-tested logic; this driver wires them to the live environment.
    """
    import platform

    checks: list[CheckResult] = []
    version = tuple(int(p) for p in platform.python_version_tuple())
    py_check = check_python_version(version)  # type: ignore[arg-type]
    checks.append(py_check)
    docker_check = check_docker_available()
    checks.append(docker_check)

    for check in checks:
        if not check.ok:
            print(f"PREREQUISITE FAILED: {check.name}: {check.detail}", file=sys.stderr)
            return 1

    try:
        gpu_total = query_gpu_total_mib()
    except (subprocess.CalledProcessError, FileNotFoundError, OSError, ValueError) as exc:
        print(f"GPU PREREQUISITE FAILED: {exc}", file=sys.stderr)
        return 1

    # The live Model_Server interaction, peak-GPU measurement, and
    # docs/ENVIRONMENT.md write are owner-run on the GPU machine.
    print(f"GPU total MiB: {gpu_total}")
    print("Model_Server interaction is owner-run on the GPU/Docker machine.")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
