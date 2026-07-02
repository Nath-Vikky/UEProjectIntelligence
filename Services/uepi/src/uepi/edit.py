from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
from typing import Any
from uuid import uuid4

from .bridge_client import call_bridge
from .result import envelope
from .store import SnapshotStore


EDIT_PLAN_SCHEMA = "uepi.edit_plan.v1"
EDIT_AUDIT_SCHEMA = "uepi.edit-audit.v1"
BACKUP_ARTIFACT_SCHEMA = "uepi.backup-artifact.v1"
MAX_BACKUP_FILE_BYTES = 64 * 1024 * 1024


SUPPORTED_PREVIEW_OPERATIONS = {
    "blueprint.add_variable": "low",
    "blueprint.set_variable_default": "low",
    "blueprint.add_component": "low",
    "blueprint.set_component_property": "low",
    "blueprint.create_function": "medium",
    "blueprint.add_event_node": "medium",
    "blueprint.add_function_call_node": "medium",
    "blueprint.add_variable_get_node": "medium",
    "blueprint.add_variable_set_node": "medium",
    "blueprint.add_branch_node": "medium",
    "blueprint.add_print_string_node": "medium",
    "blueprint.connect_pins": "medium",
    "blueprint.compile": "low",
    "actor.spawn": "medium",
    "actor.set_transform": "medium",
    "actor.set_property": "medium",
    "material.create_instance": "medium",
    "material.set_scalar_parameter": "low",
    "material.set_vector_parameter": "low",
    "material.set_texture_parameter": "low",
    "material.apply_to_actor": "medium",
    "material.apply_to_blueprint_component": "medium",
    "widget.create": "medium",
    "widget.add_text": "medium",
    "widget.add_button": "medium",
    "widget.set_slot": "medium",
    "widget.bind_button_to_custom_event": "medium",
    "input.create_action": "medium",
    "input.create_mapping_context": "medium",
    "input.add_key_mapping": "medium",
    "input.remove_key_mapping": "medium",
    "content.create_folder": "medium",
    "content.duplicate_asset": "medium",
    "content.rename_asset": "medium",
    "content.import": "medium",
}

BLOCKED_OPERATIONS = {
    "run_python",
    "run_console_command",
    "execute_shell",
    "enable_plugin",
    "disable_plugin",
    "delete_asset",
    "delete_actor",
    "save_all",
    "source_control_submit",
    "pie.start",
    "pie.stop",
    "project.config_write",
}


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


def _project_root(store: SnapshotStore) -> Path:
    if store.root.name == "UEProjectIntelligence" and store.root.parent.name == "Saved":
        return store.root.parent.parent
    return store.root


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
    path = store.root / "store" / "edit" / "plans"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _audit_dir(store: SnapshotStore) -> Path:
    path = store.root / "audit"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _backup_root(store: SnapshotStore) -> Path:
    path = store.root / "artifacts" / "backups"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _safe_file_stem(value: str) -> str:
    clean = []
    for char in value:
        clean.append(char if char.isalnum() or char in {".", "-", "_"} else "_")
    stem = "".join(clean).strip("._")
    return stem[:120] or "asset"


def _write_audit(store: SnapshotStore, entry: dict[str, Any]) -> Path:
    path = _audit_dir(store) / f"edit-{datetime.now(timezone.utc).strftime('%Y%m%d')}.jsonl"
    record = {"schema_version": EDIT_AUDIT_SCHEMA, "created_at_utc": _now(), **entry}
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
    return path


def _operation_type(operation: Any) -> str:
    if not isinstance(operation, dict):
        return "invalid"
    value = operation.get("type") or operation.get("operation") or operation.get("name")
    return str(value or "").strip()


def _operation_params(operation: Any) -> dict[str, Any]:
    if not isinstance(operation, dict):
        return {}
    params = operation.get("params")
    return params if isinstance(params, dict) else operation


