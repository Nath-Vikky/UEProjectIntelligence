from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import time
from typing import Any
from uuid import uuid4

from .bridge_client import call_bridge
from .cpp_symbols import project_module_manifest
from .identity import project_identity
from .operation_catalog import load_catalog, operation_map
from .plan import PLAN_SCHEMA, canonical_plan_hash, verify_plan_hash
from .result import envelope
from .status import resolve_status
from .diff import build_transaction_diff
from .store import SnapshotStore


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _iso(value: datetime) -> str:
    return value.isoformat().replace("+00:00", "Z")


def _state(store: SnapshotStore):
    return store.load_state()


def _identity(store: SnapshotStore) -> dict[str, Any]:
    state = _state(store)
    return project_identity(None, state.project, store.root)


def _response(
    store: SnapshotStore,
    *,
    tool: str,
    operation: str,
    result: dict[str, Any] | None = None,
    error: dict[str, Any] | None = None,
    diagnostics: list[dict[str, Any]] | None = None,
    next_actions: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    state = _state(store)
    identity = project_identity(None, state.project, store.root)
    status = resolve_status(store, state, identity, probe_bridge=False)
    return envelope(
        tool=tool,
        operation=operation,
        project=identity,
        editor=status["editor"],
        state=state.envelope_state(),
        result=result,
        error=error,
        diagnostics=(diagnostics or []) + status["diagnostics"],
        next_actions=next_actions,
    )


def _edit_dir(store: SnapshotStore) -> Path:
    path = store.store_dir / "edit"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _plan_path(store: SnapshotStore, transaction_id: str) -> Path:
    return _edit_dir(store) / f"{transaction_id}.plan.json"


def _result_path(store: SnapshotStore, transaction_id: str) -> Path:
    return _edit_dir(store) / f"{transaction_id}.result.json"


def _audit(store: SnapshotStore, event: dict[str, Any]) -> str:
    directory = store.store_dir / "audit"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "edit-v2.jsonl"
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps({"at": _iso(_now()), **event}, ensure_ascii=False, separators=(",", ":")) + "\n")
    return str(path)


def _operation_parts(value: Any, index: int) -> tuple[str, str, dict[str, Any], list[str]]:
    item = value if isinstance(value, dict) else {}
    op_type = str(item.get("type") or item.get("operation") or "")
    op_id = str(item.get("operation_id") or item.get("id") or f"op:{index + 1}")
    params = item.get("params") if isinstance(item.get("params"), dict) else {key: child for key, child in item.items() if key not in {"type", "operation", "id", "operation_id", "depends_on"}}
    depends_on = [str(child) for child in item.get("depends_on") or []]
    return op_id, op_type, params, depends_on


def _target_values(params: dict[str, Any], descriptor: dict[str, Any]) -> list[str]:
    values: list[str] = []
    for field in descriptor.get("target_fields") or []:
        value = params.get(str(field))
        if isinstance(value, str) and value.startswith("/"):
            values.append(value)
        elif isinstance(value, list):
            values.extend(child for child in value if isinstance(child, str) and child.startswith("/"))
    return values


def _destination_asset(params: dict[str, Any]) -> str | None:
    direct = params.get("destination_asset")
    if isinstance(direct, str) and direct.startswith("/"):
        return direct
    destination = params.get("destination_path")
    name = params.get("name")
    if isinstance(destination, str) and destination.startswith("/") and isinstance(name, str) and name:
        package = destination.rstrip("/") + "/" + name
        return f"{package}.{name}"
    return None


def _referenced_operation(value: Any) -> str | None:
    if not isinstance(value, dict) or not isinstance(value.get("$ref"), str):
        return None
    return str(value["$ref"]).split("#", 1)[0] or None


