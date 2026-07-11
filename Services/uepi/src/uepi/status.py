from __future__ import annotations

from datetime import datetime, timezone
from typing import Any

from .bridge_client import call_bridge, read_bridge_session
from .identity import session_matches_identity
from .store import SnapshotState, SnapshotStore, _parse_utc


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
    editor = {
        "connected": connected,
        "session_id": session_id or None,
        "pid": (session or {}).get("pid"),
        "heartbeat_age_ms": heartbeat_age_ms,
        "active_map": probe_result.get("active_map") or (session or {}).get("active_map"),
        "pie_state": probe_result.get("pie_state") or "stopped",
        "source": "bridge_probe" if probe else ("project_session" if ready else "snapshot"),
        "observed_at": probe_result.get("observed_at") or (session or {}).get("heartbeat_at") or (session or {}).get("last_seen_at_utc"),
    }
    snapshot = {
        "saved_generation": state.manifest.get("base_saved_generation") or state.manifest.get("saved_generation") or (state.generation if state.data_mode != "live" else None),
        "live_generation": state.generation if state.data_mode == "live" else None,
        "view_generation": state.generation,
        "observed_at": state.manifest.get("created_at_utc"),
        "freshness": "current",
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
        "catalog_current": bool((session or {}).get("catalog_hash")) if connected else False,
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
