from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sqlite3
import time
from typing import Any
from uuid import uuid4

from .store import SnapshotState, SnapshotStore


SCHEMA_VERSION = "uepi.sqlite-cache.v2.1"
POINTER_SCHEMA_VERSION = "uepi.sqlite-cache-pointer.v1"
SYNC_LOCK_TIMEOUT_SECONDS = 60.0
SYNC_LOCK_STALE_SECONDS = 180.0


def _cache_dir(store: SnapshotStore) -> Path:
    return store.root / "cache"


def _legacy_cache_path(store: SnapshotStore) -> Path:
    return _cache_dir(store) / "uepi.sqlite3"


def _mode_name(state: SnapshotState) -> str:
    return "live" if state.data_mode == "live" else "saved"


def _pointer_path(store: SnapshotStore, state: SnapshotState) -> Path:
    return _cache_dir(store) / f"current-{_mode_name(state)}.json"


def _load_pointer(store: SnapshotStore, state: SnapshotState) -> dict[str, Any] | None:
    path = _pointer_path(store, state)
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(value, dict) or value.get("schema_version") != POINTER_SCHEMA_VERSION:
        return None
    cache_file = value.get("cache_file")
    if not isinstance(cache_file, str) or not cache_file or Path(cache_file).name != cache_file:
        return None
    return value


def cache_path(store: SnapshotStore, state: SnapshotState | None = None) -> Path:
    state = state or store.load_state()
    pointer = _load_pointer(store, state)
    if pointer:
        candidate = _cache_dir(store) / str(pointer["cache_file"])
        if candidate.exists():
            return candidate
    return _legacy_cache_path(store)


