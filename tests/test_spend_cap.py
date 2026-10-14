"""Unit tests for hosted spend-cap tracking in aib.experiments (Req 5.2).

Issuing stops BEFORE the estimated spend would exceed the cap; the completed-Cell
count (and estimated spend) is reported via the ``stats`` mapping.
"""

from __future__ import annotations

from aib.defenses import DefenseConfig, DefenseKind
from aib.experiments import run_experiments
from aib.goals import load_injected_goals
from aib.injections import INJECTION_STYLES
from aib.tasks import load_user_tasks


class _FinalOnlyModel:
    """A fake ModelClient that always answers with a final message (no tools)."""

    def __init__(self, model: str):
        self.model = model

    def chat(self, messages, tools, temperature, seed) -> dict:
        return {"choices": [{"message": {"role": "assistant", "content": "done"}}]}


def _inputs():
    models = [_FinalOnlyModel("hosted-a")]
    defenses = [DefenseConfig(kind=DefenseKind.NONE)]
    styles = INJECTION_STYLES[:1]
    tasks = load_user_tasks()[:4]  # 4 cases per Cell at reps=1
    goals = load_injected_goals()
    return models, defenses, styles, tasks, goals


def test_issuing_stops_before_cap_exceeded():
    models, defenses, styles, tasks, goals = _inputs()
    # 4 cases * $1 = $4 to fully run; cap $2.50 allows exactly 2 cases.
    stats: dict = {}
    results = run_experiments(
        models=models,
        defenses=defenses,
        styles=styles,
        tasks=tasks,
        repetitions=1,
        seed=0,
        spend_cap_usd=2.5,
        goals=goals,
        cost_estimator=lambda _m: 1.0,
        stats=stats,
    )
    # Only 2 cases could run before the 3rd ($3 > $2.50) was refused.
    assert len(results) == 2
    assert stats["stopped"] is True
    assert stats["estimated_spend_usd"] <= 2.5
    # The Cell did not fully complete (stopped mid-Cell).
    assert stats["completed_cells"] == 0


def test_completed_cell_count_reported_when_within_budget():
    models, defenses, styles, tasks, goals = _inputs()
    stats: dict = {}
    results = run_experiments(
        models=models,
        defenses=defenses,
        styles=styles,
        tasks=tasks,
        repetitions=1,
        seed=0,
        spend_cap_usd=100.0,  # ample budget
        goals=goals,
        cost_estimator=lambda _m: 1.0,
        stats=stats,
    )
    assert len(results) == len(tasks)
    assert stats["stopped"] is False
    assert stats["completed_cells"] == 1
    assert stats["estimated_spend_usd"] == len(tasks)


def test_no_cost_estimator_means_no_spend_stop():
    models, defenses, styles, tasks, goals = _inputs()
    stats: dict = {}
    results = run_experiments(
        models=models,
        defenses=defenses,
        styles=styles,
        tasks=tasks,
        repetitions=1,
        seed=0,
        spend_cap_usd=0.0,  # zero cap, but no estimator => no-op
        goals=goals,
        stats=stats,
    )
    assert len(results) == len(tasks)
    assert stats["stopped"] is False
    assert stats["estimated_spend_usd"] == 0.0