def _operation_references(value: Any) -> set[str]:
    references: set[str] = set()
    if isinstance(value, dict):
        referenced = _referenced_operation(value)
        if referenced:
            references.add(referenced)
        for child in value.values():
            references.update(_operation_references(child))
    elif isinstance(value, list):
        for child in value:
            references.update(_operation_references(child))
    return references


def _package_file(store: SnapshotStore, object_path: str) -> Path | None:
    package = object_path.split(".", 1)[0]
    project_root = store.root.parent.parent if store.root.name == "UEProjectIntelligence" else store.root
    if package.startswith("/Game/"):
        return project_root / "Content" / (package.removeprefix("/Game/") + ".uasset")
    manifest = project_module_manifest(store.root)
    for plugin in manifest.get("plugins") or []:
        if not isinstance(plugin, dict) or not plugin.get("mounted_asset_root"):
            continue
        root = str(plugin["mounted_asset_root"]).rstrip("/")
        if package.startswith(root + "/"):
            return Path(str(plugin.get("content_directory"))) / (package.removeprefix(root + "/") + ".uasset")
    return None


def _fingerprint(store: SnapshotStore, asset: str) -> dict[str, Any]:
    path = _package_file(store, asset)
    if not path or not path.exists():
        return {"asset": asset, "exists": False, "sha256": None, "size": 0, "mtime_ns": None}
    data = path.read_bytes()
    stat = path.stat()
    return {"asset": asset, "exists": True, "sha256": "sha256:" + hashlib.sha256(data).hexdigest(), "size": len(data), "mtime_ns": stat.st_mtime_ns}


def _quality_diagnostics(operations: list[dict[str, Any]]) -> list[dict[str, Any]]:
    print_count = sum(1 for item in operations if item.get("type") == "blueprint.add_print_string_node")
    delay_count = sum(1 for item in operations if item.get("type") == "blueprint.add_function_call_node" and "delay" in json.dumps(item.get("params") or {}).casefold())
    if print_count >= 4 and delay_count >= 3:
        return [{"severity": "warning", "blocking": False, "code": "UEPI_EDIT_BLUEPRINT_UNROLLED_COUNTDOWN", "message": "The plan appears to unroll repeated countdown behavior; prefer a timer/loop plus state variable when the catalog can express it.", "phase": "plan_quality", "retryable": False, "recoverable": True}]
    return []


def _wait_refresh(store: SnapshotStore, request_path: str, timeout_seconds: float = 30.0) -> dict[str, Any] | None:
    path = Path(request_path)
    try:
        initial = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return None
    request_id = str(initial.get("request_id") or "")
    deadline = time.monotonic() + max(0.0, timeout_seconds)
    while time.monotonic() < deadline:
        for candidate in store.requests_dir.glob("*.json"):
            try:
                value = json.loads(candidate.read_text(encoding="utf-8-sig"))
            except (OSError, json.JSONDecodeError):
                continue
            if str(value.get("request_id") or "") != request_id:
                continue
            if str(value.get("status") or "").casefold() in {"succeeded", "completed", "failed", "cancelled", "expired", "aborted"}:
                value["request_path"] = str(candidate)
                return value
        time.sleep(0.1)
    return {"request_id": request_id, "status": "wait_timeout", "request_path": request_path}


