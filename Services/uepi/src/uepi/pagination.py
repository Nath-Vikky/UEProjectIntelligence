from __future__ import annotations

import base64
import hashlib
import json
from typing import Any


CURSOR_SCHEMA = "uepi.cursor.v1"


class CursorError(ValueError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def query_hash(tool: str, operation: str, arguments: dict[str, Any]) -> str:
    stable = {key: value for key, value in arguments.items() if key not in {"cursor", "page_size", "max_payload_bytes"}}
    encoded = json.dumps({"tool": tool, "operation": operation, "arguments": stable}, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return "sha256:" + hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def encode_cursor(*, query_hash_value: str, view_generation: int, offset: int, sort_key: str) -> str:
    payload = {
        "schema": CURSOR_SCHEMA,
        "query_hash": query_hash_value,
        "view_generation": int(view_generation),
        "offset": int(offset),
        "sort_key": sort_key,
    }
    raw = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")


def decode_cursor(cursor: str, *, expected_query_hash: str, view_generation: int) -> dict[str, Any]:
    try:
        padded = cursor + "=" * (-len(cursor) % 4)
        value = json.loads(base64.urlsafe_b64decode(padded.encode("ascii")).decode("utf-8"))
    except (ValueError, UnicodeError, json.JSONDecodeError) as exc:
        raise CursorError("UEPI_CURSOR_INVALID", "The pagination cursor is malformed.") from exc
    if not isinstance(value, dict) or value.get("schema") != CURSOR_SCHEMA:
        raise CursorError("UEPI_CURSOR_INVALID", "The pagination cursor schema is unsupported.")
    if value.get("query_hash") != expected_query_hash:
        raise CursorError("UEPI_CURSOR_QUERY_MISMATCH", "The query changed after the cursor was issued.")
    if int(value.get("view_generation") or -1) != int(view_generation):
        raise CursorError("UEPI_CURSOR_STALE", "The Snapshot generation changed after the cursor was issued.")
    return value
