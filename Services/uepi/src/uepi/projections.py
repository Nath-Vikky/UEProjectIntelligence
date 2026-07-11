from __future__ import annotations

import json
from typing import Any

from .pagination import CursorError, decode_cursor, encode_cursor, query_hash


PRIMARY_LIST_KEYS = (
    "matches",
    "blueprint_entities",
    "animation_entities",
    "relations",
    "entities",
    "paths",
    "top_assets",
)


def _primary_list(result: dict[str, Any]) -> tuple[str | None, list[Any] | None]:
    for key in PRIMARY_LIST_KEYS:
        value = result.get(key)
        if isinstance(value, list):
            return key, value
    return None, None


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
    for key in ("id", "kind", "canonical_key", "display_name"):
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


def apply_response_options(envelope: dict[str, Any], arguments: dict[str, Any]) -> dict[str, Any]:
    if not envelope.get("ok") or not isinstance(envelope.get("result"), dict):
        return envelope
    result = dict(envelope["result"])
    key, items = _primary_list(result)
    view_generation = int((envelope.get("snapshot") or {}).get("view_generation") or 0)
    qhash = query_hash(str(envelope.get("tool") or ""), str(envelope.get("operation") or ""), arguments)
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
            return envelope

    page_size = max(1, min(int(arguments.get("page_size") or arguments.get("limit") or 100), 500))
    total = len(items) if items is not None else 0
    if key and items is not None:
        page = items[offset : offset + page_size]
        fields = arguments.get("fields") if isinstance(arguments.get("fields"), list) else []
        if fields:
            page = _project(page, _field_tree([str(item) for item in fields]))
        if bool(arguments.get("compact", True)):
            page = _compact(page)
        result[key] = page

    evidence_level = str(arguments.get("evidence_level") or "summary")
    if evidence_level == "none":
        envelope["evidence"] = []
    elif evidence_level == "summary":
        envelope["evidence"] = _compact(envelope.get("evidence") or [])

    has_more = bool(key and items is not None and offset + len(result.get(key) or []) < total)
    next_offset = offset + len(result.get(key) or [])
    envelope["continuation"] = {
        "has_more": has_more,
        "cursor": encode_cursor(query_hash_value=qhash, view_generation=view_generation, offset=next_offset, sort_key=key or "none") if has_more else None,
    }
    envelope["truncation"] = {
        "truncated": has_more,
        "reason": "item_limit" if has_more else None,
        "item_limit_hit": has_more,
        "byte_limit_hit": False,
    }
    envelope["result"] = result

    max_bytes = max(4096, min(int(arguments.get("max_payload_bytes") or 131072), 4 * 1024 * 1024))
    encoded_size = len(json.dumps(envelope, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
    while key and isinstance(result.get(key), list) and len(result[key]) > 1 and encoded_size > max_bytes:
        result[key] = result[key][: max(1, len(result[key]) // 2)]
        next_offset = offset + len(result[key])
        envelope["continuation"] = {
            "has_more": True,
            "cursor": encode_cursor(query_hash_value=qhash, view_generation=view_generation, offset=next_offset, sort_key=key),
        }
        envelope["truncation"] = {"truncated": True, "reason": "byte_limit", "item_limit_hit": has_more, "byte_limit_hit": True}
        encoded_size = len(json.dumps(envelope, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
    if key and isinstance(result.get(key), list) and result[key] and encoded_size > max_bytes:
        result[key] = []
        envelope["omissions"] = list(dict.fromkeys((envelope.get("omissions") or []) + [f"{key}_omitted_by_payload_budget"]))
        envelope["continuation"] = {
            "has_more": True,
            "cursor": encode_cursor(query_hash_value=qhash, view_generation=view_generation, offset=offset, sort_key=key),
        }
        envelope["truncation"] = {"truncated": True, "reason": "byte_limit", "item_limit_hit": has_more, "byte_limit_hit": True}
        encoded_size = len(json.dumps(envelope, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
    envelope["payload_bytes"] = encoded_size
    return envelope
