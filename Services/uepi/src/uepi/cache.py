from __future__ import annotations

import json
from pathlib import Path
import sqlite3
import time
from typing import Any

from .store import SnapshotState, SnapshotStore


SCHEMA_VERSION = "uepi.sqlite-cache.v2"


def cache_path(store: SnapshotStore) -> Path:
    return store.root / "cache" / "uepi.sqlite3"


def _json(value: Any) -> str:
    return json.dumps(value if value is not None else {}, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


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
            evidence_json TEXT NOT NULL
        );

        CREATE INDEX idx_entities_kind ON entities(kind);
        CREATE INDEX idx_entities_display_name ON entities(display_name);
        CREATE INDEX idx_entities_canonical_key ON entities(canonical_key);
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
                attributes_json, completeness_json, diagnostics_json, evidence_json, snapshot_json
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                (
                    str(entity.get("id") or ""),
                    str(entity.get("kind") or ""),
                    str(entity.get("canonical_key") or ""),
                    str(entity.get("display_name") or ""),
                    str(entity.get("source_layer") or ""),
                    _json(entity.get("attributes")),
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
                id, type, from_id, to_id, source_layer, derived, confidence, attributes_json, evidence_json
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
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
