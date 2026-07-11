from __future__ import annotations

import re
from typing import Any

from .bridge_client import call_bridge
from .status import resolve_status


def _bridge_error(engine: Any, action: str, response: dict[str, Any]) -> dict[str, Any]:
    error = response.get("error") if isinstance(response.get("error"), dict) else {}
    diagnostics = response.get("diagnostics") if isinstance(response.get("diagnostics"), list) else []
    return engine._error(
        str(error.get("code") or "UEPI_EDITOR_READ_FAILED"),
        str(error.get("message") or f"Editor action failed: {action}"),
        diagnostics=diagnostics,
        tool="uepi_editor",
        operation=action,
    )


def execute(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    action = str(arguments.get("action") or "status")
    expected_session = str(arguments.get("expected_editor_session_id") or "") or None
    if action == "status":
        resolved = resolve_status(engine.store, engine.state, engine.identity, probe_bridge=True, expected_editor_session_id=expected_session)
        return engine._envelope(
            {"editor": resolved["editor"], "capabilities": resolved["capabilities"], "doctor": resolved["doctor"]},
            diagnostics=resolved["diagnostics"],
            tool="uepi_editor",
            operation="status",
        )

    command = {
        "selection": "editor.get_selection",
        "output_log": "editor.read_output_log",
        "viewport_capture": "editor.capture_viewport",
    }.get(action)
    if not command:
        return engine._error("UEPI_EDITOR_ACTION_UNKNOWN", f"Unknown editor action: {action}", tool="uepi_editor", operation=action)

    params = dict(arguments)
    if action == "output_log":
        params["line_limit"] = max(1, min(int(arguments.get("max_lines") or 200), 2000))
    response = call_bridge(engine.store, command, params, timeout=10.0 if action == "viewport_capture" else 2.0, expected_identity=engine.identity, expected_editor_session_id=expected_session)
    if not response.get("ok"):
        return _bridge_error(engine, action, response)
    result = response.get("result") if isinstance(response.get("result"), dict) else {}

    if action == "output_log":
        lines = [str(item) for item in result.get("lines") or []]
        categories = [str(item) for item in arguments.get("categories") or []]
        pattern = str(arguments.get("regex") or "")
        if categories:
            lines = [line for line in lines if any(category.casefold() in line.casefold() for category in categories)]
        if pattern:
            try:
                compiled = re.compile(pattern)
                lines = [line for line in lines if compiled.search(line)]
            except re.error as exc:
                return engine._error("UEPI_EDITOR_LOG_REGEX_INVALID", str(exc), tool="uepi_editor", operation=action)
        result["lines"] = lines
        result["line_count"] = len(lines)

    omissions: list[str] = []
    if action == "viewport_capture" and arguments.get("include_ui"):
        omissions.append("slate_ui_not_included_in_viewport_pixel_capture")
    return engine._envelope(result, diagnostics=response.get("diagnostics") or [], omissions=omissions, tool="uepi_editor", operation=action)