def _runtime_ticket(store: SnapshotStore, plan: dict[str, Any]) -> dict[str, Any] | None:
    verification = plan.get("verification_plan") if isinstance(plan.get("verification_plan"), dict) else None
    if not verification:
        return None
    steps = [item for item in verification.get("steps") or [] if isinstance(item, dict)]
    requested_actions = {str(item.get("action") or "") for item in steps}
    allowed_actions = sorted(({"start", "stop"} | requested_actions) & {"start", "stop", "input", "invoke", "read", "wait", "assert"})
    allowed_functions = sorted({str(item.get("function")) for item in steps if item.get("action") == "invoke" and item.get("function")} | {str(item) for item in verification.get("allowed_functions") or [] if isinstance(item, str)})
    allowed_keys = sorted({str(item.get("key")) for item in steps if item.get("action") == "input" and item.get("key")} | {str(item) for item in verification.get("allowed_keys") or [] if isinstance(item, str)})
    allowed_reads = [item for item in verification.get("allowed_reads") or [] if isinstance(item, dict)]
    allowed_reads.extend({"object_path": str(item.get("object_path")), "property": str(item.get("property"))} for item in steps if item.get("action") in {"read", "wait", "assert"} and item.get("object_path") and item.get("property"))
    ticket_id = f"uepi-runtime-ticket:{uuid4().hex}"
    ticket = {
        "schema_version": "uepi.runtime-ticket.v1",
        "ticket_id": ticket_id,
        "transaction_id": plan.get("transaction_id"),
        "plan_hash": plan.get("plan_hash"),
        "project_binding_id": plan.get("project_binding_id"),
        "editor_session_id": plan.get("editor_session_id"),
        "created_at": _iso(_now()),
        "expires_at": _iso(_now() + timedelta(minutes=20)),
        "map": verification.get("map"),
        "timeout_seconds": min(120.0, max(1.0, float(verification.get("timeout_seconds") or 60.0))),
        "allowed_actions": allowed_actions,
        "allowed_functions": allowed_functions,
        "allowed_keys": allowed_keys,
        "allowed_reads": allowed_reads,
    }
    directory = store.store_dir / "runtime"
    directory.mkdir(parents=True, exist_ok=True)
    (directory / f"{ticket_id.replace(':', '-')}.json").write_text(json.dumps(ticket, ensure_ascii=False, indent=2), encoding="utf-8")
    return ticket


def discover(store: SnapshotStore) -> dict[str, Any]:
    identity = _identity(store)
    catalog, diagnostics, bridge_error = load_catalog(store, identity, refresh=True)
    operations = list((catalog or {}).get("operations") or [])
    return _response(
        store,
        tool="uepi_edit_discover",
        operation="operation_catalog",
        result={
            "profile": "codex",
            "legacy_profile_alias": "codex_write_alpha",
            "default_enabled": True,
            "apply_enabled": bool(catalog and any(item.get("apply_supported") for item in operations if isinstance(item, dict))),
            "catalog": catalog,
            "operations": operations,
            "bridge_error": bridge_error,
        },
        diagnostics=diagnostics,
    )