def _walk_asset_values(value: Any) -> list[str]:
    found: list[str] = []
    if isinstance(value, str):
        stripped = value.strip()
        if stripped.startswith(("/Game/", "/Engine/")):
            found.append(stripped)
        return found
    if isinstance(value, list):
        for item in value:
            found.extend(_walk_asset_values(item))
        return found
    if isinstance(value, dict):
        for key, item in value.items():
            key_text = str(key).casefold()
            if any(token in key_text for token in ("asset", "object_path", "blueprint", "package", "target")):
                found.extend(_walk_asset_values(item))
            elif isinstance(item, (dict, list)):
                found.extend(_walk_asset_values(item))
        return found
    return found


def _extract_affected_assets(operations: list[Any], evidence: list[Any]) -> list[str]:
    assets: list[str] = []
    for item in operations:
        assets.extend(_walk_asset_values(item))
    for item in evidence:
        assets.extend(_walk_asset_values(item))

    clean: list[str] = []
    for asset in assets:
        normalized = asset.strip()
        if not normalized:
            continue
        if normalized not in clean:
            clean.append(normalized)
    return clean


def _risk_level(operations: list[Any]) -> str:
    rank = {"none": 0, "low": 1, "medium": 2, "high": 3}
    current = "none"
    for operation in operations:
        op_type = _operation_type(operation)
        if not op_type or op_type in BLOCKED_OPERATIONS:
            op_risk = "high"
        else:
            op_risk = SUPPORTED_PREVIEW_OPERATIONS.get(op_type, "medium")
        if rank[op_risk] > rank[current]:
            current = op_risk
    return current


def _operation_diagnostics(operations: list[Any]) -> list[dict[str, Any]]:
    diagnostics: list[dict[str, Any]] = []
    for index, operation in enumerate(operations):
        op_type = _operation_type(operation)
        if not isinstance(operation, dict) or not op_type:
            diagnostics.append(
                {
                    "severity": "error",
                    "code": "UEPI_EDIT_OPERATION_INVALID",
                    "message": f"Operation at index {index} is not a structured operation with a type.",
                    "operation_index": index,
                    "recoverable": True,
                }
            )
            continue
        if op_type in BLOCKED_OPERATIONS:
            diagnostics.append(
                {
                    "severity": "error",
                    "code": "UEPI_EDIT_OPERATION_BLOCKED",
                    "message": f"Operation is forbidden by UEPI write safety policy: {op_type}",
                    "operation_index": index,
                    "operation_type": op_type,
                    "recoverable": False,
                }
            )
            continue
        if op_type not in SUPPORTED_PREVIEW_OPERATIONS:
            diagnostics.append(
                {
                    "severity": "warning",
                    "code": "UEPI_EDIT_OPERATION_PREVIEW_ONLY_UNKNOWN",
                    "message": f"Operation can be recorded in the plan, but UEPI has no alpha executor for it yet: {op_type}",
                    "operation_index": index,
                    "operation_type": op_type,
                    "recoverable": True,
                }
            )
    return diagnostics


def _object_path_to_package_file(store: SnapshotStore, object_path: str) -> Path | None:
    if not object_path.startswith("/Game/"):
        return None
    package = object_path.split(".", 1)[0]
    relative = package.removeprefix("/Game/").replace("/", "\\")
    return (_project_root(store) / "Content" / f"{relative}.uasset").resolve()


