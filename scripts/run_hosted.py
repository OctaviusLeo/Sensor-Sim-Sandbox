"""Hosted-model benchmark sweep within the Spend_Cap (task 5.8; Req 5.2, 0.1, 0.13).

SHIPPED but NOT executed here: it needs a Hosted_Model API key, which is not
available on the build box. It runs one or two Hosted_Models through the
OpenAI-compatible client, tracking estimated spend so it STOPS issuing requests
BEFORE the owner-set Spend_Cap would be exceeded (Req 5.2), appends the resulting
case records to the Results_Store, and stamps the run with its generating command,
seed, and full 40-character commit hash via :mod:`aib.repro` (Req 0.1).

The API key is read ONLY from an environment variable and the client fails fast
(before any request) if that variable is unset or empty (Req 0.9, 0.13).

Run (on a box with the key exported):

    export OPENAI_API_KEY=sk-...
    python scripts/run_hosted.py --base-url https://api.openai.com/v1 \
        --models gpt-4o-mini --api-key-env OPENAI_API_KEY \
        --spend-cap-usd 5.00 --cost-per-case-usd 0.002 \
        --repetitions 3 --seed 1234 --results results/hosted_store.jsonl

The pure ``build_hosted_models`` / ``make_cost_estimator`` / ``stamp_path`` helpers
are importable so the script's logic is testable without the network or a key;
only ``main`` issues requests (through the OpenAI-compatible client).
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import Sequence

from aib.defenses import default_defenses
from aib.experiments import CostEstimator, run_experiments, write_results_store
from aib.goals import load_injected_goals
from aib.injections import INJECTION_STYLES
from aib.model_client import ModelClient, OpenAICompatibleClient
from aib.repro import serialize, stamp
from aib.tasks import load_user_tasks

#: Default sampling temperature for the hosted sweep (owner-tunable at M4).
DEFAULT_TEMPERATURE: float = 0.0

#: Default repetition count; >=3 so varying-output Cells are repeated (Req 5.4).
DEFAULT_REPETITIONS: int = 3

# [ME-REFINE]: the owner sets the real Spend_Cap (USD) and the per-case cost
# estimate at M4; these are benign placeholders so the pipeline is runnable.
DEFAULT_SPEND_CAP_USD: float = 5.0
DEFAULT_COST_PER_CASE_USD: float = 0.002


def build_hosted_models(
    model_names: Sequence[str],
    base_url: str,
    api_key_env: str,
) -> list[ModelClient]:
    """Build an OpenAI-compatible client per Hosted_Model name (Req 5.2, 0.13).

    ``required_key=True`` so the client fails fast (naming the missing var) before
    any request if ``api_key_env`` is unset or empty.
    """
    return [
        OpenAICompatibleClient(
            model=name,
            base_url=base_url,
            api_key_env=api_key_env,
            required_key=True,
        )
        for name in model_names
    ]


def make_cost_estimator(cost_per_case_usd: float) -> CostEstimator:
    """Return a per-case cost estimator (flat per-case estimate for the budget).

    The estimate is intentionally simple and model-agnostic; the owner can refine
    it to a token-based estimate at M4 without changing the sweep interface.
    """

    def _estimate(_model: ModelClient) -> float:
        return cost_per_case_usd

    return _estimate


def stamp_path(results_path: str) -> str:
    """Return the sidecar path that holds the run's provenance Stamp."""
    return f"{results_path}.stamp.json"


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run one or two Hosted_Models within the Spend_Cap."
    )
    parser.add_argument(
        "--base-url",
        default="https://api.openai.com/v1",
        help="OpenAI-compatible Hosted_Model base URL.",
    )
    parser.add_argument(
        "--models",
        nargs="+",
        required=True,
        help="One or two Hosted_Model names (Req 5.2).",
    )
    parser.add_argument(
        "--api-key-env",
        default="OPENAI_API_KEY",
        help="Env var holding the Hosted_Model API key (read-only, Req 0.13).",
    )
    parser.add_argument(
        "--spend-cap-usd", type=float, default=DEFAULT_SPEND_CAP_USD,
        help="Owner-set Spend_Cap; requests stop before it is exceeded (Req 5.2).",
    )
    parser.add_argument(
        "--cost-per-case-usd", type=float, default=DEFAULT_COST_PER_CASE_USD,
        help="Estimated USD cost per case used for spend tracking.",
    )
    parser.add_argument(
        "--repetitions", type=int, default=DEFAULT_REPETITIONS,
        help="Repetitions per Cell (>=3 for varying output, Req 5.4).",
    )
    parser.add_argument("--seed", type=int, default=1234, help="Base RNG seed (Req 0.2).")
    parser.add_argument(
        "--temperature", type=float, default=DEFAULT_TEMPERATURE,
        help="Sampling temperature (recorded per case, Req 5.3).",
    )
    parser.add_argument(
        "--results",
        default=os.path.join("results", "hosted_store.jsonl"),
        help="Results_Store output path (JSON Lines).",
    )
    return parser.parse_args(list(argv))


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    if not 1 <= len(args.models) <= 2:
        print("error: provide one or two Hosted_Models (Req 5.2)", file=sys.stderr)
        return 2

    models = build_hosted_models(args.models, args.base_url, args.api_key_env)

    stats: dict = {}
    results = run_experiments(
        models=models,
        defenses=default_defenses(),
        styles=INJECTION_STYLES,
        tasks=load_user_tasks(),
        repetitions=args.repetitions,
        seed=args.seed,
        spend_cap_usd=args.spend_cap_usd,
        goals=load_injected_goals(),
        temperature=args.temperature,
        cost_estimator=make_cost_estimator(args.cost_per_case_usd),
        stats=stats,
    )

    os.makedirs(os.path.dirname(args.results) or ".", exist_ok=True)
    # Append to the Results_Store (hosted runs add to local results, Req 5.2).
    with open(args.results, "a", encoding="utf-8") as fh:
        from aib.experiments import _case_to_dict  # stable JSON-Lines record form
        import json

        for case in results:
            fh.write(json.dumps(_case_to_dict(case), sort_keys=True))
            fh.write("\n")

    command = "python scripts/run_hosted.py " + " ".join(sys.argv[1:])
    provenance = stamp(command=command, seed=args.seed)
    with open(stamp_path(args.results), "w", encoding="utf-8") as fh:
        fh.write(serialize(provenance))

    print(
        f"wrote {len(results)} case records to {args.results}; "
        f"completed {stats.get('completed_cells', 0)} Cells; "
        f"estimated spend ${stats.get('estimated_spend_usd', 0.0):.4f} "
        f"(cap ${args.spend_cap_usd:.2f}; stopped={stats.get('stopped', False)})"
    )
    return 0


if __name__ == "__main__":  # pragma: no cover - shipped, not executed here
    raise SystemExit(main())
