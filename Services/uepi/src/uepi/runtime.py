from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import time
from typing import Any
from uuid import uuid4

from .bridge_client import call_bridge


MUTATING_ACTIONS = {"start", "stop", "input", "invoke"}
RUNTIME_ACTIONS = {"start", "stop", "input", "invoke", "read", "wait", "assert", "delay"}
INPUT_DELIVERIES = {"player_controller", "possessed_pawn_input_stack", "game_viewport", "enhanced_input_action"}
VERIFICATION_MODES = {"agent_objective", "human_pie", "hybrid"}


def _approved_subset(arguments: dict[str, Any], fields: tuple[str, ...]) -> dict[str, Any]:
    return {field: arguments[field] for field in fields if field in arguments}


def _matches_approved(requested: dict[str, Any], approved: list[Any]) -> bool:
    return any(isinstance(item, dict) and item == requested for item in approved)


def _value_at_path(value: Any, path: str) -> Any:
    current = value
    for segment in [item for item in path.split(".") if item]:
        if not isinstance(current, dict) or segment not in current:
            return None
        current = current[segment]
    return current


def _unwrap_typed_value(value: Any) -> Any:
    if isinstance(value, dict) and "value" in value and isinstance(value.get("type"), str):
        return value["value"]
    return value


def _human_test_steps(steps: list[dict[str, Any]]) -> list[str]:
    result: list[str] = []
    for step in steps:
        action = str(step.get("action") or "")
        if action == "start":
            result.append(f"Start PIE on {step.get('map') or 'the approved map'}.")
        elif action == "input":
            identity = step.get("input_action") or step.get("key") or "the approved input"
            result.append(f"Trigger {identity} using {step.get('delivery') or 'the possessed Pawn input stack'}.")
        elif action in {"wait", "assert", "read"}:
            observation = step.get("function") or step.get("property") or "the approved debug state"
            result.append(f"Verify {observation} matches the approved expected value.")
        elif action == "delay":
            result.append(f"Wait {step.get('seconds', step.get('timeout_seconds'))} seconds for the gameplay response.")
        elif action == "stop":
            result.append("Stop the UEPI-owned PIE session.")
    return list(dict.fromkeys(result))


def _verification_contract(mode: str, steps: list[dict[str, Any]], human_steps: list[str] | None = None) -> dict[str, Any]:
    human_required = mode in {"human_pie", "hybrid"}
    return {
        "verification_mode": mode,
        "technical_verification": "agent_objective" if mode != "human_pie" else "not_requested",
        "visual_acceptance": "human_required" if human_required else "not_required",
        "visual_status": "unreviewed" if human_required else "not_applicable",
        "visual_reviewer": "user" if human_required else "agent",
        "human_test_steps": human_steps or _human_test_steps(steps),
    }


def _ticket_path(store: Any, ticket_id: str) -> Path:
    return store.store_dir / "runtime" / f"{ticket_id.replace(':', '-')}.json"


def _plan_path(store: Any, plan_id: str) -> Path:
    return store.store_dir / "runtime" / f"{plan_id.replace(':', '-')}.plan.json"


