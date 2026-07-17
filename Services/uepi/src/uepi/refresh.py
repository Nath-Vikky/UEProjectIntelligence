from __future__ import annotations

import json
from pathlib import Path
import time
from typing import Any


TERMINAL_STATUSES = {"succeeded", "completed", "failed", "cancelled", "expired", "aborted"}


def _load(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def request_payload(path: Path | str | None) -> dict[str, Any]:
    request_path = Path(path) if path else None
    request = _load(request_path) if request_path else None
    return {
        "request": request or {},
        "request_id": str((request or {}).get("request_id") or "") or None,
        "request_path": str(request_path) if request_path else None,
    }


def _find_request(engine: Any, request_id: str) -> tuple[Path | None, dict[str, Any] | None]:
    if not request_id:
        return None, None
    for path in sorted(engine.store.requests_dir.glob("*.json")):
        value = _load(path)
        if value and str(value.get("request_id") or "") == request_id:
            return path, value
    return None, None


def execute(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    action = str(arguments.get("action") or "status")
    if action == "request":
        targets = [str(item) for item in arguments.get("targets") or [] if str(item).strip()]
        if not targets:
            return engine._error("UEPI_REFRESH_TARGET_REQUIRED", "Refresh request requires at least one exact target.", tool="uepi_refresh", operation=action)
        path = engine.store.request_refresh(
            targets,
            reason=str(arguments.get("reason") or "mcp_targeted_refresh"),
            tool_name="uepi_refresh",
            data_mode=str(arguments.get("data_mode") or "live"),
            domains=[str(item) for item in arguments.get("domains") or []],
            artifacts=[str(item) for item in arguments.get("artifacts") or []],
            project_binding_id=str(engine.identity.get("project_binding_id") or ""),
            editor_session_id=str(arguments.get("expected_editor_session_id") or ""),
        )
        return engine._envelope(request_payload(path), tool="uepi_refresh", operation=action)

    request_id = str(arguments.get("request_id") or "")
    if not request_id:
        return engine._error("UEPI_REFRESH_REQUEST_ID_REQUIRED", "Refresh status/wait requires request_id.", tool="uepi_refresh", operation=action)
    if action == "wait":
        timeout = max(0.0, min(float(arguments.get("timeout_seconds") or 30.0), 120.0))
        interval = max(0.05, min(float(arguments.get("poll_interval_ms") or 250) / 1000.0, 2.0))
        deadline = time.monotonic() + timeout
        while True:
            path, request = _find_request(engine, request_id)
            status = str((request or {}).get("status") or "").casefold()
            if request and status in TERMINAL_STATUSES:
                return engine._envelope(request_payload(path), tool="uepi_refresh", operation=action)
            if time.monotonic() >= deadline:
                return engine._envelope(
                    {"request": request, "request_path": str(path) if path else None, "timed_out": True},
                    diagnostics=[{"severity": "warning", "blocking": False, "code": "UEPI_REFRESH_WAIT_TIMEOUT", "message": "Refresh job did not reach a terminal state before the wait timeout.", "phase": "refresh_wait", "retryable": True, "recoverable": True}],
                    tool="uepi_refresh",
                    operation=action,
                )
            time.sleep(interval)
    if action == "status":
        path, request = _find_request(engine, request_id)
        if not request:
            return engine._error("UEPI_REFRESH_REQUEST_NOT_FOUND", "Refresh request was not found.", tool="uepi_refresh", operation=action)
        return engine._envelope(request_payload(path), tool="uepi_refresh", operation=action)
    return engine._error("UEPI_REFRESH_ACTION_UNKNOWN", f"Unknown refresh action: {action}", tool="uepi_refresh", operation=action)
