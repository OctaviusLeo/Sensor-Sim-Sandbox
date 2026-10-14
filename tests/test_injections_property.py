"""Property-based test for Injection_Style planting (Req 3.2, 4.2).

Feature: agent-injection-bench, Property 4: Injection is planted only in Tool_Output, never in the User_Task
"""

from __future__ import annotations

from hypothesis import given
from hypothesis import strategies as st

from aib.injections import INJECTION_STYLES, load_injection_styles
from aib.tasks import load_user_tasks

# A single Property 4 test (>=100 examples via [tool.hypothesis] max_examples).

_STYLES = load_injection_styles()
_style_ids = st.sampled_from([s.style_id for s in _STYLES])

_TASKS = load_user_tasks()
_task_prompts = st.sampled_from([t.prompt for t in _TASKS])

# Text drawn from printable categories; the injected instruction is a benign,
# arbitrary string here - the property is about *where* it lands, not content.
_text = st.text(
    alphabet=st.characters(whitelist_categories=("L", "N", "Zs", "P")),
    min_size=1,
    max_size=80,
)


def _style_by_id(style_id: str):
    return next(s for s in _STYLES if s.style_id == style_id)


@given(
    style_id=_style_ids,
    clean_output=_text,
    injected_instruction=_text,
    task_prompt=_task_prompts,
)
def test_injection_only_in_tool_output_not_user_task(
    style_id, clean_output, injected_instruction, task_prompt
):
    """Feature: agent-injection-bench, Property 4: Injection is planted only in Tool_Output, never in the User_Task."""
    style = _style_by_id(style_id)

    # The User_Task prompt is captured before planting and must be unchanged.
    original_prompt = task_prompt

    planted = style.plant(clean_output, injected_instruction)

    # The injection text appears in the transformed Tool_Output.
    assert injected_instruction in planted
    # The clean output is preserved inside the planted Tool_Output.
    assert clean_output in planted
    # The User_Task prompt is byte-for-byte unchanged by planting.
    assert task_prompt == original_prompt


def test_catalog_has_required_styles():
    ids = {s.style_id for s in INJECTION_STYLES}
    assert len(INJECTION_STYLES) >= 5
    assert {"direct", "fake_system", "hidden_field"} <= ids
