"""Property-based tests for the metrics layer (Correctness Properties 5, 6, 8, 9).

Feature: agent-injection-bench, Property 5: Attack_Success_Rate counts only classified injected cases
Feature: agent-injection-bench, Property 6: Rates and filter metrics lie in their ranges
Feature: agent-injection-bench, Property 8: Defense_Cost equals the task-success-rate difference from the control
Feature: agent-injection-bench, Property 9: Metrics are deterministic under a fixed seed
"""

from __future__ import annotations

import math

from hypothesis import given
from hypothesis import strategies as st

from aib.defenses import FilterDecision
from aib.metrics import (
    attack_success_rate,
    compute_cell_metrics,
    defense_cost,
    filter_precision_recall,
    metrics_to_jsonable,
    task_success_rate,
)
from aib.runner import CaseResult

# ``injection_success`` is True (followed), False (not), or None (unclassified).
_INJ = st.sampled_from([True, False, None])


@st.composite
def _case(draw, model=None, defense=None, style=None):
    """Build one CaseResult; errored cases carry injection_success=None."""
    errored = draw(st.booleans())
    inj = None if errored else draw(_INJ)
    return CaseResult(
        model=model or draw(st.sampled_from(["m0", "m1"])),
        defense=defense or draw(st.sampled_from(["none", "delimit", "filter"])),
        style=style or draw(st.sampled_from(["none", "direct", "hidden"])),
        task_id=draw(st.sampled_from(["t0", "t1", "t2"])),
        repetition=draw(st.integers(min_value=0, max_value=3)),
        injection_success=inj,
        task_success=draw(st.booleans()),
        errored=errored,
        cause="boom" if errored else None,
        temperature=draw(st.sampled_from([0.0, 0.7])),
        sampling={"seed": draw(st.integers(0, 5)), "max_turns": 8},
        filter_decisions=(),
    )


_cases = st.lists(_case(), min_size=0, max_size=20)


# ---------------------------------------------------------------------------
# Property 5: Attack_Success_Rate counts only classified injected cases.
# ---------------------------------------------------------------------------
@given(cases=_cases)
def test_attack_success_rate_counts_only_classified(cases):
    """Feature: agent-injection-bench, Property 5: Attack_Success_Rate counts only classified injected cases."""
    rate = attack_success_rate(cases, seed=0)

    non_errored = [c for c in cases if not c.errored]
    followed = sum(1 for c in non_errored if c.injection_success is True)
    not_followed = sum(1 for c in non_errored if c.injection_success is False)
    unclassified = sum(1 for c in non_errored if c.injection_success is None)
    classified = followed + not_followed

    # The reported unclassified count excludes errored cases.
    assert rate.unclassified == unclassified

    if classified == 0:
        # Documented rule: no classified injected cases => "not computed".
        assert rate.ci is None
        assert rate.cause is not None and "not computed" in rate.cause
        assert rate.n == 0
    else:
        assert rate.ci is not None
        assert rate.n == classified
        # Denominator excludes unclassified + errored; value == followed/classified.
        assert math.isclose(rate.ci.point, followed / classified, abs_tol=1e-9)
        assert 0.0 <= rate.ci.point <= 1.0


# ---------------------------------------------------------------------------
# Property 6: Rates and filter metrics lie in their ranges.
# ---------------------------------------------------------------------------
@given(
    cases=_cases,
    flags=st.lists(st.booleans(), min_size=0, max_size=15),
    truth=st.lists(st.booleans(), min_size=0, max_size=15),
)
def test_rates_and_filter_metrics_in_range(cases, flags, truth):
    """Feature: agent-injection-bench, Property 6: Rates and filter metrics lie in their ranges."""
    asr = attack_success_rate(cases, seed=1)
    tsr = task_success_rate(cases, seed=1)
    for rate in (asr, tsr):
        if rate.ci is not None:
            assert 0.0 <= rate.ci.lower <= rate.ci.point <= rate.ci.upper <= 1.0

    n = min(len(flags), len(truth))
    decisions = [FilterDecision(f"o{i}", flags[i]) for i in range(n)]
    precision, recall = filter_precision_recall(decisions, truth[:n])
    assert 0.0 <= precision <= 1.0
    assert 0.0 <= recall <= 1.0

    # Identity cases for the ASR: all-followed => 1.0, none-followed => 0.0.
    all_followed = [
        CaseResult("m", "none", "direct", "t", 0, True, True, False, None, 0.0, {}, ())
        for _ in range(3)
    ]
    none_followed = [
        CaseResult("m", "none", "direct", "t", 0, False, True, False, None, 0.0, {}, ())
        for _ in range(3)
    ]
    assert attack_success_rate(all_followed, seed=1).ci.point == 1.0
    assert attack_success_rate(none_followed, seed=1).ci.point == 0.0


# ---------------------------------------------------------------------------
# Property 8: Defense_Cost equals the task-success-rate difference from control.
# ---------------------------------------------------------------------------
@given(
    a=st.floats(min_value=0.0, max_value=1.0),
    b=st.floats(min_value=0.0, max_value=1.0),
)
def test_defense_cost_is_control_minus_defense(a, b):
    """Feature: agent-injection-bench, Property 8: Defense_Cost equals the task-success-rate difference from the control."""
    assert math.isclose(defense_cost(a, b), a - b, abs_tol=1e-6)


# ---------------------------------------------------------------------------
# Property 9: Metrics are deterministic under a fixed seed.
# ---------------------------------------------------------------------------
def _flatten_numbers(obj, floats, others):
    """Split a JSON-able structure into float values and int/categorical values."""
    if isinstance(obj, bool):
        others.append(obj)
    elif isinstance(obj, float):
        floats.append(obj)
    elif isinstance(obj, int):
        others.append(obj)
    elif isinstance(obj, str) or obj is None:
        others.append(obj)
    elif isinstance(obj, dict):
        for key in sorted(obj):
            others.append(key)
            _flatten_numbers(obj[key], floats, others)
    elif isinstance(obj, (list, tuple)):
        for item in obj:
            _flatten_numbers(item, floats, others)


@given(cases=_cases, seed=st.integers(min_value=0, max_value=1000))
def test_metrics_deterministic_under_fixed_seed(cases, seed):
    """Feature: agent-injection-bench, Property 9: Metrics are deterministic under a fixed seed."""
    first = metrics_to_jsonable(compute_cell_metrics(cases, seed=seed, method="bootstrap"))
    second = metrics_to_jsonable(compute_cell_metrics(cases, seed=seed, method="bootstrap"))

    floats_a: list[float] = []
    others_a: list = []
    floats_b: list[float] = []
    others_b: list = []
    _flatten_numbers(first, floats_a, others_a)
    _flatten_numbers(second, floats_b, others_b)

    # Ints and categoricals compare exactly; floats within 1e-6.
    assert others_a == others_b
    assert len(floats_a) == len(floats_b)
    for x, y in zip(floats_a, floats_b):
        assert abs(x - y) <= 1e-6
