from __future__ import annotations

import json
from typing import Any
from uuid import uuid4


ENVELOPE_SCHEMA_VERSION = "uepi.mcp-envelope.v2"


def _as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def _as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def data_mode_for_state(state: dict[str, Any]) -> str:
    freshness = str(state.get("freshness") or "")
    if freshness in {"stale", "refresh_requested"}:
        return freshness
    return str(state.get("data_mode") or "saved")


def snapshot_for_state(state: dict[str, Any]) -> dict[str, Any]:
    saved_generation = state.get("saved_generation")
    live_generation = state.get("live_generation")
    return {
        "saved_generation": saved_generation,
        "live_generation": live_generation,
        "view_generation": live_generation or saved_generation,
        "observed_at": state.get("snapshot_observed_at"),
        "freshness": state.get("freshness"),
        "manifest_path": state.get("manifest_path"),
    }


def normalize_project(project: dict[str, Any]) -> dict[str, Any]:
    if "project_binding_id" in project:
        return dict(project)
    return {
        "id": project.get("id"),
        "name": project.get("name"),
        "engine_version": project.get("engine_version"),
        "project_id": project.get("id"),
        "project_name": project.get("name"),
        "project_root": project.get("project_root"),
    }


def normalize_diagnostics(diagnostics: list[dict[str, Any]] | None) -> list[dict[str, Any]]:
    normalized: list[dict[str, Any]] = []
    for item in diagnostics or []:
        if not isinstance(item, dict):
            continue
        diagnostic = dict(item)
        code = str(diagnostic.get("code") or "")
        diagnostic.setdefault("severity", "warning" if code else "info")
        diagnostic.setdefault("blocking", diagnostic.get("severity") in {"error", "critical"})
        diagnostic.setdefault("phase", "query")
        diagnostic.setdefault("retryable", False)
        if code == "UEPI_REFRESH_REQUESTED":
            diagnostic.setdefault("recoverable", True)
            diagnostic.setdefault(
                "recommended_user_action",
                "Keep the Unreal Editor open and retry after UEPI processes the targeted refresh request.",
            )
            diagnostic.setdefault("recommended_agent_action", {"tool": "uepi_status", "after_seconds": 2})
        elif code == "UEPI_SNAPSHOT_STALE":
            diagnostic.setdefault("recoverable", True)
            diagnostic.setdefault(
                "recommended_user_action",
                "Open the Unreal Editor with UEPI enabled or run a Snapshot scan before expecting current data.",
            )
            diagnostic.setdefault("recommended_agent_action", {"tool": "uepi_status", "after_seconds": 2})
        else:
            diagnostic.setdefault("recoverable", False)
        normalized.append(diagnostic)
    return normalized


def _compact_evidence_item(item: dict[str, Any], fallback_kind: str) -> dict[str, Any]:
    return {
        "kind": item.get("kind") or fallback_kind,
        "asset": item.get("asset") or item.get("asset_path") or item.get("object_path"),
        "graph": item.get("graph") or item.get("graph_name"),
        "node_guid": item.get("node_guid") or item.get("guid"),
        "pin_id": item.get("pin_id"),
        "property_path": item.get("property_path"),
        "relation_id": item.get("relation_id") or item.get("id") if fallback_kind == "relation" else item.get("relation_id"),
        "confidence": item.get("confidence"),
        "source_layer": item.get("source_layer"),
        "observed_at": item.get("observed_at") or item.get("observed_at_utc"),
    }


