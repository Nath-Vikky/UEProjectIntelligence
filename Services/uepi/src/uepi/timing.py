from __future__ import annotations

from contextvars import ContextVar
import os
from time import perf_counter
from typing import Any


TIMING_FIELDS = (
    "total_ms",
    "mcp_queue_ms",
    "snapshot_query_ms",
    "bridge_connect_ms",
    "bridge_wait_ms",
    "editor_dispatch_ms",
    "editor_execute_ms",
    "serialization_ms",
)

_BRIDGE_FIELDS = (
    "bridge_connect_ms",
    "bridge_wait_ms",
    "editor_dispatch_ms",
    "editor_execute_ms",
)

_CURRENT: ContextVar[dict[str, Any] | None] = ContextVar("uepi_request_timing", default=None)


def begin_request(received_at: float | None = None) -> object:
    started_at = perf_counter()
    state: dict[str, Any] = {
        "started_at": started_at,
        "stages": {field: 0.0 for field in _BRIDGE_FIELDS},
        "mcp_queue_ms": max(0.0, (started_at - received_at) * 1000.0) if received_at is not None else 0.0,
    }
    return _CURRENT.set(state)


def end_request(token: object) -> None:
    _CURRENT.reset(token)  # type: ignore[arg-type]


def record(stage: str, elapsed_ms: float) -> None:
    state = _CURRENT.get()
    if state is None or stage not in _BRIDGE_FIELDS:
        return
    stages = state["stages"]
    stages[stage] = float(stages.get(stage, 0.0)) + max(0.0, float(elapsed_ms))


def absorb_editor_timing(value: Any) -> float:
    if not isinstance(value, dict):
        return 0.0
    absorbed_ms = 0.0
    for field in ("editor_dispatch_ms", "editor_execute_ms"):
        raw = value.get(field)
        if isinstance(raw, (int, float)) and not isinstance(raw, bool):
            elapsed_ms = max(0.0, float(raw))
            record(field, elapsed_ms)
            absorbed_ms += elapsed_ms
    return absorbed_ms


def _rounded(value: float) -> float:
    return round(max(0.0, value), 3)


def _slow_threshold_ms() -> float:
    raw = os.environ.get("UEPI_SLOW_OPERATION_MS", "5000")
    try:
        return max(1.0, float(raw))
    except ValueError:
        return 5000.0


def _timing_value(state: dict[str, Any], *, serialization_ms: float = 0.0) -> dict[str, Any]:
    stages = state["stages"]
    elapsed_ms = (perf_counter() - float(state["started_at"])) * 1000.0
    bridge_ms = sum(float(stages.get(field, 0.0)) for field in _BRIDGE_FIELDS)
    snapshot_ms = max(0.0, elapsed_ms - bridge_ms)
    return {
        "total_ms": _rounded(elapsed_ms + serialization_ms),
        "mcp_queue_ms": _rounded(float(state["mcp_queue_ms"])),
        "snapshot_query_ms": _rounded(snapshot_ms),
        "bridge_connect_ms": _rounded(float(stages["bridge_connect_ms"])),
        "bridge_wait_ms": _rounded(float(stages["bridge_wait_ms"])),
        "editor_dispatch_ms": _rounded(float(stages["editor_dispatch_ms"])),
        "editor_execute_ms": _rounded(float(stages["editor_execute_ms"])),
        "serialization_ms": _rounded(serialization_ms),
    }


def attach_timing(envelope: dict[str, Any]) -> None:
    state = _CURRENT.get()
    if state is None:
        return
    envelope["timing"] = _timing_value(state)
    _attach_slow_diagnostic(envelope)


def finalize_timing(envelope: dict[str, Any], serialization_ms: float) -> None:
    state = _CURRENT.get()
    if state is None:
        return
    envelope["timing"] = _timing_value(state, serialization_ms=serialization_ms)
    _attach_slow_diagnostic(envelope)


def _attach_slow_diagnostic(envelope: dict[str, Any]) -> None:
    timing = envelope.get("timing")
    if not isinstance(timing, dict) or float(timing.get("total_ms") or 0.0) < _slow_threshold_ms():
        return
    diagnostics = envelope.setdefault("diagnostics", [])
    if not isinstance(diagnostics, list) or any(item.get("code") == "UEPI_SLOW_OPERATION" for item in diagnostics if isinstance(item, dict)):
        return
    diagnostics.append(
        {
            "severity": "warning",
            "blocking": False,
            "code": "UEPI_SLOW_OPERATION",
            "message": f"UEPI operation took {timing['total_ms']:.3f} ms, exceeding the {_slow_threshold_ms():.0f} ms threshold.",
            "retryable": True,
            "timing": dict(timing),
        }
    )
