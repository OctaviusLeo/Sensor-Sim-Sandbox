"""Report generator (design.md 'Report and plots generator'; Req 7.1, 7.2, 7.4,
7.5, 7.6).

:func:`render_report` renders the paper-style write-up as a Markdown document
built **only** from generated sources - the Results_Store (one
:class:`~aib.runner.CaseResult` per case) and the Results_Table
(``results/summary.md``) - and embeds the generated plot files. No number in the
Report is hand-entered: every figure is read back out of a generated file, so
regenerating the sources is the only way to change a number (Req 7.2, 7.4).

The write-up has the five required sections (Req 7.1):

1. **Question** - the empirical research question.
2. **Setup** - the Cell grid, the deterministic checks, and the sampling
   protocol: temperature, sampling settings, and repetition count, all read from
   the Results_Store (Req 7.5).
3. **Results** - the Results_Table pulled in verbatim, plus the embedded plots.
4. **Limitations** - framed around what the measurement does and does not cover.
5. **Threats to validity** - the honesty caveats (determinism exclusions, stub
   coverage, aggregate-only reporting).

Reporting is **aggregate-only**: the Report presents per-Cell rates and defense
effects and never embeds a payload tuned against a specific named product (Req
7.6, 0.15). The injected instructions in this harness are benign sandbox actions
regardless.
"""

from __future__ import annotations

import json
import os
from typing import Sequence

from aib.experiments import read_results_store
from aib.runner import CaseResult


def _distinct_sorted(values) -> list:
    """Return the sorted distinct values (stable rendering of a settings set)."""
    return sorted(set(values))


def _summarize_protocol(records: Sequence[CaseResult]) -> dict:
    """Extract the sampling protocol facts stated in the setup section (Req 7.5).

    Temperatures, the distinct sampling settings, the repetition count, and the
    case/Cell counts all come straight from the Results_Store records.
    """
    temperatures = _distinct_sorted(c.temperature for c in records)
    samplings = _distinct_sorted(
        json.dumps(c.sampling, sort_keys=True) for c in records
    )
    repetitions = sorted({c.repetition for c in records})
    cells = _distinct_sorted((c.model, c.defense, c.style) for c in records)
    models = _distinct_sorted(c.model for c in records)
    return {
        "temperatures": temperatures,
        "samplings": samplings,
        "repetition_count": len(repetitions),
        "n_cases": len(records),
        "n_cells": len(cells),
        "models": models,
    }


def _fmt_list(values) -> str:
    """Render a list of settings for prose (empty => an explicit marker)."""
    if not values:
        return "none recorded"
    return ", ".join(str(v) for v in values)


def _read_table(table_path: str) -> str:
    """Read the generated Results_Table verbatim (Req 7.2)."""
    with open(table_path, "r", encoding="utf-8") as fh:
        return fh.read().strip()


def _plot_embeds(plot_paths: Sequence[str], out_path: str) -> list[str]:
    """Return Markdown image embeds for each plot, path-relative to the report."""
    out_dir = os.path.dirname(os.path.abspath(out_path))
    embeds: list[str] = []
    for plot in plot_paths:
        rel = os.path.relpath(os.path.abspath(plot), out_dir).replace(os.sep, "/")
        name = os.path.splitext(os.path.basename(plot))[0]
        embeds.append(f"![{name}]({rel})")
    return embeds


