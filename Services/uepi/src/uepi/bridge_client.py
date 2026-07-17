from __future__ import annotations

import json
from pathlib import Path
import socket
import struct
import time
from typing import Any
from uuid import uuid4

from .store import SnapshotStore
from .identity import project_identity, session_matches_identity
from .timing import absorb_editor_timing, record


BRIDGE_SESSION_SCHEMAS = {"uepi.editor-bridge-session.v1", "uepi.editor-bridge-session.v2"}
BRIDGE_PROTOCOL = "uepi-bridge-v2"


def bridge_session_path(store: SnapshotStore) -> Path:
    return store.sessions_dir / "editor-bridge.json"


def read_bridge_session(store: SnapshotStore) -> dict[str, Any] | None:
    path = bridge_session_path(store)
    if not path.exists():
        return None
    for attempt in range(3):
        try:
            value = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, json.JSONDecodeError):
            if attempt < 2:
                time.sleep(0.01)
                continue
            return None
        return value if isinstance(value, dict) else None
    return None


def _token_path_candidates(session: dict[str, Any], session_path: Path) -> list[Path]:
    token_path = session.get("token_path")
    if not isinstance(token_path, str) or not token_path:
        return []
    raw = Path(token_path).expanduser()
    candidates = [raw]
    if not raw.is_absolute():
        candidates.append((session_path.parent / raw).resolve())
        candidates.append(session_path.parent / raw.name)
    return candidates


def _read_token(session: dict[str, Any], session_path: Path) -> str | None:
    seen: set[str] = set()
    for candidate in _token_path_candidates(session, session_path):
        key = str(candidate)
        if key in seen:
            continue
        seen.add(key)
        try:
            token = candidate.read_text(encoding="utf-8-sig").strip()
        except OSError:
            continue
        if token:
            return token
    return None


def call_bridge(
    store: SnapshotStore,
    command: str,
    params: dict[str, Any] | None = None,
    timeout: float = 2.0,
    *,
    expected_identity: dict[str, Any] | None = None,
    expected_editor_session_id: str | None = None,
) -> dict[str, Any]:
    session_path = bridge_session_path(store)
    session = read_bridge_session(store)
    if not session or session.get("schema_version") not in BRIDGE_SESSION_SCHEMAS:
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_NOT_CONFIGURED", "message": "Bridge session file is missing."}}
    if not session.get("active") or not session.get("transport_ready"):
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_NOT_READY", "message": "Bridge session exists but transport is not ready."}}
    if expected_identity is None:
        try:
            state = store.load_state()
            expected_identity = project_identity(None, state.project, store.root)
        except Exception:
            expected_identity = None
    if expected_identity and not session_matches_identity(session, expected_identity):
        return {"ok": False, "error": {"code": "UEPI_PROJECT_BINDING_MISMATCH", "message": "Bridge session belongs to a different project binding."}}
    actual_session_id = str(session.get("editor_session_id") or session.get("session_id") or "")
    if expected_editor_session_id and actual_session_id != expected_editor_session_id:
        return {"ok": False, "error": {"code": "UEPI_EDITOR_SESSION_MISMATCH", "message": "Bridge session does not match the requested Editor session."}}
    token = _read_token(session, session_path)
    if not token:
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_TOKEN_MISSING", "message": "Bridge token file is missing or unreadable."}}

    host = str(session.get("host") or "127.0.0.1")
    port = int(session.get("port") or 0)
    if host != "127.0.0.1" or port <= 0:
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_BAD_ENDPOINT", "message": f"Unsupported bridge endpoint: {host}:{port}"}}

    request = {
        "id": f"req:{uuid4().hex}",
        "protocol": str(session.get("protocol") or BRIDGE_PROTOCOL),
        "token": token,
        "command": command,
        "params": {
            **(params or {}),
            "expected_project_binding_id": (expected_identity or {}).get("project_binding_id"),
            "expected_editor_session_id": expected_editor_session_id,
        },
    }
    payload = json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    connect_started_at = time.perf_counter()
    wait_started_at: float | None = None
    try:
        with socket.create_connection((host, port), timeout=timeout) as connection:
            record("bridge_connect_ms", (time.perf_counter() - connect_started_at) * 1000.0)
            wait_started_at = time.perf_counter()
            connection.settimeout(timeout)
            connection.sendall(struct.pack(">I", len(payload)) + payload)
            header = connection.recv(4)
            if len(header) != 4:
                raise OSError("Bridge response header was incomplete.")
            size = struct.unpack(">I", header)[0]
            if size <= 0 or size > 4 * 1024 * 1024:
                raise OSError(f"Bridge response size is invalid: {size}")
            chunks: list[bytes] = []
            remaining = size
            while remaining > 0:
                chunk = connection.recv(remaining)
                if not chunk:
                    raise OSError("Bridge response ended before payload completed.")
                chunks.append(chunk)
                remaining -= len(chunk)
    except OSError as exc:
        if wait_started_at is None:
            record("bridge_connect_ms", (time.perf_counter() - connect_started_at) * 1000.0)
        else:
            record("bridge_wait_ms", (time.perf_counter() - wait_started_at) * 1000.0)
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_CALL_FAILED", "message": str(exc)}}
    wait_elapsed_ms = (time.perf_counter() - wait_started_at) * 1000.0 if wait_started_at is not None else 0.0

    try:
        value = json.loads(b"".join(chunks).decode("utf-8"))
    except json.JSONDecodeError as exc:
        record("bridge_wait_ms", wait_elapsed_ms)
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_BAD_RESPONSE", "message": str(exc)}}
    if not isinstance(value, dict):
        record("bridge_wait_ms", wait_elapsed_ms)
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_BAD_RESPONSE", "message": "Bridge response was not an object."}}
    editor_elapsed_ms = absorb_editor_timing(value.pop("timing", None), wait_elapsed_ms)
    record("bridge_wait_ms", max(0.0, wait_elapsed_ms - editor_elapsed_ms))
    return value


