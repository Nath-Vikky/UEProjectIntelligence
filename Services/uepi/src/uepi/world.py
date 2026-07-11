from __future__ import annotations

from typing import Any

from .bridge_client import call_bridge


def execute(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    action = str(arguments.get("action") or "read")
    world = str(arguments.get("world") or "editor")
    if world not in {"editor", "pie"}:
        return engine._error("UEPI_WORLD_KIND_INVALID", f"Unsupported world: {world}", tool="uepi_world", operation=action)
    if action not in {"read", "actor", "component"}:
        return engine._error("UEPI_WORLD_ACTION_UNKNOWN", f"Unknown world action: {action}", tool="uepi_world", operation=action)
    response = call_bridge(
        engine.store,
        "editor.read_world",
        dict(arguments),
        timeout=3.0,
        expected_identity=engine.identity,
        expected_editor_session_id=str(arguments.get("expected_editor_session_id") or "") or None,
    )
    if not response.get("ok"):
        error = response.get("error") if isinstance(response.get("error"), dict) else {}
        return engine._error(
            str(error.get("code") or "UEPI_WORLD_READ_FAILED"),
            str(error.get("message") or "The live Editor world could not be read."),
            diagnostics=response.get("diagnostics") if isinstance(response.get("diagnostics"), list) else [],
            tool="uepi_world",
            operation=action,
        )
    result = response.get("result") if isinstance(response.get("result"), dict) else {}
    return engine._envelope(result, diagnostics=response.get("diagnostics") or [], tool="uepi_world", operation=action)