def _write_backup_artifact(store: SnapshotStore, transaction_id: str, affected_assets: list[str]) -> dict[str, Any]:
    artifact_dir = _backup_root(store) / transaction_id
    artifact_dir.mkdir(parents=True, exist_ok=True)
    project_root = _project_root(store).resolve()
    files: list[dict[str, Any]] = []
    for asset in affected_assets:
        source = _object_path_to_package_file(store, asset)
        if source is None:
            files.append({"asset": asset, "status": "unsupported_path", "reason": "Only /Game package paths can be mapped without the editor."})
            continue
        try:
            source.relative_to(project_root)
        except ValueError:
            files.append({"asset": asset, "source": str(source), "status": "outside_project"})
            continue
        if not source.exists():
            files.append({"asset": asset, "source": str(source), "status": "missing"})
            continue
        size = source.stat().st_size
        if size > MAX_BACKUP_FILE_BYTES:
            files.append({"asset": asset, "source": str(source), "size_bytes": size, "status": "too_large"})
            continue
        destination = artifact_dir / _safe_file_stem(source.relative_to(project_root).as_posix())
        shutil.copy2(source, destination)
        files.append(
            {
                "asset": asset,
                "source": str(source),
                "backup_path": str(destination),
                "size_bytes": size,
                "status": "copied",
            }
        )

    manifest = {
        "schema_version": BACKUP_ARTIFACT_SCHEMA,
        "transaction_id": transaction_id,
        "created_at_utc": _now(),
        "artifact_uri": f"uepi://artifact/backups/{transaction_id}",
        "artifact_directory": str(artifact_dir),
        "affected_assets": affected_assets,
        "files": files,
    }
    manifest_path = artifact_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    return {
        "required": True,
        "artifact_uri": manifest["artifact_uri"],
        "artifact_directory": str(artifact_dir),
        "manifest_path": str(manifest_path),
        "file_count": len(files),
        "copied_file_count": sum(1 for item in files if item.get("status") == "copied"),
    }


def _load_plan(store: SnapshotStore, transaction_id: str) -> dict[str, Any] | None:
    if not transaction_id:
        return None
    path = _edit_dir(store) / f"{transaction_id}.json"
    if not path.exists():
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def discover(store: SnapshotStore) -> dict[str, Any]:
    bridge_discover = call_bridge(store, "edit.discover", timeout=1.5)
    bridge_operations: dict[str, dict[str, Any]] = {}
    if bridge_discover.get("ok"):
        for operation in (bridge_discover.get("result") or {}).get("operations") or []:
            if isinstance(operation, dict) and isinstance(operation.get("name"), str):
                bridge_operations[operation["name"]] = operation

    operations = []
    for name, risk in SUPPORTED_PREVIEW_OPERATIONS.items():
        bridge_operation = bridge_operations.get(name, {})
        apply_supported = bool(bridge_operation.get("apply_supported"))
        operations.append(
            {
                "name": name,
                "status": bridge_operation.get("status") or ("alpha_apply" if apply_supported else "preview_supported"),
                "risk": risk,
                "preview_supported": True,
                "apply_supported": apply_supported,
                "required_evidence": ["uepi_status", "uepi_context_or_domain_read", "affected_asset_path"],
            }
        )
    apply_enabled = any(item.get("apply_supported") for item in operations)
    return _response(
        store,
        tool="uepi_edit_discover",
        operation="discover",
        result={
            "schema_version": "uepi.edit-discover.v1",
            "profile": "codex",
            "legacy_profile_alias": "codex_write_alpha",
            "default_enabled": False,
            "apply_enabled": apply_enabled,
            "bridge": {
                "connected": bool(bridge_discover.get("ok")),
                "discover": bridge_discover,
            },
            "plan_schema_version": EDIT_PLAN_SCHEMA,
            "operations": operations,
            "safety_rules": [
                "No low-level write operation is exposed as an MCP tool.",
                "edit_preview must produce a transaction_id before edit_apply.",
                "edit_apply requires the live editor bridge, explicit write settings, and approved=true.",
                "Forbidden operations include arbitrary Python, console commands, deletes, save_all, PIE, config writes, and source-control submit.",
                "Apply requires user approval, scoped operations, backup artifacts, validation, rescan, and diff.",
            ],
        },
        next_actions=[
            {
                "reason": "Generate a dry-run operation plan from a user intent before requesting approval.",
                "tool": "uepi_edit_preview",
                "arguments": {"intent": "<user requested edit>", "operations": []},
            }
        ],
    )