def preview(store: SnapshotStore, intent: str = "", operations: list[Any] | None = None, evidence: list[Any] | None = None, verification_plan: dict[str, Any] | None = None) -> dict[str, Any]:
    identity = _identity(store)
    catalog, diagnostics, bridge_error = load_catalog(store, identity, refresh=True)
    descriptors = operation_map(catalog)
    if not catalog:
        blocking = {"severity": "error", "blocking": True, "code": "UEPI_EDIT_CATALOG_UNAVAILABLE", "message": "Preview requires the exact-project Editor Operation Registry.", "phase": "preview", "retryable": True, "recoverable": True}
        return _response(store, tool="uepi_edit_preview", operation="preview", error={"code": blocking["code"], "message": blocking["message"], "retryable": True, "candidates": []}, diagnostics=diagnostics + [blocking])

    normalized: list[dict[str, Any]] = []
    affected: list[str] = []
    operation_assets: dict[str, str] = {}
    seen_ids: set[str] = set()
    for index, raw in enumerate(operations or []):
        op_id, op_type, params, depends_on = _operation_parts(raw, index)
        descriptor = descriptors.get(op_type)
        if not descriptor or not descriptor.get("apply_supported"):
            diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_EDIT_OPERATION_UNSUPPORTED", "message": f"Operation is not apply-supported by the active Editor Registry: {op_type}", "phase": "preview", "operation_index": index, "operation_id": op_id, "retryable": False, "recoverable": True})
            continue
        references = _operation_references(params)
        if op_id in seen_ids or any(dependency not in seen_ids for dependency in depends_on) or any(reference not in depends_on for reference in references):
            diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_EDIT_OPERATION_DEPENDENCY_INVALID", "message": f"Operation dependency order is invalid: {op_id}", "phase": "preview", "operation_index": index, "operation_id": op_id, "retryable": False, "recoverable": True})
            continue
        seen_ids.add(op_id)
        normalized.append({
            "operation_id": op_id,
            "type": op_type,
            "version": int((raw or {}).get("version") or 1) if isinstance(raw, dict) else 1,
            "idempotency_key": str((raw or {}).get("idempotency_key") or op_id) if isinstance(raw, dict) else op_id,
            "params": params,
            "depends_on": depends_on,
            "if_exists": str((raw or {}).get("if_exists") or "fail") if isinstance(raw, dict) else "fail",
            "expected_before": (raw or {}).get("expected_before") if isinstance((raw or {}).get("expected_before"), dict) else {},
        })
        affected.extend(_target_values(params, descriptor))
        created_asset = _destination_asset(params) if op_type.startswith(("content.", "material.", "widget.", "input.")) or op_type == "animation.create_montage_from_sequence" else None
        if created_asset:
            operation_assets[op_id] = created_asset
            affected.append(created_asset)
        for candidate in (params.get("asset"), params.get("source"), params.get("context"), params.get("action")):
            referenced = _referenced_operation(candidate)
            if referenced and referenced in operation_assets:
                affected.append(operation_assets[referenced])

    diagnostics.extend(_quality_diagnostics(normalized))
    if verification_plan:
        supported_runtime_actions = {"start", "stop", "input", "invoke", "read", "wait", "assert"}
        map_path = verification_plan.get("map")
        if map_path is not None and (not isinstance(map_path, str) or not map_path.startswith("/Game/")):
            diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_RUNTIME_MAP_NOT_APPROVED", "message": "verification_plan.map must be an exact /Game package path.", "phase": "preview", "retryable": False, "recoverable": True})
        for step_index, step in enumerate(verification_plan.get("steps") or []):
            action = str(step.get("action") or "") if isinstance(step, dict) else ""
            if action not in supported_runtime_actions:
                diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_RUNTIME_ACTION_NOT_APPROVED", "message": f"Unsupported verification action at step {step_index}: {action}", "phase": "preview", "retryable": False, "recoverable": True})
            elif action == "invoke" and not step.get("function"):
                diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_RUNTIME_FUNCTION_NOT_APPROVED", "message": f"Invoke step {step_index} requires an exact function name.", "phase": "preview", "retryable": False, "recoverable": True})
            elif action == "input" and not step.get("key"):
                diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_RUNTIME_INPUT_NOT_APPROVED", "message": f"Input step {step_index} requires an exact key.", "phase": "preview", "retryable": False, "recoverable": True})
            elif action in {"read", "wait", "assert"} and (not step.get("object_path") or not step.get("property")):
                diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_RUNTIME_READ_NOT_APPROVED", "message": f"Read/assert step {step_index} requires object_path and property.", "phase": "preview", "retryable": False, "recoverable": True})
    affected = sorted(set(affected))
    status = resolve_status(store, _state(store), identity, probe_bridge=True)
    session_id = str(status["editor"].get("session_id") or "")
    if not session_id:
        diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_EDITOR_SESSION_REQUIRED", "message": "Preview for a writable plan requires an exact live Editor session.", "phase": "preview", "retryable": True, "recoverable": True})
    if any(item.get("blocking") for item in diagnostics):
        return _response(store, tool="uepi_edit_preview", operation="preview", error={"code": next(item["code"] for item in diagnostics if item.get("blocking")), "message": "Edit Preview is blocked; no Apply action is available.", "retryable": False, "candidates": []}, diagnostics=diagnostics)

    transaction_id = f"uepi-tx:{uuid4().hex}"
    created = _now()
    snapshot_state = _state(store)
    nonce = uuid4().hex
    operation_order = [str(item.get("operation_id")) for item in normalized]
    touched_packages = sorted({asset.split(".", 1)[0] for asset in affected})
    risk_order = {"low": 0, "medium": 1, "high": 2, "critical": 3}
    risk_level = max((str(descriptors.get(str(item.get("type")), {}).get("risk") or "medium") for item in normalized), key=lambda value: risk_order.get(value, 1), default="low")
    plan = {
        "schema_version": PLAN_SCHEMA,
        "schema_aliases": ["uepi.edit-plan.v2"],
        "transaction_id": transaction_id,
        "intent": intent,
        "created_at": _iso(created),
        "expires_at": _iso(created + timedelta(minutes=10)),
        "project_id": identity.get("project_id"),
        "project_binding_id": identity.get("project_binding_id"),
        "editor_session_id": session_id,
        "catalog_version": catalog.get("catalog_version"),
        "catalog_hash": catalog.get("catalog_hash"),
        "engine_version": catalog.get("engine_version"),
        "plugin_build_id": catalog.get("plugin_build_id"),
        "approval_nonce": nonce,
        "project": {"project_id": identity.get("project_id"), "project_binding_id": identity.get("project_binding_id"), "project_file": identity.get("project_file")},
        "editor": {"session_id": session_id, "pid": status["editor"].get("pid")},
        "base": {
            "saved_generation": status["snapshot"].get("saved_generation"),
            "live_generation": status["snapshot"].get("live_generation"),
            "view_generation": snapshot_state.generation,
            "catalog_hash": catalog.get("catalog_hash"),
            "plugin_build_id": catalog.get("plugin_build_id"),
        },
        "operations": normalized,
        "operation_order": operation_order,
        "affected_assets": affected,
        "predicted_touched_objects": affected,
        "predicted_touched_packages": touched_packages,
        "before_fingerprints": [_fingerprint(store, asset) for asset in affected],
        "preconditions": ["exact_project", "exact_editor_session", "catalog_unchanged", "before_fingerprints_unchanged", "targets_clean_and_writable"],
        "dirty_policy": "fail",
        "save_policy": "after_validation",
        "validation_policy": "typed_registry",
        "validation_plan": [{"operation_id": item.get("operation_id"), "validator": descriptors.get(str(item.get("type")), {}).get("validation_behavior") or descriptors.get(str(item.get("type")), {}).get("validation_mode") or "generic_uobject"} for item in normalized],
        "risk": {"level": risk_level, "requires_user_approval": True, "operation_count": len(normalized), "asset_count": len(affected)},
        "approval": {"required": True, "nonce": nonce},
        "evidence": evidence or [],
        "verification_plan": verification_plan,
    }
    plan["plan_hash"] = canonical_plan_hash(plan)
    plan["approval"]["plan_hash"] = plan["plan_hash"]
    path = _plan_path(store, transaction_id.replace(":", "-"))
    path.write_text(json.dumps(plan, ensure_ascii=False, indent=2), encoding="utf-8")
    audit_path = _audit(store, {"event": "preview", "transaction_id": transaction_id, "plan_hash": plan["plan_hash"], "affected_assets": affected})
    return _response(
        store,
        tool="uepi_edit_preview",
        operation="preview",
        result={"plan": plan, "audit_path": audit_path, "approval_request": {"transaction_id": transaction_id, "plan_hash": plan["plan_hash"], "approval_nonce": plan["approval_nonce"], "summary": intent, "affected_assets": affected}},
        diagnostics=diagnostics,
        next_actions=[{"reason": "Apply only after one explicit user approval of this immutable plan.", "tool": "uepi_edit_apply", "arguments": {"transaction_id": transaction_id, "plan_hash": plan["plan_hash"], "approval_nonce": plan["approval_nonce"], "approved": True}}],
    )


