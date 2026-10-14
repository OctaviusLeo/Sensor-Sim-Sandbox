"""Unit tests for the metrics layer (aib/metrics.py).

Covers the empty-Cell "not computed" path (Req 6.8), Wilson and bootstrap CIs on
known inputs (Req 6.2), filter precision/recall on a labeled fixture (Req 6.5),
Attack/Task rate denominators (Req 6.3), and the CLI entry point (Req 6.7).
"""

from __future__ import annotations

import json

import pytest

from aib.defenses import FilterDecision
from aib.metrics import (
    attack_success_rate,
    bootstrap_interval,
    compute_cell_metrics,
    defense_cost,
    filter_precision_recall,
    main,
    task_success_rate,
    wilson_interval,
)
from aib.runner import CaseResult


def _case(**kw) -> CaseResult:
    base = dict(
        model="m0",
        defense="none",
        style="direct",
        task_id="t0",
        repetition=0,
        injection_success=False,
        task_success=True,
        errored=False,
        cause=None,
        temperature=0.0,
        sampling={"seed": 0, "max_turns": 8},
        filter_decisions=(),
    )
    base.update(kw)
    return CaseResult(**base)


# ---------------------------------------------------------------------------
# Wilson / bootstrap on known inputs.
# ---------------------------------------------------------------------------
def test_wilson_known_input_half():
    ci = wilson_interval(5, 10)
    assert ci.method == "wilson"
    assert ci.point == pytest.approx(0.5)
    assert ci.lower < 0.5 < ci.upper
    # Wilson 95% CI for 5/10 is approximately [0.237, 0.763].
    assert ci.lower == pytest.approx(0.2366, abs=1e-3)
    assert ci.upper == pytest.approx(0.7634, abs=1e-3)


def test_wilson_all_successes_upper_is_one_ish():
    ci = wilson_interval(10, 10)
    assert ci.point == 1.0
    assert ci.lower <= 1.0 <= ci.upper + 1e-9
    assert ci.upper == pytest.approx(1.0, abs=1e-9)


def test_wilson_requires_positive_n():
    with pytest.raises(ValueError):
        wilson_interval(0, 0)
    with pytest.raises(ValueError):
        wilson_interval(3, 2)


def test_bootstrap_all_ones_is_degenerate():
    ci = bootstrap_interval([1, 1, 1, 1], seed=7)
    assert ci.method == "bootstrap"
    assert ci.point == 1.0
    assert ci.lower == 1.0 and ci.upper == 1.0


def test_bootstrap_deterministic_and_in_range():
    data = [1, 0, 1, 1, 0, 0, 1, 0]
    a = bootstrap_interval(data, seed=42)
    b = bootstrap_interval(data, seed=42)
    assert (a.point, a.lower, a.upper) == pytest.approx((b.point, b.lower, b.upper))
    assert 0.0 <= a.lower <= a.point <= a.upper <= 1.0


def test_bootstrap_empty_raises():
    with pytest.raises(ValueError):
        bootstrap_interval([], seed=0)


# ---------------------------------------------------------------------------
# Rates and the empty-Cell "not computed" path.
# ---------------------------------------------------------------------------
def test_attack_success_rate_excludes_unclassified_and_errored():
    cases = [
        _case(injection_success=True),
        _case(injection_success=False),
        _case(injection_success=None),  # unclassified: excluded
        _case(injection_success=None, errored=True, cause="x"),  # errored: excluded
    ]
    rate = attack_success_rate(cases)
    assert rate.n == 2  # only the two classified cases
    assert rate.unclassified == 1  # the one non-errored None
    assert rate.ci is not None
    assert rate.ci.point == pytest.approx(0.5)


def test_attack_success_rate_not_computed_when_no_classified():
    cases = [_case(injection_success=None), _case(errored=True, injection_success=None, cause="x")]
    rate = attack_success_rate(cases)
    assert rate.ci is None
    assert "not computed" in rate.cause
    assert rate.n == 0


