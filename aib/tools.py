"""Simulated_Tools (design.md 'Sandbox and Simulated_Tools'; Req 2.1, 2.2).

Four tools model the surfaces an agent typically touches - email, calendar, a
file store, and web fetch - but each reads and writes **only** the in-memory
:class:`SandboxState`. The web-fetch tool returns the canned ``web_pages``
fixture body for a known URL and never opens a socket (Req 0.14); an unknown
URL yields an ``ok=False`` result rather than any outbound request.

Every tool is a pure function of ``(state, args)``: for a fixed state and the
same arguments it returns an equal :class:`ToolResult` (and applies the same
mutation), which is what Property 1 and the per-tool unit tests assert.
"""

from __future__ import annotations

from typing import Sequence

from aib.sandbox import SandboxState, SimulatedTool, ToolResult


class EmailTool:
    """Read the inbox or send a sandbox email (writes into ``state.emails``)."""

    name = "email"

    def invoke(self, state: SandboxState, **args) -> ToolResult:
        action = args.get("action", "list")

        if action == "list":
            if not state.emails:
                return ToolResult(output="(no emails)", ok=True)
            lines = [
                f"{msg['id']}: {msg['subject']} - from {msg['from']}"
                for msg in state.emails
                if not msg.get("sent")
            ]
            body = "\n".join(lines) if lines else "(no received emails)"
            return ToolResult(output=body, ok=True)

        if action == "read":
            msg_id = args.get("id")
            for msg in state.emails:
                if msg.get("id") == msg_id:
                    return ToolResult(output=msg.get("body", ""), ok=True)
            return ToolResult(output=f"email {msg_id!r} not found", ok=False)

        if action == "send":
            to = args.get("to")
            subject = args.get("subject", "")
            body = args.get("body", "")
            if not to:
                return ToolResult(output="missing 'to' address", ok=False)
            sent_id = f"sent-{sum(1 for m in state.emails if m.get('sent')) + 1}"
            state.emails.append(
                {
                    "id": sent_id,
                    "from": "me@sandbox.local",
                    "to": to,
                    "subject": subject,
                    "body": body,
                    "sent": True,
                }
            )
            return ToolResult(output=f"sent {sent_id} to {to}", ok=True)

        return ToolResult(output=f"unknown action {action!r}", ok=False)


class CalendarTool:
    """List calendar events or add a sandbox event (writes ``state.calendar``)."""

    name = "calendar"

    def invoke(self, state: SandboxState, **args) -> ToolResult:
        action = args.get("action", "list")

        if action == "list":
            if not state.calendar:
                return ToolResult(output="(no events)", ok=True)
            lines = [
                f"{evt['id']}: {evt['title']} on {evt['day']} at {evt['hour']:02d}:00"
                for evt in state.calendar
            ]
            return ToolResult(output="\n".join(lines), ok=True)

        if action == "add":
            title = args.get("title")
            day = args.get("day")
            hour = args.get("hour")
            if not title or not day or hour is None:
                return ToolResult(output="missing title/day/hour", ok=False)
            evt_id = f"evt-{len(state.calendar) + 1}"
            state.calendar.append(
                {"id": evt_id, "title": title, "day": day, "hour": int(hour)}
            )
            return ToolResult(output=f"added {evt_id}: {title} on {day}", ok=True)

        return ToolResult(output=f"unknown action {action!r}", ok=False)


class FileStoreTool:
    """Read, write, or list files in the sandbox file store (``state.files``)."""

    name = "files"

    def invoke(self, state: SandboxState, **args) -> ToolResult:
        action = args.get("action", "list")

        if action == "list":
            if not state.files:
                return ToolResult(output="(empty)", ok=True)
            return ToolResult(output="\n".join(sorted(state.files)), ok=True)

        if action == "read":
            path = args.get("path")
            if path in state.files:
                return ToolResult(output=state.files[path], ok=True)
            return ToolResult(output=f"file {path!r} not found", ok=False)

        if action == "write":
            path = args.get("path")
            content = args.get("content", "")
            if not path:
                return ToolResult(output="missing 'path'", ok=False)
            state.files[path] = content
            return ToolResult(output=f"wrote {path}", ok=True)

        return ToolResult(output=f"unknown action {action!r}", ok=False)


class WebFetchTool:
    """Fetch a canned local fixture body for a known URL (Req 0.14).

    This tool *models* network access but never performs any: a known URL
    returns its fixture body from ``state.web_pages``; an unknown URL returns an
    ``ok=False`` result. No socket is ever opened.
    """

    name = "web_fetch"

    def invoke(self, state: SandboxState, **args) -> ToolResult:
        url = args.get("url")
        if url in state.web_pages:
            return ToolResult(output=state.web_pages[url], ok=True)
        return ToolResult(output=f"no fixture for url {url!r}", ok=False)


def default_tools() -> Sequence[SimulatedTool]:
    """Return the >=4 default Simulated_Tools (Req 2.1)."""
    return (EmailTool(), CalendarTool(), FileStoreTool(), WebFetchTool())
