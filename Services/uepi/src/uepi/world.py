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
    filters = arguments.get("filters") if isinstance(arguments.get("filters"), dict) else {}
    unknown_filters = sorted(set(filters) - {"class_paths", "labels", "object_paths"})
    if unknown_filters:
        return engine._error(
            "UEPI_WORLD_FILTER_UNKNOWN",
            f"Unsupported world filter keys: {', '.join(unknown_filters)}",
            candidates=["class_paths", "labels", "object_paths"],
            tool="uepi_world",
            operation=action,
        )
    actor_path = str(arguments.get("actor") or "")
    if not actor_path and len(filters.get("object_paths") or []) == 1:
        actor_path = str(filters["object_paths"][0])
        arguments = {**arguments, "actor": actor_path}
    if action in {"actor", "component"} and not actor_path:
        return engine._error("UEPI_WORLD_ACTOR_REQUIRED", f"World action {action} requires an exact actor path.", tool="uepi_world", operation=action)
    if action == "component" and not str(arguments.get("component") or ""):
        return engine._error("UEPI_WORLD_COMPONENT_REQUIRED", "World action component requires an exact component path or name.", tool="uepi_world", operation=action)
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
