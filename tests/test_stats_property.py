"""Property-based test for the confidence intervals (Correctness Property 7).

Feature: agent-injection-bench, Property 7: Confidence interval brackets the point estimate and is deterministic
"""

from __future__ import annotations

from hypothesis import given
from hypothesis import strategies as st

from aib.metrics import bootstrap_interval, wilson_interval


@given(
    data=st.integers(min_value=1, max_value=500).flatmap(
        lambda n: st.tuples(st.just(n), st.integers(min_value=0, max_value=n))
    ),
    outcomes=st.lists(st.integers(min_value=0, max_value=1), min_size=1, max_size=50),
    seed=st.integers(min_value=0, max_value=10_000),
)
def test_ci_brackets_point_and_is_deterministic(data, outcomes, seed):
    """Feature: agent-injection-bench, Property 7: Confidence interval brackets the point estimate and is deterministic."""
    n, successes = data

    # Wilson: (successes, n).
    w1 = wilson_interval(successes, n)
    w2 = wilson_interval(successes, n)
    assert w1.lower <= w1.point <= w1.upper
    assert 0.0 <= w1.point <= 1.0
    assert abs(w1.point - w2.point) <= 1e-6
    assert abs(w1.lower - w2.lower) <= 1e-6
    assert abs(w1.upper - w2.upper) <= 1e-6

    # Bootstrap: (outcomes, seed) - deterministic under the same seed.
    b1 = bootstrap_interval(outcomes, seed=seed)
    b2 = bootstrap_interval(outcomes, seed=seed)
    assert b1.lower <= b1.point <= b1.upper
    assert 0.0 <= b1.point <= 1.0
    assert abs(b1.point - b2.point) <= 1e-6
    assert abs(b1.lower - b2.lower) <= 1e-6
    assert abs(b1.upper - b2.upper) <= 1e-6