def live_context(store: SnapshotStore, include_log: bool = True) -> dict[str, Any]:
    context: dict[str, Any] = {}
    status = call_bridge(store, "editor.get_status", timeout=1.5)
    context["status"] = status
    if status.get("ok"):
        context["selection"] = call_bridge(store, "editor.get_selection", timeout=1.5)
        if include_log:
            context["output_log"] = call_bridge(store, "editor.read_output_log", {"line_limit": 80}, timeout=1.5)
    return context


def bridge_status(store: SnapshotStore) -> dict[str, Any]:
    path = bridge_session_path(store)
    session = read_bridge_session(store)
    configured = session is not None and session.get("schema_version") in BRIDGE_SESSION_SCHEMAS
    active = bool(configured and session and session.get("active"))
    ready = bool(active and session and session.get("transport_ready"))
    connection_probe: dict[str, Any] | None = None
    connected = False
    if ready:
        connection_probe = call_bridge(store, "editor.get_status", timeout=1.0)
        connected = bool(connection_probe.get("ok"))
    return {
        "supported": True,
        "enabled": active,
        "configured": configured,
        "ready": ready,
        "connected": connected,
        "transport": str(session.get("protocol") or "tcp-json-localhost") if configured and session else "tcp-json-localhost",
        "session_path": str(path),
        "session": session if configured else None,
        "connection_probe": connection_probe,
        "capabilities": [
            "editor.get_status",
            "editor.get_selection",
            "editor.capture_viewport",
            "editor.read_output_log",
            "asset.refresh_now",
            "edit.discover",
            "edit.apply",
            "edit.validate",
            "edit.rollback",
        ],
        "diagnostics": [] if configured else [
            {
                "severity": "info",
                "code": "UEPI_BRIDGE_NOT_CONFIGURED",
                "message": "Optional live editor command bridge is not enabled; Snapshot read tools remain available.",
                "recoverable": True,
                "recommended_user_action": "No action is required for read-only Snapshot queries.",
                "recommended_agent_action": {"tool": "uepi_status"},
            }
        ],
    }
