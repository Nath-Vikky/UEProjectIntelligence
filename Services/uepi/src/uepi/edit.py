from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
from typing import Any
from uuid import uuid4

from .result import envelope
from .store import SnapshotStore


EDIT_PLAN_SCHEMA = "uepi.edit-plan.v1"
EDIT_AUDIT_SCHEMA = "uepi.edit-audit.v1"


def _now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _project(store: SnapshotStore) -> dict[str, Any]:
    try:
        state = store.load_state()
        project = state.project
    except Exception:
        state = None
        project = {}
    root = store.root.parent.parent if store.root.name == "UEProjectIntelligence" else store.root
    return {
        "id": project.get("id"),
        "name": project.get("name"),
        "engine_version": project.get("engine_version"),
        "project_root": str(root),
        "_state": state,
    }


def _state(store: SnapshotStore) -> dict[str, Any]:
    state = _project(store).get("_state")
    if state is not None:
        return state.envelope_state()
    return {
        "data_mode": "saved",
        "editor_connected": False,
        "saved_generation": None,
        "live_generation": None,
        "snapshot_observed_at": None,
        "freshness": "stale",
        "manifest_path": None,
    }


def _response(store: SnapshotStore, *, tool: str, operation: str, result: dict[str, Any] | None = None, error: dict[str, Any] | None = None, diagnostics: list[dict[str, Any]] | None = None, next_actions: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    project = _project(store)
    project.pop("_state", None)
    return envelope(
        tool=tool,
        operation=operation,
        project=project,
        state=_state(store),
        result=result,
        error=error,
        diagnostics=diagnostics,
        next_actions=next_actions,
    )


def _edit_dir(store: SnapshotStore) -> Path:
    path = store.root / "store" / "edit"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _write_audit(store: SnapshotStore, entry: dict[str, Any]) -> Path:
    audit_dir = _edit_dir(store)
    path = audit_dir / "audit.jsonl"
    record = {"schema_version": EDIT_AUDIT_SCHEMA, "created_at_utc": _now(), **entry}
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
    return path


def discover(store: SnapshotStore) -> dict[str, Any]:
    operations = [
        {
            "name": "blueprint.add_variable",
            "status": "planned",
            "preview_supported": True,
            "apply_supported": False,
            "required_evidence": ["uepi_blueprint.semantic_summary", "pin_guid_or_variable_name"],
        },
        {
            "name": "blueprint.add_function_call_node",
            "status": "planned",
            "preview_supported": True,
            "apply_supported": False,
            "required_evidence": ["uepi_blueprint", "target_function", "pin_names"],
        },
        {
            "name": "blueprint.connect_pins",
            "status": "planned",
            "preview_supported": True,
            "apply_supported": False,
            "required_evidence": ["source_node_guid", "source_pin_name", "target_node_guid", "target_pin_name"],
        },
        {
            "name": "asset.refresh_now",
            "status": "read_bridge_planned",
            "preview_supported": True,
            "apply_supported": False,
            "required_evidence": ["asset_object_path"],
        },
    ]
    return _response(
        store,
        tool="uepi_edit_discover",
        operation="discover",
        result={
            "schema_version": "uepi.edit-discover.v1",
            "profile": "codex_write_alpha",
            "default_enabled": False,
            "apply_enabled": False,
            "operations": operations,
            "safety_rules": [
                "No low-level write operation is exposed as an MCP tool.",
                "edit_preview must produce a transaction_id before edit_apply.",
                "edit_apply is disabled in this foundation build.",
                "Future apply requires user approval, scoped operations, backup artifacts, validation, rescan, and diff.",
            ],
        },
        next_actions=[
            {
                "reason": "Generate a dry-run operation plan from a user intent before any future apply.",
                "tool": "uepi_edit_preview",
                "arguments": {"intent": "<user requested edit>", "operations": []},
            }
        ],
    )


def preview(store: SnapshotStore, intent: str = "", operations: list[Any] | None = None, evidence: list[Any] | None = None) -> dict[str, Any]:
    transaction_id = f"uepi-preview-{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}-{uuid4().hex[:8]}"
    clean_operations = operations if isinstance(operations, list) else []
    plan = {
        "schema_version": EDIT_PLAN_SCHEMA,
        "transaction_id": transaction_id,
        "created_at_utc": _now(),
        "status": "preview_only",
        "intent": intent,
        "operations": clean_operations,
        "evidence": evidence if isinstance(evidence, list) else [],
        "requires_user_approval": True,
        "apply_supported": False,
        "validation_plan": [
            "Open editor bridge session.",
            "Run scoped transaction.",
            "Compile or validate touched assets.",
            "Run targeted Snapshot refresh.",
            "Return uepi_diff against the previous generation.",
        ],
        "rejection_reasons": ["edit_apply is intentionally disabled in this foundation build."],
    }
    path = _edit_dir(store) / f"{transaction_id}.json"
    path.write_text(json.dumps(plan, ensure_ascii=False, indent=2), encoding="utf-8")
    audit_path = _write_audit(store, {"event": "preview_created", "transaction_id": transaction_id, "plan_path": str(path)})
    return _response(
        store,
        tool="uepi_edit_preview",
        operation="preview",
        result={"plan": plan, "plan_path": str(path), "audit_path": str(audit_path)},
        next_actions=[
            {
                "reason": "Review the preview with the user. This build will reject apply.",
                "tool": "uepi_edit_apply",
                "arguments": {"transaction_id": transaction_id, "approved": True},
            }
        ],
    )


def reject_apply(store: SnapshotStore, transaction_id: str = "") -> dict[str, Any]:
    audit_path = _write_audit(store, {"event": "apply_rejected", "transaction_id": transaction_id, "reason": "edit_apply_disabled"})
    return _response(
        store,
        tool="uepi_edit_apply",
        operation="apply",
        error={
            "code": "UEPI_EDIT_APPLY_DISABLED",
            "message": "codex_write_alpha apply is not enabled in this foundation build.",
            "retryable": False,
            "candidates": [],
        },
        diagnostics=[
            {
                "severity": "warning",
                "code": "UEPI_EDIT_APPLY_DISABLED",
                "message": "Only edit discovery and preview are available. No Unreal asset was modified.",
                "audit_path": str(audit_path),
                "recoverable": False,
                "recommended_user_action": "Use the read-only tools or wait for a build with explicit write support.",
                "recommended_agent_action": {"tool": "uepi_edit_discover"},
            }
        ],
        next_actions=[],
    )


def validate(store: SnapshotStore, transaction_id: str = "") -> dict[str, Any]:
    return _response(
        store,
        tool="uepi_edit_validate",
        operation="validate",
        error={
            "code": "UEPI_EDIT_NOT_APPLIED",
            "message": "No editable transaction can be validated because apply is disabled.",
            "retryable": False,
            "candidates": [],
        },
        next_actions=[{"reason": "Use uepi_diff for saved Snapshot generation comparisons.", "tool": "uepi_diff", "arguments": {}}],
    )


def rollback(store: SnapshotStore, transaction_id: str = "") -> dict[str, Any]:
    return _response(
        store,
        tool="uepi_edit_rollback",
        operation="rollback",
        error={
            "code": "UEPI_EDIT_ROLLBACK_UNAVAILABLE",
            "message": "No UEPI edit transaction has been applied, so rollback is unavailable.",
            "retryable": False,
            "candidates": [],
        },
        next_actions=[],
    )
