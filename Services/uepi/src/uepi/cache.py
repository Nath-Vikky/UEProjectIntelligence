from __future__ import annotations

import json
from pathlib import Path
import sqlite3
import time
from typing import Any

from .store import SnapshotState, SnapshotStore


SCHEMA_VERSION = "uepi.sqlite-cache.v2.1"


def cache_path(store: SnapshotStore) -> Path:
    return store.root / "cache" / "uepi.sqlite3"


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
    connection.execute("PRAGMA journal_mode=WAL")
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


def sync_cache(store: SnapshotStore) -> dict[str, Any]:
    state = store.load_state()
    scan = store.load_project_scan(state)
    entities = [entity for entity in scan.get("entities") or [] if isinstance(entity, dict)]
    relations = [relation for relation in scan.get("relations") or [] if isinstance(relation, dict)]

    destination = cache_path(store)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temp_path = destination.with_suffix(".sqlite3.tmp")
    if temp_path.exists():
        temp_path.unlink()

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

    temp_path.replace(destination)
    return {
        "schema_version": SCHEMA_VERSION,
        "cache_path": str(destination),
        "data_mode": state.data_mode,
        "generation": state.generation,
        "entity_count": len(entities),
        "relation_count": len(relations),
    }


def cache_status(store: SnapshotStore, state: SnapshotState | None = None) -> dict[str, Any]:
    state = state or store.load_state()
    path = cache_path(store)
    if not path.exists():
        return {
            "available": False,
            "synced": False,
            "path": str(path),
            "reason": "cache_missing",
        }

    try:
        connection = sqlite3.connect(path)
        rows = dict(connection.execute("SELECT key, value FROM metadata").fetchall())
    except sqlite3.Error as exc:
        return {
            "available": False,
            "synced": False,
            "path": str(path),
            "reason": f"cache_unreadable: {exc}",
        }
    finally:
        try:
            connection.close()
        except Exception:
            pass

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
        "schema_version": rows.get("schema_version"),
        "data_mode": rows.get("data_mode"),
        "generation": int(rows.get("generation") or 0),
        "entity_count": int(rows.get("entity_count") or 0),
        "relation_count": int(rows.get("relation_count") or 0),
    }


class SQLiteSnapshotCache:
    def __init__(self, path: Path):
        self.path = path
        self.connection = sqlite3.connect(path)
        self.connection.row_factory = sqlite3.Row

    @classmethod
    def open_if_synced(cls, store: SnapshotStore, state: SnapshotState) -> "SQLiteSnapshotCache | None":
        status = cache_status(store, state)
        if not status.get("available") or not status.get("synced"):
            return None
        cache: SQLiteSnapshotCache | None = None
        try:
            cache = cls(Path(str(status["path"])))
            cache.connection.execute("SELECT typed_attributes_json FROM entities LIMIT 1").fetchone()
            cache.connection.execute("SELECT typed_attributes_json FROM relations LIMIT 1").fetchone()
            return cache
        except sqlite3.Error:
            if cache is not None:
                try:
                    cache.connection.close()
                except Exception:
                    pass
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
