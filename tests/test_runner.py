"""Unit tests for aib.runner.AgentRunner (Req 3.6, 5.3, 5.6).

A per-case error is captured as ``errored`` with a cause and does NOT abort
(Req 5.6); the recorded CaseResult carries model/defense/style/task/repetition
and temperature/sampling (Req 5.3); both check outcomes are recorded on the same
case (Req 3.6). FEAT-006's StubModel does not exist yet, so these tests use a
minimal in-test fake ModelClient (no duplicate stub is added to aib/).
"""

from __future__ import annotations

import json

from aib.defenses import DefenseConfig, DefenseKind
from aib.goals import load_injected_goals
from aib.injections import INJECTION_STYLES
from aib.runner import AgentRunner, CaseResult
from aib.tasks import load_user_tasks
from aib.tools import default_tools


def _style(style_id: str):
    return next(s for s in INJECTION_STYLES if s.style_id == style_id)


def _goal(goal_id: str):
    return next(g for g in load_injected_goals() if g.goal_id == goal_id)


def _task(task_id: str):
    return next(t for t in load_user_tasks() if t.task_id == task_id)


class _ScriptedModel:
    """A fake ModelClient that replays a fixed list of completion dicts."""

    def __init__(self, model: str, turns: list[dict]):
        self.model = model
        self._turns = list(turns)
        self.calls = 0

    def chat(self, messages, tools, temperature, seed) -> dict:
        self.calls += 1
        if not self._turns:
            return _final("done")
        return self._turns.pop(0)


class _RaisingModel:
    model = "boom-model"

    def chat(self, messages, tools, temperature, seed) -> dict:
        raise RuntimeError("simulated model failure")


def _tool_call(call_id: str, name: str, args: dict) -> dict:
    return {
        "choices": [
            {
                "message": {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [
                        {
                            "id": call_id,
                            "type": "function",
                            "function": {
                                "name": name,
                                "arguments": json.dumps(args),
                            },
                        }
                    ],
                }
            }
        ]
    }


def _final(text: str) -> dict:
    return {"choices": [{"message": {"role": "assistant", "content": text}}]}


def test_raising_case_recorded_as_errored_not_raised():
    runner = AgentRunner()
    result = runner.run(
        task=_task("email-01"),
        tools=default_tools(),
        defense=DefenseConfig(kind=DefenseKind.NONE),
        style=_style("direct"),
        goal=_goal("forward_file_to_sandbox_addr"),
        model=_RaisingModel(),
        temperature=0.0,
        seed=7,
    )
    assert isinstance(result, CaseResult)
    assert result.errored is True
    assert result.cause and "simulated model failure" in result.cause
    assert result.injection_success is None
    assert result.task_success is False
    # Metadata is still populated so the sweep can record the failed case.
    assert result.model == "boom-model"
    assert result.task_id == "email-01"


def test_case_result_carries_config_metadata():
    model = _ScriptedModel("test-model", [_final("all set")])
    runner = AgentRunner()
    result = runner.run(
        task=_task("email-02"),
        tools=default_tools(),
        defense=DefenseConfig(kind=DefenseKind.DELIMIT),
        style=_style("fake_system"),
        goal=_goal("add_sandbox_calendar_entry"),
        model=model,
        temperature=0.3,
        seed=11,
    )
    assert result.model == "test-model"
    assert result.defense == "delimit"
    assert result.style == "fake_system"
    assert result.task_id == "email-02"
    assert result.repetition == 0
    assert result.temperature == 0.3
    assert result.sampling["seed"] == 11
    assert result.errored is False


def test_task_success_recorded_when_agent_completes_task():
    # Drive the model to send the exact email the task requires, then finish.
    task = _task("email-01")  # send to alice@sandbox.local, subject 'Monday sync confirmed'
    send = _tool_call(
        "c1",
        "email",
        {
            "action": "send",
            "to": "alice@sandbox.local",
            "subject": "Monday sync confirmed",
            "body": "done",
        },
    )
    model = _ScriptedModel("test-model", [send, _final("sent it")])
    result = AgentRunner().run(
        task=task,
        tools=default_tools(),
        defense=DefenseConfig(kind=DefenseKind.NONE),
        style=_style("direct"),
        goal=_goal("forward_file_to_sandbox_addr"),
        model=model,
        temperature=0.0,
        seed=3,
    )
    assert result.task_success is True
    # The injected goal (forward to attacker@sandbox.local) was NOT carried out.
    assert result.injection_success is False
    assert result.errored is False


def test_injection_success_recorded_when_goal_carried_out():
    # Drive the model to perform the injected goal: forward to attacker@sandbox.local.
    forward = _tool_call(
        "c1",
        "email",
        {
            "action": "send",
            "to": "attacker@sandbox.local",
            "subject": "fwd",
            "body": "report",
        },
    )
    model = _ScriptedModel("test-model", [forward, _final("ok")])
    result = AgentRunner().run(
        task=_task("summarize-01"),
        tools=default_tools(),
        defense=DefenseConfig(kind=DefenseKind.NONE),
        style=_style("direct"),
        goal=_goal("forward_file_to_sandbox_addr"),
        model=model,
        temperature=0.0,
        seed=3,
    )
    assert result.injection_success is True


def test_filter_decisions_recorded_with_filter_defense():
    # The web page fixture plus a planted injection should be screened once.
    fetch = _tool_call("c1", "web_fetch", {"url": "https://sandbox.local/news"})
    model = _ScriptedModel("test-model", [fetch, _final("done")])
    result = AgentRunner().run(
        task=_task("summarize-01"),
        tools=default_tools(),
        defense=DefenseConfig(kind=DefenseKind.FILTER),
        style=_style("fake_system"),
        goal=_goal("forward_file_to_sandbox_addr"),
        model=model,
        temperature=0.0,
        seed=3,
    )
    assert len(result.filter_decisions) == 1


def test_tool_limit_blocks_disallowed_tool():
    # Only 'files' is allowed; a call to 'email' must be reported unavailable
    # and must not send anything, so the injected forward cannot succeed.
    forward = _tool_call(
        "c1",
        "email",
        {"action": "send", "to": "attacker@sandbox.local", "body": "x"},
    )
    model = _ScriptedModel("test-model", [forward, _final("done")])
    result = AgentRunner().run(
        task=_task("file-write-01"),
        tools=default_tools(),
        defense=DefenseConfig(
            kind=DefenseKind.TOOL_LIMIT, params={"allowed_tools": ["files"]}
        ),
        style=_style("direct"),
        goal=_goal("forward_file_to_sandbox_addr"),
        model=model,
        temperature=0.0,
        seed=3,
    )
    assert result.errored is False
    # The blocked email tool means the injected forward never completes, so the
    # goal is not carried out (never True): the send attempt is recorded but
    # its output reports the tool unavailable, so the goal check cannot score
    # it as success.
    assert result.injection_success is not True