def test_task_success_rate_excludes_errored():
    cases = [
        _case(task_success=True),
        _case(task_success=False),
        _case(task_success=False, errored=True, cause="x"),
    ]
    rate = task_success_rate(cases)
    assert rate.n == 2
    assert rate.ci.point == pytest.approx(0.5)


def test_task_success_rate_not_computed_when_all_errored():
    rate = task_success_rate([_case(errored=True, cause="x")])
    assert rate.ci is None
    assert "not computed" in rate.cause


# ---------------------------------------------------------------------------
# Defense_Cost and filter precision/recall.
# ---------------------------------------------------------------------------
def test_defense_cost_arithmetic():
    assert defense_cost(0.8, 0.6) == pytest.approx(0.2)
    assert defense_cost(0.5, 0.9) == pytest.approx(-0.4)


def test_filter_precision_recall_labeled_fixture():
    # 2 TP, 1 FP, 1 FN, 1 TN.
    decisions = [
        FilterDecision("a", True),   # TP (injected)
        FilterDecision("b", True),   # TP (injected)
        FilterDecision("c", True),   # FP (clean, flagged)
        FilterDecision("d", False),  # FN (injected, missed)
        FilterDecision("e", False),  # TN (clean, not flagged)
    ]
    truth = [True, True, False, True, False]
    precision, recall = filter_precision_recall(decisions, truth)
    assert precision == pytest.approx(2 / 3)
    assert recall == pytest.approx(2 / 3)


def test_filter_precision_recall_empty_conventions():
    # Nothing flagged => precision 1.0; nothing injected => recall 1.0.
    precision, recall = filter_precision_recall(
        [FilterDecision("a", False)], [False]
    )
    assert precision == 1.0
    assert recall == 1.0


def test_filter_precision_recall_length_mismatch():
    with pytest.raises(ValueError):
        filter_precision_recall([FilterDecision("a", True)], [True, False])


# ---------------------------------------------------------------------------
# Whole-store aggregation: Defense_Cost references the right control.
# ---------------------------------------------------------------------------
def test_compute_cell_metrics_defense_cost_vs_control():
    cases = [
        # Control: none/direct, TSR = 1.0
        _case(defense="none", style="direct", task_success=True),
        _case(defense="none", style="direct", task_success=True, task_id="t1"),
        # Defense: filter/direct, TSR = 0.5
        _case(defense="filter", style="direct", task_success=True),
        _case(defense="filter", style="direct", task_success=False, task_id="t1"),
    ]
    blocks = compute_cell_metrics(cases, seed=0)
    control = blocks[("m0", "none", "direct")]
    defended = blocks[("m0", "filter", "direct")]
    assert control["defense_cost"] == 0.0
    assert defended["defense_cost"] == pytest.approx(1.0 - 0.5)


def test_compute_cell_metrics_empty_cell_still_computes_others():
    cases = [
        _case(defense="none", injection_success=True),
        _case(defense="filter", injection_success=None),  # only unclassified
    ]
    blocks = compute_cell_metrics(cases, seed=0)
    assert blocks[("m0", "none", "direct")]["asr"].ci is not None
    # The filter Cell has no classified injected cases: ASR not computed.
    assert blocks[("m0", "filter", "direct")]["asr"].ci is None


# ---------------------------------------------------------------------------
# CLI (Req 6.7).
# ---------------------------------------------------------------------------
def test_cli_help_exits_zero(capsys):
    with pytest.raises(SystemExit) as exc:
        main(["--help"])
    assert exc.value.code == 0


def test_cli_runs_over_store(tmp_path, capsys):
    from aib.experiments import write_results_store

    store = tmp_path / "store.jsonl"
    write_results_store(
        [_case(injection_success=True), _case(injection_success=False)], str(store)
    )
    code = main(["--results", str(store), "--seed", "3"])
    assert code == 0
    out = json.loads(capsys.readouterr().out)
    assert "stamp" in out and "cells" in out
    assert out["stamp"]["seed"] == 3
    assert len(out["cells"]) == 1
