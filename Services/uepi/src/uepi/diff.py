from __future__ import annotations

import json
from pathlib import Path
from typing import Any


def _load(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def transaction_diff(store: Any, transaction_id: str) -> dict[str, Any] | None:
    safe = transaction_id.replace(":", "-")
    result = _load(store.store_dir / "edit" / f"{safe}.result.json")
    plan = _load(store.store_dir / "edit" / f"{safe}.plan.json")
    if not result or not plan:
        return None
    if isinstance(result.get("transaction_diff"), dict):
        return result["transaction_diff"]
    return build_transaction_diff(plan, result)


def build_transaction_diff(plan: dict[str, Any], apply_result: dict[str, Any], after_fingerprints: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    properties: list[dict[str, Any]] = []
    nodes: list[dict[str, Any]] = []
    links: list[dict[str, Any]] = []
    created_assets: list[str] = []
    operation_types = {str(item.get("operation_id")): str(item.get("type")) for item in plan.get("operations") or [] if isinstance(item, dict)}
    for index, item in enumerate(apply_result.get("operations") or []):
        if not isinstance(item, dict):
            continue
        detail = item.get("detail") if isinstance(item.get("detail"), dict) else {}
        properties.extend(child for child in detail.get("property_diff") or [] if isinstance(child, dict))
        if isinstance(detail.get("node"), dict):
            nodes.append({"change": "added", "node": detail["node"]})
        if isinstance(detail.get("source_node"), dict) and isinstance(detail.get("target_node"), dict):
            links.append({"change": "connected", "source_node": detail["source_node"].get("node_guid"), "target_node": detail["target_node"].get("node_guid")})
        op_type = str(item.get("type") or operation_types.get(f"op:{index + 1}") or "")
        if op_type in {"content.create_asset", "material.create_instance", "widget.create", "input.create_action", "input.create_mapping_context"}:
            asset = detail.get("asset")
            if isinstance(asset, dict):
                asset = asset.get("path")
            if isinstance(asset, str):
                created_assets.append(asset)
    return {
        "schema_version": "uepi.transaction-diff.v1",
        "transaction_id": plan.get("transaction_id"),
        "plan_hash": plan.get("plan_hash"),
        "created_assets": sorted(set(created_assets)),
        "removed_assets": [],
        "property_changes": properties,
        "node_changes": nodes,
        "link_changes": links,
        "before_fingerprints": plan.get("before_fingerprints") or [],
        "after_fingerprints": after_fingerprints or [],
        "saved": bool(apply_result.get("saved")),
        "saved_file_hashes": apply_result.get("saved_file_hashes") or [],
        "validation_ok": bool(apply_result.get("validation_ok")),
    }


def execute(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    transaction_id = str(arguments.get("transaction_id") or "")
    value = transaction_diff(engine.store, transaction_id)
    if not value:
        return engine._error("UEPI_DIFF_TRANSACTION_NOT_FOUND", "Transaction diff was not found.", tool="uepi_diff", operation="transaction_diff")
    return engine._envelope(value, tool="uepi_diff", operation="transaction_diff")