def preview(store: SnapshotStore, intent: str = "", operations: list[Any] | None = None, evidence: list[Any] | None = None) -> dict[str, Any]:
    transaction_id = f"uepi-preview-{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}-{uuid4().hex[:8]}"
    clean_operations = operations if isinstance(operations, list) else []
    clean_evidence = evidence if isinstance(evidence, list) else []
    affected_assets = _extract_affected_assets(clean_operations, clean_evidence)
    diagnostics = _operation_diagnostics(clean_operations)
    backup = _write_backup_artifact(store, transaction_id, affected_assets)
    risk_level = _risk_level(clean_operations)
    bridge_discover = call_bridge(store, "edit.discover", timeout=1.5)
    supported_apply = set()
    if bridge_discover.get("ok"):
        for operation in (bridge_discover.get("result") or {}).get("operations") or []:
            if isinstance(operation, dict) and operation.get("apply_supported") and isinstance(operation.get("name"), str):
                supported_apply.add(operation["name"])
    plan_operation_types = [_operation_type(operation) for operation in clean_operations]
    apply_supported = bool(clean_operations) and bool(supported_apply) and all(op_type in supported_apply for op_type in plan_operation_types)
    apply_reasons = [] if apply_supported else ["Bridge apply is unavailable or the plan includes operations without alpha executors."]
    if any(item.get("severity") == "error" for item in diagnostics):
        apply_reasons.append("The plan includes invalid or blocked operations.")
    plan = {
        "schema_version": EDIT_PLAN_SCHEMA,
        "schema_aliases": ["uepi.edit-plan.v1"],
        "transaction_id": transaction_id,
        "created_at_utc": _now(),
        "status": "preview_only",
        "intent": intent,
        "operations": clean_operations,
        "evidence": clean_evidence,
        "affected_assets": affected_assets,
        "risk": {
            "level": risk_level,
            "requires_user_approval": True,
            "blocked_operation_count": sum(1 for item in diagnostics if item.get("code") == "UEPI_EDIT_OPERATION_BLOCKED"),
        },
        "safety": {
            "dry_run": True,
            "mutates_unreal_assets": False,
            "forbidden_operations": sorted(BLOCKED_OPERATIONS),
            "requires_editor_bridge": True,
            "allow_saving": False,
        },
        "bridge": {
            "connected": bool(bridge_discover.get("ok")),
            "discover": bridge_discover,
        },
        "backup": backup,
        "requires_user_approval": True,
        "apply_supported": apply_supported,
        "validation_plan": [
            "Open editor bridge session.",
            "Run scoped transaction.",
            "Compile or validate touched assets.",
            "Run targeted Snapshot refresh.",
            "Return uepi_diff against the previous generation.",
        ],
        "rollback_plan": {
            "strategy": "transaction_undo_or_backup_restore",
            "backup_artifact": backup["artifact_uri"],
            "save_required_for_persistence": False,
        },
        "diagnostics": diagnostics,
        "rejection_reasons": apply_reasons,
    }
    path = _edit_dir(store) / f"{transaction_id}.json"
    path.write_text(json.dumps(plan, ensure_ascii=False, indent=2), encoding="utf-8")
    audit_path = _write_audit(
        store,
        {
            "event": "edit.preview",
            "transaction_id": transaction_id,
            "plan_path": str(path),
            "backup_artifact": backup["artifact_uri"],
            "affected_assets": affected_assets,
            "risk_level": risk_level,
        },
    )
    return _response(
        store,
        tool="uepi_edit_preview",
        operation="preview",
        result={"plan": plan, "plan_path": str(path), "audit_path": str(audit_path)},
        diagnostics=diagnostics,
        next_actions=[
            {
                "reason": "Review the preview with the user before any apply attempt.",
                "tool": "uepi_edit_apply",
                "arguments": {"transaction_id": transaction_id, "approved": True},
            }
        ],
    )


