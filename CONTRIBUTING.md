# Contributing

## Checks to run before a commit

The two checks GitHub Actions runs on every push and pull request
(`.github/workflows/ci-sensor-sim-sandbox.yml`), from the repository root on Windows
PowerShell after the Quickstart's `py -3.12 -m venv .venv`:

```powershell
.\.venv\Scripts\python -m pip install -r analysis\requirements.txt
.\.venv\Scripts\python -m pytest analysis/tests -q
.\.venv\Scripts\python -m pip install ruff==0.17.3   # the version CI pins
.\.venv\Scripts\ruff check analysis                   # rules in analysis/ruff.toml
```

On Linux or macOS use `.venv/bin/python` and `.venv/bin/ruff`. The Unreal Automation tests
need a local UE 5.8 install: `pwsh -NoProfile -File scripts\run_ue_tests.ps1` (see "Test
commands" in the README).

## Results are generated

Never edit a file under `results/` by hand. `results/summary.md` and the plots are written by
`analysis/analyze.py` from a run log, and `results/smoke_result.json` by `scripts/smoke_test.ps1`;
each records the command and commit that produced it, and the summary its seed. A changed result comes from
re-running the script on a clean commit. Raw run data stays under `runs/`, which is not committed.

## Bumping a Python dependency

`analysis/requirements.txt` holds the exact pins, and `THIRD_PARTY.md` and `docs/ENVIRONMENT.md`
must list the same versions. For each bump (Dependabot opens them):

1. Check on PyPI that the package's license is unchanged, and update its row in `THIRD_PARTY.md`.
2. Install the new pins into `.venv` and record them with `.\.venv\Scripts\python -m pip freeze`;
   that output is the evidence cited in the package rows of `docs/ENVIRONMENT.md`, so update
   those rows to match.
3. Run the pytest and ruff checks above, then commit the three files together.

## Branches

`master` is the default branch and its CI stays green. Dependabot bumps are merged with a merge
commit once their checks pass.
