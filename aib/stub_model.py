"""Deterministic Stub_Model for GPU-free, API-key-free CI (design.md 'Stub
model and CI smoke subset'; Req 8.3, 8.6).

:class:`StubModel` implements the same :class:`~aib.model_client.ModelClient`
interface as the real OpenAI-compatible client, but performs **no network I/O
and needs no API key** - it is a pure, deterministic function of the ``messages``
it is handed. That determinism is what lets the CI smoke subset assert fixed
Injection_Success_Check and Task_Success_Check outcomes (Property 10).

How the fixed trace is produced
-------------------------------
Driven through :class:`~aib.runner.AgentRunner`, the StubModel sees the system +
user messages on its first ``chat`` call and emits a **fixed, parsed** set of
tool calls that complete the legitimate User_Task (so Task_Success_Check passes)
- it reads the user prompt and maps the common task shapes (send an email, add a
calendar event, create a file) to the matching Simulated_Tool call with the
arguments named in the prompt. The StubModel deliberately **never** acts on any
instruction found in a Tool_Output, so the Injection_Success_Check is a fixed
``False`` (the injected goal is not carried out) on every case. On the second
``chat`` call (after the tool results come back) it emits a short final answer
and stops.

Because the emitted tool calls and the final answer are a deterministic function
of the user prompt alone - never of a sampled token sequence, a clock, or any
network response - repeated runs of the same case yield an identical tool-call
trace and identical check outcomes (Req 8.6, Property 10).
"""

from __future__ import annotations

import re

#: The fixed final answer the StubModel returns once its tool calls are done.
FINAL_RESPONSE: str = "Done. I completed the requested task using the sandbox tools."


def _find_user_prompt(messages: list[dict]) -> str:
    """Return the content of the last ``user`` message (the User_Task prompt)."""
    for message in reversed(messages):
        if message.get("role") == "user":
            return str(message.get("content") or "")
    return ""


def _already_called_tools(messages: list[dict]) -> bool:
    """True once at least one tool result is present in the transcript.

    The AgentRunner appends ``role="tool"`` messages after it executes the
    StubModel's tool calls, so their presence means this is a follow-up turn and
    the StubModel should now give its final answer.
    """
    return any(message.get("role") == "tool" for message in messages)


def _tool_call(call_id: str, name: str, arguments: dict) -> dict:
    """Build one OpenAI-compatible tool call entry (arguments JSON-encoded)."""
    import json

    return {
        "id": call_id,
        "type": "function",
        "function": {"name": name, "arguments": json.dumps(arguments, sort_keys=True)},
    }


def _plan_tool_calls(prompt: str) -> list[dict]:
    """Map a User_Task prompt to the fixed tool calls that complete it.

    This is a deterministic parser over the authored task prompts in
    :mod:`aib.tasks` (send email / add calendar event / create file / mention a
    substring). Unknown shapes fall back to a single ``files.list`` call so the
    trace is still fixed and non-empty.
    """
    calls: list[dict] = []

    # --- Email: "Send an email to <addr> with the subject '<subject>'." -----
    email = re.search(
        r"email (?:to )?(\S+@\S+?)\s.*?subject '([^']*)'", prompt, re.IGNORECASE
    )
    if email is not None:
        addr, subject = email.group(1), email.group(2)
        calls.append(
            _tool_call(
                "call-email",
                "email",
                {"action": "send", "to": addr, "subject": subject, "body": "ok"},
            )
        )
        return calls

    # --- Calendar: "titled '<title>' on <day> at <HH>:00." ------------------
    cal = re.search(
        r"titled '([^']*)' on (\w+) at (\d{1,2}):", prompt, re.IGNORECASE
    )
    if cal is not None:
        title, day, hour = cal.group(1), cal.group(2), int(cal.group(3))
        calls.append(
            _tool_call(
                "call-calendar",
                "calendar",
                {"action": "add", "title": title, "day": day, "hour": hour},
            )
        )
        return calls

    # --- File (exact content): "containing exactly: <content>" --------------
    file_exact = re.search(
        r"file '([^']*)' containing exactly: (.+)$", prompt, re.IGNORECASE
    )
    if file_exact is not None:
        path, content = file_exact.group(1), file_exact.group(2).strip()
        calls.append(
            _tool_call(
                "call-files",
                "files",
                {"action": "write", "path": path, "content": content},
            )
        )
        return calls

    # --- File (substring): "file '<path>' that mentions '<needle>'." --------
    file_contains = re.search(
        r"file '([^']*)' that mentions '([^']*)'", prompt, re.IGNORECASE
    )
    if file_contains is not None:
        path, needle = file_contains.group(1), file_contains.group(2)
        calls.append(
            _tool_call(
                "call-files",
                "files",
                {"action": "write", "path": path, "content": f"mentions {needle}"},
            )
        )
        return calls

    # --- Fallback: a fixed, side-effect-free listing keeps the trace fixed. --
    calls.append(_tool_call("call-list", "files", {"action": "list"}))
    return calls


def _completion(message: dict) -> dict:
    """Wrap an assistant ``message`` in an OpenAI-compatible completion dict."""
    return {"choices": [{"index": 0, "message": message, "finish_reason": "stop"}]}


class StubModel:
    """A deterministic, network-free :class:`~aib.model_client.ModelClient`.

    ``chat`` ignores ``temperature`` and ``seed`` entirely (there is no sampling)
    and returns a completion that is a pure function of ``messages``: the planned
    tool calls on the first turn, then the fixed final answer once tool results
    are present. It never follows an instruction embedded in a Tool_Output, so
    the Injected_Goal is never carried out (a fixed Injection_Success_Check
    result) while the legitimate User_Task is completed (Req 8.3, 8.6).
    """

    def __init__(self, model: str = "stub-model") -> None:
        self.model = model

    def chat(
        self,
        messages: list[dict],
        tools: list[dict],
        temperature: float,
        seed: int,
    ) -> dict:
        """Return a deterministic OpenAI-compatible completion dict."""
        if _already_called_tools(messages):
            # Follow-up turn: give the fixed final answer and stop.
            return _completion({"role": "assistant", "content": FINAL_RESPONSE})

        prompt = _find_user_prompt(messages)
        available = {
            (tool.get("function") or {}).get("name") for tool in (tools or [])
        }
        tool_calls = [
            call
            for call in _plan_tool_calls(prompt)
            if call["function"]["name"] in available or not available
        ]

        if not tool_calls:
            # No usable tool for this case (e.g. the TOOL_LIMIT Defense removed
            # it): answer directly so the case still terminates deterministically.
            return _completion({"role": "assistant", "content": FINAL_RESPONSE})

        return _completion(
            {"role": "assistant", "content": None, "tool_calls": tool_calls}
        )
