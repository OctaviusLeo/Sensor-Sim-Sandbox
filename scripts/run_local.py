"""Full local-model benchmark sweep (task 4.8; Req 5.1, 5.4, 5.5, 0.1).

SHIPPED but NOT executed here: it needs the two Local_Models served on the
RTX 3080 through an OpenAI-compatible Model_Server, which is unavailable on the
build box (no GPU/Docker). It sweeps the Cell grid over the User_Tasks with the
configured repetitions and writes the Results_Store, stamped with the generating
command, seed, and full 40-character commit hash via :mod:`aib.repro` (Req 0.1).

Run (on a box with the Model_Server up):

    python scripts/run_local.py --base-url http://localhost:11434/v1 \
        --models llama3.2:3b qwen2.5:7b --repetitions 3 --seed 1234 \
        --results results/local_store.jsonl

The pure ``build_local_models`` / ``stamp_path`` helpers are importable so the
script's logic can be exercised without a live server; only ``main`` touches the
network (through the OpenAI-compatible client).
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import Sequence

from aib.defenses import default_defenses
from aib.experiments import run_experiments, write_results_store
from aib.goals import load_injected_goals
from aib.injections import INJECTION_STYLES
from aib.model_client import ModelClient, OpenAICompatibleClient
from aib.repro import serialize, stamp
from aib.tasks import load_user_tasks

#: Default sampling temperature for the local sweep (owner-tunable at M4).
DEFAULT_TEMPERATURE: float = 0.0

#: Default repetition count; >=3 so varying-output Cells are repeated (Req 5.4).
DEFAULT_REPETITIONS: int = 3


def build_local_models(
    model_names: Sequence[str],
    base_url: str,
    api_key_env: str | None,
) -> list[ModelClient]:
    """Build an OpenAI-compatible client per Local_Model name (Req 5.1).

    A local Model_Server usually needs no real key, so ``required_key`` is False;
    pass ``api_key_env`` only if the chosen server enforces one.
    """
    return [
        OpenAICompatibleClient(
            model=name,
            base_url=base_url,
            api_key_env=api_key_env,
            required_key=api_key_env is not None,
        )
        for name in model_names
    ]


def stamp_path(results_path: str) -> str:
    """Return the sidecar path that holds the run's provenance Stamp."""
    return f"{results_path}.stamp.json"


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the local-model benchmark sweep.")
    parser.add_argument(
        "--base-url",
        default="http://localhost:11434/v1",
        help="OpenAI-compatible Model_Server base URL.",
    )
    parser.add_argument(
        "--models",
        nargs="+",
        required=True,
        help="Two or more Local_Model names served by the Model_Server (Req 5.1).",
    )
    parser.add_argument(
        "--api-key-env",
        default=None,
        help="Env var holding the server API key, if the server requires one.",
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
        default=os.path.join("results", "local_store.jsonl"),
        help="Results_Store output path (JSON Lines).",
    )
    return parser.parse_args(list(argv))


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    if len(args.models) < 2:
        print("error: at least two Local_Models are required (Req 5.1)", file=sys.stderr)
        return 2

    models = build_local_models(args.models, args.base_url, args.api_key_env)
    results = run_experiments(
        models=models,
        defenses=default_defenses(),
        styles=INJECTION_STYLES,
        tasks=load_user_tasks(),
        repetitions=args.repetitions,
        seed=args.seed,
        spend_cap_usd=None,  # local models incur no hosted spend
        goals=load_injected_goals(),
        temperature=args.temperature,
    )

    os.makedirs(os.path.dirname(args.results) or ".", exist_ok=True)
    write_results_store(results, args.results)

    command = "python scripts/run_local.py " + " ".join(sys.argv[1:])
    provenance = stamp(command=command, seed=args.seed)
    with open(stamp_path(args.results), "w", encoding="utf-8") as fh:
        fh.write(serialize(provenance))

    print(f"wrote {len(results)} case records to {args.results}")
    return 0


if __name__ == "__main__":  # pragma: no cover - shipped, not executed here
    raise SystemExit(main())
