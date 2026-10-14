"""Experiment runner + Results_Store (design.md 'Experiment runner and
Results_Store'; Req 5.2, 5.4, 5.5).

:func:`run_experiments` sweeps the Cell grid - every
``(Model, Defense_Condition, Injection_Style)`` combination - over the
User_Tasks with ``repetitions`` repeats, running one case per
``(cell, task, repetition)`` through :class:`~aib.runner.AgentRunner` and
writing exactly one :class:`~aib.runner.CaseResult` record per case (Req 5.5).
At least three repetitions are used where Agent output varies (Req 5.4); each
repetition gets its own derived seed so repeated draws differ while the whole
sweep stays reproducible under the top-level ``seed``.

Hosted spend tracking (Req 5.2)
-------------------------------
When a per-case cost estimate is supplied (``cost_estimator``), the sweep tracks
estimated Hosted_Model spend and STOPS issuing further hosted requests BEFORE the
``spend_cap_usd`` would be exceeded: a case whose estimated cost would push the
running total past the cap is not run, and the sweep stops cleanly, returning the
records completed so far. The number of fully completed Cells (and the estimated
spend) is reported via the optional ``stats`` mapping. For a local-only sweep no
``cost_estimator`` is given (local models incur no hosted spend), so ``spend_cap_usd``
is a no-op and no Cell is skipped - preserving FEAT-004's behavior exactly.

Results_Store on-disk format (consumed by FEAT-005)
---------------------------------------------------
:func:`write_results_store` writes JSON Lines: a UTF-8 text file with one JSON
object per line, each the full :class:`CaseResult` field layout with
``filter_decisions`` serialized as a list of ``{"tool_output_id", "flagged"}``
objects. :func:`read_results_store` round-trips it back to ``CaseResult``s.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, replace
from typing import Sequence

from aib.defenses import DefenseConfig, FilterDecision
from aib.goals import InjectedGoal
from aib.injections import InjectionStyle
from aib.model_client import ModelClient
from aib.runner import AgentRunner, CaseResult
from aib.tasks import UserTask
from typing import Callable, MutableMapping

#: Minimum repetitions for Cells whose Agent output varies (Req 5.4).
MIN_REPETITIONS: int = 3

#: Type of a per-case hosted-cost estimator: ``(model) -> estimated USD per case``.
CostEstimator = Callable[[ModelClient], float]


@dataclass(frozen=True)
class Cell:
    """A ``(model, defense, style)`` triple; metrics are reported per Cell."""

    model: str
    defense: str
    style: str


def _derive_seed(seed: int, repetition: int) -> int:
    """Return a reproducible per-repetition seed derived from the base seed.

    Mixing in the repetition index makes repeated runs of a varying-output Cell
    draw differently while keeping the whole sweep reproducible under ``seed``.
    """
    return (seed * 1_000_003 + repetition) & 0x7FFF_FFFF


def run_experiments(
    models: Sequence[ModelClient],
    defenses: Sequence[DefenseConfig],
    styles: Sequence[InjectionStyle],
    tasks: Sequence[UserTask],
    repetitions: int,
    seed: int,
    spend_cap_usd: float | None,
    *,
    goals: Sequence[InjectedGoal] | None = None,
    temperature: float = 0.0,
    runner: AgentRunner | None = None,
    cost_estimator: CostEstimator | None = None,
    stats: MutableMapping | None = None,
) -> list[CaseResult]:
    """Sweep the Cell grid over the User_Tasks, one record per case (Req 5.5).

    Produces exactly one :class:`CaseResult` per
    ``(model, defense, style, task, repetition)``. The injected goal paired with
    each style cycles through ``goals`` (or :func:`aib.goals.load_injected_goals`)
    so every case has a benign Injected_Goal to score.

    Hosted spend tracking (Req 5.2): when ``cost_estimator`` is supplied, before
    each case the estimated hosted cost of that model is added to a running total;
    if that would push the total past ``spend_cap_usd`` the case is NOT run and the
    sweep stops cleanly, returning the records completed so far. When
    ``cost_estimator`` is ``None`` (local-only sweep) ``spend_cap_usd`` is a no-op.
    If ``stats`` is given it is populated with ``completed_cells`` (fully completed
    ``(model, defense, style)`` Cells), ``estimated_spend_usd``, and ``stopped``.
    """
    if goals is None:
        from aib.goals import load_injected_goals

        goals = load_injected_goals()
    if not goals:
        raise ValueError("run_experiments needs at least one InjectedGoal")

    reps = max(repetitions, 1)
    agent = runner or AgentRunner()
    tools = _default_tools()

    results: list[CaseResult] = []
    estimated_spend = 0.0
    completed_cells = 0
    stopped = False

    for model in models:
        if stopped:
            break
        for defense in defenses:
            if stopped:
                break
            for style in styles:
                if stopped:
                    break
                cell_complete = True
                for task in tasks:
                    # Pair each (style, task) with a benign goal deterministically.
                    goal = goals[len(results) % len(goals)]
                    for rep in range(reps):
                        # Stop BEFORE issuing a hosted request that would exceed
                        # the Spend_Cap (Req 5.2).
                        if cost_estimator is not None and spend_cap_usd is not None:
                            case_cost = cost_estimator(model)
                            if estimated_spend + case_cost > spend_cap_usd:
                                stopped = True
                                cell_complete = False
                                break
                            estimated_spend += case_cost

                        rep_seed = _derive_seed(seed, rep)
                        case = agent.run(
                            task=task,
                            tools=tools,
                            defense=defense,
                            style=style,
                            goal=goal,
                            model=model,
                            temperature=temperature,
                            seed=rep_seed,
                        )
                        results.append(replace(case, repetition=rep))
                    if stopped:
                        break
                if cell_complete and not stopped:
                    completed_cells += 1

    if stats is not None:
        stats["completed_cells"] = completed_cells
        stats["estimated_spend_usd"] = estimated_spend
        stats["stopped"] = stopped
    return results


def _default_tools():
    """Lazy import of the default tool set (avoids an import cycle at module load)."""
    from aib.tools import default_tools

    return default_tools()


# ---------------------------------------------------------------------------
# Results_Store serialization (JSON Lines; one CaseResult per line).
# ---------------------------------------------------------------------------


def _case_to_dict(case: CaseResult) -> dict:
    return {
        "model": case.model,
        "defense": case.defense,
        "style": case.style,
        "task_id": case.task_id,
        "repetition": case.repetition,
        "injection_success": case.injection_success,
        "task_success": case.task_success,
        "errored": case.errored,
        "cause": case.cause,
        "temperature": case.temperature,
        "sampling": case.sampling,
        "filter_decisions": [
            {"tool_output_id": d.tool_output_id, "flagged": d.flagged}
            for d in case.filter_decisions
        ],
    }


def _case_from_dict(obj: dict) -> CaseResult:
    return CaseResult(
        model=obj["model"],
        defense=obj["defense"],
        style=obj["style"],
        task_id=obj["task_id"],
        repetition=int(obj["repetition"]),
        injection_success=obj["injection_success"],
        task_success=bool(obj["task_success"]),
        errored=bool(obj["errored"]),
        cause=obj["cause"],
        temperature=float(obj["temperature"]),
        sampling=dict(obj["sampling"]),
        filter_decisions=tuple(
            FilterDecision(
                tool_output_id=d["tool_output_id"], flagged=bool(d["flagged"])
            )
            for d in obj.get("filter_decisions", [])
        ),
    )


def write_results_store(results: Sequence[CaseResult], path: str) -> None:
    """Write one JSON record per case to ``path`` (JSON Lines, UTF-8)."""
    with open(path, "w", encoding="utf-8") as fh:
        for case in results:
            fh.write(json.dumps(_case_to_dict(case), sort_keys=True))
            fh.write("\n")


def read_results_store(path: str) -> list[CaseResult]:
    """Read a Results_Store written by :func:`write_results_store` (round-trips)."""
    out: list[CaseResult] = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            out.append(_case_from_dict(json.loads(line)))
    return out
