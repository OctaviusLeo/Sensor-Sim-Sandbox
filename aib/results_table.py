"""Results_Table writer (design.md 'Report and plots generator'; Req 6, 7.2).

:func:`write_results_table` renders ``results/summary.md`` - the single generated
source for every reported rate. It has one row per Cell with:

* Attack_Success_Rate with its 95% CI,
* Task_Success_Rate with its 95% CI,
* Defense_Cost relative to the No_Defense control for the same Model and
  Injection_Style,
* Filter_Classifier precision/recall where the Filter ran,
* the repetition count, and
* the temperature / sampling settings used.

The file is a GENERATED artifact: no value is editable except by regenerating it
(Req 7.2). To keep FEAT-006's traceability check and the README able to match the
numbers exactly, the formatting is stable and documented in :data:`RATE_DECIMALS`
and the ``_fmt_*`` helpers:

* rates and CI bounds: fixed 3 decimals (e.g. ``0.500``),
* a rate with a CI renders as ``point [lower, upper]`` (e.g. ``0.500 [0.187, 0.813]``),
* a "not computed" rate renders its stable cause text,
* Defense_Cost: signed, fixed 3 decimals (e.g. ``+0.000`` / ``-0.125``); ``n/a``
  when a control TSR is unavailable,
* precision/recall: fixed 3 decimals, or ``-`` where the Filter did not run.

:func:`write_results_table` accepts the per-Cell blocks produced by
:func:`aib.metrics.compute_cell_metrics` (keyed by the ``(model, defense, style)``
tuple or by :class:`aib.experiments.Cell`); :func:`build_table_from_store` is the
one-call path from a Results_Store on disk.
"""

from __future__ import annotations

from typing import Mapping

from aib.metrics import CellRate, compute_cell_metrics

#: Fixed decimal places for every rate, CI bound, cost, and P/R value.
RATE_DECIMALS: int = 3

#: The exact column header row written to ``results/summary.md`` (FEAT-006 and
#: the README match this verbatim).
COLUMNS: tuple[str, ...] = (
    "Model",
    "Defense",
    "Injection_Style",
    "ASR (95% CI)",
    "TSR (95% CI)",
    "Defense_Cost",
    "Filter_P",
    "Filter_R",
    "Repetitions",
    "Temperature",
    "Sampling",
)


def _cell_key(cell) -> tuple[str, str, str]:
    """Normalize a dict key that may be a tuple or an :class:`aib.experiments.Cell`."""
    if isinstance(cell, tuple):
        return (str(cell[0]), str(cell[1]), str(cell[2]))
    return (str(cell.model), str(cell.defense), str(cell.style))


def _fmt_num(x: float) -> str:
    """Format a plain float to the stable fixed-decimal rate form."""
    return f"{x:.{RATE_DECIMALS}f}"


def _fmt_rate(rate: CellRate) -> str:
    """Render a rate as ``point [lower, upper]`` or its stable "not computed" cause."""
    if rate.ci is None:
        return rate.cause or "not computed"
    ci = rate.ci
    return f"{_fmt_num(ci.point)} [{_fmt_num(ci.lower)}, {_fmt_num(ci.upper)}]"


def _fmt_cost(cost) -> str:
    """Render Defense_Cost as a signed fixed-decimal value, or ``n/a``."""
    if cost is None:
        return "n/a"
    return f"{cost:+.{RATE_DECIMALS}f}"


def _fmt_filter(block: dict, which: str) -> str:
    """Render a filter precision/recall value, or ``-`` when the Filter did not run."""
    flt = block.get("filter")
    if not flt:
        return "-"
    return _fmt_num(flt[which])


def _fmt_temperature(block: dict) -> str:
    return ", ".join(_fmt_num(t) for t in block.get("temperatures", []))


def _fmt_sampling(block: dict) -> str:
    # Join the distinct sampling JSON strings; escape pipes so the Markdown table
    # cell stays well-formed.
    return "; ".join(s.replace("|", "\\|") for s in block.get("samplings", []))


def _row(block: dict) -> list[str]:
    return [
        block["model"],
        block["defense"],
        block["style"],
        _fmt_rate(block["asr"]),
        _fmt_rate(block["tsr"]),
        _fmt_cost(block["defense_cost"]),
        _fmt_filter(block, "precision"),
        _fmt_filter(block, "recall"),
        str(block["repetitions"]),
        _fmt_temperature(block),
        _fmt_sampling(block),
    ]


def render_results_table(cells: Mapping) -> str:
    """Return the Markdown text of the Results_Table for ``cells``.

    ``cells`` maps a ``(model, defense, style)`` tuple (or :class:`Cell`) to the
    per-Cell block produced by :func:`aib.metrics.compute_cell_metrics`.
    """
    ordered = sorted(cells.items(), key=lambda kv: _cell_key(kv[0]))
    lines: list[str] = []
    lines.append("# Results_Table")
    lines.append("")
    lines.append(
        "Generated file - do not hand-edit. Regenerate with "
        "`python -m aib.metrics` + the Results_Table writer."
    )
    lines.append("")
    lines.append("| " + " | ".join(COLUMNS) + " |")
    lines.append("| " + " | ".join("---" for _ in COLUMNS) + " |")
    for _, block in ordered:
        lines.append("| " + " | ".join(_row(block)) + " |")
    lines.append("")
    return "\n".join(lines)


def write_results_table(cells: Mapping, path: str) -> None:
    """Write ``results/summary.md`` for ``cells`` (Req 7.2); regeneration only."""
    text = render_results_table(cells)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def build_table_from_store(
    store_path: str, out_path: str, *, seed: int = 0, method: str = "wilson"
) -> None:
    """Read a Results_Store, compute per-Cell metrics, and write the table.

    One-call convenience path: ``Results_Store -> compute_cell_metrics ->
    write_results_table``. Deterministic under ``seed`` (Req 6.6).
    """
    from aib.experiments import read_results_store

    records = read_results_store(store_path)
    blocks = compute_cell_metrics(records, seed=seed, method=method)
    write_results_table(blocks, out_path)
