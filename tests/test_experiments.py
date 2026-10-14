"""Unit tests for aib.experiments (Req 5.4, 5.5).

The grid produces exactly one record per (cell, task, repetition); varying-output
cells use >=3 repetitions; write_results_store/read_results_store round-trip.
"""

from __future__ import annotations

import os

from aib.defenses import DefenseConfig, DefenseKind
from aib.experiments import (
    MIN_REPETITIONS,
    Cell,
    read_results_store,
    run_experiments,
    write_results_store,
)
from aib.goals import load_injected_goals
from aib.injections import INJECTION_STYLES
from aib.runner import CaseResult
from aib.tasks import load_user_tasks


class _FinalOnlyModel:
    """A fake ModelClient that always answers with a final message (no tools)."""

    def __init__(self, model: str):
        self.model = model

    def chat(self, messages, tools, temperature, seed) -> dict:
        return {"choices": [{"message": {"role": "assistant", "content": "done"}}]}


def _small_inputs():
    models = [_FinalOnlyModel("m1"), _FinalOnlyModel("m2")]
    defenses = [
        DefenseConfig(kind=DefenseKind.NONE),
        DefenseConfig(kind=DefenseKind.DELIMIT),
    ]
    styles = INJECTION_STYLES[:2]
    tasks = load_user_tasks()[:3]
    goals = load_injected_goals()
    return models, defenses, styles, tasks, goals


def test_one_record_per_cell_task_repetition():
    models, defenses, styles, tasks, goals = _small_inputs()
    reps = 3
    results = run_experiments(
        models=models,
        defenses=defenses,
        styles=styles,
        tasks=tasks,
        repetitions=reps,
        seed=42,
        spend_cap_usd=None,
        goals=goals,
    )
    expected = len(models) * len(defenses) * len(styles) * len(tasks) * reps
    assert len(results) == expected
    assert all(isinstance(r, CaseResult) for r in results)

    # Exactly one record per (cell, task, repetition) key.
    keys = [
        (r.model, r.defense, r.style, r.task_id, r.repetition) for r in results
    ]
    assert len(keys) == len(set(keys))


def test_repetitions_cover_min_for_varying_output():
    models, defenses, styles, tasks, goals = _small_inputs()
    results = run_experiments(
        models=models,
        defenses=defenses,
        styles=styles,
        tasks=tasks,
        repetitions=MIN_REPETITIONS,
        seed=1,
        spend_cap_usd=None,
        goals=goals,
    )
    reps = {r.repetition for r in results}
    assert reps == set(range(MIN_REPETITIONS))
    assert len(reps) >= MIN_REPETITIONS


def test_cell_grid_covers_every_combination():
    models, defenses, styles, tasks, goals = _small_inputs()
    results = run_experiments(
        models=models,
        defenses=defenses,
        styles=styles,
        tasks=tasks,
        repetitions=1,
        seed=5,
        spend_cap_usd=None,
        goals=goals,
    )
    cells = {Cell(r.model, r.defense, r.style) for r in results}
    expected_cells = {
        Cell(m.model, d.kind.value, s.style_id)
        for m in models
        for d in defenses
        for s in styles
    }
    assert cells == expected_cells


def test_results_store_round_trips(tmp_path):
    models, defenses, styles, tasks, goals = _small_inputs()
    results = run_experiments(
        models=models,
        defenses=defenses,
        styles=styles,
        tasks=tasks,
        repetitions=2,
        seed=9,
        spend_cap_usd=None,
        goals=goals,
    )
    path = os.path.join(str(tmp_path), "results.jsonl")
    write_results_store(results, path)
    loaded = read_results_store(path)
    assert loaded == results


def test_spend_cap_param_is_accepted_as_noop_for_local():
    models, defenses, styles, tasks, goals = _small_inputs()
    results = run_experiments(
        models=models[:1],
        defenses=defenses[:1],
        styles=styles[:1],
        tasks=tasks[:1],
        repetitions=1,
        seed=0,
        spend_cap_usd=5.0,  # accepted; not enforced for local-only sweeps
        goals=goals,
    )
    assert len(results) == 1
