from __future__ import annotations

import json
from pathlib import Path
import socket
import struct
from typing import Any
from uuid import uuid4

from .store import SnapshotStore


BRIDGE_SESSION_SCHEMA = "uepi.editor-bridge-session.v1"
BRIDGE_PROTOCOL = "uepi-bridge-v1"


def bridge_session_path(store: SnapshotStore) -> Path:
    return store.sessions_dir / "editor-bridge.json"


def read_bridge_session(store: SnapshotStore) -> dict[str, Any] | None:
    path = bridge_session_path(store)
    if not path.exists():
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def _read_token(session: dict[str, Any]) -> str | None:
    token_path = session.get("token_path")
    if not isinstance(token_path, str) or not token_path:
        return None
    try:
        return Path(token_path).read_text(encoding="utf-8-sig").strip()
    except OSError:
        return None


def call_bridge(store: SnapshotStore, command: str, params: dict[str, Any] | None = None, timeout: float = 2.0) -> dict[str, Any]:
    session = read_bridge_session(store)
    if not session or session.get("schema_version") != BRIDGE_SESSION_SCHEMA:
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_NOT_CONFIGURED", "message": "Bridge session file is missing."}}
    if not session.get("active") or not session.get("transport_ready"):
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_NOT_READY", "message": "Bridge session exists but transport is not ready."}}
    token = _read_token(session)
    if not token:
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_TOKEN_MISSING", "message": "Bridge token file is missing or unreadable."}}

    host = str(session.get("host") or "127.0.0.1")
    port = int(session.get("port") or 0)
    if host != "127.0.0.1" or port <= 0:
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_BAD_ENDPOINT", "message": f"Unsupported bridge endpoint: {host}:{port}"}}

    request = {
        "id": f"req:{uuid4().hex}",
        "protocol": BRIDGE_PROTOCOL,
        "token": token,
        "command": command,
        "params": params or {},
    }
    payload = json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    try:
        with socket.create_connection((host, port), timeout=timeout) as connection:
            connection.settimeout(timeout)
            connection.sendall(struct.pack(">I", len(payload)) + payload)
            header = connection.recv(4)
            if len(header) != 4:
                raise OSError("Bridge response header was incomplete.")
            size = struct.unpack(">I", header)[0]
            if size <= 0 or size > 1024 * 1024:
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
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_CALL_FAILED", "message": str(exc)}}

    try:
        value = json.loads(b"".join(chunks).decode("utf-8"))
    except json.JSONDecodeError as exc:
        return {"ok": False, "error": {"code": "UEPI_BRIDGE_BAD_RESPONSE", "message": str(exc)}}
    return value if isinstance(value, dict) else {"ok": False, "error": {"code": "UEPI_BRIDGE_BAD_RESPONSE", "message": "Bridge response was not an object."}}


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
    configured = session is not None and session.get("schema_version") == BRIDGE_SESSION_SCHEMA
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
