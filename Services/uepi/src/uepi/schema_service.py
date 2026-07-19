from __future__ import annotations

from typing import Any

from .bridge_client import call_bridge
from .operation_catalog import load_catalog, operation_map


def execute(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    action = str(arguments.get("action") or "class_property")
    if action == "edit_operation":
        catalog, diagnostics, _ = load_catalog(engine.store, engine.identity, refresh=True)
        query_value = str(arguments.get("query") or "")
        kind_value = str(arguments.get("kind") or "")
        if query_value and kind_value and query_value != kind_value:
            return engine._error("UEPI_SCHEMA_OPERATION_QUERY_CONFLICT", "For action=edit_operation, query and kind must match when both are supplied.", diagnostics=diagnostics, tool="uepi_schema", operation=action)
        query = query_value or kind_value
        if not query:
            return engine._error("UEPI_SCHEMA_OPERATION_QUERY_REQUIRED", "For action=edit_operation, provide the operation name in query or kind.", diagnostics=diagnostics, tool="uepi_schema", operation=action)
        descriptor = operation_map(catalog).get(query)
        if not descriptor:
            return engine._error("UEPI_SCHEMA_OPERATION_NOT_FOUND", f"Operation descriptor was not found: {query}", diagnostics=diagnostics, tool="uepi_schema", operation=action)
        return engine._envelope({"operation": descriptor, "catalog_hash": (catalog or {}).get("catalog_hash")}, diagnostics=diagnostics, tool="uepi_schema", operation=action)
    if action not in {"asset_property", "class_property", "blueprint_node", "runtime_function"}:
        return engine._error("UEPI_SCHEMA_ACTION_UNSUPPORTED", f"Unsupported schema action: {action}", tool="uepi_schema", operation=action)
    response = call_bridge(engine.store, "schema.get", dict(arguments), timeout=5.0, expected_identity=engine.identity, expected_editor_session_id=str(arguments.get("expected_editor_session_id") or "") or None)
    if not response.get("ok"):
        error = response.get("error") if isinstance(response.get("error"), dict) else {}
        code = str(error.get("code") or next((item.get("code") for item in response.get("diagnostics") or [] if isinstance(item, dict)), "UEPI_SCHEMA_READ_FAILED"))
        return engine._error(code, str(error.get("message") or "Editor reflection schema read failed."), diagnostics=response.get("diagnostics") if isinstance(response.get("diagnostics"), list) else [], tool="uepi_schema", operation=action)
    return engine._envelope(response.get("result") if isinstance(response.get("result"), dict) else {}, diagnostics=response.get("diagnostics") or [], tool="uepi_schema", operation=action)