def reject_apply(store: SnapshotStore, transaction_id: str = "") -> dict[str, Any]:
    plan = _load_plan(store, transaction_id)
    audit_path = _write_audit(
        store,
        {
            "event": "edit.apply",
            "transaction_id": transaction_id,
            "status": "rejected",
            "reason": "edit_apply_disabled",
            "plan_found": bool(plan),
        },
    )
    return _response(
        store,
        tool="uepi_edit_apply",
        operation="apply",
        error={
            "code": "UEPI_EDIT_APPLY_DISABLED",
            "message": "UEPI edit apply requires the live editor bridge, user approval, and explicit UEPI write settings.",
            "retryable": False,
            "candidates": [],
        },
        diagnostics=[
            {
                "severity": "warning",
                "code": "UEPI_EDIT_APPLY_DISABLED",
                "message": "No Unreal asset was modified because edit apply is not enabled for this session.",
                "audit_path": str(audit_path),
                "plan_found": bool(plan),
                "recoverable": False,
                "recommended_user_action": "Enable the live editor bridge and explicit write flags only for a test project or sandbox.",
                "recommended_agent_action": {"tool": "uepi_edit_discover"},
            }
        ],
        next_actions=[],
    )


def apply(store: SnapshotStore, transaction_id: str = "", approved: bool = False) -> dict[str, Any]:
    plan = _load_plan(store, transaction_id)
    if plan is None:
        audit_path = _write_audit(
            store,
            {
                "event": "edit.apply",
                "transaction_id": transaction_id,
                "status": "rejected",
                "reason": "plan_not_found",
            },
        )
        return _response(
            store,
            tool="uepi_edit_apply",
            operation="apply",
            error={
                "code": "UEPI_EDIT_PLAN_NOT_FOUND",
                "message": "edit_apply requires a transaction_id produced by edit_preview.",
                "retryable": False,
                "candidates": [],
            },
            result={"audit_path": str(audit_path)},
        )
    if not approved:
        audit_path = _write_audit(
            store,
            {
                "event": "edit.apply",
                "transaction_id": transaction_id,
                "status": "rejected",
                "reason": "approval_missing",
            },
        )
        return _response(
            store,
            tool="uepi_edit_apply",
            operation="apply",
            error={
                "code": "UEPI_EDIT_APPROVAL_REQUIRED",
                "message": "edit_apply requires approved=true after user review.",
                "retryable": False,
                "candidates": [],
            },
            result={"plan": plan, "audit_path": str(audit_path)},
        )

    bridge_result = call_bridge(
        store,
        "edit.apply",
        {"transaction_id": transaction_id, "approved": True, "plan": plan},
        timeout=30.0,
    )
    audit_path = _write_audit(
        store,
        {
            "event": "edit.apply",
            "transaction_id": transaction_id,
            "status": "applied" if bridge_result.get("ok") else "failed",
            "bridge_ok": bool(bridge_result.get("ok")),
            "affected_assets": plan.get("affected_assets") if isinstance(plan.get("affected_assets"), list) else [],
        },
    )
    if not bridge_result.get("ok"):
        bridge_error = bridge_result.get("error") or {}
        bridge_code = str(bridge_error.get("code") or "")
        edit_code = "UEPI_EDIT_BRIDGE_REQUIRED" if bridge_code.startswith("UEPI_BRIDGE_") else bridge_code or "UEPI_EDIT_BRIDGE_APPLY_FAILED"
        return _response(
            store,
            tool="uepi_edit_apply",
            operation="apply",
            error={
                "code": edit_code,
                "message": bridge_error.get("message") or "Editor bridge rejected or failed edit.apply.",
                "bridge_error_code": bridge_code,
                "retryable": False,
                "candidates": [],
            },
            result={"plan": plan, "bridge": bridge_result, "audit_path": str(audit_path)},
            diagnostics=bridge_result.get("diagnostics") if isinstance(bridge_result.get("diagnostics"), list) else [],
        )
    return _response(
        store,
        tool="uepi_edit_apply",
        operation="apply",
        result={"plan": plan, "bridge": bridge_result, "audit_path": str(audit_path)},
        diagnostics=bridge_result.get("diagnostics") if isinstance(bridge_result.get("diagnostics"), list) else [],
        next_actions=[
            {
                "reason": "Validate compile status and refresh/diff evidence after apply.",
                "tool": "uepi_edit_validate",
                "arguments": {"transaction_id": transaction_id},
            }
        ],
    )


