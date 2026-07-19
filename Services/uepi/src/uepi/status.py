from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
from typing import Any

from . import __version__
from .bridge_client import call_bridge, read_bridge_session
from .identity import session_matches_identity
from .store import SnapshotState, SnapshotStore, SnapshotStoreError, _parse_utc


def _cached_catalog_hash(store: SnapshotStore) -> str:
    path = store.store_dir / "catalog" / "operation-catalog.json"
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return ""
    return str(value.get("catalog_hash") or "") if isinstance(value, dict) else ""


def _installed_plugin_version(identity: dict[str, Any]) -> str:
    project_root = identity.get("project_root")
    if not isinstance(project_root, str) or not project_root:
        return ""
    descriptor = Path(project_root) / "Plugins" / "UEProjectIntelligence" / "UEProjectIntelligence.uplugin"
    try:
        value = json.loads(descriptor.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return ""
    return str(value.get("VersionName") or "") if isinstance(value, dict) else ""


def _heartbeat_age_ms(session: dict[str, Any]) -> int | None:
    stamp = (
        _parse_utc(session.get("heartbeat_at"))
        or _parse_utc(session.get("last_heartbeat"))
        or _parse_utc(session.get("last_seen_at_utc"))
        or _parse_utc(session.get("started_at"))
    )
    if stamp is None:
        return None
    return max(0, int((datetime.now(timezone.utc) - stamp).total_seconds() * 1000.0))


def resolve_status(
    store: SnapshotStore,
    state: SnapshotState,
    identity: dict[str, Any],
    *,
    probe_bridge: bool = False,
    expected_editor_session_id: str | None = None,
    session_ttl_seconds: float = 30.0,
) -> dict[str, Any]:
    diagnostics: list[dict[str, Any]] = []
    session = read_bridge_session(store)
    matched = bool(session and session_matches_identity(session, identity))
    heartbeat_age_ms = _heartbeat_age_ms(session or {})
    fresh = heartbeat_age_ms is not None and heartbeat_age_ms <= int(session_ttl_seconds * 1000.0)
    session_id = str((session or {}).get("editor_session_id") or (session or {}).get("session_id") or "")

    if session and not matched:
        diagnostics.append(
            {
                "severity": "error",
                "blocking": True,
                "code": "UEPI_PROJECT_BINDING_MISMATCH",
                "message": "The local Editor Bridge session belongs to a different project binding.",
                "phase": "session_guard",
                "retryable": False,
                "recoverable": True,
            }
        )
    if expected_editor_session_id and session_id != expected_editor_session_id:
        diagnostics.append(
            {
                "severity": "error",
                "blocking": True,
                "code": "UEPI_EDITOR_SESSION_MISMATCH",
                "message": "The live request targets a different Editor session.",
                "phase": "session_guard",
                "retryable": False,
                "recoverable": True,
                "expected_editor_session_id": expected_editor_session_id,
                "current_editor_session_id": session_id or None,
                "recommended_agent_action": {"action": "retry", "expected_editor_session_id": session_id} if session_id else {"tool": "uepi_status"},
            }
        )
    if matched and not fresh:
        diagnostics.append(
            {
                "severity": "warning",
                "blocking": False,
                "code": "UEPI_EDITOR_SESSION_STALE",
                "message": "The project-matched Editor session heartbeat is stale.",
                "phase": "session_guard",
                "retryable": True,
                "recoverable": True,
            }
        )

    probe: dict[str, Any] | None = None
    connected = False
    ready = bool(matched and fresh and session and session.get("active") and session.get("transport_ready"))
    if probe_bridge and ready:
        probe = call_bridge(store, "editor.get_status", timeout=1.0, expected_identity=identity, expected_editor_session_id=expected_editor_session_id)
        connected = bool(probe.get("ok"))
        if not connected:
            diagnostics.append(
                {
                    "severity": "warning",
                    "blocking": False,
                    "code": "UEPI_BRIDGE_CALL_FAILED",
                    "message": str((probe.get("error") or {}).get("message") or "The exact project bridge probe failed."),
                    "phase": "status",
                    "retryable": True,
                    "recoverable": True,
                }
            )
    elif ready:
        connected = True

    probe_result = probe.get("result") if isinstance(probe, dict) and isinstance(probe.get("result"), dict) else {}
    live_catalog_hash = str(probe_result.get("catalog_hash") or (session or {}).get("catalog_hash") or "")
    cached_catalog_hash = _cached_catalog_hash(store)
    plugin_version = str(probe_result.get("plugin_version") or (session or {}).get("plugin_version") or _installed_plugin_version(identity))
    plugin_build_id = str(probe_result.get("plugin_build_id") or (session or {}).get("plugin_build_id") or (f"uepi-{plugin_version}" if plugin_version else ""))
    editor = {
        "connected": connected,
        "session_id": session_id or None,
        "pid": (session or {}).get("pid"),
        "heartbeat_age_ms": heartbeat_age_ms,
        "active_map": probe_result.get("active_map") or (session or {}).get("active_map"),
        "pie_map": probe_result.get("pie_map"),
        "pie_state": probe_result.get("pie_state") or "stopped",
        "simulating": bool(probe_result.get("simulating", False)),
        "pie_owned_by_uepi": bool(probe_result.get("pie_owned_by_uepi", False)),
        "runtime_session_id": probe_result.get("runtime_session_id") or None,
        "gameplay_context": probe_result.get("gameplay_context") if isinstance(probe_result.get("gameplay_context"), dict) else {},
        "recovery_required": bool(probe_result.get("recovery_required", False)),
        "pending_recovery_transaction_id": probe_result.get("pending_recovery_transaction_id") or None,
        "recovery_markers": probe_result.get("recovery_markers") if isinstance(probe_result.get("recovery_markers"), list) else [],
        "catalog_hash": live_catalog_hash or None,
        "plugin_version": plugin_version or None,
        "plugin_build_id": plugin_build_id or None,
        "service_version": __version__,
        "data_mode": probe_result.get("data_mode") or ("live" if connected else state.data_mode),
        "source": "bridge_probe" if probe else ("project_session" if ready else "snapshot"),
        "observed_at": probe_result.get("observed_at") or (session or {}).get("heartbeat_at") or (session or {}).get("last_seen_at_utc"),
    }
    try:
        saved_state = store.load_state("saved")
        saved_generation = saved_state.generation
    except SnapshotStoreError:
        saved_generation = state.manifest.get("base_saved_generation") or (state.generation if state.data_mode != "live" else None)
    live_generation = state.generation if state.data_mode == "live" else None
    live_base_generation = state.manifest.get("base_saved_generation") if state.data_mode == "live" else None
    live_base_current = not live_generation or not saved_generation or not live_base_generation or int(live_base_generation) == int(saved_generation)
    freshness = "current" if live_base_current else "stale"
    if not live_base_current:
        diagnostics.append(
            {
                "severity": "warning",
                "blocking": False,
                "code": "UEPI_LIVE_BASE_STALE",
                "message": "The active live overlay was created from an older saved Snapshot generation.",
                "phase": "status",
                "retryable": True,
                "recoverable": True,
            }
        )
    snapshot = {
        "saved_generation": saved_generation,
        "live_generation": state.generation if state.data_mode == "live" else None,
        "live_base_saved_generation": live_base_generation,
        "view_generation": state.generation,
        "view_identity": f"saved:{saved_generation or 0}|live:{live_generation or 0}|base:{live_base_generation or 0}",
        "generation_semantics": "independent_saved_and_live_streams",
        "generation_comparable": False,
        "observed_at": state.manifest.get("created_at_utc"),
        "freshness": freshness,
        "manifest_path": str(state.manifest_path),
    }
    capabilities = {
        "snapshot_read": state.generation > 0,
        "live_read": connected,
        "mutation": connected and bool((session or {}).get("write_enabled", True)),
        "save": connected and bool((session or {}).get("allow_save", False)),
        "pie_control": connected and bool((session or {}).get("allow_pie", False)),
        "runtime_invoke": connected and bool((session or {}).get("allow_runtime_invoke", False)),
    }
    doctor = {
        "project_bound": bool(identity.get("project_binding_id")),
        "session_bound": bool(matched and fresh),
        "bridge_reachable": connected,
        "snapshot_ready": state.generation > 0,
        "catalog_current": bool(connected and live_catalog_hash),
        "catalog_cache_current": bool(cached_catalog_hash and live_catalog_hash and cached_catalog_hash == live_catalog_hash),
        "plugin_version": plugin_version or None,
        "plugin_build_id": plugin_build_id or None,
        "catalog_hash": live_catalog_hash or cached_catalog_hash or None,
        "service_version": __version__,
        "live_catalog_hash": live_catalog_hash or None,
        "cached_catalog_hash": cached_catalog_hash or None,
    }
    return {
        "project": identity,
        "editor": editor,
        "snapshot": snapshot,
        "capabilities": capabilities,
        "doctor": doctor,
        "diagnostics": diagnostics,
        "session": session if matched else None,
        "probe": probe,
    }
