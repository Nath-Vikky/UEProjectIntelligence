from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .bridge_client import call_bridge
from .store import SnapshotStore


def catalog_path(store: SnapshotStore) -> Path:
    return store.store_dir / "catalog" / "operation-catalog.json"


def _load(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def load_catalog(store: SnapshotStore, identity: dict[str, Any], *, refresh: bool = True) -> tuple[dict[str, Any] | None, list[dict[str, Any]], dict[str, Any] | None]:
    diagnostics: list[dict[str, Any]] = []
    bridge_error: dict[str, Any] | None = None
    if refresh:
        response = call_bridge(store, "edit.discover", {}, timeout=2.0, expected_identity=identity)
        if response.get("ok") and isinstance(response.get("result"), dict):
            catalog = dict(response["result"])
            catalog["project_binding_id"] = identity.get("project_binding_id")
            path = catalog_path(store)
            path.parent.mkdir(parents=True, exist_ok=True)
            temp = path.with_suffix(".tmp")
            temp.write_text(json.dumps(catalog, ensure_ascii=False, indent=2), encoding="utf-8")
            temp.replace(path)
            return catalog, diagnostics, None
        bridge_error = response.get("error") if isinstance(response.get("error"), dict) else {
            "code": next((item.get("code") for item in response.get("diagnostics") or [] if isinstance(item, dict)), "UEPI_BRIDGE_NOT_READY"),
            "message": "The guarded Editor Bridge did not return an operation catalog.",
        }

    cached = _load(catalog_path(store))
    if cached and cached.get("project_binding_id") == identity.get("project_binding_id"):
        diagnostics.append(
            {
                "severity": "warning",
                "blocking": False,
                "code": "UEPI_EDIT_CATALOG_CACHED",
                "message": "Using the last Editor-exported operation catalog; Apply will revalidate its hash.",
                "phase": "catalog",
                "retryable": False,
                "recoverable": True,
            }
        )
        return cached, diagnostics, bridge_error
    diagnostics.append(
        {
            "severity": "warning",
            "blocking": False,
            "code": "UEPI_EDIT_CATALOG_UNAVAILABLE",
            "message": "No exact-project Editor catalog is available. Open the project Editor before planning writes.",
            "phase": "catalog",
            "retryable": True,
            "recoverable": True,
        }
    )
    return None, diagnostics, bridge_error


def operation_map(catalog: dict[str, Any] | None) -> dict[str, dict[str, Any]]:
    if not catalog:
        return {}
    return {
        str(item.get("name")): item
        for item in catalog.get("operations") or []
        if isinstance(item, dict) and item.get("name")
    }
