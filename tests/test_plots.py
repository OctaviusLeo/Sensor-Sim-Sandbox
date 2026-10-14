"""Unit tests for the plot generators (aib/plots.py).

Given a small ``results/summary.md``, each generator writes a non-empty figure
file (Req 7.3). The plots are generated from the table only.
"""

from __future__ import annotations

from aib.metrics import compute_cell_metrics
from aib.plots import parse_results_table, plot_asr_by_defense, plot_defense_cost
from aib.results_table import write_results_table
from aib.runner import CaseResult


def _case(**kw) -> CaseResult:
    base = dict(
        model="m0",
        defense="none",
        style="direct",
        task_id="t0",
        repetition=0,
        injection_success=True,
        task_success=True,
        errored=False,
        cause=None,
        temperature=0.0,
        sampling={"seed": 0, "max_turns": 8},
        filter_decisions=(),
    )
    base.update(kw)
    return CaseResult(**base)


def _summary(tmp_path):
    cases = [
        _case(defense="none", style="direct", injection_success=True, task_success=True),
        _case(defense="none", style="direct", injection_success=False, task_success=True, task_id="t1"),
        _case(defense="filter", style="direct", injection_success=False, task_success=True),
        _case(defense="filter", style="direct", injection_success=False, task_success=False, task_id="t1"),
    ]
    blocks = compute_cell_metrics(cases, seed=0)
    out = tmp_path / "summary.md"
    write_results_table(blocks, str(out))
    return str(out)


def test_parse_results_table_reads_rows(tmp_path):
    rows = parse_results_table(_summary(tmp_path))
    assert len(rows) == 2
    none_row = next(r for r in rows if r.defense == "none")
    assert none_row.asr is not None
    assert 0.0 <= none_row.asr[0] <= 1.0


def test_plot_asr_by_defense_writes_nonempty(tmp_path):
    summary = _summary(tmp_path)
    out = tmp_path / "asr.png"
    plot_asr_by_defense(summary, str(out))
    assert out.exists()
    assert out.stat().st_size > 0


def test_plot_defense_cost_writes_nonempty(tmp_path):
    summary = _summary(tmp_path)
    out = tmp_path / "cost.png"
    plot_defense_cost(summary, str(out))
    assert out.exists()
    assert out.stat().st_size > 0
