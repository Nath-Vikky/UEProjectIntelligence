from __future__ import annotations

import json
from typing import Any

from .pagination import CursorError, decode_cursor, encode_cursor, query_hash


DEFAULT_PAGE_ROOTS = (
    "matches",
    "blueprint_entities",
    "animation_entities",
    "properties",
    "operations",
    "actors",
    "components",
    "relations",
    "entities",
    "paths",
    "top_assets",
)

PAGE_ROOTS_BY_TOOL: dict[str, tuple[str, ...]] = {
    "uepi_search": ("matches",),
    "uepi_context": ("matches",),
    "uepi_blueprint": ("blueprint_entities", "relations"),
    "uepi_blueprint_trace": ("paths",),
    "uepi_animation": ("animation_entities", "driver_track_curves"),
    "uepi_impact": ("relations", "entities"),
    "uepi_world": ("actors", "components"),
    "uepi_schema": ("properties", "operations", "pins", "functions"),
    "uepi_edit_discover": ("operations",),
}

ARTIFACT_PAYLOAD_MANIFESTS = {
    "bone_motion_profile": "bone_motion_profile_manifest",
    "reconstruction_profile": "reconstruction_profile_manifest",
    "driver_track_curves": "reconstruction_profile_manifest",
    "full_pose_artifact": "reconstruction_profile_manifest",
}

IDENTITY_FIELDS = ("id", "kind", "canonical_key", "display_name", "name", "path")
LOW_PRIORITY_KEYS = (
    "snapshot",
    "asset_snapshot",
    "live_editor",
    "output_log",
    "sections",
    "full_pose_artifact",
    "reconstruction_profile",
    "bone_motion_profile",
    "driver_track_curves",
)


def _field_tree(fields: list[str]) -> dict[str, Any]:
    tree: dict[str, Any] = {}
    for field in fields:
        parts = [part for part in str(field).split(".") if part and part != "result"]
        node = tree
        for part in parts:
            node = node.setdefault(part, {})
    return tree


def _project(value: Any, tree: dict[str, Any]) -> Any:
    if not tree:
        return value
    if isinstance(value, list):
        return [_project(item, tree) for item in value]
    if not isinstance(value, dict):
        return value
    projected: dict[str, Any] = {}
    for key, subtree in tree.items():
        if key in value:
            projected[key] = _project(value[key], subtree)
    for key in IDENTITY_FIELDS:
        if key in value and key not in projected:
            projected[key] = value[key]
    return projected


def _compact(value: Any) -> Any:
    if isinstance(value, list):
        return [_compact(item) for item in value]
    if not isinstance(value, dict):
        return value
    omitted = {"snapshot", "evidence", "diagnostics", "completeness"}
    return {key: _compact(child) for key, child in value.items() if key not in omitted and child not in (None, [], {}, "")}


def _path_value(value: dict[str, Any], path: str) -> Any:
    current: Any = value
    for part in path.split("."):
        if not isinstance(current, dict) or part not in current:
            return None
        current = current[part]
    return current


def _set_path(value: dict[str, Any], path: str, replacement: Any) -> bool:
    parts = path.split(".")
    current: Any = value
    for part in parts[:-1]:
        if not isinstance(current, dict) or part not in current:
            return False
        current = current[part]
    if not isinstance(current, dict) or parts[-1] not in current:
        return False
    current[parts[-1]] = replacement
    return True


def _delete_path(value: dict[str, Any], path: str) -> bool:
    parts = path.split(".")
    current: Any = value
    for part in parts[:-1]:
        if not isinstance(current, dict) or part not in current:
            return False
        current = current[part]
    if not isinstance(current, dict) or parts[-1] not in current:
        return False
    del current[parts[-1]]
    return True


def _list_paths(value: Any, prefix: str = "") -> list[str]:
    paths: list[str] = []
    if isinstance(value, dict):
        for key, child in value.items():
            path = f"{prefix}.{key}" if prefix else key
            if isinstance(child, list):
                paths.append(path)
            paths.extend(_list_paths(child, path))
    return paths


