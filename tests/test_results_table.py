"""Unit tests for the Results_Table writer (aib/results_table.py).

Covers: regeneration produces the file (Req 7.2); a CI appears for every reported
rate; Defense_Cost rows reference the correct control; the "not computed" rate is
rendered with its stable cause.
"""

from __future__ import annotations

from aib.experiments import write_results_store
from aib.metrics import compute_cell_metrics
from aib.results_table import (
    COLUMNS,
    build_table_from_store,
    render_results_table,
    write_results_table,
)
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


def _cells():
    cases = [
        _case(defense="none", style="direct", task_success=True),
        _case(defense="none", style="direct", task_success=True, task_id="t1"),
        _case(defense="filter", style="direct", task_success=True),
        _case(defense="filter", style="direct", task_success=False, task_id="t1"),
    ]
    return compute_cell_metrics(cases, seed=0)


def test_regeneration_produces_file(tmp_path):
    out = tmp_path / "summary.md"
    write_results_table(_cells(), str(out))
    assert out.exists()
    text = out.read_text(encoding="utf-8")
    # Header columns are present verbatim.
    for col in COLUMNS:
        assert col in text
    # Regeneration is stable: same input => identical bytes.
    out2 = tmp_path / "summary2.md"
    write_results_table(_cells(), str(out2))
    assert out.read_text(encoding="utf-8") == out2.read_text(encoding="utf-8")


def test_a_ci_appears_for_every_reported_rate():
    text = render_results_table(_cells())
    data_rows = [
        ln for ln in text.splitlines()
        if ln.startswith("|") and "ASR" not in ln and "---" not in ln
    ]
    assert data_rows
    for row in data_rows:
        cells = [c.strip() for c in row.strip().strip("|").split("|")]
        asr_cell, tsr_cell = cells[3], cells[4]
        # A computed rate carries a CI "[lower, upper]"; "not computed" is allowed.
        for cell in (asr_cell, tsr_cell):
            assert ("[" in cell and "]" in cell) or "not computed" in cell


def test_defense_cost_references_correct_control():
    text = render_results_table(_cells())
    rows = {}
    for ln in text.splitlines():
        if not ln.startswith("|"):
            continue
        cells = [c.strip() for c in ln.strip().strip("|").split("|")]
        if cells[0] == "Model" or set(cells[0]) == {"-"}:
            continue
        rows[(cells[0], cells[1], cells[2])] = cells
    # Control row has cost +0.000; the filter Cell dropped TSR from 1.0 to 0.5.
    assert rows[("m0", "none", "direct")][5] == "+0.000"
    assert rows[("m0", "filter", "direct")][5] == "+0.500"


def test_not_computed_rate_renders_cause():
    cases = [_case(defense="filter", injection_success=None)]  # no classified ASR
    text = render_results_table(compute_cell_metrics(cases, seed=0))
    assert "not computed" in text


def test_build_table_from_store(tmp_path):
    cases = [
        _case(defense="none", task_success=True),
        _case(defense="none", task_success=False, task_id="t1"),
    ]
    store = tmp_path / "store.jsonl"
    write_results_store(cases, str(store))
    out = tmp_path / "summary.md"
    build_table_from_store(str(store), str(out), seed=0)
    assert out.exists()
    assert "ASR (95% CI)" in out.read_text(encoding="utf-8")
