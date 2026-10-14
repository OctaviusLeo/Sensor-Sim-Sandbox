"""Property-based tests for the Sandbox.

Feature: agent-injection-bench, Property 1: Sandbox never performs real network I/O
Feature: agent-injection-bench, Property 2: Sandbox initialization is deterministic under a seed
"""

from __future__ import annotations

import socket
from contextlib import contextmanager

from hypothesis import given
from hypothesis import strategies as st

from aib.sandbox import SandboxState, init_sandbox
from aib.tasks import load_user_tasks
from aib.tools import default_tools

_TASKS = load_user_tasks()
_task_ids = st.sampled_from([t.task_id for t in _TASKS])
_seeds = st.integers(min_value=0, max_value=2**31 - 1)


def _task_by_id(task_id: str):
    return next(t for t in _TASKS if t.task_id == task_id)


# A small catalog of valid tool-call arguments per tool, used to exercise each
# tool under the network guard with realistic (state, args) draws.
_TOOL_ARGS = {
    "email": [
        {"action": "list"},
        {"action": "read", "id": "msg-1"},
        {"action": "send", "to": "x@sandbox.local", "subject": "s", "body": "b"},
    ],
    "calendar": [
        {"action": "list"},
        {"action": "add", "title": "t", "day": "Monday", "hour": 9},
    ],
    "files": [
        {"action": "list"},
        {"action": "read", "path": "readme.txt"},
        {"action": "write", "path": "p.txt", "content": "c"},
    ],
    "web_fetch": [
        {"url": "https://sandbox.local/news"},
        {"url": "https://unknown.example/x"},
    ],
}


@contextmanager
def _network_guard():
    """Make any real outbound socket connection raise for the duration."""

    def _blocked(*_a, **_k):  # pragma: no cover - only runs if a tool misbehaves
        raise AssertionError("tool attempted a real outbound network connection")

    orig_connect = socket.socket.connect
    orig_connect_ex = socket.socket.connect_ex
    socket.socket.connect = _blocked  # type: ignore[method-assign]
    socket.socket.connect_ex = _blocked  # type: ignore[method-assign]
    try:
        yield
    finally:
        socket.socket.connect = orig_connect  # type: ignore[method-assign]
        socket.socket.connect_ex = orig_connect_ex  # type: ignore[method-assign]


@given(task_id=_task_ids, seed=_seeds, arg_index=st.integers(min_value=0, max_value=2))
def test_sandbox_no_real_network_io(task_id, seed, arg_index):
    """Feature: agent-injection-bench, Property 1: Sandbox never performs real network I/O."""
    task = _task_by_id(task_id)

    with _network_guard():
        for tool in default_tools():
            candidates = _TOOL_ARGS[tool.name]
            args = candidates[arg_index % len(candidates)]

            # Pure function of (state, args): two fresh identical states yield
            # equal ToolResult outputs, and no outbound connection was opened.
            state_a = init_sandbox(task, seed)
            state_b = init_sandbox(task, seed)
            result_a = tool.invoke(state_a, **args)
            result_b = tool.invoke(state_b, **args)
            assert result_a == result_b


@given(task_id=_task_ids, seed=_seeds)
def test_sandbox_init_deterministic(task_id, seed):
    """Feature: agent-injection-bench, Property 2: Sandbox initialization is deterministic under a seed."""
    task = _task_by_id(task_id)
    first = init_sandbox(task, seed)
    second = init_sandbox(task, seed)
    assert isinstance(first, SandboxState)
    assert first == second