def collect_evidence(value: Any, limit: int = 50) -> list[dict[str, Any]]:
    evidence: list[dict[str, Any]] = []

    def visit(current: Any, fallback_kind: str = "snapshot") -> None:
        if len(evidence) >= limit:
            return
        if isinstance(current, dict):
            if isinstance(current.get("evidence"), list):
                for item in current["evidence"]:
                    if isinstance(item, dict):
                        evidence.append(_compact_evidence_item(item, fallback_kind))
                        if len(evidence) >= limit:
                            return
            if current.get("id") and fallback_kind == "relation":
                evidence.append(_compact_evidence_item(current, "relation"))
                if len(evidence) >= limit:
                    return
            for key, child in current.items():
                if key in {"snapshot", "attributes", "typed_attributes"}:
                    continue
                next_kind = "relation" if key in {"relations", "incoming", "outgoing"} else fallback_kind
                visit(child, next_kind)
                if len(evidence) >= limit:
                    return
        elif isinstance(current, list):
            for child in current:
                visit(child, fallback_kind)
                if len(evidence) >= limit:
                    return

    visit(value)
    compact: list[dict[str, Any]] = []
    seen: set[str] = set()
    for item in evidence:
        cleaned = {key: val for key, val in item.items() if val not in (None, "", [])}
        key = json.dumps(cleaned, ensure_ascii=False, sort_keys=True)
        if key in seen:
            continue
        seen.add(key)
        compact.append(cleaned)
    return compact[:limit]


def default_next_actions(tool: str, diagnostics: list[dict[str, Any]], result: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    actions: list[dict[str, Any]] = []
    if any(item.get("code") == "UEPI_REFRESH_REQUESTED" for item in diagnostics):
        actions.append(
            {
                "reason": "A targeted refresh has been queued for fresher editor data.",
                "tool": tool,
                "arguments": {},
                "after_seconds": 2,
            }
        )
    if tool == "uepi_search" and result is not None and _as_list(result.get("matches")):
        first = _as_list(result["matches"])[0]
        actions.append(
            {
                "reason": "Open the highest-ranked match for nearby relations and typed attributes.",
                "tool": "uepi_asset",
                "arguments": {"asset": first.get("canonical_key") or first.get("display_name") or first.get("id")},
            }
        )
    if tool == "uepi_context":
        actions.append(
            {
                "reason": "Use a domain-specific tool for any asset selected from this context pack.",
                "tool": "uepi_asset",
                "arguments": {"asset": "<asset from result.matches or route candidates>"},
            }
        )
    return actions


def envelope(
    *,
    tool: str,
    operation: str,
    project: dict[str, Any],
    state: dict[str, Any],
    editor: dict[str, Any] | None = None,
    result: dict[str, Any] | None = None,
    error: dict[str, Any] | None = None,
    diagnostics: list[dict[str, Any]] | None = None,
    omissions: list[str] | None = None,
    truncation: dict[str, Any] | None = None,
    continuation: dict[str, Any] | None = None,
    evidence: list[dict[str, Any]] | None = None,
    next_actions: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    normalized_diagnostics = normalize_diagnostics(diagnostics)
    normalized_error = error
    if normalized_error is None:
        blocking = next((item for item in normalized_diagnostics if item.get("blocking")), None)
        if blocking:
            normalized_error = {
                "code": blocking.get("code") or "UEPI_BLOCKED",
                "message": blocking.get("message") or "The request was blocked by a diagnostic.",
                "retryable": bool(blocking.get("retryable")),
                "candidates": [],
            }
    body: dict[str, Any] = {
        "schema_version": ENVELOPE_SCHEMA_VERSION,
        "ok": normalized_error is None,
        "request_id": str(uuid4()),
        "tool": tool,
        "operation": operation,
        "project": normalize_project(project),
        "editor": editor or {"connected": False, "session_id": None, "source": "snapshot"},
        "snapshot": snapshot_for_state(state),
        "diagnostics": normalized_diagnostics,
        "evidence": evidence if evidence is not None else collect_evidence(result or error or {}),
        "omissions": omissions or [],
        "truncation": truncation or {"truncated": False, "reason": None, "item_limit_hit": False, "byte_limit_hit": False},
        "continuation": continuation or {"has_more": False, "cursor": None},
    }
    if normalized_error is not None:
        body["error"] = normalized_error
        body["result"] = result if result is not None else None
    else:
        body["result"] = result or {}
        body["error"] = None
    body["next_actions"] = next_actions if next_actions is not None else default_next_actions(tool, normalized_diagnostics, result)
    return body


def tool_response(value: dict[str, Any]) -> dict[str, Any]:
    text = json.dumps(value, ensure_ascii=False, indent=2)
    return {
        "content": [{"type": "text", "text": text}],
        "structuredContent": value,
    }
