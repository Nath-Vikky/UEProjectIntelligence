from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

from .bridge_client import call_bridge


TERMINAL_PHASES = {"applied", "failed_rolled_back", "rollback_failed", "rolled_back", "recovery_finalized", "recovery_discarded"}


def _file_fingerprint(path: Path) -> dict[str, Any]:
    try:
        data = path.read_bytes()
        stat = path.stat()
    except OSError:
        return {"exists": False, "sha256": None, "size": 0, "mtime_ns": None}
    return {
        "exists": True,
        "sha256": "sha256:" + hashlib.sha256(data).hexdigest(),
        "size": len(data),
        "mtime_ns": stat.st_mtime_ns,
    }


def _project_root(store: Any) -> Path:
    root = Path(store.root).resolve()
    if root.name.casefold() == "ueprojectintelligence" and root.parent.name.casefold() == "saved":
        return root.parent.parent
    return root


def _resolve_journal_path(store: Any, value: str) -> Path:
    text = str(value or "").strip().replace("\\", "/")
    candidate = Path(text).expanduser()
    if candidate.is_absolute():
        return candidate.resolve()
    folded = text.casefold()
    for anchor in ("/content/", "/saved/"):
        index = folded.find(anchor)
        if index >= 0:
            return (_project_root(store) / text[index + 1:]).resolve()
    return (_project_root(store) / text).resolve()


