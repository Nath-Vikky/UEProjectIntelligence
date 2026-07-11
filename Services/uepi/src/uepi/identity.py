from __future__ import annotations

import hashlib
import os
from pathlib import Path
from typing import Any


PROJECT_BINDING_PREFIX = "sha256:"


def _resolve_uproject(value: str | Path) -> Path:
    path = Path(value).expanduser()
    if path.suffix.casefold() == ".uproject":
        return path.resolve(strict=False)
    directory = path.resolve(strict=False)
    candidates = sorted(directory.glob("*.uproject")) if directory.is_dir() else []
    if len(candidates) == 1:
        return candidates[0].resolve(strict=False)
    return directory.resolve(strict=False)


def canonical_project_file(value: str | Path) -> str:
    path = _resolve_uproject(value)
    normalized = path.as_posix().rstrip("/")
    return normalized.casefold() if os.name == "nt" else normalized


def project_binding_id(value: str | Path) -> str:
    canonical = canonical_project_file(value)
    return PROJECT_BINDING_PREFIX + hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def project_identity(
    configured_project: str | Path | None,
    snapshot_project: dict[str, Any] | None = None,
    store_root: str | Path | None = None,
) -> dict[str, Any]:
    snapshot_project = snapshot_project if isinstance(snapshot_project, dict) else {}
    project_file = configured_project or snapshot_project.get("project_file")
    if not project_file and store_root:
        root = Path(store_root).resolve(strict=False)
        project_root = root.parent.parent if root.name == "UEProjectIntelligence" else root
        candidates = sorted(project_root.glob("*.uproject"))
        if len(candidates) == 1:
            project_file = candidates[0]

    canonical = canonical_project_file(project_file) if project_file else ""
    resolved = _resolve_uproject(project_file) if project_file else None
    root = resolved.parent if resolved and resolved.suffix.casefold() == ".uproject" else resolved
    name = str(snapshot_project.get("name") or (resolved.stem if resolved and resolved.suffix.casefold() == ".uproject" else ""))
    return {
        "project_id": snapshot_project.get("id") or snapshot_project.get("project_id"),
        "project_name": name,
        "project_file": resolved.as_posix() if resolved else None,
        "canonical_project_file": canonical or None,
        "project_root": root.as_posix() if root else None,
        "project_binding_id": project_binding_id(project_file) if project_file else None,
        "engine_version": snapshot_project.get("engine_version"),
    }


def project_guard_diagnostics(
    identity: dict[str, Any],
    *,
    expected_project_file: str | Path | None = None,
    expected_project_binding_id: str | None = None,
) -> list[dict[str, Any]]:
    diagnostics: list[dict[str, Any]] = []
    actual_binding = str(identity.get("project_binding_id") or "")
    if expected_project_file:
        expected_binding = project_binding_id(expected_project_file)
        if not actual_binding or expected_binding != actual_binding:
            diagnostics.append(
                {
                    "severity": "error",
                    "blocking": True,
                    "code": "UEPI_PROJECT_MISMATCH",
                    "message": "The request project path does not match the MCP-bound project.",
                    "phase": "request_guard",
                    "retryable": False,
                    "recoverable": True,
                }
            )
    if expected_project_binding_id and expected_project_binding_id != actual_binding:
        diagnostics.append(
            {
                "severity": "error",
                "blocking": True,
                "code": "UEPI_PROJECT_BINDING_MISMATCH",
                "message": "The request project binding does not match the MCP-bound project.",
                "phase": "request_guard",
                "retryable": False,
                "recoverable": True,
            }
        )
    return diagnostics


def session_matches_identity(session: dict[str, Any], identity: dict[str, Any]) -> bool:
    expected = str(identity.get("project_binding_id") or "")
    if not expected:
        return False
    session_binding = str(session.get("project_binding_id") or "")
    if not session_binding and session.get("project_file"):
        session_binding = project_binding_id(str(session["project_file"]))
    return session_binding == expected