def _page_root(result: dict[str, Any], tool: str, fields: list[str]) -> tuple[str | None, list[Any] | None]:
    available = [path for path in _list_paths(result) if isinstance(_path_value(result, path), list)]
    if not available:
        return None, None
    requested_roots = [field.removeprefix("result.") for field in fields]
    for requested in requested_roots:
        for path in available:
            if requested == path or requested.startswith(path + "."):
                return path, _path_value(result, path)
    preferred = PAGE_ROOTS_BY_TOOL.get(tool, DEFAULT_PAGE_ROOTS)
    for root in preferred:
        for path in available:
            if path == root or path.endswith("." + root):
                return path, _path_value(result, path)
    path = available[0]
    return path, _path_value(result, path)


def _encoded_size(envelope: dict[str, Any]) -> int:
    return len(json.dumps(envelope, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))


def _set_payload_size(envelope: dict[str, Any]) -> int:
    envelope["payload_bytes"] = int(envelope.get("payload_bytes") or 0)
    for _ in range(4):
        size = _encoded_size(envelope)
        if envelope["payload_bytes"] == size:
            return size
        envelope["payload_bytes"] = size
    size = _encoded_size(envelope)
    envelope["payload_bytes"] = size
    return _encoded_size(envelope)


def _mark_omission(envelope: dict[str, Any], path: str) -> None:
    omission = f"result.{path}_omitted_by_payload_budget"
    envelope["omissions"] = list(dict.fromkeys([*(envelope.get("omissions") or []), omission]))
    truncation = dict(envelope.get("truncation") or {})
    truncation.update({"truncated": True, "reason": "byte_limit", "byte_limit_hit": True})
    truncation.setdefault("item_limit_hit", False)
    envelope["truncation"] = truncation