def validate(store: SnapshotStore, transaction_id: str = "") -> dict[str, Any]:
    plan = _load_plan(store, transaction_id)
    bridge_result = call_bridge(
        store,
        "edit.validate",
        {"transaction_id": transaction_id, "plan": plan} if plan is not None else {"transaction_id": transaction_id},
        timeout=30.0,
    )
    audit_path = _write_audit(
        store,
        {
            "event": "edit.validate",
            "transaction_id": transaction_id,
            "status": "validated" if bridge_result.get("ok") else "failed",
            "plan_found": bool(plan),
            "bridge_ok": bool(bridge_result.get("ok")),
        },
    )
    if bridge_result.get("ok"):
        return _response(
            store,
            tool="uepi_edit_validate",
            operation="validate",
            result={"plan_found": bool(plan), "bridge": bridge_result, "audit_path": str(audit_path)},
            diagnostics=bridge_result.get("diagnostics") if isinstance(bridge_result.get("diagnostics"), list) else [],
            next_actions=[{"reason": "Use uepi_diff after the targeted refresh has produced a new Snapshot generation.", "tool": "uepi_diff", "arguments": {}}],
        )
    return _response(
        store,
        tool="uepi_edit_validate",
        operation="validate",
        error={
            "code": (bridge_result.get("error") or {}).get("code") or "UEPI_EDIT_VALIDATE_UNAVAILABLE",
            "message": (bridge_result.get("error") or {}).get("message") or "Editor bridge validation is unavailable.",
            "retryable": False,
            "candidates": [],
        },
        result={"plan_found": bool(plan), "bridge": bridge_result, "audit_path": str(audit_path)},
        next_actions=[{"reason": "Use uepi_diff for saved Snapshot generation comparisons.", "tool": "uepi_diff", "arguments": {}}],
    )


def rollback(store: SnapshotStore, transaction_id: str = "") -> dict[str, Any]:
    plan = _load_plan(store, transaction_id)
    bridge_result = call_bridge(store, "edit.rollback", {"transaction_id": transaction_id}, timeout=10.0)
    audit_path = _write_audit(
        store,
        {
            "event": "edit.rollback",
            "transaction_id": transaction_id,
            "status": "rolled_back" if bridge_result.get("ok") else "failed",
            "plan_found": bool(plan),
            "bridge_ok": bool(bridge_result.get("ok")),
        },
    )
    if bridge_result.get("ok"):
        return _response(
            store,
            tool="uepi_edit_rollback",
            operation="rollback",
            result={"plan_found": bool(plan), "bridge": bridge_result, "audit_path": str(audit_path)},
        )
    return _response(
        store,
        tool="uepi_edit_rollback",
        operation="rollback",
        error={
            "code": (bridge_result.get("error") or {}).get("code") or "UEPI_EDIT_ROLLBACK_UNAVAILABLE",
            "message": (bridge_result.get("error") or {}).get("message") or "Editor bridge rollback is unavailable.",
            "retryable": False,
            "candidates": [],
        },
        result={"plan_found": bool(plan), "bridge": bridge_result, "audit_path": str(audit_path)},
        next_actions=[],
    )
