from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .store import SnapshotStore


BRIDGE_SESSION_SCHEMA = "uepi.editor-bridge-session.v1"


def bridge_session_path(store: SnapshotStore) -> Path:
    return store.root / "runtime" / "editor-bridge.json"


def read_bridge_session(store: SnapshotStore) -> dict[str, Any] | None:
    path = bridge_session_path(store)
    if not path.exists():
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def bridge_status(store: SnapshotStore) -> dict[str, Any]:
    path = bridge_session_path(store)
    session = read_bridge_session(store)
    configured = session is not None and session.get("schema_version") == BRIDGE_SESSION_SCHEMA
    return {
        "supported": True,
        "enabled": False,
        "configured": configured,
        "ready": False,
        "connected": False,
        "transport": "tcp-json-localhost",
        "session_path": str(path),
        "session": session if configured else None,
        "capabilities": [
            "editor.get_status",
            "editor.get_selection",
            "editor.capture_viewport",
            "editor.read_output_log",
            "asset.refresh_now",
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
