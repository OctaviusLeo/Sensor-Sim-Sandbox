"""Plot generators (design.md 'Report and plots generator'; Req 7.3).

:func:`plot_asr_by_defense` and :func:`plot_defense_cost` each READ the generated
Results_Table (``results/summary.md``) and emit a figure file. Both show the 95%
Confidence_Intervals (as error bars) so no number is hand-entered - the plots are
generated artifacts derived only from the generated table (Req 7.2, 7.3).

matplotlib runs under the non-interactive **Agg** backend (set before pyplot is
imported) so figures are written to files with no display and never hand-edited.

The parser here reads the exact table emitted by :mod:`aib.results_table`: a
Markdown table whose header is :data:`aib.results_table.COLUMNS`, rates rendered
as ``point [lower, upper]``, and Defense_Cost rendered as a signed fixed-decimal
value (``n/a`` where no control TSR exists).
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Sequence

import matplotlib

matplotlib.use("Agg")  # non-interactive backend; must precede pyplot import.

import matplotlib.pyplot as plt  # noqa: E402  (after backend selection)

#: Matches a rate cell ``point [lower, upper]`` emitted by the Results_Table.
_RATE_RE = re.compile(
    r"^\s*([0-9.]+)\s*\[\s*([0-9.]+)\s*,\s*([0-9.]+)\s*\]\s*$"
)
#: Matches a signed Defense_Cost cell such as ``+0.000`` or ``-0.125``.
_COST_RE = re.compile(r"^\s*([+-][0-9.]+)\s*$")


@dataclass(frozen=True)
class TableRow:
    """One parsed Results_Table row (only the fields the plots need)."""

    model: str
    defense: str
    style: str
    asr: tuple[float, float, float] | None  # (point, lower, upper)
    tsr: tuple[float, float, float] | None
    defense_cost: float | None


def _parse_rate(cell: str) -> tuple[float, float, float] | None:
    match = _RATE_RE.match(cell)
    if not match:
        return None
    point, lower, upper = (float(match.group(i)) for i in (1, 2, 3))
    return point, lower, upper


def _parse_cost(cell: str) -> float | None:
    match = _COST_RE.match(cell)
    if not match:
        return None
    return float(match.group(1))


def parse_results_table(table_path: str) -> list[TableRow]:
    """Parse a ``results/summary.md`` into :class:`TableRow` records.

    Only well-formed data rows (a leading ``|`` with the expected column count)
    are read; the header, separator, title, and prose lines are skipped.
    """
    rows: list[TableRow] = []
    with open(table_path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line.startswith("|"):
                continue
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if len(cells) < 6:
                continue
            if cells[0] in ("Model", "---") or set(cells[0]) == {"-"}:
                continue
            rows.append(
                TableRow(
                    model=cells[0],
                    defense=cells[1],
                    style=cells[2],
                    asr=_parse_rate(cells[3]),
                    tsr=_parse_rate(cells[4]),
                    defense_cost=_parse_cost(cells[5]),
                )
            )
    return rows


def _asymmetric_errors(
    rates: Sequence[tuple[float, float, float]]
) -> list[list[float]]:
    """Build matplotlib ``yerr`` [lower_err, upper_err] from (point, lo, hi) tuples."""
    lower_err = [max(0.0, point - lo) for point, lo, _ in rates]
    upper_err = [max(0.0, hi - point) for point, _, hi in rates]
    return [lower_err, upper_err]


def plot_asr_by_defense(table_path: str, out_path: str) -> None:
    """Bar chart of Attack_Success_Rate per Cell with 95% CI error bars (Req 7.3).

    Reads ``results/summary.md`` and writes a figure to ``out_path``. Only Cells
    whose ASR is computed (not "not computed") are plotted.
    """
    rows = [r for r in parse_results_table(table_path) if r.asr is not None]
    labels = [f"{r.model}\n{r.defense}/{r.style}" for r in rows]
    points = [r.asr[0] for r in rows]
    yerr = _asymmetric_errors([r.asr for r in rows]) if rows else [[], []]

    fig, ax = plt.subplots(figsize=(max(6.0, 1.2 * max(len(rows), 1)), 4.0))
    x = range(len(rows))
    ax.bar(x, points, yerr=yerr, capsize=4, color="#b23a48")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=45, ha="right", fontsize=7)
    ax.set_ylim(0.0, 1.0)
    ax.set_ylabel("Attack_Success_Rate")
    ax.set_title("Attack_Success_Rate by Defense (95% CI)")
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


def plot_defense_cost(table_path: str, out_path: str) -> None:
    """Bar chart of Defense_Cost per Cell relative to No_Defense (Req 7.3).

    Reads ``results/summary.md`` and writes a figure to ``out_path``. Cells whose
    Defense_Cost is ``n/a`` (no control TSR) are skipped.
    """
    rows = [r for r in parse_results_table(table_path) if r.defense_cost is not None]
    labels = [f"{r.model}\n{r.defense}/{r.style}" for r in rows]
    costs = [r.defense_cost for r in rows]

    fig, ax = plt.subplots(figsize=(max(6.0, 1.2 * max(len(rows), 1)), 4.0))
    x = range(len(rows))
    ax.bar(x, costs, color="#3a6ea5")
    ax.axhline(0.0, color="black", linewidth=0.8)
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=45, ha="right", fontsize=7)
    ax.set_ylabel("Defense_Cost (TSR drop vs No_Defense)")
    ax.set_title("Task-success cost of each Defense")
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
