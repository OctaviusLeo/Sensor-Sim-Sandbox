"""Unit tests for the User_Task catalog, loader, and validator (Req 2.3, 2.7)."""

from __future__ import annotations

from aib.sandbox import init_sandbox
from aib.tasks import (
    CheckOutcome,
    MIN_USER_TASKS,
    UserTask,
    load_user_tasks,
    validate_catalog,
)
from aib.tools import CalendarTool, EmailTool, FileStoreTool


def test_catalog_meets_minimum_count():
    tasks = load_user_tasks()
    assert len(tasks) >= MIN_USER_TASKS


def test_catalog_task_ids_unique():
    ids = [t.task_id for t in load_user_tasks()]
    assert len(ids) == len(set(ids))


def test_every_task_has_resolvable_check():
    report = validate_catalog()
    assert report.total >= MIN_USER_TASKS
    assert report.missing_check == ()


def test_validator_reports_missing_check():
    bad = UserTask(task_id="broken", prompt="p", success_check=None)  # type: ignore[arg-type]
    report = validate_catalog([bad])
    assert report.total == 1
    assert report.missing_check == ("broken",)


def test_correct_completion_passes_email_task():
    task = next(t for t in load_user_tasks() if t.task_id == "email-01")
    state = init_sandbox(task, seed=3)
    # Carry out the task: send to alice@sandbox.local with the expected subject.
    EmailTool().invoke(
        state,
        action="send",
        to="alice@sandbox.local",
        subject="Monday sync confirmed",
        body="done",
    )
    outcome = task.success_check(state, "sent")
    assert outcome.passed is True
    assert outcome.unmet is None


def test_incorrect_completion_fails_with_reason():
    task = next(t for t in load_user_tasks() if t.task_id == "email-01")
    state = init_sandbox(task, seed=3)
    # Do nothing: the task is not completed.
    outcome = task.success_check(state, "")
    assert outcome.passed is False
    assert outcome.unmet is not None and outcome.unmet != ""


def test_calendar_task_pass_and_fail():
    task = next(t for t in load_user_tasks() if t.task_id == "calendar-01")
    state = init_sandbox(task, seed=5)
    assert task.success_check(state, "").passed is False
    CalendarTool().invoke(state, action="add", title="Dentist", day="Tuesday", hour=9)
    assert task.success_check(state, "").passed is True


def test_file_task_pass_and_fail():
    task = next(t for t in load_user_tasks() if t.task_id == "file-write-01")
    state = init_sandbox(task, seed=5)
    assert task.success_check(state, "").passed is False
    FileStoreTool().invoke(
        state, action="write", path="notes.txt", content="Meeting notes: discuss roadmap."
    )
    assert task.success_check(state, "").passed is True


def test_outcome_type():
    task = load_user_tasks()[0]
    state = init_sandbox(task, seed=1)
    assert isinstance(task.success_check(state, ""), CheckOutcome)
