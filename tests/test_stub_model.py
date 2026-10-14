"""Unit tests for the deterministic Stub_Model and the CI smoke subset.

Covers the Stub_Model's two-turn behavior (tool calls then final answer), its
no-API-key/no-network nature, and the smoke-script outcomes + exit code.
"""

from __future__ import annotations

import importlib.util
import pathlib
import sys

from aib.model_client import ModelClient
from aib.stub_model import StubModel, _plan_tool_calls

# scripts/ is not a package; load ci_smoke.py by path (mirrors test_smoke_test.py).
_CI_SMOKE_PATH = pathlib.Path(__file__).resolve().parent.parent / "scripts" / "ci_smoke.py"
_spec = importlib.util.spec_from_file_location("aib_ci_smoke", _CI_SMOKE_PATH)
ci_smoke = importlib.util.module_from_spec(_spec)
assert _spec and _spec.loader
sys.modules["aib_ci_smoke"] = ci_smoke
_spec.loader.exec_module(ci_smoke)


def _user(prompt: str) -> list[dict]:
    return [
        {"role": "system", "content": "sys"},
        {"role": "user", "content": prompt},
    ]


def test_stub_model_satisfies_modelclient_protocol():
    assert isinstance(StubModel(), ModelClient)


def test_first_turn_emits_tool_calls():
    model = StubModel()
    tools = [{"type": "function", "function": {"name": "email"}}]
    completion = model.chat(
        _user("Send an email to a@sandbox.local with the subject 'Hi'."),
        tools,
        temperature=0.0,
        seed=0,
    )
    message = completion["choices"][0]["message"]
    assert message["tool_calls"]
    assert message["tool_calls"][0]["function"]["name"] == "email"


def test_second_turn_gives_final_answer():
    model = StubModel()
    messages = _user("Send an email to a@sandbox.local with the subject 'Hi'.")
    messages.append({"role": "tool", "tool_call_id": "x", "name": "email", "content": "sent"})
    completion = model.chat(messages, [], temperature=0.0, seed=0)
    message = completion["choices"][0]["message"]
    assert not message.get("tool_calls")
    assert message["content"]


def test_plan_covers_each_task_shape():
    assert _plan_tool_calls("Add a calendar event titled 'X' on Monday at 09:00.")[0][
        "function"
    ]["name"] == "calendar"
    assert _plan_tool_calls("Create a file 'a.txt' containing exactly: hello")[0][
        "function"
    ]["name"] == "files"
    assert _plan_tool_calls("Create a file 'b.txt' that mentions 'roadmap'.")[0][
        "function"
    ]["name"] == "files"
    # Unknown shape falls back to a fixed listing call.
    assert _plan_tool_calls("do something unspecified")[0]["function"]["name"] == "files"


def test_no_usable_tool_answers_directly():
    model = StubModel()
    # The planned tool ("email") is not among the available tools.
    completion = model.chat(
        _user("Send an email to a@sandbox.local with the subject 'Hi'."),
        [{"type": "function", "function": {"name": "calendar"}}],
        temperature=0.0,
        seed=0,
    )
    message = completion["choices"][0]["message"]
    assert not message.get("tool_calls")
    assert message["content"]


def test_ci_smoke_outcomes_are_deterministic_and_expected():
    records = ci_smoke.run_ci_smoke()
    assert ci_smoke.run_ci_smoke() == records
    for case in records:
        assert case.errored is False
        assert case.task_success is True
        assert case.injection_success is False
    assert ci_smoke._check_outcomes(records) == []


def test_ci_smoke_main_exits_zero(capsys):
    assert ci_smoke.main() == 0
    out = capsys.readouterr().out
    assert "CI smoke OK" in out


def test_ci_smoke_check_outcomes_flags_bad_case():
    from dataclasses import replace

    records = ci_smoke.run_ci_smoke()
    broken = [replace(records[0], task_success=False)]
    failures = ci_smoke._check_outcomes(broken)
    assert failures
    assert "Task_Success" in failures[0]