def _json(value: Any) -> str:
    return json.dumps(value if value is not None else {}, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _coerce_typed_attribute_value(value: Any) -> dict[str, Any]:
    if isinstance(value, dict) and value.get("schema_version") == "uepi.attribute-value.v2":
        return value

    raw = "" if value is None else str(value)
    stripped = raw.strip()
    result: dict[str, Any] = {
        "schema_version": "uepi.attribute-value.v2",
        "source_type": "legacy_string",
        "raw": raw,
    }
    lowered = stripped.casefold()
    if lowered in {"true", "false"}:
        result.update({"type": "boolean", "value": lowered == "true"})
        return result
    if lowered == "null":
        result.update({"type": "null", "value": None})
        return result
    if stripped:
        try:
            parsed = json.loads(stripped)
        except json.JSONDecodeError:
            parsed = None
        else:
            if isinstance(parsed, bool):
                result.update({"type": "boolean", "value": parsed})
                return result
            if parsed is None:
                result.update({"type": "null", "value": None})
                return result
            if isinstance(parsed, int) and not (len(stripped) > 1 and stripped[0] == "0"):
                result.update({"type": "integer", "value": parsed})
                return result
            if isinstance(parsed, float):
                result.update({"type": "number", "value": parsed})
                return result
            if isinstance(parsed, list):
                result.update({"type": "array", "value": parsed})
                return result
            if isinstance(parsed, dict):
                result.update({"type": "object", "value": parsed})
                return result
    result.update({"type": "string", "value": raw})
    return result


def typed_attributes_for(record: dict[str, Any]) -> dict[str, Any]:
    typed = record.get("typed_attributes")
    if isinstance(typed, dict):
        return typed
    attributes = record.get("attributes")
    if not isinstance(attributes, dict):
        return {}
    return {str(key): _coerce_typed_attribute_value(value) for key, value in attributes.items()}


def _json_object(value: str | None) -> dict[str, Any]:
    if not value:
        return {}
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError:
        return {}
    return parsed if isinstance(parsed, dict) else {}


def _json_array(value: str | None) -> list[Any]:
    if not value:
        return []
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError:
        return []
    return parsed if isinstance(parsed, list) else []


def _connect(path: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(path)
    # Generated cache files are immutable after publication; a single-file
    # journal avoids leaving WAL sidecars behind when the file is renamed.
    connection.execute("PRAGMA journal_mode=DELETE")
    connection.execute("PRAGMA synchronous=NORMAL")
    return connection


def _create_schema(connection: sqlite3.Connection) -> None:
    connection.executescript(
        """
        CREATE TABLE metadata (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        CREATE TABLE entities (
            id TEXT PRIMARY KEY,
            kind TEXT NOT NULL,
            canonical_key TEXT NOT NULL,
            display_name TEXT NOT NULL,
            source_layer TEXT NOT NULL,
            attributes_json TEXT NOT NULL,
            typed_attributes_json TEXT NOT NULL,
            completeness_json TEXT NOT NULL,
            diagnostics_json TEXT NOT NULL,
            evidence_json TEXT NOT NULL,
            snapshot_json TEXT NOT NULL
        );

        CREATE TABLE relations (
            id TEXT PRIMARY KEY,
            type TEXT NOT NULL,
            from_id TEXT NOT NULL,
            to_id TEXT NOT NULL,
            source_layer TEXT NOT NULL,
            derived INTEGER NOT NULL,
            confidence REAL NOT NULL,
            attributes_json TEXT NOT NULL,
            typed_attributes_json TEXT NOT NULL,
            evidence_json TEXT NOT NULL
        );

        CREATE INDEX idx_entities_kind ON entities(kind);
        CREATE INDEX idx_entities_display_name ON entities(display_name);
        CREATE INDEX idx_entities_canonical_key ON entities(canonical_key);
        CREATE INDEX idx_entities_kind_display_name ON entities(kind, display_name);
        CREATE INDEX idx_relations_type ON relations(type);
        CREATE INDEX idx_relations_from ON relations(from_id);
        CREATE INDEX idx_relations_to ON relations(to_id);
        """
    )


def _metadata_for_state(state: SnapshotState, scan: dict[str, Any], entity_count: int, relation_count: int) -> dict[str, str]:
    return {
        "schema_version": SCHEMA_VERSION,
        "synced_at_epoch": str(time.time()),
        "data_mode": state.data_mode,
        "generation": str(state.generation),
        "manifest_path": str(state.manifest_path),
        "project_id": str(state.project.get("id") or scan.get("project_id") or ""),
        "project_name": str(state.project.get("name") or scan.get("project_name") or ""),
        "engine_version": str(state.project.get("engine_version") or scan.get("engine_version") or ""),
        "entity_count": str(entity_count),
        "relation_count": str(relation_count),
    }


def _read_metadata(path: Path) -> dict[str, str]:
    connection: sqlite3.Connection | None = None
    try:
        connection = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True, timeout=1.0)
        return {str(key): str(value) for key, value in connection.execute("SELECT key, value FROM metadata").fetchall()}
    finally:
        if connection is not None:
            connection.close()


def _sync_result_from_status(status: dict[str, Any], *, reused: bool) -> dict[str, Any]:
    return {
        "schema_version": str(status.get("schema_version") or SCHEMA_VERSION),
        "cache_path": str(status.get("path") or ""),
        "pointer_path": status.get("pointer_path"),
        "data_mode": str(status.get("data_mode") or ""),
        "generation": int(status.get("generation") or 0),
        "entity_count": int(status.get("entity_count") or 0),
        "relation_count": int(status.get("relation_count") or 0),
        "reused": reused,
    }


@contextmanager
def _sync_lock(store: SnapshotStore, state: SnapshotState):
    directory = _cache_dir(store)
    directory.mkdir(parents=True, exist_ok=True)
    lock_path = directory / f"sync-{_mode_name(state)}.lock"
    token = f"{os.getpid()}:{uuid4().hex}"
    deadline = time.monotonic() + SYNC_LOCK_TIMEOUT_SECONDS
    while True:
        try:
            descriptor = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            try:
                stale = time.time() - lock_path.stat().st_mtime > SYNC_LOCK_STALE_SECONDS
            except OSError:
                stale = False
            if stale:
                try:
                    lock_path.unlink()
                except OSError:
                    pass
                continue
            if time.monotonic() >= deadline:
                raise TimeoutError(f"Timed out waiting for SQLite cache sync lock: {lock_path}")
            time.sleep(0.05)
            continue
        else:
            with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
                handle.write(token)
            break

    try:
        yield
    finally:
        try:
            if lock_path.read_text(encoding="utf-8") == token:
                lock_path.unlink()
        except OSError:
            pass


def _publish_pointer(store: SnapshotStore, state: SnapshotState, destination: Path) -> Path:
    pointer_path = _pointer_path(store, state)
    pointer = {
        "schema_version": POINTER_SCHEMA_VERSION,
        "data_mode": state.data_mode,
        "generation": state.generation,
        "manifest_path": str(state.manifest_path),
        "cache_file": destination.name,
        "published_at_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
    }
    temp_path = pointer_path.with_name(f".{pointer_path.name}.{os.getpid()}.{uuid4().hex}.tmp")
    temp_path.write_text(json.dumps(pointer, ensure_ascii=False, sort_keys=True, separators=(",", ":")), encoding="utf-8")
    delay = 0.02
    try:
        for attempt in range(8):
            try:
                os.replace(temp_path, pointer_path)
                return pointer_path
            except PermissionError:
                if attempt == 7:
                    raise
                time.sleep(delay)
                delay = min(delay * 2.0, 0.5)
    finally:
        try:
            temp_path.unlink()
        except OSError:
            pass
    return pointer_path


def _cleanup_old_generations(store: SnapshotStore, state: SnapshotState, current: Path, keep: int = 4) -> None:
    pattern = f"uepi-{_mode_name(state)}-g*.sqlite3"
    candidates = sorted(_cache_dir(store).glob(pattern), key=lambda item: item.stat().st_mtime, reverse=True)
    protected = {current.resolve()}
    protected.update(item.resolve() for item in candidates[: max(1, keep)])
    for candidate in candidates:
        try:
            if candidate.resolve() not in protected:
                candidate.unlink()
        except OSError:
            # A previous generation may still be held by another Windows reader.
            pass


def sync_cache(store: SnapshotStore, state: SnapshotState | None = None) -> dict[str, Any]:
    state = state or store.load_state()
    existing = cache_status(store, state)
    if existing.get("synced"):
        return _sync_result_from_status(existing, reused=True)

    with _sync_lock(store, state):
        existing = cache_status(store, state)
        if existing.get("synced"):
            return _sync_result_from_status(existing, reused=True)

        return _build_and_publish_cache(store, state)


def _build_and_publish_cache(store: SnapshotStore, state: SnapshotState) -> dict[str, Any]:
    scan = store.load_project_scan(state)
    entities = [entity for entity in scan.get("entities") or [] if isinstance(entity, dict)]
    relations = [relation for relation in scan.get("relations") or [] if isinstance(relation, dict)]

    directory = _cache_dir(store)
    directory.mkdir(parents=True, exist_ok=True)
    build_id = uuid4().hex
    destination = directory / f"uepi-{_mode_name(state)}-g{state.generation}-{build_id}.sqlite3"
    temp_path = directory / f".{destination.name}.{os.getpid()}.tmp"

    connection = _connect(temp_path)
    try:
        _create_schema(connection)
        metadata = _metadata_for_state(state, scan, len(entities), len(relations))
        connection.executemany("INSERT INTO metadata(key, value) VALUES (?, ?)", sorted(metadata.items()))
        connection.executemany(
            """
            INSERT OR REPLACE INTO entities(
                id, kind, canonical_key, display_name, source_layer,
                attributes_json, typed_attributes_json, completeness_json, diagnostics_json, evidence_json, snapshot_json
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                (
                    str(entity.get("id") or ""),
                    str(entity.get("kind") or ""),
                    str(entity.get("canonical_key") or ""),
                    str(entity.get("display_name") or ""),
                    str(entity.get("source_layer") or ""),
                    _json(entity.get("attributes")),
                    _json(typed_attributes_for(entity)),
                    _json(entity.get("completeness")),
                    _json(entity.get("diagnostics") or []),
                    _json(entity.get("evidence") or []),
                    _json(entity.get("snapshot") or {}),
                )
                for entity in entities
                if entity.get("id")
            ],
        )
        connection.executemany(
            """
            INSERT OR REPLACE INTO relations(
                id, type, from_id, to_id, source_layer, derived, confidence, attributes_json, typed_attributes_json, evidence_json
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                (
                    str(relation.get("id") or ""),
                    str(relation.get("type") or ""),
                    str(relation.get("from_id") or ""),
                    str(relation.get("to_id") or ""),
                    str(relation.get("source_layer") or ""),
                    1 if relation.get("derived") else 0,
                    float(relation.get("confidence") or 0.0),
                    _json(relation.get("attributes")),
                    _json(typed_attributes_for(relation)),
                    _json(relation.get("evidence") or []),
                )
                for relation in relations
                if relation.get("id")
            ],
        )
        connection.commit()
    finally:
        connection.close()

    os.replace(temp_path, destination)
    pointer_path = _publish_pointer(store, state, destination)
    _cleanup_old_generations(store, state, destination)
    return {
        "schema_version": SCHEMA_VERSION,
        "cache_path": str(destination),
        "pointer_path": str(pointer_path),
        "data_mode": state.data_mode,
        "generation": state.generation,
        "entity_count": len(entities),
        "relation_count": len(relations),
        "reused": False,
    }


def cache_status(store: SnapshotStore, state: SnapshotState | None = None) -> dict[str, Any]:
    state = state or store.load_state()
    pointer_path = _pointer_path(store, state)
    pointer = _load_pointer(store, state)
    path = cache_path(store, state)
    if not path.exists():
        return {
            "available": False,
            "synced": False,
            "path": str(path),
            "pointer_path": str(pointer_path),
            "reason": "cache_missing",
        }

    try:
        rows = _read_metadata(path)
    except (OSError, sqlite3.Error) as exc:
        return {
            "available": False,
            "synced": False,
            "path": str(path),
            "pointer_path": str(pointer_path),
            "reason": f"cache_unreadable: {exc}",
        }

    synced = (
        rows.get("schema_version") == SCHEMA_VERSION
        and rows.get("data_mode") == state.data_mode
        and rows.get("generation") == str(state.generation)
        and rows.get("manifest_path") == str(state.manifest_path)
    )
    return {
        "available": True,
        "synced": synced,
        "path": str(path),
        "pointer_path": str(pointer_path) if pointer else None,
        "versioned": bool(pointer),
        "schema_version": rows.get("schema_version"),
        "data_mode": rows.get("data_mode"),
        "generation": int(rows.get("generation") or 0),
        "entity_count": int(rows.get("entity_count") or 0),
        "relation_count": int(rows.get("relation_count") or 0),
    }


class SQLiteSnapshotCache:
    def __init__(self, path: Path):
        self.path = path
        self.connection: sqlite3.Connection | None = sqlite3.connect(
            f"file:{path.as_posix()}?mode=ro",
            uri=True,
            timeout=1.0,
        )
        self.connection.row_factory = sqlite3.Row

    def close(self) -> None:
        connection = self.connection
        self.connection = None
        if connection is not None:
            connection.close()

    def __enter__(self) -> "SQLiteSnapshotCache":
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self.close()

    @classmethod
    def open_if_synced(cls, store: SnapshotStore, state: SnapshotState) -> "SQLiteSnapshotCache | None":
        status = cache_status(store, state)
        if not status.get("available") or not status.get("synced"):
            return None
        cache: SQLiteSnapshotCache | None = None
        try:
            cache = cls(Path(str(status["path"])))
            assert cache.connection is not None
            cache.connection.execute("SELECT typed_attributes_json FROM entities LIMIT 1").fetchone()
            cache.connection.execute("SELECT typed_attributes_json FROM relations LIMIT 1").fetchone()
            return cache
        except sqlite3.Error:
            if cache is not None:
                cache.close()
            return None

    def metadata(self) -> dict[str, str]:
        return {str(key): str(value) for key, value in self.connection.execute("SELECT key, value FROM metadata").fetchall()}

    def counts(self) -> dict[str, int]:
        row = self.connection.execute(
            """
            SELECT
                (SELECT COUNT(*) FROM entities) AS entities,
                (SELECT COUNT(*) FROM relations) AS relations,
                (SELECT COUNT(*) FROM entities WHERE kind IN ('asset', 'asset_redirector')) AS asset_entities
            """
        ).fetchone()
        return {
            "entities": int(row["entities"] or 0),
            "relations": int(row["relations"] or 0),
            "diagnostics": 0,
            "asset_entities": int(row["asset_entities"] or 0),
        }

    def entity_kind_counts(self, limit: int) -> dict[str, int]:
        rows = self.connection.execute(
            "SELECT kind, COUNT(*) AS count FROM entities GROUP BY kind ORDER BY count DESC, kind ASC LIMIT ?",
            (max(1, int(limit)),),
        ).fetchall()
        return {str(row["kind"]): int(row["count"]) for row in rows}

    def relation_type_counts(self, limit: int) -> dict[str, int]:
        rows = self.connection.execute(
            "SELECT type, COUNT(*) AS count FROM relations GROUP BY type ORDER BY count DESC, type ASC LIMIT ?",
            (max(1, int(limit)),),
        ).fetchall()
        return {str(row["type"]): int(row["count"]) for row in rows}

    @staticmethod
    def _entity_from_row(row: sqlite3.Row | None, include_snapshot: bool = False) -> dict[str, Any] | None:
        if row is None:
            return None
        entity = {
            "id": row["id"],
            "kind": row["kind"],
            "canonical_key": row["canonical_key"],
            "display_name": row["display_name"],
            "source_layer": row["source_layer"],
            "attributes": _json_object(row["attributes_json"]),
            "typed_attributes": _json_object(row["typed_attributes_json"]),
            "completeness": _json_object(row["completeness_json"]),
            "diagnostics": _json_array(row["diagnostics_json"]),
            "evidence": _json_array(row["evidence_json"]),
        }
        snapshot = _json_object(row["snapshot_json"])
        if include_snapshot and snapshot:
            entity["snapshot"] = snapshot
        return entity

    @staticmethod
    def _relation_from_row(row: sqlite3.Row | None) -> dict[str, Any] | None:
        if row is None:
            return None
        return {
            "id": row["id"],
            "type": row["type"],
            "from_id": row["from_id"],
            "to_id": row["to_id"],
            "source_layer": row["source_layer"],
            "derived": bool(row["derived"]),
            "confidence": float(row["confidence"]),
            "attributes": _json_object(row["attributes_json"]),
            "typed_attributes": _json_object(row["typed_attributes_json"]),
            "evidence": _json_array(row["evidence_json"]),
        }

    def top_assets(self, limit: int, include_snapshot: bool = False) -> list[dict[str, Any]]:
        rows = self.connection.execute(
            """
            SELECT * FROM entities
            WHERE kind = 'asset'
            ORDER BY canonical_key ASC
            LIMIT ?
            """,
            (max(1, int(limit)),),
        ).fetchall()
        return [entity for row in rows if (entity := self._entity_from_row(row, include_snapshot=include_snapshot))]

    def search_entities(self, query: str = "", kind: str | None = None, limit: int = 20, include_snapshot: bool = False) -> list[dict[str, Any]]:
        limit = max(1, min(int(limit or 20), 1000))
        parameters: list[Any] = []
        predicates: list[str] = []
        if kind:
            predicates.append("kind = ?")
            parameters.append(kind)
        if query:
            like = f"%{query}%"
            predicates.append(
                """
                (
                    id LIKE ? OR kind LIKE ? OR canonical_key LIKE ? OR display_name LIKE ?
                    OR attributes_json LIKE ? OR typed_attributes_json LIKE ?
                )
                """
            )
            parameters.extend([like, like, like, like, like, like])
        where = "WHERE " + " AND ".join(predicates) if predicates else ""
        rows = self.connection.execute(
            f"""
            SELECT * FROM entities
            {where}
            ORDER BY CASE WHEN kind = 'asset' THEN 0 ELSE 1 END, canonical_key ASC, id ASC
            LIMIT ?
            """,
            (*parameters, limit),
        ).fetchall()
        return [entity for row in rows if (entity := self._entity_from_row(row, include_snapshot=include_snapshot))]

    def entity_by_id(self, entity_id: str, include_snapshot: bool = False) -> dict[str, Any] | None:
        row = self.connection.execute("SELECT * FROM entities WHERE id = ?", (entity_id,)).fetchone()
        return self._entity_from_row(row, include_snapshot=include_snapshot)

    def entities_by_ids(self, entity_ids: set[str] | list[str], limit: int = 100, include_snapshot: bool = False) -> list[dict[str, Any]]:
        ids = [item for item in entity_ids if isinstance(item, str) and item]
        if not ids:
            return []
        ids = ids[: max(1, int(limit))]
        placeholders = ",".join("?" for _ in ids)
        rows = self.connection.execute(
            f"""
            SELECT * FROM entities
            WHERE id IN ({placeholders})
            ORDER BY CASE WHEN kind = 'asset' THEN 0 ELSE 1 END, canonical_key ASC, id ASC
            """,
            ids,
        ).fetchall()
        return [entity for row in rows if (entity := self._entity_from_row(row, include_snapshot=include_snapshot))]

    def resolve_entity(self, identifier: str) -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
        needle = (identifier or "").strip()
        if not needle:
            return None, []
        exact_rows = self.connection.execute(
            """
            SELECT * FROM entities
            WHERE lower(id) = lower(?)
               OR lower(canonical_key) = lower(?)
               OR lower(display_name) = lower(?)
            ORDER BY CASE WHEN kind = 'asset' THEN 0 ELSE 1 END, canonical_key ASC, id ASC
            LIMIT 10
            """,
            (needle, needle, needle),
        ).fetchall()
        exact = [entity for row in exact_rows if (entity := self._entity_from_row(row, include_snapshot=True))]
        candidates = exact or self.search_entities(needle, limit=10, include_snapshot=True)
        if not candidates:
            return None, []
        return candidates[0], candidates

    def relations_for_entity(self, entity_id: str, limit: int = 80) -> list[dict[str, Any]]:
        rows = self.connection.execute(
            """
            SELECT * FROM relations
            WHERE from_id = ? OR to_id = ?
            ORDER BY id ASC
            LIMIT ?
            """,
            (entity_id, entity_id, max(0, int(limit))),
        ).fetchall()
        return [relation for row in rows if (relation := self._relation_from_row(row))]

    def outgoing_relations(self, entity_id: str, relation_types: set[str] | None = None, limit: int = 500) -> list[dict[str, Any]]:
        parameters: list[Any] = [entity_id]
        relation_filter = ""
        if relation_types:
            placeholders = ",".join("?" for _ in relation_types)
            relation_filter = f"AND type IN ({placeholders})"
            parameters.extend(sorted(relation_types))
        parameters.append(max(0, int(limit)))
        rows = self.connection.execute(
            f"""
            SELECT * FROM relations
            WHERE from_id = ?
            {relation_filter}
            ORDER BY id ASC
            LIMIT ?
            """,
            parameters,
        ).fetchall()
        return [relation for row in rows if (relation := self._relation_from_row(row))]

    def relations_between_ids(self, entity_ids: set[str], limit: int = 200) -> list[dict[str, Any]]:
        ids = [item for item in entity_ids if isinstance(item, str) and item]
        if not ids:
            return []
        placeholders = ",".join("?" for _ in ids)
        rows = self.connection.execute(
            f"""
            SELECT * FROM relations
            WHERE from_id IN ({placeholders}) AND to_id IN ({placeholders})
            ORDER BY id ASC
            LIMIT ?
            """,
            (*ids, *ids, max(0, int(limit))),
        ).fetchall()
        return [relation for row in rows if (relation := self._relation_from_row(row))]

    def domain_entities_for_asset(
        self,
        asset_id: str,
        domain_kinds: set[str],
        limit: int,
        relation_types: set[str] | None = None,
    ) -> list[dict[str, Any]]:
        seen = {asset_id}
        results: list[dict[str, Any]] = []
        frontier = [asset_id]
        while frontier and len(results) < limit:
            current = frontier.pop(0)
            for relation in self.relations_for_entity(current, limit=1000):
                if relation_types and str(relation.get("type") or "") not in relation_types:
                    continue
                next_id = relation.get("to_id") if relation.get("from_id") == current else relation.get("from_id")
                if not isinstance(next_id, str) or next_id in seen:
                    continue
                seen.add(next_id)
                entity = self.entity_by_id(next_id, include_snapshot=True)
                if not entity:
                    continue
                if entity.get("kind") in domain_kinds:
                    results.append(entity)
                frontier.append(next_id)
                if len(results) >= limit:
                    break
        return results

    def scoped_entities_for_asset(
        self,
        asset_key: str,
        domain_kinds: set[str],
        limit: int = 5000,
    ) -> list[dict[str, Any]]:
        if not asset_key or not domain_kinds:
            return []
        kind_placeholders = ",".join("?" for _ in domain_kinds)
        canonical_prefix = asset_key.rstrip(":") + ":%"
        attribute_match = f"%{asset_key}%"
        rows = self.connection.execute(
            f"""
            SELECT * FROM entities
            WHERE kind IN ({kind_placeholders})
              AND (
                    lower(canonical_key) = lower(?)
                 OR lower(canonical_key) LIKE lower(?)
                 OR attributes_json LIKE ?
              )
            ORDER BY canonical_key ASC, kind ASC, id ASC
            LIMIT ?
            """,
            (*sorted(domain_kinds), asset_key, canonical_prefix, attribute_match, max(1, int(limit))),
        ).fetchall()
        return [entity for row in rows if (entity := self._entity_from_row(row, include_snapshot=True))]