def _discard_confirmation_token(files: list[dict[str, Any]]) -> str:
    state = [
        {
            "package_file": str(item.get("package_file") or ""),
            "exists": bool((item.get("package") or {}).get("exists")),
            "sha256": (item.get("package") or {}).get("sha256"),
        }
        for item in sorted(files, key=lambda value: str(value.get("package_file") or "").casefold())
    ]
    payload = json.dumps(state, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def inspect_recovery(store: Any, transaction_id: str = "") -> list[dict[str, Any]]:
    directory = store.store_dir / "transactions"
    if not directory.is_dir():
        return []
    results: list[dict[str, Any]] = []
    for marker in sorted(directory.glob("*.prepared.json")):
        prefix = marker.name.removesuffix(".prepared.json")
        if any((directory / f"{prefix}.{phase}.json").exists() for phase in TERMINAL_PHASES):
            continue
        try:
            journal = json.loads(marker.read_text(encoding="utf-8-sig"))
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(journal, dict):
            continue
        current_transaction_id = str(journal.get("transaction_id") or "")
        if transaction_id and current_transaction_id != transaction_id:
            continue
        files: list[dict[str, Any]] = []
        all_backups_available = True
        disk_matches_backup = True
        for item in journal.get("backups") or []:
            if not isinstance(item, dict) or not item.get("package_file"):
                continue
            package_file = _resolve_journal_path(store, str(item["package_file"]))
            backup_text = str(item.get("backup_file") or "")
            backup_file = _resolve_journal_path(store, backup_text) if backup_text else None
            package = _file_fingerprint(package_file)
            backup = _file_fingerprint(backup_file) if backup_file else {"exists": True, "sha256": None, "size": 0, "mtime_ns": None}
            backup_represents_missing_package = backup_file is None
            matches = (not package["exists"]) if backup_represents_missing_package else (
                package["exists"] and backup["exists"] and package["sha256"] == backup["sha256"]
            )
            all_backups_available = all_backups_available and bool(backup["exists"])
            disk_matches_backup = disk_matches_backup and matches
            files.append(
                {
                    "package_file": str(package_file),
                    "backup_file": str(backup_file) if backup_file else "",
                    "package": package,
                    "backup": backup,
                    "backup_represents_missing_package": backup_represents_missing_package,
                    "disk_matches_backup": matches,
                }
            )
        recommended_action = "manual_review"
        if all_backups_available:
            recommended_action = "finalize" if disk_matches_backup else "review_current_or_rollback"
        discard_token = _discard_confirmation_token(files)
        results.append(
            {
                "schema_version": "uepi.recovery-inspection.v1",
                "transaction_id": current_transaction_id,
                "marker_path": str(marker),
                "phase": journal.get("phase"),
                "observed_at": journal.get("observed_at"),
                "message": journal.get("message"),
                "affected_assets": journal.get("affected_assets") or [],
                "files": files,
                "all_backups_available": all_backups_available,
                "disk_matches_backup": disk_matches_backup,
                "recommended_action": recommended_action,
                "discard_current_state": {
                    "available": bool(files),
                    "requires_explicit_confirmation": True,
                    "confirmation_token": discard_token,
                    "warning": "Discarding the marker keeps the current package bytes and permanently gives up automatic rollback for this prepared transaction.",
                },
            }
        )
    return results


def inspect(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    transaction_id = str(arguments.get("transaction_id") or "")
    markers = inspect_recovery(engine.store, transaction_id)
    if transaction_id and not markers:
        return engine._error("UEPI_RECOVERY_MARKER_NOT_FOUND", "No unresolved recovery marker matched the requested transaction.", tool="uepi_recovery_inspect", operation="inspect")
    next_actions = []
    for marker in markers:
        transaction = str(marker.get("transaction_id") or "")
        action = str(marker.get("recommended_action") or "")
        if transaction and action == "finalize":
            next_actions.append(
                {
                    "reason": "Disk already matches the backup state; acknowledge the restored transaction.",
                    "tool": f"uepi_recovery_{action}",
                    "arguments": {"transaction_id": transaction},
                }
            )
    return engine._envelope(
        {"marker_count": len(markers), "markers": markers},
        next_actions=next_actions,
        tool="uepi_recovery_inspect",
        operation="inspect",
    )


def _mutate(engine: Any, arguments: dict[str, Any], action: str, bridge_arguments: dict[str, Any] | None = None) -> dict[str, Any]:
    transaction_id = str(arguments.get("transaction_id") or "")
    if not transaction_id:
        return engine._error("UEPI_RECOVERY_TRANSACTION_REQUIRED", "An exact recovery transaction_id is required.", tool=f"uepi_recovery_{action}", operation=action)
    response = call_bridge(
        engine.store,
        f"recovery.{action}",
        {"transaction_id": transaction_id, **(bridge_arguments or {})},
        timeout=30.0,
        expected_identity=engine.identity,
        expected_editor_session_id=str(arguments.get("expected_editor_session_id") or "") or None,
    )
    if not response.get("ok"):
        error = response.get("error") if isinstance(response.get("error"), dict) else {}
        code = str(error.get("code") or next((item.get("code") for item in response.get("diagnostics") or [] if isinstance(item, dict)), f"UEPI_RECOVERY_{action.upper()}_FAILED"))
        return engine._error(code, str(error.get("message") or f"Recovery {action} failed."), diagnostics=response.get("diagnostics") or [], tool=f"uepi_recovery_{action}", operation=action)
    return engine._envelope(
        response.get("result") if isinstance(response.get("result"), dict) else {},
        diagnostics=response.get("diagnostics") or [],
        next_actions=[{"reason": "Confirm recovery is cleared and mutation capability is restored.", "tool": "uepi_status", "arguments": {}}],
        tool=f"uepi_recovery_{action}",
        operation=action,
    )


def finalize(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    return _mutate(engine, arguments, "finalize")


def rollback(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    return _mutate(engine, arguments, "rollback")


def discard(engine: Any, arguments: dict[str, Any]) -> dict[str, Any]:
    transaction_id = str(arguments.get("transaction_id") or "")
    confirmation_token = str(arguments.get("confirmation_token") or "")
    if not bool(arguments.get("acknowledge_keep_current", False)):
        return engine._error(
            "UEPI_RECOVERY_DISCARD_CONFIRMATION_REQUIRED",
            "Discard requires explicit acknowledgement that the current package bytes must be kept.",
            tool="uepi_recovery_discard",
            operation="discard",
        )
    markers = inspect_recovery(engine.store, transaction_id)
    if len(markers) != 1:
        return engine._error(
            "UEPI_RECOVERY_MARKER_NOT_FOUND",
            "No exact unresolved recovery marker matched the discard request.",
            tool="uepi_recovery_discard",
            operation="discard",
        )
    marker = markers[0]
    expected_token = str((marker.get("discard_current_state") or {}).get("confirmation_token") or "")
    if not confirmation_token or confirmation_token != expected_token:
        return engine._error(
            "UEPI_RECOVERY_DISCARD_STATE_CHANGED",
            "The current package fingerprints differ from the inspected state. Inspect again before discarding the marker.",
            tool="uepi_recovery_discard",
            operation="discard",
        )
    confirmed_files = [
        {
            "package_file": item.get("package_file"),
            "package_exists": bool((item.get("package") or {}).get("exists")),
            "package_sha256": (item.get("package") or {}).get("sha256") or "",
        }
        for item in marker.get("files") or []
    ]
    return _mutate(
        engine,
        arguments,
        "discard",
        {
            "acknowledge_keep_current": True,
            "confirmed_current_files": confirmed_files,
        },
    )