def _load_plan(store: SnapshotStore, transaction_id: str) -> dict[str, Any] | None:
    path = _plan_path(store, transaction_id.replace(":", "-"))
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def apply(store: SnapshotStore, transaction_id: str = "", approved: bool = False, plan_hash: str = "", approval_nonce: str = "") -> dict[str, Any]:
    plan = _load_plan(store, transaction_id)
    if not plan:
        return _response(store, tool="uepi_edit_apply", operation="apply", error={"code": "UEPI_EDIT_PLAN_NOT_FOUND", "message": "Preview plan was not found.", "retryable": False, "candidates": []})
    result_path = _result_path(store, transaction_id.replace(":", "-"))
    if result_path.exists():
        return _response(store, tool="uepi_edit_apply", operation="apply", result={"idempotent_replay": True, "apply": json.loads(result_path.read_text(encoding="utf-8-sig"))})
    if not approved or plan_hash != plan.get("plan_hash") or approval_nonce != plan.get("approval_nonce") or not verify_plan_hash(plan):
        return _response(store, tool="uepi_edit_apply", operation="apply", error={"code": "UEPI_EDIT_APPROVAL_MISMATCH", "message": "Apply approval does not match the immutable Preview plan.", "retryable": False, "candidates": []})
    expires = datetime.fromisoformat(str(plan.get("expires_at")).replace("Z", "+00:00"))
    if expires < _now():
        return _response(store, tool="uepi_edit_apply", operation="apply", error={"code": "UEPI_EDIT_PLAN_EXPIRED", "message": "Preview plan expired; run Preview again.", "retryable": False, "candidates": []})
    expected_fingerprints = plan.get("before_fingerprints") if isinstance(plan.get("before_fingerprints"), list) else []
    current_fingerprints = [_fingerprint(store, str(item.get("asset") or "")) for item in expected_fingerprints if isinstance(item, dict) and item.get("asset")]
    expected_by_asset = {str(item.get("asset") or ""): item for item in expected_fingerprints if isinstance(item, dict)}
    changed_assets = [
        item["asset"]
        for item in current_fingerprints
        if item != expected_by_asset.get(str(item.get("asset") or ""))
    ]
    if changed_assets:
        return _response(
            store,
            tool="uepi_edit_apply",
            operation="apply",
            error={"code": "UEPI_EDIT_BEFORE_FINGERPRINT_CHANGED", "message": "One or more target package files changed after Preview.", "retryable": False, "candidates": changed_assets},
        )
    identity = _identity(store)
    catalog, diagnostics, bridge_error = load_catalog(store, identity, refresh=True)
    if not catalog or catalog.get("catalog_hash") != plan.get("catalog_hash"):
        return _response(store, tool="uepi_edit_apply", operation="apply", error={"code": "UEPI_EDIT_CATALOG_STALE", "message": "Operation catalog changed after Preview.", "retryable": False, "candidates": []}, diagnostics=diagnostics)
    response = call_bridge(store, "edit.apply", {"transaction_id": transaction_id, "approved": True, "plan_hash": plan_hash, "approval_nonce": approval_nonce, "save": True, "plan": plan}, timeout=30.0, expected_identity=identity, expected_editor_session_id=str(plan.get("editor_session_id") or ""))
    if not response.get("ok"):
        code = str((response.get("error") or {}).get("code") or next((item.get("code") for item in response.get("diagnostics") or [] if isinstance(item, dict)), "UEPI_EDIT_APPLY_FAILED"))
        return _response(store, tool="uepi_edit_apply", operation="apply", error={"code": code, "message": str((response.get("error") or {}).get("message") or "Editor Apply failed during preflight or execution."), "retryable": False, "candidates": []}, diagnostics=diagnostics + list(response.get("diagnostics") or []))
    result = response.get("result") if isinstance(response.get("result"), dict) else {}
    refresh_result = _wait_refresh(store, str(result.get("refresh_request_path") or "")) if result.get("refresh_request_path") else None
    after_fingerprints = [_fingerprint(store, asset) for asset in plan.get("affected_assets") or []]
    result["transaction_diff"] = build_transaction_diff(plan, result, after_fingerprints)
    runtime_ticket = _runtime_ticket(store, plan) if result.get("validation_ok") and result.get("saved") else None
    result["post_apply"] = {
        "validated": bool(result.get("validation_ok")),
        "saved": bool(result.get("saved")),
        "refresh_request_path": result.get("refresh_request_path"),
        "refresh": refresh_result,
        "diff_ready": True,
        "runtime_ticket": runtime_ticket,
    }
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    audit_path = _audit(store, {"event": "apply", "transaction_id": transaction_id, "plan_hash": plan_hash, "result": result})
    next_actions = [{"reason": "Inspect the transaction-bound semantic diff.", "tool": "uepi_diff", "arguments": {"mode": "transaction", "transaction_id": transaction_id}}]
    if runtime_ticket:
        next_actions.append({"reason": "Start controlled PIE verification for the approved transaction.", "tool": "uepi_runtime", "arguments": {"action": "start", "ticket_id": runtime_ticket["ticket_id"]}})
    return _response(store, tool="uepi_edit_apply", operation="apply", result={"apply": result, "transaction_diff": result["transaction_diff"], "runtime_ticket": runtime_ticket, "audit_path": audit_path}, diagnostics=diagnostics + list(response.get("diagnostics") or []), next_actions=next_actions)


