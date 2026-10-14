"""Property-based test for the deterministic success checks.

Feature: agent-injection-bench, Property 3: Success checks are deterministic boolean predicates with a reason on failure
"""

from __future__ import annotations

from hypothesis import given
from hypothesis import strategies as st

from aib.sandbox import SandboxState, init_sandbox
from aib.tasks import load_user_tasks

_TASKS = load_user_tasks()
_task_ids = st.sampled_from([t.task_id for t in _TASKS])
_seeds = st.integers(min_value=0, max_value=2**31 - 1)
_responses = st.text(
    alphabet=st.characters(whitelist_categories=("L", "N", "Zs", "P")),
    min_size=0,
    max_size=60,
)


def _task_by_id(task_id: str):
    return next(t for t in _TASKS if t.task_id == task_id)


@given(task_id=_task_ids, seed=_seeds, response=_responses)
def test_success_checks_deterministic_with_reason(task_id, seed, response):
    """Feature: agent-injection-bench, Property 3: Success checks are deterministic boolean predicates with a reason on failure."""
    task = _task_by_id(task_id)

    # Evaluate the same check twice over equal (state, response) inputs.
    state_first = init_sandbox(task, seed)
    state_second = init_sandbox(task, seed)
    first = task.success_check(state_first, response)
    second = task.success_check(state_second, response)

    # Deterministic: repeated evaluation agrees exactly.
    assert first == second
    assert isinstance(first.passed, bool)

    # passed is False implies a non-empty unmet reason; passed is True implies none.
    if first.passed is False:
        assert first.unmet is not None and first.unmet != ""
    else:
        assert first.unmet is None
