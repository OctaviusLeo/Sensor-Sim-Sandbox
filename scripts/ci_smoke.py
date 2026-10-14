"""GPU-free, API-key-free CI smoke subset (design.md 'Stub model and CI smoke
subset'; Req 8.2, 8.3, 8.5, 8.6).

Runs a **small, fixed** Cell subset driven by the deterministic
:class:`~aib.stub_model.StubModel` so continuous integration can gate the build
with **no GPU and no Hosted_Model API key** (Req 8.3). Because the Stub_Model's
tool-call trace and final answer are a pure function of the User_Task prompt, the
Injection_Success_Check and Task_Success_Check outcomes are fixed and assertable
(Req 8.5, 8.6):

* the StubModel completes the legitimate User_Task  => Task_Success is ``True``;
* the StubModel never acts on an injected instruction => Injection_Success is
  ``False`` (the Injected_Goal is not carried out).

The script exits ``0`` only when every case in the subset ran and met those
expected outcomes; otherwise it prints the cause and exits non-zero so the CI
run fails (Req 8.5).

Single command: ``python scripts/ci_smoke.py``.
"""

from __future__ import annotations

import sys

from aib.defenses import DefenseConfig, DefenseKind
from aib.goals import load_injected_goals
from aib.injections import load_injection_styles
from aib.runner import AgentRunner, CaseResult
from aib.stub_model import StubModel
from aib.tasks import load_user_tasks
from aib.tools import default_tools

#: Seed for the smoke subset (fixed so the run is reproducible, Req 0.2).
SMOKE_SEED: int = 7

#: Task ids pulled from the authored catalog that cover the main success shapes
#: (email send / calendar add / file write). Kept small so CI stays fast.
_SMOKE_TASK_IDS: tuple[str, ...] = ("email-01", "calendar-01", "file-write-01")

#: Defense_Conditions exercised in the smoke subset: the No_Defense control plus
#: one output-transforming Defense, so the loop and the Defense seam both run.
_SMOKE_DEFENSES: tuple[DefenseConfig, ...] = (
    DefenseConfig(kind=DefenseKind.NONE),
    DefenseConfig(kind=DefenseKind.DELIMIT),
)


def run_ci_smoke(seed: int = SMOKE_SEED) -> list[CaseResult]:
    """Run the fixed smoke subset with the Stub_Model and return the records.

    One case per (smoke task, smoke defense) using the first Injection_Style and
    Injected_Goal; every case is driven by the deterministic Stub_Model so the
    outcomes are fixed (Req 8.5, 8.6).
    """
    tasks_by_id = {t.task_id: t for t in load_user_tasks()}
    tasks = [tasks_by_id[tid] for tid in _SMOKE_TASK_IDS]
    style = load_injection_styles()[0]
    goal = load_injected_goals()[0]
    tools = default_tools()

    runner = AgentRunner()
    model = StubModel()

    records: list[CaseResult] = []
    for task in tasks:
        for defense in _SMOKE_DEFENSES:
            records.append(
                runner.run(
                    task=task,
                    tools=tools,
                    defense=defense,
                    style=style,
                    goal=goal,
                    model=model,
                    temperature=0.0,
                    seed=seed,
                )
            )
    return records


def _check_outcomes(records: list[CaseResult]) -> list[str]:
    """Return a list of failure messages; empty means every expectation held."""
    failures: list[str] = []
    if not records:
        return ["smoke subset produced no case records"]
    for case in records:
        label = f"{case.model}/{case.defense}/{case.style}/{case.task_id}"
        if case.errored:
            failures.append(f"{label}: case errored ({case.cause})")
            continue
        # Deterministic expected outcomes for the Stub_Model (Req 8.6):
        if case.task_success is not True:
            failures.append(f"{label}: expected Task_Success=True, got {case.task_success}")
        if case.injection_success is not False:
            failures.append(
                f"{label}: expected Injection_Success=False, got {case.injection_success}"
            )
    return failures


def main(argv: list[str] | None = None) -> int:
    """Run the smoke subset and assert its deterministic outcomes (Req 8.5)."""
    records = run_ci_smoke()

    # Determinism guard: a second run must reproduce the records exactly.
    if run_ci_smoke() != records:
        print("CI smoke FAILED: the Stub_Model subset was not reproducible", file=sys.stderr)
        return 1

    failures = _check_outcomes(records)
    if failures:
        print("CI smoke FAILED - expected deterministic outcomes not met:", file=sys.stderr)
        for message in failures:
            print(f"  - {message}", file=sys.stderr)
        return 1

    print(f"CI smoke OK: {len(records)} cases, all deterministic.")
    for case in records:
        print(
            f"  {case.defense:9s} {case.task_id:14s} "
            f"Task_Success={case.task_success} Injection_Success={case.injection_success}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
