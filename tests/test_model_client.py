"""Unit tests for aib.model_client: fail-fast secrets + typed timeout (Req 0.13).

The HTTP layer is mocked throughout; no real network egress.
"""

from __future__ import annotations

import httpx
import pytest
from openai import APITimeoutError

from aib.model_client import (
    MissingSecretError,
    ModelTimeoutError,
    OpenAICompatibleClient,
)


class _FakeCompletions:
    def __init__(self, exc=None, payload=None):
        self._exc = exc
        self._payload = payload
        self.called = False

    def create(self, **kwargs):
        self.called = True
        if self._exc is not None:
            raise self._exc
        return self._payload


class _FakeChat:
    def __init__(self, completions):
        self.completions = completions


class _FakeClient:
    def __init__(self, completions):
        self.chat = _FakeChat(completions)


class _DictPayload:
    def __init__(self, data):
        self._data = data

    def model_dump(self):
        return dict(self._data)


def test_missing_required_secret_fails_fast_before_any_request(monkeypatch):
    monkeypatch.delenv("AIB_TEST_KEY", raising=False)
    with pytest.raises(MissingSecretError) as excinfo:
        OpenAICompatibleClient(
            model="m",
            base_url="http://localhost:1/v1",
            api_key_env="AIB_TEST_KEY",
            required_key=True,
        )
    assert "AIB_TEST_KEY" in str(excinfo.value)


def test_empty_required_secret_fails_fast(monkeypatch):
    monkeypatch.setenv("AIB_TEST_KEY", "")
    with pytest.raises(MissingSecretError):
        OpenAICompatibleClient(
            model="m",
            base_url="http://localhost:1/v1",
            api_key_env="AIB_TEST_KEY",
            required_key=True,
        )


def test_timeout_surfaces_as_typed_error():
    timeout = APITimeoutError(request=httpx.Request("POST", "http://localhost:1/v1"))
    completions = _FakeCompletions(exc=timeout)
    client = OpenAICompatibleClient(
        model="m",
        base_url="http://localhost:1/v1",
        required_key=False,
        client=_FakeClient(completions),
    )
    with pytest.raises(ModelTimeoutError):
        client.chat(messages=[{"role": "user", "content": "hi"}], tools=[], temperature=0.0, seed=1)
    assert completions.called is True


def test_chat_returns_plain_dict_from_completion():
    payload = _DictPayload({"id": "x", "choices": []})
    completions = _FakeCompletions(payload=payload)
    client = OpenAICompatibleClient(
        model="m",
        base_url="http://localhost:1/v1",
        required_key=False,
        client=_FakeClient(completions),
    )
    result = client.chat(
        messages=[{"role": "user", "content": "hi"}],
        tools=[],
        temperature=0.0,
        seed=1,
    )
    assert result == {"id": "x", "choices": []}


def test_optional_key_absent_does_not_raise(monkeypatch):
    monkeypatch.delenv("AIB_TEST_KEY", raising=False)
    completions = _FakeCompletions(payload=_DictPayload({"ok": True}))
    client = OpenAICompatibleClient(
        model="m",
        base_url="http://localhost:1/v1",
        api_key_env="AIB_TEST_KEY",
        required_key=False,
        client=_FakeClient(completions),
    )
    assert client.model == "m"