def _canonical_hash(value: dict[str, Any]) -> str:
    payload = {key: child for key, child in value.items() if key != "plan_hash"}
    return "sha256:" + hashlib.sha256(json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()


def _write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


def _ticket_from_plan(engine: Any, plan: dict[str, Any]) -> dict[str, Any]:
    steps = [item for item in plan.get("steps") or [] if isinstance(item, dict)]
    verification_mode = str(plan.get("verification_mode") or "hybrid")
    implicit_actions = set() if verification_mode == "human_pie" else {"stop"}
    allowed_actions = sorted((implicit_actions | {str(item.get("action") or "") for item in steps}) & RUNTIME_ACTIONS)
    allowed_functions = sorted({str(item.get("function")) for item in steps if item.get("function")} | {str(item) for item in plan.get("allowed_functions") or [] if isinstance(item, str)})
    allowed_keys = sorted({str(item.get("key")) for item in steps if item.get("action") == "input" and item.get("key")} | {str(item) for item in plan.get("allowed_keys") or [] if isinstance(item, str)})
    allowed_deliveries = sorted({str(item.get("delivery") or "possessed_pawn_input_stack") for item in steps if item.get("action") == "input"} | {str(item) for item in plan.get("allowed_deliveries") or [] if isinstance(item, str)})
    allowed_inputs = [
        _approved_subset(item, ("key", "event", "delivery", "input_action", "value"))
        for item in steps
        if item.get("action") == "input"
    ]
    allowed_reads = [item for item in plan.get("allowed_reads") or [] if isinstance(item, dict)]
    allowed_reads.extend({_key: item[_key] for _key in ("object_path", "target", "property", "function", "field", "arguments", "equals") if _key in item} for item in steps if item.get("action") in {"read", "wait", "assert"} and (item.get("object_path") or item.get("target")))
    allowed_invocations = [{key: item[key] for key in ("object_path", "target", "function", "arguments") if key in item} for item in steps if item.get("action") == "invoke"]
    ticket_id = f"uepi-runtime-ticket:{uuid4().hex}"
    ticket = {
        "schema_version": "uepi.runtime-ticket.v1",
        "ticket_id": ticket_id,
        "runtime_plan_id": plan.get("runtime_plan_id"),
        "plan_hash": plan.get("plan_hash"),
        "project_binding_id": plan.get("project_binding_id"),
        "editor_session_id": plan.get("editor_session_id"),
        "created_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "expires_at": (datetime.now(timezone.utc) + timedelta(minutes=20)).isoformat().replace("+00:00", "Z"),
        "map": plan.get("map"),
        "timeout_seconds": min(120.0, max(1.0, float(plan.get("timeout_seconds") or 60.0))),
        "allowed_actions": allowed_actions,
        "allowed_functions": allowed_functions,
        "allowed_invocations": allowed_invocations,
        "allowed_keys": allowed_keys,
        "allowed_deliveries": allowed_deliveries,
        "allowed_inputs": allowed_inputs,
        "allowed_reads": allowed_reads,
        "steps": steps,
        **_verification_contract(
            verification_mode,
            steps,
            [str(item) for item in plan.get("human_test_steps") or [] if str(item).strip()],
        ),
    }
    _write_json(_ticket_path(engine.store, ticket_id), ticket)
    return ticket


def preview(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    status = call_bridge(engine.store, "editor.get_status", timeout=2.0, expected_identity=engine.identity, expected_editor_session_id=str(arguments.get("expected_editor_session_id") or "") or None)
    if not status.get("ok"):
        diagnostics = [item for item in status.get("diagnostics") or [] if isinstance(item, dict)]
        code = str(next((item.get("code") for item in diagnostics), "UEPI_EDITOR_SESSION_REQUIRED"))
        return engine._error(code, "Runtime Preview requires the exact live Editor session.", diagnostics=diagnostics, tool="uepi_runtime_preview", operation="preview")
    editor = status.get("result") if isinstance(status.get("result"), dict) else {}
    session_id = str(editor.get("editor_session_id") or editor.get("session_id") or "")
    authorization_settings = editor.get("write_authorization") if isinstance(editor.get("write_authorization"), dict) else {}
    authorization_mode = str(authorization_settings.get("mode") or "ReviewEachPlan")
    if not bool(authorization_settings.get("allow_runtime_control", False)):
        return engine._error("UEPI_RUNTIME_POLICY_REJECTED", "Runtime control is disabled by the active project authorization policy.", tool="uepi_runtime_preview", operation="preview")
    automatically_authorized = authorization_mode in {"TrustedSession", "TrustedProject"} and bool(authorization_settings.get("allow_runtime_control", False))
    if authorization_mode == "TrustedProject":
        automatically_authorized = automatically_authorized and str(authorization_settings.get("trusted_project_binding_id") or "") == str(engine.identity.get("project_binding_id") or "")
    if authorization_mode in {"TrustedSession", "TrustedProject"} and not automatically_authorized:
        return engine._error("UEPI_RUNTIME_TRUST_POLICY_REJECTED", "Runtime control is outside the active trusted authorization policy.", tool="uepi_runtime_preview", operation="preview")
    steps = [item for item in arguments.get("steps") or [] if isinstance(item, dict)]
    diagnostics: list[dict[str, Any]] = []
    map_path = str(arguments.get("map") or editor.get("active_map") or "")
    verification_mode = str(arguments.get("verification_mode") or "hybrid")
    if verification_mode not in VERIFICATION_MODES:
        diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_RUNTIME_VERIFICATION_MODE_INVALID", "message": "verification_mode must be agent_objective, human_pie, or hybrid."})
    if verification_mode == "human_pie":
        disallowed = [str(step.get("action") or "") for step in steps if str(step.get("action") or "") in MUTATING_ACTIONS]
        if disallowed:
            diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_RUNTIME_HUMAN_PIE_OBSERVATION_ONLY", "message": f"human_pie plans are observation-only; remove actions: {', '.join(disallowed)}"})
    if not map_path.startswith("/Game/"):
        diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_RUNTIME_MAP_NOT_APPROVED", "message": "Runtime Preview requires an exact /Game map package."})
    if not steps:
        diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_RUNTIME_PLAN_EMPTY", "message": "Runtime Preview requires at least one verification step."})
    for index, step in enumerate(steps):
        action = str(step.get("action") or "")
        if action not in RUNTIME_ACTIONS:
            diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_RUNTIME_ACTION_NOT_APPROVED", "message": f"Unsupported runtime step {index}: {action}"})
        if action == "input":
            delivery = str(step.get("delivery") or "possessed_pawn_input_stack")
            has_identity = bool(step.get("input_action")) if delivery == "enhanced_input_action" else bool(step.get("key"))
            if not has_identity or delivery not in INPUT_DELIVERIES:
                diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_RUNTIME_INPUT_NOT_APPROVED", "message": f"Input step {index} requires an exact key, or an exact input_action for Enhanced Input, and a supported delivery."})
        if action == "delay":
            try:
                seconds = float(step.get("seconds", step.get("timeout_seconds", 0.0)))
            except (TypeError, ValueError):
                seconds = 0.0
            if seconds <= 0.0 or seconds > 120.0:
                diagnostics.append({"severity": "error", "blocking": True, "code": "UEPI_RUNTIME_DELAY_INVALID", "message": f"Delay step {index} requires seconds in the range (0, 120]."})
    if diagnostics:
        return engine._error(str(diagnostics[0]["code"]), "Runtime Preview is blocked.", diagnostics=diagnostics, tool="uepi_runtime_preview", operation="preview")
    plan_id = f"uepi-runtime-plan:{uuid4().hex}"
    now = datetime.now(timezone.utc)
    nonce = uuid4().hex
    plan = {
        "schema_version": "uepi.runtime-plan.v1",
        "runtime_plan_id": plan_id,
        "intent": str(arguments.get("intent") or "Runtime verification"),
        "project_binding_id": engine.identity.get("project_binding_id"),
        "project_file": engine.identity.get("project_file"),
        "editor_session_id": session_id,
        "map": map_path,
        "steps": steps,
        "allowed_functions": [str(item) for item in arguments.get("allowed_functions") or []],
        "allowed_keys": [str(item) for item in arguments.get("allowed_keys") or []],
        "allowed_deliveries": [str(item) for item in arguments.get("allowed_deliveries") or []],
        "allowed_reads": [item for item in arguments.get("allowed_reads") or [] if isinstance(item, dict)],
        "timeout_seconds": min(120.0, max(1.0, float(arguments.get("timeout_seconds") or 60.0))),
        "verification_mode": verification_mode,
        "human_test_steps": [str(item) for item in arguments.get("human_test_steps") or [] if str(item).strip()],
        "authorization": {
            "mode": authorization_mode,
            "policy_decision": "authorized" if automatically_authorized else "review_required",
            "automatically_authorized": automatically_authorized,
            "project_binding_id": engine.identity.get("project_binding_id"),
            "editor_session_id": session_id,
        },
        "approval_nonce": nonce,
        "created_at": now.isoformat().replace("+00:00", "Z"),
        "expires_at": (now + timedelta(minutes=10)).isoformat().replace("+00:00", "Z"),
    }
    plan["plan_hash"] = _canonical_hash(plan)
    _write_json(_plan_path(engine.store, plan_id), plan)
    approval_required = not automatically_authorized
    return engine._envelope(
        {"plan": plan, "approval_required": approval_required, "authorization": plan["authorization"], "verification": _verification_contract(verification_mode, steps, plan["human_test_steps"])},
        next_actions=[{"reason": "Issue the trusted-policy runtime ticket immediately." if automatically_authorized else "After one explicit user approval, issue the session-bound runtime ticket.", "tool": "uepi_runtime_approve", "arguments": {"runtime_plan_id": plan_id, "plan_hash": plan["plan_hash"], "approval_nonce": nonce, "approved": approval_required}}],
        tool="uepi_runtime_preview",
        operation="preview",
    )


def approve(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    plan_id = str(arguments.get("runtime_plan_id") or "")
    try:
        plan = json.loads(_plan_path(engine.store, plan_id).read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return engine._error("UEPI_RUNTIME_PLAN_NOT_FOUND", "Runtime verification plan was not found.", tool="uepi_runtime_approve", operation="approve")
    authorization = plan.get("authorization") if isinstance(plan.get("authorization"), dict) else {}
    automatically_authorized = bool(authorization.get("automatically_authorized") and authorization.get("policy_decision") == "authorized")
    if (not arguments.get("approved") and not automatically_authorized) or arguments.get("plan_hash") != plan.get("plan_hash") or arguments.get("approval_nonce") != plan.get("approval_nonce") or _canonical_hash(plan) != plan.get("plan_hash"):
        return engine._error("UEPI_RUNTIME_APPROVAL_MISMATCH", "Runtime approval does not match the immutable Preview plan.", tool="uepi_runtime_approve", operation="approve")
    expires = datetime.fromisoformat(str(plan.get("expires_at")).replace("Z", "+00:00"))
    if expires < datetime.now(timezone.utc):
        return engine._error("UEPI_RUNTIME_PLAN_EXPIRED", "Runtime verification plan expired; preview it again.", tool="uepi_runtime_approve", operation="approve")
    status = call_bridge(engine.store, "editor.get_status", timeout=2.0, expected_identity=engine.identity, expected_editor_session_id=str(plan.get("editor_session_id") or ""))
    if not status.get("ok"):
        return engine._error("UEPI_EDITOR_SESSION_MISMATCH", "The Editor session changed after Runtime Preview; create a new plan for the current session.", diagnostics=status.get("diagnostics") or [], tool="uepi_runtime_approve", operation="approve")
    editor = status.get("result") if isinstance(status.get("result"), dict) else {}
    current_policy = editor.get("write_authorization") if isinstance(editor.get("write_authorization"), dict) else {}
    current_mode = str(current_policy.get("mode") or "ReviewEachPlan")
    if current_mode != str(authorization.get("mode") or "ReviewEachPlan"):
        return engine._error("UEPI_RUNTIME_TRUST_POLICY_REJECTED", "Runtime authorization mode changed after Preview; create a new plan.", tool="uepi_runtime_approve", operation="approve")
    if not bool(current_policy.get("allow_runtime_control", False)):
        return engine._error("UEPI_RUNTIME_POLICY_REJECTED", "Runtime control was disabled after Preview.", tool="uepi_runtime_approve", operation="approve")
    if automatically_authorized and current_mode == "TrustedProject" and str(current_policy.get("trusted_project_binding_id") or "") != str(engine.identity.get("project_binding_id") or ""):
        return engine._error("UEPI_RUNTIME_TRUST_POLICY_REJECTED", "TrustedProject binding changed after Preview; create a new plan.", tool="uepi_runtime_approve", operation="approve")
    ticket = _ticket_from_plan(engine, plan)
    first_step = next((item for item in ticket.get("steps") or [] if isinstance(item, dict)), None)
    next_actions = []
    if first_step:
        next_actions.append({"reason": "Execute the first approved runtime verification step.", "tool": "uepi_runtime", "arguments": {"ticket_id": ticket["ticket_id"], **first_step}})
    return engine._envelope({"ticket": ticket}, next_actions=next_actions, tool="uepi_runtime_approve", operation="approve")


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
    params["action"] = "observe" if action == "read" and arguments.get("function") else action
    if ticket:
        params["verification_mode"] = ticket.get("verification_mode") or "hybrid"
    return call_bridge(
        engine.store,
        "runtime.control",
        params,
        timeout=10.0,
        expected_identity=engine.identity,
        expected_editor_session_id=str((ticket or {}).get("editor_session_id") or arguments.get("expected_editor_session_id") or "") or None,
    )


def _cleanup_owned_pie(engine: Any, ticket: dict[str, Any] | None) -> None:
    if ticket and ticket.get("verification_mode") != "human_pie":
        _bridge(engine, "stop", {}, ticket)


def execute(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    action = str(arguments.get("action") or "status")
    ticket: dict[str, Any] | None = None
    if action != "status":
        ticket, error = load_ticket(engine, str(arguments.get("ticket_id") or ""))
        if error:
            return engine._error(error["code"], error["message"], tool="uepi_runtime", operation=action)
        if action not in set(ticket.get("allowed_actions") or []):
            return engine._error("UEPI_RUNTIME_ACTION_NOT_APPROVED", f"Runtime action is not approved by the ticket: {action}", tool="uepi_runtime", operation=action)
        if action == "start" and ticket.get("map") and str(arguments.get("map") or ticket.get("map")) != str(ticket.get("map")):
            return engine._error("UEPI_RUNTIME_MAP_NOT_APPROVED", "Requested PIE map differs from the approved verification plan.", tool="uepi_runtime", operation=action)
        if action == "start" and ticket.get("map") and not arguments.get("map"):
            arguments = {**arguments, "map": ticket["map"]}
        if action == "invoke":
            if str(arguments.get("function") or "") not in set(ticket.get("allowed_functions") or []):
                return engine._error("UEPI_RUNTIME_FUNCTION_NOT_APPROVED", "Runtime function is not listed in the approved verification plan.", tool="uepi_runtime", operation=action)
            requested_invoke = _approved_subset(arguments, ("object_path", "target", "function", "arguments"))
            if not _matches_approved(requested_invoke, ticket.get("allowed_invocations") or []):
                return engine._error("UEPI_RUNTIME_INVOCATION_NOT_APPROVED", "Runtime target, function, or arguments differ from the approved verification plan.", tool="uepi_runtime", operation=action)
        if action == "input" and str(arguments.get("delivery") or "possessed_pawn_input_stack") != "enhanced_input_action" and str(arguments.get("key") or "") not in set(ticket.get("allowed_keys") or []):
            return engine._error("UEPI_RUNTIME_INPUT_NOT_APPROVED", "Runtime key is not listed in the approved verification plan.", tool="uepi_runtime", operation=action)
        if action == "input" and str(arguments.get("delivery") or "possessed_pawn_input_stack") not in set(ticket.get("allowed_deliveries") or []):
            return engine._error("UEPI_RUNTIME_INPUT_DELIVERY_NOT_APPROVED", "Runtime input delivery is not listed in the approved verification plan.", tool="uepi_runtime", operation=action)
        if action == "input":
            requested_input = _approved_subset(arguments, ("key", "event", "delivery", "input_action", "value"))
            requested_input.setdefault("event", "pressed")
            requested_input.setdefault("delivery", "possessed_pawn_input_stack")
            approved_inputs = []
            for item in ticket.get("allowed_inputs") or []:
                if not isinstance(item, dict):
                    continue
                normalized = dict(item)
                normalized.setdefault("event", "pressed")
                normalized.setdefault("delivery", "possessed_pawn_input_stack")
                approved_inputs.append(normalized)
            if not _matches_approved(requested_input, approved_inputs):
                return engine._error("UEPI_RUNTIME_INPUT_NOT_APPROVED", "Runtime input identity, event, delivery, or value differs from the approved verification plan.", tool="uepi_runtime", operation=action)
        if action in {"read", "wait", "assert"}:
            if action == "wait" and not (arguments.get("object_path") or arguments.get("target") or arguments.get("property") or arguments.get("function")):
                action = "delay"
            else:
                requested_read = _approved_subset(arguments, ("object_path", "target", "property", "function", "field", "arguments", "equals"))
                if not _matches_approved(requested_read, ticket.get("allowed_reads") or []):
                    return engine._error("UEPI_RUNTIME_READ_NOT_APPROVED", "Runtime object/property is not listed in the approved verification plan.", tool="uepi_runtime", operation=action)
                if arguments.get("function") and str(arguments.get("function")) not in set(ticket.get("allowed_functions") or []):
                    return engine._error("UEPI_RUNTIME_FUNCTION_NOT_APPROVED", "Runtime observation function is not listed in the approved verification plan.", tool="uepi_runtime", operation=action)

    if action in {"status", "start", "stop", "input", "invoke", "read"}:
        response = _bridge(engine, action, arguments, ticket)
        if not response.get("ok"):
            if action not in {"status", "start", "stop"}:
                _cleanup_owned_pie(engine, ticket)
            error = response.get("error") if isinstance(response.get("error"), dict) else {}
            code = str(error.get("code") or next((item.get("code") for item in response.get("diagnostics") or [] if isinstance(item, dict)), "UEPI_RUNTIME_ACTION_FAILED"))
            return engine._error(code, str(error.get("message") or f"Runtime action failed: {action}"), diagnostics=response.get("diagnostics") if isinstance(response.get("diagnostics"), list) else [], tool="uepi_runtime", operation=action)
        result = response.get("result") if isinstance(response.get("result"), dict) else {}
        if action == "start" and bool(arguments.get("wait_until_running", True)):
            deadline = time.monotonic() + max(1.0, min(float(arguments.get("timeout_seconds") or 30.0), 120.0))
            while time.monotonic() <= deadline:
                status = _bridge(engine, "status", {}, ticket)
                status_result = status.get("result") if isinstance(status.get("result"), dict) else {}
                if status.get("ok") and status_result.get("state") == "running":
                    result = {**result, "state": "running", "runtime_status": status_result}
                    break
                time.sleep(0.1)
            else:
                _cleanup_owned_pie(engine, ticket)
                return engine._error("UEPI_RUNTIME_START_TIMEOUT", "PIE did not reach running state before the approved timeout.", tool="uepi_runtime", operation=action)
        if ticket:
            result = {**result, "verification": {key: ticket.get(key) for key in ("verification_mode", "technical_verification", "visual_acceptance", "visual_status", "visual_reviewer", "human_test_steps")}}
        return engine._envelope(result, diagnostics=response.get("diagnostics") or [], tool="uepi_runtime", operation=action)

    if action == "delay":
        try:
            seconds = float(arguments.get("seconds", arguments.get("timeout_seconds", 0.0)))
        except (TypeError, ValueError):
            seconds = 0.0
        if seconds <= 0.0 or seconds > 120.0:
            return engine._error("UEPI_RUNTIME_DELAY_INVALID", "Runtime delay requires seconds in the range (0, 120].", tool="uepi_runtime", operation=action)
        time.sleep(seconds)
        return engine._envelope({"delayed": True, "seconds": seconds}, tool="uepi_runtime", operation=action)

    if action in {"wait", "assert"}:
        timeout = max(0.0, min(float(arguments.get("timeout_seconds") or 10.0), 120.0))
        interval = max(0.05, min(float(arguments.get("poll_interval_ms") or 100) / 1000.0, 2.0))
        expected = _unwrap_typed_value(arguments.get("equals"))
        deadline = time.monotonic() + timeout
        last: dict[str, Any] = {}
        observed: Any = None
        while time.monotonic() <= deadline:
            bridge_action = "read"
            response = _bridge(engine, bridge_action, arguments, ticket)
            if response.get("ok") and isinstance(response.get("result"), dict):
                last = response["result"]
                observed = _unwrap_typed_value(_value_at_path(last, str(arguments.get("field") or "value")))
                if observed == expected:
                    return engine._envelope({"matched": True, "assertion": action == "assert", "observed": last, "observed_value": observed}, tool="uepi_runtime", operation=action)
            time.sleep(interval)
        code = "UEPI_RUNTIME_ASSERT_FAILED" if action == "assert" else "UEPI_RUNTIME_WAIT_TIMEOUT"
        _cleanup_owned_pie(engine, ticket)
        return engine._error(code, f"Runtime condition did not match before timeout; last value: {observed!r}", tool="uepi_runtime", operation=action)

    return engine._error("UEPI_RUNTIME_ACTION_UNSUPPORTED", f"Runtime action is not implemented: {action}", tool="uepi_runtime", operation=action)
