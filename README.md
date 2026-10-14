# Agent Injection Bench

A private portfolio research harness that measures, under controlled and
reproducible conditions, how often a tool-using LLM agent follows instructions
planted in a tool output (prompt injection), and how much each defense lowers
that attack-success rate without hurting the agent's legitimate task-success
rate. Everything runs inside a simulated sandbox with no real network egress:
every tool reads and writes only local, seeded fixture state.

Every reported number is a Measured_Value produced by a repository script and
recorded with its generating command, seed, and full commit hash. The
Results_Table, the Report, and the plots are generated files; a traceability
check fails the build if any number in this README or the Report cannot be
traced back to a generated source.

> This section of the README is assembled from generated sources. The owner
> prose for known limitations and safety-and-scope is left as marked
> placeholders below and is authored during review, not generated.

## Architecture

```text
case = (User_Task, Injection_Style, Defense_Condition, Model, repetition)
  -> Sandbox.init(seed)                      deterministic fixture state
  -> AgentRunner.run(task, tools, defense)   OpenAI-compatible tool loop
       - Simulated_Tool returns Tool_Output with the Injection planted
       - Defense applied: none | delimit | instruction-hierarchy | filter | tool-limit
  -> record: Injection_Success_Check(final_state, tool_trace) -> bool | unclassified
             Task_Success_Check(final_state, response)        -> bool
  -> Results_Store (one record per case)
      -> Metrics: ASR, TSR, Defense_Cost, filter P/R + Wilson/bootstrap CIs
         -> Results_Table (generated) -> Report + plots (generated)
```

The sandbox is the trust boundary: a tool that *models* network access serves a
canned local fixture and never opens a socket. An injection is planted only in a
Tool_Output, never in the User_Task, and the two deterministic checks score the
same case so attack success and task success are jointly observed. The AgentRunner
is model-agnostic: local models, hosted models, and the deterministic Stub_Model
all present the same OpenAI-compatible tool-calling interface.

## Install

Run from the repository root:

```bash
python -m pip install -e ".[test]"
```

## Run the tests

The single command that runs every unit and property test suite with coverage:

```bash
python -m pytest --cov=aib --cov-report=term-missing
```

The single command that runs the GPU-free, API-key-free CI smoke subset (driven
by the deterministic Stub_Model, with fixed assertable outcomes):

```bash
python scripts/ci_smoke.py
```

The single command that checks every number in this README and the Report traces
to a generated source:

```bash
python scripts/check_traceability.py
```

## Reproduce a result

The metrics layer is invocable from one non-interactive command; it reads the
Results_Store and prints the per-Cell metrics stamped with its command, seed, and
commit hash:

```bash
python -m aib.metrics --results results/store.jsonl --seed 0
```

Regenerate the Results_Table (`results/summary.md`) from a Results_Store:

```bash
python -c "from aib.results_table import build_table_from_store as b; b('results/store.jsonl', 'results/summary.md', seed=0)"
```

## Run the full local-model benchmark

The full benchmark sweeps the Cell grid on the local models through the
Model_Server. It requires a GPU and a running OpenAI-compatible Model_Server, so
it is not exercised in CI (see `docs/ENVIRONMENT.md` for the proven environment):

```bash
python scripts/run_local.py --results results/store.jsonl --seed 0
```

The optional hosted-model run (one or two hosted models, within the owner's
Spend_Cap, API key read only from an environment variable) is shipped but off by
default:

```bash
python scripts/run_hosted.py --results results/store.jsonl --api-key-env OPENAI_API_KEY
```

## Regenerate the Report and plots

The single command that regenerates the plots and the paper-style Report from the
generated Results_Store and Results_Table:

```bash
python -c "from aib.plots import plot_asr_by_defense, plot_defense_cost; from aib.report import render_report; plot_asr_by_defense('results/summary.md','results/asr.png'); plot_defense_cost('results/summary.md','results/defense_cost.png'); render_report('results/store.jsonl','results/summary.md',['results/asr.png','results/defense_cost.png'],'results/report.md')"
```

## Results

The Results_Table is a generated file (`results/summary.md`): one row per Cell
with the Attack_Success_Rate and Task_Success_Rate (each with a confidence
interval), the Defense_Cost relative to the No_Defense control for the same model
and injection style, the Filter_Classifier precision/recall where it ran, the
repetition count, and the temperature/sampling settings. It is produced by the
regeneration command above and is not hand-edited, so it is not committed here;
run the benchmark and the regeneration command to produce it, then read it (and
the generated Report at `results/report.md`) for the measured rates and their
confidence intervals.

See the generated **Report** (`results/report.md`) for the paper-style write-up:
question, setup, results, limitations, and threats to validity.

## Known limitations

<!-- TODO [ME]: known limitations -->

## Safety and scope

<!-- TODO [ME]: safety and scope -->