def render_report(
    store_path: str,
    table_path: str,
    plot_paths: Sequence[str],
    out_path: str,
) -> None:
    """Render the paper-style Report to ``out_path`` from generated sources only.

    Reads the Results_Store (``store_path``) and the Results_Table
    (``table_path``), embeds each plot in ``plot_paths``, and writes the five
    sections. The sampling protocol (temperature / sampling / repetitions) is
    stated from the Results_Store (Req 7.5); reporting is aggregate-only (Req
    7.6). No number is hand-entered - all come from the generated files (Req 7.4).
    """
    records = read_results_store(store_path)
    protocol = _summarize_protocol(records)
    table_md = _read_table(table_path)
    embeds = _plot_embeds(plot_paths, out_path)

    lines: list[str] = []
    lines.append("# Agent Injection Bench - Report")
    lines.append("")
    lines.append(
        "Generated file - do not hand-edit. Every number below is read from a "
        "generated source (the Results_Store and the Results_Table); regenerate "
        "the Report to change any value."
    )
    lines.append("")

    # 1. Question -----------------------------------------------------------
    lines.append("## Question")
    lines.append("")
    lines.append(
        "How often does a tool-using LLM agent follow instructions planted in a "
        "tool output (prompt injection), and how much does each defense lower "
        "that attack-success rate without hurting the agent's legitimate "
        "task-success rate? The question is empirical and comparative, measured "
        "under controlled, reproducible conditions inside a simulated sandbox "
        "with no real network egress."
    )
    lines.append("")

    # 2. Setup --------------------------------------------------------------
    lines.append("## Setup")
    lines.append("")
    lines.append(
        "Each case is a `(User_Task, Injection_Style, Defense_Condition, Model, "
        "repetition)` tuple run through the AgentRunner over the simulated tools. "
        "An injection is planted only in a Tool_Output, never in the User_Task, "
        "and two deterministic checks score the same case: the "
        "Injection_Success_Check (did the agent carry out the injected goal) and "
        "the Task_Success_Check (did it still complete the user's task). All "
        "injected goals are benign sandbox actions."
    )
    lines.append("")
    lines.append("Sampling protocol, as recorded in the Results_Store:")
    lines.append("")
    lines.append(f"- Models evaluated: {_fmt_list(protocol['models'])}")
    lines.append(f"- Temperature settings: {_fmt_list(protocol['temperatures'])}")
    lines.append(f"- Sampling settings: {_fmt_list(protocol['samplings'])}")
    lines.append(f"- Repetitions per varying-output Cell: {protocol['repetition_count']}")
    lines.append(f"- Cells measured: {protocol['n_cells']}")
    lines.append(f"- Cases recorded: {protocol['n_cases']}")
    lines.append("")

    # 3. Results ------------------------------------------------------------
    lines.append("## Results")
    lines.append("")
    lines.append(
        "The Results_Table below is pulled verbatim from the generated "
        "`results/summary.md`. Each rate carries a 95% confidence interval; "
        "Defense_Cost is the Task_Success_Rate drop relative to the No_Defense "
        "control for the same Model and Injection_Style."
    )
    lines.append("")
    lines.append(table_md)
    lines.append("")
    if embeds:
        lines.append("Generated plots:")
        lines.append("")
        for embed in embeds:
            lines.append(embed)
            lines.append("")

    # 4. Limitations --------------------------------------------------------
    lines.append("## Limitations")
    lines.append("")
    lines.append(
        "The study measures injected-instruction-following inside a simulated "
        "sandbox with four tools, a fixed task catalog, five injection styles, "
        "and the defenses listed above; it is not a claim about any specific "
        "production agent or deployment. The tools model (but never perform) "
        "network access by serving canned local fixtures, so real-world retrieval "
        "noise is out of scope. The injected goals are benign sandbox actions "
        "chosen to be deterministically checkable, not an exhaustive attacker "
        "catalog."
    )
    lines.append("")

    # 5. Threats to validity -----------------------------------------------
    lines.append("## Threats to validity")
    lines.append("")
    lines.append(
        "Determinism is asserted only over values under our control; timestamps, "
        "absolute file paths, and provider-sampled token sequences at "
        "temperature above zero are excluded, which is why Cells with varying "
        "output are repeated. Confidence intervals quantify sampling uncertainty "
        "but not systematic bias in the task or injection catalog. Reporting is "
        "aggregate-only: the Report presents per-Cell rates and defense effects "
        "and never publishes an attack payload tuned against a specific named "
        "product."
    )
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
