"""OpenAI-compatible model client with tool calling and fail-fast secret
handling (design.md 'Model client and AgentRunner'; Req 1.5, 5.1, 5.2, 0.13).

The :class:`ModelClient` Protocol is the single server-agnostic seam the
AgentRunner talks to, so Local_Models (via the Model_Server), Hosted_Models,
and the deterministic Stub_Model are interchangeable. The concrete
:class:`OpenAICompatibleClient` wraps the pinned ``openai`` SDK (which itself
uses the pinned ``httpx``) and talks to any ``/v1/chat/completions`` endpoint
with tool/function calling.

Secret handling is fail-fast (Req 0.13): when an API key is required, the key
is read from ``os.environ[api_key_env]`` and a :class:`MissingSecretError`
naming the missing variable is raised **before** any request is attempted. A
request that exceeds the timeout surfaces as a typed :class:`ModelTimeoutError`
rather than an SDK-specific exception.
"""

from __future__ import annotations

import os
from typing import Any, Protocol, runtime_checkable

from openai import APITimeoutError, OpenAI

#: Request timeout cap (seconds). The M0 smoke test uses a 120 s cap (task 1.4).
DEFAULT_TIMEOUT_S: float = 120.0


class ModelClientError(Exception):
    """Base class for model-client errors surfaced to callers."""


class MissingSecretError(ModelClientError):
    """Raised before any request when a required API key env var is unset/empty."""


class ModelTimeoutError(ModelClientError):
    """Raised when a request exceeds the configured timeout."""


@runtime_checkable
class ModelClient(Protocol):
    """Server-agnostic OpenAI-compatible chat interface (design.md)."""

    model: str

    def chat(
        self,
        messages: list[dict],
        tools: list[dict],
        temperature: float,
        seed: int,
    ) -> dict:
        """Return one OpenAI-compatible chat completion as a plain ``dict``."""
        ...


def _resolve_api_key(api_key_env: str | None, required: bool) -> str | None:
    """Read and validate the API key from the environment (fail-fast, Req 0.13).

    When ``required`` is true and ``api_key_env`` is unset or empty, raise
    :class:`MissingSecretError` naming the variable before any request runs.
    """
    if not required:
        return os.environ.get(api_key_env) if api_key_env else None

    if not api_key_env:
        raise MissingSecretError(
            "an API key is required but no api_key_env variable name was provided"
        )
    value = os.environ.get(api_key_env)
    if not value:
        raise MissingSecretError(
            f"required API key environment variable {api_key_env!r} is unset or empty"
        )
    return value


class OpenAICompatibleClient:
    """Concrete OpenAI-compatible :class:`ModelClient` over the pinned SDK."""

    def __init__(
        self,
        model: str,
        base_url: str,
        api_key_env: str | None = None,
        *,
        required_key: bool = True,
        timeout_s: float = DEFAULT_TIMEOUT_S,
        client: OpenAI | None = None,
    ) -> None:
        self.model = model
        self.base_url = base_url
        self.api_key_env = api_key_env
        self.timeout_s = timeout_s

        # Fail fast on a missing required secret BEFORE constructing/using any
        # client or issuing a request (Req 0.13).
        api_key = _resolve_api_key(api_key_env, required_key)

        # A local Model_Server often needs no real key; the OpenAI SDK still
        # requires a non-empty string, so fall back to a sentinel local key.
        self._client = client or OpenAI(
            base_url=base_url,
            api_key=api_key or "sk-no-key-required",
            timeout=timeout_s,
            max_retries=0,
        )

    def chat(
        self,
        messages: list[dict],
        tools: list[dict],
        temperature: float,
        seed: int,
    ) -> dict:
        """Issue one chat completion with tool calling; typed timeout error."""
        kwargs: dict[str, Any] = {
            "model": self.model,
            "messages": messages,
            "temperature": temperature,
            "seed": seed,
        }
        if tools:
            kwargs["tools"] = tools

        try:
            completion = self._client.chat.completions.create(**kwargs)
        except APITimeoutError as exc:
            raise ModelTimeoutError(
                f"model request to {self.base_url!r} timed out after {self.timeout_s}s"
            ) from exc

        # Normalize to a plain dict so downstream code never depends on the SDK
        # object shape (the Stub_Model returns the same dict shape).
        if hasattr(completion, "model_dump"):
            return completion.model_dump()
        return dict(completion)
