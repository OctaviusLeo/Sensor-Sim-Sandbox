# Environment

Every dependency is pinned to an exact version and proven on this machine. GPU,
Docker, and real-model paths are proven on the owner's target machine (see the
`[ME]` sections below).

## Tooling

| Tool | Version |
|---|---|
| Python | 3.11.5 |
| pip | 24.3.1 |

## Dependencies (proven on this machine)

Each pin below was confirmed resolvable on Python 3.11.5 via
`pip index versions <name>` and `pip install --dry-run` (no resolver conflicts).

| Name | Version | Verification artifact |
|---|---|---|
| openai | 3.29.0 | pip index versions / pip install --dry-run succeeded on Python 3.11.5 |
| httpx | 0.28.1 | pip index versions / pip install --dry-run succeeded on Python 3.11.5 |
| numpy | 2.4.6 | pip index versions / pip install --dry-run succeeded on Python 3.11.5 (last 2.4.x supporting 3.11) |
| scipy | 1.15.0 | pip install --dry-run with numpy==2.4.6 succeeded on Python 3.11.5 (nearest release above candidate 1.14.1, which pins numpy<2.3 and conflicts) |
| matplotlib | 3.9.2 | pip index versions / pip install --dry-run with numpy==2.4.6 succeeded on Python 3.11.5 |
| pytest | 9.1.1 | pip index versions / pip install --dry-run succeeded on Python 3.11.5 |
| pytest-cov | 7.1.0 | pip index versions / pip install --dry-run succeeded on Python 3.11.5 |
| hypothesis | 6.169.0 | pip index versions / pip install --dry-run succeeded on Python 3.11.5 |

## Local model + server [ME]

> Owner task (1.4). Record the selected Model_Server (e.g. Ollama /
> `llama-server` / vLLM), its exact version, the chosen open-weight
> Local_Model (name, version, quantization), the OpenAI-compatible base URL,
> and the measured peak GPU memory vs `nvidia-smi` total on the owner's
> RTX 3080 machine.

## Smoke-test evidence [ME]

> Filled by `scripts/smoke_test.py` at M0 on the GPU/Docker machine. Records
> the Python/Docker/GPU prerequisite checks, the single question/answer, and
> the measured peak GPU MiB. Not runnable in CI or on this dev box (no GPU,
> no Docker).
