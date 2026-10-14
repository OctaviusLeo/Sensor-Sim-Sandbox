"""Property-based test for the deterministic Stub_Model (Correctness Property 10).

Feature: agent-injection-bench, Property 10: Stub_Model drives a deterministic case outcome
"""

from __future__ import annotations

from dataclasses import replace

from hypothesis import given
from hypothesis import strategies as st

from aib.defenses import DefenseConfig, DefenseKind
from aib.goals import load_injected_goals
from aib.injections import load_injection_styles
from aib.runner import AgentRunner
from aib.stub_model import StubModel
from aib.tasks import load_user_tasks
from aib.tools import default_tools

# The CI smoke subset draws from these task shapes (email / calendar / file).
_SMOKE_TASK_IDS = ["email-01", "calendar-01", "file-write-01", "file-contains-01"]
_TASKS_BY_ID = {t.task_id: t for t in load_user_tasks()}
_STYLES = load_injection_styles()
_GOALS = load_injected_goals()
_TOOLS = default_tools()

_DEFENSE_KINDS = [
    DefenseKind.NONE,
    DefenseKind.DELIMIT,
    DefenseKind.HIERARCHY,
    DefenseKind.FILTER,
]


def _trace_key(case):
    """Compare on the fields under our control (exclude the always-0 repetition)."""
    return replace(case, repetition=0)


@given(
    task_id=st.sampled_from(_SMOKE_TASK_IDS),
    style_idx=st.integers(min_value=0, max_value=len(_STYLES) - 1),
    goal_idx=st.integers(min_value=0, max_value=len(_GOALS) - 1),
    defense_kind=st.sampled_from(_DEFENSE_KINDS),
    temperature=st.sampled_from([0.0, 0.7]),
    seed=st.integers(min_value=0, max_value=1000),
)
def test_stub_model_drives_a_deterministic_case_outcome(
    task_id, style_idx, goal_idx, defense_kind, temperature, seed
):
    """Feature: agent-injection-bench, Property 10: Stub_Model drives a deterministic case outcome."""
    task = _TASKS_BY_ID[task_id]
    style = _STYLES[style_idx]
    goal = _GOALS[goal_idx]
    defense = DefenseConfig(kind=defense_kind)

    runner = AgentRunner()
    model = StubModel()

    first = runner.run(task, _TOOLS, defense, style, goal, model, temperature, seed)
    second = runner.run(task, _TOOLS, defense, style, goal, model, temperature, seed)

    # Identical trace + identical check outcomes on repeat (Req 8.6).
    assert _trace_key(first) == _trace_key(second)
    assert first.injection_success == second.injection_success
    assert first.task_success == second.task_success

    # The Stub_Model never acts on an injected instruction, so the Injected_Goal
    # is never carried out: a fixed, assertable Injection_Success outcome.
    assert first.errored is False
    assert first.injection_success is False
