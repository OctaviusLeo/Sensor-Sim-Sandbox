"""Unit tests for the Report generator (aib.report; Req 7.1, 7.2, 7.4, 7.5, 7.6).

Asserts the five sections are present, every number comes from the generated
files, and the sampling protocol (temperature / sampling / repetitions) is
stated.
"""

from __future__ import annotations

import os

from aib.defenses import DefenseConfig, DefenseKind
from aib.experiments import run_experiments, write_results_store
from aib.goals import load_injected_goals
from aib.injections import load_injection_styles
from aib.plots import plot_asr_by_defense, plot_defense_cost
from aib.report import render_report
from aib.results_table import build_table_from_store
from aib.runner import AgentRunner
from aib.stub_model import StubModel
from aib.tasks import load_user_tasks


def _generate_sources(tmp_path):
    """Produce a Results_Store + Results_Table + plots from a small Stub run."""
    tasks = load_user_tasks()[:2]
    styles = load_injection_styles()[:1]
    defenses = [
        DefenseConfig(kind=DefenseKind.NONE),
        DefenseConfig(kind=DefenseKind.DELIMIT),
    ]
    records = run_experiments(
        models=[StubModel()],
        defenses=defenses,
        styles=styles,
        tasks=tasks,
        repetitions=2,
        seed=7,
        spend_cap_usd=None,
        goals=load_injected_goals()[:1],
        runner=AgentRunner(),
    )
    store = os.path.join(tmp_path, "store.jsonl")
    table = os.path.join(tmp_path, "summary.md")
    write_results_store(records, store)
    build_table_from_store(store, table, seed=7)

    asr_plot = os.path.join(tmp_path, "asr.png")
    cost_plot = os.path.join(tmp_path, "cost.png")
    plot_asr_by_defense(table, asr_plot)
    plot_defense_cost(table, cost_plot)
    return store, table, [asr_plot, cost_plot]


def test_report_has_all_five_sections(tmp_path):
    store, table, plots = _generate_sources(str(tmp_path))
    out = os.path.join(str(tmp_path), "report.md")
    render_report(store, table, plots, out)

    text = open(out, encoding="utf-8").read()
    for heading in (
        "## Question",
        "## Setup",
        "## Results",
        "## Limitations",
        "## Threats to validity",
    ):
        assert heading in text


def test_report_states_temperature_sampling_repetitions(tmp_path):
    store, table, plots = _generate_sources(str(tmp_path))
    out = os.path.join(str(tmp_path), "report.md")
    render_report(store, table, plots, out)
    text = open(out, encoding="utf-8").read()

    assert "Temperature settings:" in text
    assert "Sampling settings:" in text
    assert "Repetitions per varying-output Cell: 2" in text


def test_report_numbers_come_from_generated_files(tmp_path):
    store, table, plots = _generate_sources(str(tmp_path))
    out = os.path.join(str(tmp_path), "report.md")
    render_report(store, table, plots, out)
    text = open(out, encoding="utf-8").read()

    # The Results_Table is embedded verbatim, so its content appears in the Report.
    table_md = open(table, encoding="utf-8").read().strip()
    assert table_md in text

    # The plots are embedded as Markdown image references.
    assert "![asr]" in text
    assert "![cost]" in text


def test_report_embeds_without_plots(tmp_path):
    store, table, _ = _generate_sources(str(tmp_path))
    out = os.path.join(str(tmp_path), "report.md")
    render_report(store, table, [], out)
    text = open(out, encoding="utf-8").read()
    assert "## Results" in text
    assert "Generated plots:" not in text
