from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

from .bridge_client import call_bridge


TERMINAL_PHASES = {"applied", "failed_rolled_back", "rollback_failed", "rolled_back", "recovery_finalized"}


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
            package_file = Path(str(item["package_file"]))
            backup_text = str(item.get("backup_file") or "")
            backup_file = Path(backup_text) if backup_text else None
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
            recommended_action = "finalize" if disk_matches_backup else "rollback"
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
        if transaction and action in {"finalize", "rollback"}:
            next_actions.append(
                {
                    "reason": "Disk already matches the backup state; acknowledge the restored transaction." if action == "finalize" else "Restore the exact prepared backup set before allowing new mutations.",
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


def _mutate(engine: Any, arguments: dict[str, Any], action: str) -> dict[str, Any]:
    transaction_id = str(arguments.get("transaction_id") or "")
    if not transaction_id:
        return engine._error("UEPI_RECOVERY_TRANSACTION_REQUIRED", "An exact recovery transaction_id is required.", tool=f"uepi_recovery_{action}", operation=action)
    response = call_bridge(
        engine.store,
        f"recovery.{action}",
        {"transaction_id": transaction_id},
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
