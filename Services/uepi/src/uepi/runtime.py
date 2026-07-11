from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
import time
from typing import Any

from .bridge_client import call_bridge


MUTATING_ACTIONS = {"start", "stop", "input", "invoke"}


def _ticket_path(store: Any, ticket_id: str) -> Path:
    return store.store_dir / "runtime" / f"{ticket_id.replace(':', '-')}.json"


def load_ticket(engine: Any, ticket_id: str) -> tuple[dict[str, Any] | None, dict[str, Any] | None]:
    try:
        ticket = json.loads(_ticket_path(engine.store, ticket_id).read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return None, {"code": "UEPI_RUNTIME_TICKET_NOT_FOUND", "message": "Runtime verification ticket was not found."}
    if not isinstance(ticket, dict) or ticket.get("schema_version") != "uepi.runtime-ticket.v1":
        return None, {"code": "UEPI_RUNTIME_TICKET_INVALID", "message": "Runtime verification ticket is invalid."}
    expires = datetime.fromisoformat(str(ticket.get("expires_at")).replace("Z", "+00:00"))
    if expires < datetime.now(timezone.utc):
        return None, {"code": "UEPI_RUNTIME_TICKET_EXPIRED", "message": "Runtime verification ticket expired."}
    if ticket.get("project_binding_id") != engine.identity.get("project_binding_id"):
        return None, {"code": "UEPI_PROJECT_BINDING_MISMATCH", "message": "Runtime ticket belongs to another project."}
    return ticket, None


def _bridge(engine: Any, action: str, arguments: dict[str, Any], ticket: dict[str, Any] | None) -> dict[str, Any]:
    params = dict(arguments)
    params["action"] = action
    return call_bridge(
        engine.store,
        "runtime.control",
        params,
        timeout=10.0,
        expected_identity=engine.identity,
        expected_editor_session_id=str((ticket or {}).get("editor_session_id") or arguments.get("expected_editor_session_id") or "") or None,
    )


def execute(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    action = str(arguments.get("action") or "status")
    ticket: dict[str, Any] | None = None
    if action != "status":
        ticket, error = load_ticket(engine, str(arguments.get("ticket_id") or ""))
        if error:
            return engine._error(error["code"], error["message"], tool="uepi_runtime", operation=action)
        if action not in set(ticket.get("allowed_actions") or []):
            return engine._error("UEPI_RUNTIME_ACTION_NOT_APPROVED", f"Runtime action is not approved by the ticket: {action}", tool="uepi_runtime", operation=action)

    if action in {"status", "start", "stop", "input", "invoke", "read"}:
        response = _bridge(engine, action, arguments, ticket)
        if not response.get("ok"):
            error = response.get("error") if isinstance(response.get("error"), dict) else {}
            code = str(error.get("code") or next((item.get("code") for item in response.get("diagnostics") or [] if isinstance(item, dict)), "UEPI_RUNTIME_ACTION_FAILED"))
            return engine._error(code, str(error.get("message") or f"Runtime action failed: {action}"), diagnostics=response.get("diagnostics") if isinstance(response.get("diagnostics"), list) else [], tool="uepi_runtime", operation=action)
        return engine._envelope(response.get("result") if isinstance(response.get("result"), dict) else {}, diagnostics=response.get("diagnostics") or [], tool="uepi_runtime", operation=action)

    if action in {"wait", "assert"}:
        timeout = max(0.0, min(float(arguments.get("timeout_seconds") or 10.0), 120.0))
        interval = max(0.05, min(float(arguments.get("poll_interval_ms") or 100) / 1000.0, 2.0))
        expected = arguments.get("equals")
        deadline = time.monotonic() + timeout
        last: dict[str, Any] = {}
        while time.monotonic() <= deadline:
            response = _bridge(engine, "read", arguments, ticket)
            if response.get("ok") and isinstance(response.get("result"), dict):
                last = response["result"]
                if last.get("value") == expected:
                    return engine._envelope({"matched": True, "assertion": action == "assert", "observed": last}, tool="uepi_runtime", operation=action)
            time.sleep(interval)
        code = "UEPI_RUNTIME_ASSERT_FAILED" if action == "assert" else "UEPI_RUNTIME_WAIT_TIMEOUT"
        return engine._error(code, f"Runtime condition did not match before timeout; last value: {last.get('value')!r}", tool="uepi_runtime", operation=action)

    return engine._error("UEPI_RUNTIME_ACTION_UNSUPPORTED", f"Runtime action is not implemented: {action}", tool="uepi_runtime", operation=action)