def validate(store: SnapshotStore, transaction_id: str = "") -> dict[str, Any]:
    plan = _load_plan(store, transaction_id)
    if not plan:
        return _response(store, tool="uepi_edit_validate", operation="validate", error={"code": "UEPI_EDIT_PLAN_NOT_FOUND", "message": "Edit plan was not found.", "retryable": False, "candidates": []})
    response = call_bridge(store, "edit.validate", {"transaction_id": transaction_id, "plan": plan}, timeout=20.0, expected_identity=_identity(store), expected_editor_session_id=str(plan.get("editor_session_id") or ""))
    if not response.get("ok"):
        code = str((response.get("error") or {}).get("code") or next((item.get("code") for item in response.get("diagnostics") or [] if isinstance(item, dict)), "UEPI_EDIT_VALIDATE_FAILED"))
        return _response(store, tool="uepi_edit_validate", operation="validate", error={"code": code, "message": "Typed validation failed.", "retryable": False, "candidates": []}, diagnostics=list(response.get("diagnostics") or []))
    return _response(store, tool="uepi_edit_validate", operation="validate", result=response.get("result") or {}, diagnostics=list(response.get("diagnostics") or []))


def rollback(store: SnapshotStore, transaction_id: str = "") -> dict[str, Any]:
    plan = _load_plan(store, transaction_id)
    expected_session = str((plan or {}).get("editor_session_id") or "")
    response = call_bridge(store, "edit.rollback", {"transaction_id": transaction_id}, timeout=30.0, expected_identity=_identity(store), expected_editor_session_id=expected_session or None)
    if not response.get("ok"):
        code = str((response.get("error") or {}).get("code") or next((item.get("code") for item in response.get("diagnostics") or [] if isinstance(item, dict)), "UEPI_EDIT_ROLLBACK_FAILED"))
        return _response(store, tool="uepi_edit_rollback", operation="rollback", error={"code": code, "message": "Rollback failed or did not verify.", "retryable": False, "candidates": []}, diagnostics=list(response.get("diagnostics") or []))
    _audit(store, {"event": "rollback", "transaction_id": transaction_id, "result": response.get("result")})
    return _response(store, tool="uepi_edit_rollback", operation="rollback", result=response.get("result") or {}, diagnostics=list(response.get("diagnostics") or []))
