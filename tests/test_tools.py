"""Unit tests for the Simulated_Tools (Req 2.1, 2.2).

Each tool reads/writes only Sandbox state and is deterministic for a fixed
``(state, args)``.
"""

from __future__ import annotations

from aib.sandbox import SandboxState, init_sandbox
from aib.tasks import load_user_tasks
from aib.tools import (
    CalendarTool,
    EmailTool,
    FileStoreTool,
    WebFetchTool,
    default_tools,
)

_TASK = load_user_tasks()[0]


def _fresh() -> SandboxState:
    return init_sandbox(_TASK, seed=7)


def test_default_tools_has_at_least_four():
    tools = default_tools()
    assert len(tools) >= 4
    names = {t.name for t in tools}
    assert {"email", "calendar", "files", "web_fetch"} <= names


def test_email_list_and_send():
    tool = EmailTool()
    state = _fresh()
    listed = tool.invoke(state, action="list")
    assert listed.ok

    before = len(state.emails)
    sent = tool.invoke(state, action="send", to="x@sandbox.local", subject="hi", body="b")
    assert sent.ok
    assert len(state.emails) == before + 1
    assert any(m.get("sent") and m["to"] == "x@sandbox.local" for m in state.emails)


def test_email_send_requires_to():
    tool = EmailTool()
    state = _fresh()
    res = tool.invoke(state, action="send", subject="s")
    assert not res.ok


def test_email_read_missing():
    tool = EmailTool()
    state = _fresh()
    res = tool.invoke(state, action="read", id="nope")
    assert not res.ok


def test_calendar_add():
    tool = CalendarTool()
    state = _fresh()
    before = len(state.calendar)
    res = tool.invoke(state, action="add", title="Sync", day="Monday", hour=10)
    assert res.ok
    assert len(state.calendar) == before + 1
    assert any(e["title"] == "Sync" and e["day"] == "Monday" for e in state.calendar)


def test_calendar_add_missing_fields():
    tool = CalendarTool()
    state = _fresh()
    res = tool.invoke(state, action="add", title="x")
    assert not res.ok


def test_file_write_read_roundtrip():
    tool = FileStoreTool()
    state = _fresh()
    w = tool.invoke(state, action="write", path="n.txt", content="hello")
    assert w.ok
    r = tool.invoke(state, action="read", path="n.txt")
    assert r.ok and r.output == "hello"


def test_file_read_missing():
    tool = FileStoreTool()
    state = _fresh()
    res = tool.invoke(state, action="read", path="missing.txt")
    assert not res.ok


def test_web_fetch_known_and_unknown():
    tool = WebFetchTool()
    state = _fresh()
    known = tool.invoke(state, url="https://sandbox.local/news")
    assert known.ok and known.output == state.web_pages["https://sandbox.local/news"]
    unknown = tool.invoke(state, url="https://real.example/x")
    assert not unknown.ok


def test_tools_are_deterministic_for_fixed_state_and_args():
    for tool in default_tools():
        state_a = _fresh()
        state_b = _fresh()
        args = {"action": "list"} if tool.name != "web_fetch" else {
            "url": "https://sandbox.local/news"
        }
        assert tool.invoke(state_a, **args) == tool.invoke(state_b, **args)


def test_unknown_action_is_not_ok():
    assert not EmailTool().invoke(_fresh(), action="bogus").ok
    assert not CalendarTool().invoke(_fresh(), action="bogus").ok
    assert not FileStoreTool().invoke(_fresh(), action="bogus").ok