def _largest_removable_path(value: Any, protected_path: str | None, prefix: str = "") -> tuple[str | None, int]:
    best_path: str | None = None
    best_size = 0
    if not isinstance(value, dict):
        return best_path, best_size
    for key, child in value.items():
        path = f"{prefix}.{key}" if prefix else key
        if path == protected_path or (protected_path and protected_path.startswith(path + ".")):
            nested_path, nested_size = _largest_removable_path(child, protected_path, path)
            if nested_size > best_size:
                best_path, best_size = nested_path, nested_size
            continue
        if key in IDENTITY_FIELDS or key.endswith("_manifest") or child in (None, "", [], {}):
            continue
        size = len(json.dumps(child, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
        if isinstance(child, dict):
            nested_path, nested_size = _largest_removable_path(child, protected_path, path)
            if nested_size > size:
                path, size = nested_path or path, nested_size
        if size > best_size:
            best_path, best_size = path, size
    return best_path, best_size


def _ensure_artifact_manifests(original: dict[str, Any], projected: dict[str, Any], fields: list[str]) -> None:
    requested = {field.removeprefix("result.").split(".", 1)[0] for field in fields}
    for payload_key, manifest_key in ARTIFACT_PAYLOAD_MANIFESTS.items():
        if payload_key in requested and manifest_key in original and manifest_key not in projected:
            projected[manifest_key] = original[manifest_key]


def _trim_to_budget(
    envelope: dict[str, Any],
    result: dict[str, Any],
    *,
    max_bytes: int,
    page_path: str | None,
    offset: int,
    qhash: str,
    view_generation: int,
) -> None:
    page_items = _path_value(result, page_path) if page_path else None
    while isinstance(page_items, list) and len(page_items) > 1 and _set_payload_size(envelope) > max_bytes:
        page_items = page_items[: max(1, len(page_items) // 2)]
        _set_path(result, page_path or "", page_items)
        envelope["continuation"] = {
            "has_more": True,
            "cursor": encode_cursor(query_hash_value=qhash, view_generation=view_generation, offset=offset + len(page_items), sort_key=page_path or "none"),
        }
        envelope["truncation"] = {"truncated": True, "reason": "byte_limit", "item_limit_hit": True, "byte_limit_hit": True}

    if _set_payload_size(envelope) <= max_bytes:
        return

    for payload_key, manifest_key in ARTIFACT_PAYLOAD_MANIFESTS.items():
        if payload_key in result and manifest_key in result and _delete_path(result, payload_key):
            _mark_omission(envelope, payload_key)
            if _set_payload_size(envelope) <= max_bytes:
                return

    for low_priority_key in LOW_PRIORITY_KEYS:
        candidates = [path for path in _list_container_paths(result) if path.split(".")[-1] == low_priority_key]
        for path in candidates:
            if page_path and (path == page_path or page_path.startswith(path + ".")):
                continue
            if _delete_path(result, path):
                _mark_omission(envelope, path)
                if _set_payload_size(envelope) <= max_bytes:
                    return

    while _set_payload_size(envelope) > max_bytes:
        path, size = _largest_removable_path(result, page_path)
        if not path or size <= 0 or not _delete_path(result, path):
            break
        _mark_omission(envelope, path)

    page_items = _path_value(result, page_path) if page_path else None
    while _set_payload_size(envelope) > max_bytes and isinstance(page_items, list):
        largest_item_path: str | None = None
        largest_item_index = -1
        largest_item_size = 0
        for index, item in enumerate(page_items):
            path, size = _largest_removable_path(item, None)
            if path and size > largest_item_size:
                largest_item_path = path
                largest_item_index = index
                largest_item_size = size
        if largest_item_index < 0 or largest_item_path is None:
            break
        if not _delete_path(page_items[largest_item_index], largest_item_path):
            break
        _mark_omission(envelope, f"{page_path}[{largest_item_index}].{largest_item_path}")

    if _set_payload_size(envelope) > max_bytes and isinstance(envelope.get("evidence"), list):
        envelope["evidence"] = _compact(envelope["evidence"][:3])
        _mark_omission(envelope, "evidence")
    if _set_payload_size(envelope) > max_bytes and isinstance(envelope.get("next_actions"), list):
        envelope["next_actions"] = []
        _mark_omission(envelope, "next_actions")
    if _set_payload_size(envelope) > max_bytes and isinstance(envelope.get("diagnostics"), list):
        compact_diagnostics: list[dict[str, Any]] = []
        for item in envelope["diagnostics"]:
            if not isinstance(item, dict):
                continue
            compact_diagnostics.append(
                {
                    key: (str(item[key])[:320] if key == "message" else item[key])
                    for key in ("severity", "blocking", "code", "message", "phase", "retryable", "recoverable")
                    if key in item
                }
            )
        envelope["diagnostics"] = compact_diagnostics[:8]
        _mark_omission(envelope, "diagnostics_detail")
    if _set_payload_size(envelope) > max_bytes:
        envelope["omissions"] = list(envelope.get("omissions") or [])[:24]
        envelope["result"] = {"omitted_by_payload_budget": True}
        _mark_omission(envelope, "result")
    if _set_payload_size(envelope) > max_bytes:
        envelope["evidence"] = []
        envelope["next_actions"] = []


def _list_container_paths(value: Any, prefix: str = "") -> list[str]:
    paths: list[str] = []
    if not isinstance(value, dict):
        return paths
    for key, child in value.items():
        path = f"{prefix}.{key}" if prefix else key
        if isinstance(child, (dict, list)):
            paths.append(path)
        if isinstance(child, dict):
            paths.extend(_list_container_paths(child, path))
    return paths


def apply_response_options(envelope: dict[str, Any], arguments: dict[str, Any]) -> dict[str, Any]:
    max_bytes = max(4096, min(int(arguments.get("max_payload_bytes") or 131072), 4 * 1024 * 1024))
    if not isinstance(envelope.get("result"), dict):
        _set_payload_size(envelope)
        return envelope

    original_result = dict(envelope["result"])
    result = dict(original_result)
    fields = [str(item) for item in arguments.get("fields") or [] if isinstance(item, str)]
    if fields:
        result = _project(result, _field_tree(fields))
        _ensure_artifact_manifests(original_result, result, fields)

    tool = str(envelope.get("tool") or "")
    page_path, items = _page_root(result, tool, fields)
    view_generation = int((envelope.get("snapshot") or {}).get("view_generation") or 0)
    qhash = query_hash(tool, str(envelope.get("operation") or ""), arguments)
    offset = 0
    cursor = arguments.get("cursor")
    if isinstance(cursor, str) and cursor:
        try:
            decoded = decode_cursor(cursor, expected_query_hash=qhash, view_generation=view_generation)
            offset = max(0, int(decoded.get("offset") or 0))
        except CursorError as exc:
            envelope["ok"] = False
            envelope["error"] = {"code": exc.code, "message": str(exc), "retryable": False, "candidates": []}
            envelope["result"] = None
            envelope.setdefault("diagnostics", []).append(
                {"severity": "error", "blocking": True, "code": exc.code, "message": str(exc), "phase": "pagination", "retryable": False, "recoverable": True}
            )
            _set_payload_size(envelope)
            return envelope

    page_size = max(1, min(int(arguments.get("page_size") or arguments.get("limit") or 100), 500))
    total = len(items) if items is not None else 0
    if page_path and items is not None:
        page = items[offset : offset + page_size]
        if bool(arguments.get("compact", True)):
            page = _compact(page)
        _set_path(result, page_path, page)

    evidence_level = str(arguments.get("evidence_level") or "summary")
    if evidence_level == "none":
        envelope["evidence"] = []
    elif evidence_level == "summary":
        envelope["evidence"] = _compact(envelope.get("evidence") or [])

    page_count = len(_path_value(result, page_path) or []) if page_path else 0
    has_more = bool(page_path and items is not None and offset + page_count < total)
    envelope["continuation"] = {
        "has_more": has_more,
        "cursor": encode_cursor(query_hash_value=qhash, view_generation=view_generation, offset=offset + page_count, sort_key=page_path or "none") if has_more else None,
    }
    envelope["truncation"] = {
        "truncated": has_more,
        "reason": "item_limit" if has_more else None,
        "item_limit_hit": has_more,
        "byte_limit_hit": False,
    }
    envelope["result"] = result
    envelope["payload_bytes"] = 0
    _trim_to_budget(
        envelope,
        result,
        max_bytes=max_bytes,
        page_path=page_path,
        offset=offset,
        qhash=qhash,
        view_generation=view_generation,
    )
    _set_payload_size(envelope)
    return envelope


def enforce_response_budget(envelope: dict[str, Any], arguments: dict[str, Any]) -> dict[str, Any]:
    """Rebalance an already projected envelope after late metadata is attached."""
    max_bytes = max(4096, min(int(arguments.get("max_payload_bytes") or 131072), 4 * 1024 * 1024))
    if _set_payload_size(envelope) <= max_bytes or not isinstance(envelope.get("result"), dict):
        return envelope

    result = envelope["result"]
    fields = [str(item) for item in arguments.get("fields") or [] if isinstance(item, str)]
    tool = str(envelope.get("tool") or "")
    page_path, _ = _page_root(result, tool, fields)
    view_generation = int((envelope.get("snapshot") or {}).get("view_generation") or 0)
    qhash = query_hash(tool, str(envelope.get("operation") or ""), arguments)
    offset = 0
    cursor = arguments.get("cursor")
    if isinstance(cursor, str) and cursor:
        try:
            offset = max(0, int(decode_cursor(cursor, expected_query_hash=qhash, view_generation=view_generation).get("offset") or 0))
        except CursorError:
            offset = 0
    _trim_to_budget(
        envelope,
        result,
        max_bytes=max_bytes,
        page_path=page_path,
        offset=offset,
        qhash=qhash,
        view_generation=view_generation,
    )
    _set_payload_size(envelope)
    return envelope
