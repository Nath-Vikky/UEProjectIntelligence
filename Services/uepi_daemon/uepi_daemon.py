#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
from collections import Counter
import datetime as _dt
import hashlib
import html
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import re
import shlex
import secrets
import sqlite3
import subprocess
import sys
import time
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse


SCHEMA_VERSION = 1
PACKAGE_HASH_CHUNK_SIZE = 1024 * 1024
ARTIFACT_RANGE_MAX_BYTES = 1024 * 1024
DEFAULT_WORKER_TTL_SECONDS = 60
DEFAULT_JOB_TIMEOUT_SECONDS = 900
DEFAULT_LONG_POLL_SECONDS = 0
JOB_TERMINAL_STATES = {"succeeded", "failed", "cancelled"}
JOB_ACTIVE_STATES = {"assigned", "running", "partial"}
JOB_MUTABLE_STATES = {"queued", "assigned", "running", "partial", "retry_wait"}


def unique_paths(paths: list[Path]) -> list[Path]:
    seen: set[str] = set()
    result: list[Path] = []
    for path in paths:
        resolved = path.expanduser().resolve()
        key = str(resolved).lower()
        if key not in seen:
            seen.add(key)
            result.append(resolved)
    return result


def path_is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def daemon_sandbox_roots(db_path: Path | None = None) -> list[Path]:
    roots = [Path.cwd(), Path(__file__).resolve().parents[2]]
    if db_path is not None:
        roots.extend([db_path.parent, db_path.parent.parent])
    return unique_paths([root for root in roots if root.exists()])


def require_sandbox_path(path: Path, label: str, db_path: Path | None = None, must_exist: bool = True) -> Path:
    resolved = path.expanduser().resolve()
    if must_exist and not resolved.exists():
        raise SystemExit(f"{label} does not exist: {path}")
    roots = daemon_sandbox_roots(db_path)
    if not any(path_is_within(resolved, root) for root in roots):
        root_text = ", ".join(str(root) for root in roots)
        raise SystemExit(f"{label} is outside the UEPI path sandbox: {resolved}. Allowed roots: {root_text}")
    return resolved


def utc_now() -> str:
    return _dt.datetime.now(_dt.timezone.utc).isoformat().replace("+00:00", "Z")


def json_text(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def json_object(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def json_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def new_id(prefix: str) -> str:
    return f"{prefix}_{secrets.token_hex(12)}"


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def seconds_from_now(seconds: int) -> str:
    return (_dt.datetime.now(_dt.timezone.utc) + _dt.timedelta(seconds=max(1, seconds))).isoformat().replace("+00:00", "Z")


def stable_trace_id(seed: str | None = None) -> str:
    if seed:
        digest = hashlib.sha256(seed.encode("utf-8")).hexdigest()[:16]
        return f"trace_{digest}"
    return new_id("trace")


def connect(db_path: Path) -> sqlite3.Connection:
    db_path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(db_path))
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    conn.execute("PRAGMA busy_timeout = 5000")
    conn.execute("PRAGMA journal_mode = WAL")
    init_db(conn)
    return conn


def init_db(conn: sqlite3.Connection) -> None:
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS scans (
            scan_id TEXT PRIMARY KEY,
            schema_version TEXT NOT NULL,
            project_id TEXT NOT NULL,
            project_name TEXT NOT NULL,
            project_file TEXT NOT NULL,
            engine_version TEXT NOT NULL,
            started_at_utc TEXT NOT NULL,
            finished_at_utc TEXT NOT NULL,
            ingested_at_utc TEXT NOT NULL,
            scan_path TEXT NOT NULL,
            entity_count INTEGER NOT NULL,
            relation_count INTEGER NOT NULL,
            diagnostic_count INTEGER NOT NULL,
            completeness_json TEXT NOT NULL,
            git_json TEXT NOT NULL DEFAULT '{}'
        );

        CREATE TABLE IF NOT EXISTS entities (
            scan_id TEXT NOT NULL,
            id TEXT NOT NULL,
            kind TEXT NOT NULL,
            canonical_key TEXT NOT NULL,
            display_name TEXT NOT NULL,
            source_layer TEXT NOT NULL,
            attributes_json TEXT NOT NULL,
            snapshot_json TEXT NOT NULL DEFAULT '{}',
            completeness_json TEXT NOT NULL,
            diagnostics_json TEXT NOT NULL,
            evidence_json TEXT NOT NULL,
            PRIMARY KEY (scan_id, id),
            FOREIGN KEY (scan_id) REFERENCES scans(scan_id) ON DELETE CASCADE
        );

        CREATE TABLE IF NOT EXISTS relations (
            scan_id TEXT NOT NULL,
            id TEXT NOT NULL,
            type TEXT NOT NULL,
            from_id TEXT NOT NULL,
            to_id TEXT NOT NULL,
            source_layer TEXT NOT NULL,
            derived INTEGER NOT NULL,
            confidence REAL NOT NULL,
            attributes_json TEXT NOT NULL DEFAULT '{}',
            evidence_json TEXT NOT NULL,
            PRIMARY KEY (scan_id, id),
            FOREIGN KEY (scan_id) REFERENCES scans(scan_id) ON DELETE CASCADE
        );

        CREATE TABLE IF NOT EXISTS diagnostics (
            scan_id TEXT NOT NULL,
            ordinal INTEGER NOT NULL,
            code TEXT NOT NULL,
            severity TEXT NOT NULL,
            message TEXT NOT NULL,
            context_json TEXT NOT NULL,
            PRIMARY KEY (scan_id, ordinal),
            FOREIGN KEY (scan_id) REFERENCES scans(scan_id) ON DELETE CASCADE
        );

        CREATE TABLE IF NOT EXISTS asset_revisions (
            revision_id TEXT PRIMARY KEY,
            asset_key TEXT NOT NULL,
            asset_entity_id TEXT NOT NULL,
            scan_id TEXT NOT NULL,
            project_id TEXT NOT NULL,
            project_name TEXT NOT NULL,
            asset_class_path TEXT NOT NULL,
            object_path TEXT NOT NULL,
            package_name TEXT NOT NULL,
            package_path TEXT NOT NULL,
            canonical_hash TEXT NOT NULL,
            file_fingerprint_json TEXT NOT NULL,
            snapshot_artifact_id TEXT NOT NULL,
            valid_from_scan TEXT NOT NULL,
            valid_to_scan TEXT,
            created_at_utc TEXT NOT NULL,
            is_current INTEGER NOT NULL,
            FOREIGN KEY (scan_id) REFERENCES scans(scan_id) ON DELETE CASCADE
        );

        CREATE TABLE IF NOT EXISTS worker_sessions (
            session_id TEXT PRIMARY KEY,
            worker_id TEXT NOT NULL,
            worker_type TEXT NOT NULL,
            status TEXT NOT NULL,
            token_hash TEXT NOT NULL,
            capabilities_json TEXT NOT NULL,
            registered_at_utc TEXT NOT NULL,
            last_seen_utc TEXT NOT NULL,
            expires_at_utc TEXT NOT NULL,
            trace_id TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS jobs (
            job_id TEXT PRIMARY KEY,
            job_type TEXT NOT NULL,
            state TEXT NOT NULL,
            priority INTEGER NOT NULL,
            request_json TEXT NOT NULL,
            result_json TEXT NOT NULL DEFAULT '{}',
            error_json TEXT NOT NULL DEFAULT '{}',
            artifacts_json TEXT NOT NULL DEFAULT '[]',
            assigned_worker_id TEXT NOT NULL DEFAULT '',
            assigned_session_id TEXT NOT NULL DEFAULT '',
            lease_until_utc TEXT NOT NULL DEFAULT '',
            timeout_seconds INTEGER NOT NULL,
            retry_count INTEGER NOT NULL DEFAULT 0,
            max_retries INTEGER NOT NULL DEFAULT 1,
            trace_id TEXT NOT NULL,
            created_at_utc TEXT NOT NULL,
            updated_at_utc TEXT NOT NULL,
            started_at_utc TEXT NOT NULL DEFAULT '',
            finished_at_utc TEXT NOT NULL DEFAULT '',
            cancel_reason TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS job_events (
            job_id TEXT NOT NULL,
            sequence INTEGER NOT NULL,
            timestamp_utc TEXT NOT NULL,
            event_type TEXT NOT NULL,
            payload_json TEXT NOT NULL,
            PRIMARY KEY (job_id, sequence),
            FOREIGN KEY (job_id) REFERENCES jobs(job_id) ON DELETE CASCADE
        );

        CREATE TABLE IF NOT EXISTS job_chunks (
            job_id TEXT NOT NULL,
            artifact_id TEXT NOT NULL,
            ordinal INTEGER NOT NULL,
            sha256 TEXT NOT NULL,
            byte_count INTEGER NOT NULL,
            payload_path TEXT NOT NULL,
            received_at_utc TEXT NOT NULL,
            PRIMARY KEY (job_id, artifact_id, ordinal),
            FOREIGN KEY (job_id) REFERENCES jobs(job_id) ON DELETE CASCADE
        );

        CREATE TABLE IF NOT EXISTS source_symbols (
            symbol_id TEXT PRIMARY KEY,
            project_root TEXT NOT NULL,
            kind TEXT NOT NULL,
            name TEXT NOT NULL,
            qualified_name TEXT NOT NULL,
            file_path TEXT NOT NULL,
            line INTEGER NOT NULL,
            signature TEXT NOT NULL,
            metadata_json TEXT NOT NULL,
            indexed_at_utc TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS source_references (
            reference_id TEXT PRIMARY KEY,
            project_root TEXT NOT NULL,
            kind TEXT NOT NULL,
            value TEXT NOT NULL,
            file_path TEXT NOT NULL,
            line INTEGER NOT NULL,
            context TEXT NOT NULL,
            indexed_at_utc TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS config_values (
            config_value_id TEXT PRIMARY KEY,
            project_root TEXT NOT NULL,
            file_path TEXT NOT NULL,
            line INTEGER NOT NULL,
            section TEXT NOT NULL,
            key TEXT NOT NULL,
            normalized_key TEXT NOT NULL,
            operator TEXT NOT NULL,
            value TEXT NOT NULL,
            raw_line TEXT NOT NULL,
            indexed_at_utc TEXT NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_entities_scan_kind ON entities(scan_id, kind);
        CREATE INDEX IF NOT EXISTS idx_entities_scan_canonical ON entities(scan_id, canonical_key);
        CREATE INDEX IF NOT EXISTS idx_relations_scan_type ON relations(scan_id, type);
        CREATE INDEX IF NOT EXISTS idx_relations_from ON relations(scan_id, from_id);
        CREATE INDEX IF NOT EXISTS idx_relations_to ON relations(scan_id, to_id);
        CREATE INDEX IF NOT EXISTS idx_asset_revisions_asset_key ON asset_revisions(asset_key, created_at_utc DESC);
        CREATE INDEX IF NOT EXISTS idx_asset_revisions_scan ON asset_revisions(scan_id);
        CREATE INDEX IF NOT EXISTS idx_asset_revisions_current ON asset_revisions(asset_key, is_current);
        CREATE INDEX IF NOT EXISTS idx_worker_sessions_worker ON worker_sessions(worker_id, last_seen_utc DESC);
        CREATE INDEX IF NOT EXISTS idx_worker_sessions_status ON worker_sessions(status, expires_at_utc);
        CREATE INDEX IF NOT EXISTS idx_jobs_state_priority ON jobs(state, priority DESC, created_at_utc);
        CREATE INDEX IF NOT EXISTS idx_jobs_assigned_session ON jobs(assigned_session_id, state);
        CREATE INDEX IF NOT EXISTS idx_job_chunks_job_artifact ON job_chunks(job_id, artifact_id, ordinal);
        CREATE INDEX IF NOT EXISTS idx_source_symbols_root_kind ON source_symbols(project_root, kind);
        CREATE INDEX IF NOT EXISTS idx_source_symbols_name ON source_symbols(project_root, name);
        CREATE INDEX IF NOT EXISTS idx_source_references_root_kind ON source_references(project_root, kind);
        CREATE INDEX IF NOT EXISTS idx_source_references_value ON source_references(project_root, value);
        CREATE INDEX IF NOT EXISTS idx_config_values_root_section_key ON config_values(project_root, section, normalized_key);
        CREATE INDEX IF NOT EXISTS idx_config_values_root_file ON config_values(project_root, file_path, line);
        """
    )
    ensure_column(conn, "entities", "snapshot_json", "TEXT NOT NULL DEFAULT '{}'")
    ensure_column(conn, "relations", "attributes_json", "TEXT NOT NULL DEFAULT '{}'")
    ensure_column(conn, "scans", "git_json", "TEXT NOT NULL DEFAULT '{}'")
    conn.execute(
        "INSERT OR REPLACE INTO meta(key, value) VALUES('schema_version', ?)",
        (str(SCHEMA_VERSION),),
    )

    try:
        conn.execute(
            """
            CREATE VIRTUAL TABLE IF NOT EXISTS entity_fts
            USING fts5(scan_id UNINDEXED, id UNINDEXED, kind, display_name, canonical_key, attributes)
            """
        )
        conn.execute("INSERT OR REPLACE INTO meta(key, value) VALUES('fts5_available', '1')")
    except sqlite3.OperationalError:
        conn.execute("INSERT OR REPLACE INTO meta(key, value) VALUES('fts5_available', '0')")


def ensure_column(conn: sqlite3.Connection, table: str, column: str, declaration: str) -> None:
    rows = conn.execute(f"PRAGMA table_info({table})").fetchall()
    if any(row["name"] == column for row in rows):
        return
    conn.execute(f"ALTER TABLE {table} ADD COLUMN {column} {declaration}")


def fts_available(conn: sqlite3.Connection) -> bool:
    row = conn.execute("SELECT value FROM meta WHERE key = 'fts5_available'").fetchone()
    return bool(row and row["value"] == "1")


def latest_scan_id(conn: sqlite3.Connection) -> str:
    row = conn.execute(
        "SELECT scan_id FROM scans ORDER BY ingested_at_utc DESC LIMIT 1"
    ).fetchone()
    if not row:
        raise SystemExit("No UEPI scans have been ingested into this database.")
    return row["scan_id"]


def resolve_scan_id(conn: sqlite3.Connection, scan_id: str, label: str) -> str:
    rows = conn.execute(
        """
        SELECT scan_id FROM scans
        WHERE scan_id = ? OR scan_id LIKE ?
        ORDER BY ingested_at_utc DESC
        """,
        (scan_id, f"{scan_id}%"),
    ).fetchall()
    if not rows:
        raise SystemExit(f"{label} scan not found: {scan_id}")
    if len(rows) > 1 and all(row["scan_id"] != scan_id for row in rows):
        raise SystemExit(f"{label} scan id prefix is ambiguous: {scan_id}")
    return rows[0]["scan_id"]


def latest_two_scan_ids(conn: sqlite3.Connection) -> tuple[str, str]:
    rows = conn.execute(
        """
        SELECT scan_id FROM scans
        ORDER BY ingested_at_utc DESC
        LIMIT 2
        """
    ).fetchall()
    if len(rows) < 2:
        raise SystemExit("At least two ingested scans are required for diff.")
    return rows[1]["scan_id"], rows[0]["scan_id"]


def load_scan(scan_path: Path) -> tuple[str, dict[str, Any], bytes]:
    data = scan_path.read_bytes()
    scan = json.loads(data.decode("utf-8-sig"))
    scan_id = hashlib.sha256(data).hexdigest()
    return scan_id, scan, data


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(PACKAGE_HASH_CHUNK_SIZE), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def package_file_candidates(project_file: str, package_name: str) -> list[Path]:
    if not project_file or not package_name.startswith("/"):
        return []

    project_root = Path(project_file).parent
    normalized = package_name.strip("/")
    parts = normalized.split("/")
    if not parts:
        return []

    mount = parts[0]
    relative_parts = parts[1:]
    content_roots: list[Path] = []

    if mount == "Game":
        content_roots.append(project_root / "Content")
    else:
        plugins_root = project_root / "Plugins"
        if plugins_root.exists():
            for plugin_dir in sorted(plugins_root.iterdir()):
                if plugin_dir.is_dir() and plugin_dir.name == mount:
                    content_roots.append(plugin_dir / "Content")

    candidates: list[Path] = []
    for content_root in content_roots:
        package_base = content_root.joinpath(*relative_parts)
        candidates.append(package_base.with_suffix(".uasset"))
        candidates.append(package_base.with_suffix(".umap"))
    return candidates


def package_file_fingerprint(project_file: str, package_name: str) -> dict[str, Any]:
    candidates = package_file_candidates(project_file, package_name)
    result: dict[str, Any] = {
        "resolved": False,
        "reason": "unsupported_mount" if package_name.startswith("/") else "missing_package_name",
        "path": "",
        "exists": False,
        "size": None,
        "mtime_ns": None,
        "sha256": "",
    }
    if not candidates:
        return result

    result["reason"] = "package_file_missing"
    result["path"] = str(candidates[0])
    for candidate in candidates:
        if candidate.exists():
            stat = candidate.stat()
            return {
                "resolved": True,
                "reason": "current",
                "path": str(candidate),
                "exists": True,
                "size": stat.st_size,
                "mtime_ns": stat.st_mtime_ns,
                "sha256": sha256_file(candidate),
            }
    return result


def git_metadata(project_file: str) -> dict[str, Any]:
    project_root = Path(project_file).parent if project_file else Path.cwd()

    def run_git(args: list[str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", "-C", str(project_root), *args],
            capture_output=True,
            text=True,
            timeout=5,
        )

    try:
        inside = run_git(["rev-parse", "--is-inside-work-tree"])
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {
            "available": False,
            "reason": type(exc).__name__,
            "worktree": str(project_root),
        }

    if inside.returncode != 0 or inside.stdout.strip().lower() != "true":
        return {
            "available": False,
            "reason": "not_a_git_worktree",
            "worktree": str(project_root),
            "stderr": inside.stderr.strip(),
        }

    def git_text(args: list[str]) -> str:
        result = run_git(args)
        return result.stdout.strip() if result.returncode == 0 else ""

    status = git_text(["status", "--porcelain"])
    return {
        "available": True,
        "worktree": str(project_root),
        "commit": git_text(["rev-parse", "HEAD"]),
        "short_commit": git_text(["rev-parse", "--short", "HEAD"]),
        "branch": git_text(["branch", "--show-current"]) or git_text(["rev-parse", "--abbrev-ref", "HEAD"]),
        "commit_time": git_text(["show", "-s", "--format=%cI", "HEAD"]),
        "dirty": bool(status),
        "status_porcelain": status.splitlines(),
    }


def add_job_event(conn: sqlite3.Connection, job_id: str, event_type: str, payload: dict[str, Any] | None = None) -> None:
    sequence = conn.execute(
        "SELECT COALESCE(MAX(sequence), 0) + 1 AS next_sequence FROM job_events WHERE job_id = ?",
        (job_id,),
    ).fetchone()["next_sequence"]
    conn.execute(
        """
        INSERT INTO job_events(job_id, sequence, timestamp_utc, event_type, payload_json)
        VALUES(?, ?, ?, ?, ?)
        """,
        (job_id, sequence, utc_now(), event_type, json_text(payload or {})),
    )


def worker_session_row(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "session_id": row["session_id"],
        "worker_id": row["worker_id"],
        "worker_type": row["worker_type"],
        "status": row["status"],
        "capabilities": json.loads(row["capabilities_json"]),
        "registered_at_utc": row["registered_at_utc"],
        "last_seen_utc": row["last_seen_utc"],
        "expires_at_utc": row["expires_at_utc"],
        "trace_id": row["trace_id"],
    }


def job_row(conn: sqlite3.Connection, row: sqlite3.Row, include_events: bool = False) -> dict[str, Any]:
    chunk_rows = conn.execute(
        """
        SELECT artifact_id, COUNT(*) AS chunk_count, SUM(byte_count) AS byte_count
        FROM job_chunks
        WHERE job_id = ?
        GROUP BY artifact_id
        ORDER BY artifact_id
        """,
        (row["job_id"],),
    ).fetchall()
    result = {
        "job_id": row["job_id"],
        "job_type": row["job_type"],
        "state": row["state"],
        "priority": row["priority"],
        "request": json.loads(row["request_json"]),
        "result": json.loads(row["result_json"]),
        "error": json.loads(row["error_json"]),
        "artifacts": json.loads(row["artifacts_json"]),
        "uploaded_artifacts": [
            {
                "artifact_id": chunk["artifact_id"],
                "chunk_count": chunk["chunk_count"],
                "byte_count": chunk["byte_count"] or 0,
            }
            for chunk in chunk_rows
        ],
        "assigned_worker_id": row["assigned_worker_id"],
        "assigned_session_id": row["assigned_session_id"],
        "lease_until_utc": row["lease_until_utc"],
        "timeout_seconds": row["timeout_seconds"],
        "retry_count": row["retry_count"],
        "max_retries": row["max_retries"],
        "trace_id": row["trace_id"],
        "created_at_utc": row["created_at_utc"],
        "updated_at_utc": row["updated_at_utc"],
        "started_at_utc": row["started_at_utc"],
        "finished_at_utc": row["finished_at_utc"],
        "cancel_reason": row["cancel_reason"],
    }
    if include_events:
        events = conn.execute(
            """
            SELECT sequence, timestamp_utc, event_type, payload_json
            FROM job_events
            WHERE job_id = ?
            ORDER BY sequence
            """,
            (row["job_id"],),
        ).fetchall()
        result["events"] = [
            {
                "sequence": event["sequence"],
                "timestamp_utc": event["timestamp_utc"],
                "event_type": event["event_type"],
                "payload": json.loads(event["payload_json"]),
            }
            for event in events
        ]
    return result


def get_worker_session(conn: sqlite3.Connection, session_id: str, session_token: str) -> sqlite3.Row:
    row = conn.execute("SELECT * FROM worker_sessions WHERE session_id = ?", (session_id,)).fetchone()
    if not row:
        raise SystemExit(f"Unknown worker session: {session_id}")
    if row["token_hash"] != token_hash(session_token):
        raise SystemExit("Invalid worker session token.")
    if row["expires_at_utc"] < utc_now():
        raise SystemExit(f"Worker session expired: {session_id}")
    return row


def agent_protocol_document() -> dict[str, Any]:
    return {
        "schema_version": "uepi.agent_protocol.v1",
        "session_token": {
            "header": "X-UEPI-Session-Token",
            "scope": "worker heartbeat, poll, update, and chunk upload",
        },
        "job_states": ["queued", "assigned", "running", "partial", "retry_wait", "succeeded", "failed", "cancelled"],
        "worker_endpoints": [
            "POST /v1/workers/register",
            "POST /v1/workers/heartbeat",
            "GET /v1/workers",
            "POST /v1/jobs",
            "GET /v1/jobs",
            "GET /v1/jobs/{job_id}",
            "POST /v1/jobs/poll",
            "POST /v1/jobs/update",
            "POST /v1/jobs/cancel",
            "POST /v1/jobs/chunk",
            "POST /v1/jobs/recover",
        ],
        "trace_id": "Every job stores a trace_id and emits ordered job_events.",
        "read_only_contract": "Jobs may request scans and artifact uploads; the protocol does not expose package save, delete, rename, or compile actions.",
    }


def register_worker(
    db_path: Path,
    worker_id: str,
    worker_type: str = "editor",
    capabilities: dict[str, Any] | None = None,
    ttl_seconds: int = DEFAULT_WORKER_TTL_SECONDS,
) -> dict[str, Any]:
    if not worker_id:
        worker_id = new_id("worker")
    if not worker_type:
        worker_type = "editor"
    session_id = new_id("session")
    session_token = secrets.token_urlsafe(32)
    now = utc_now()
    expires_at = seconds_from_now(ttl_seconds)
    conn = connect(db_path)
    try:
        with conn:
            conn.execute(
                """
                INSERT INTO worker_sessions(
                    session_id, worker_id, worker_type, status, token_hash, capabilities_json,
                    registered_at_utc, last_seen_utc, expires_at_utc, trace_id
                ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    session_id,
                    worker_id,
                    worker_type,
                    "online",
                    token_hash(session_token),
                    json_text(capabilities or {}),
                    now,
                    now,
                    expires_at,
                    stable_trace_id(f"{worker_id}:{now}"),
                ),
            )
        return {
            "session_id": session_id,
            "session_token": session_token,
            "worker_id": worker_id,
            "worker_type": worker_type,
            "status": "online",
            "expires_at_utc": expires_at,
            "protocol": agent_protocol_document(),
        }
    finally:
        conn.close()


def worker_heartbeat(
    db_path: Path,
    session_id: str,
    session_token: str,
    status: str = "online",
    capabilities: dict[str, Any] | None = None,
    ttl_seconds: int = DEFAULT_WORKER_TTL_SECONDS,
) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        with conn:
            row = get_worker_session(conn, session_id, session_token)
            capabilities_json = json_text(capabilities) if capabilities is not None else row["capabilities_json"]
            expires_at = seconds_from_now(ttl_seconds)
            conn.execute(
                """
                UPDATE worker_sessions
                SET status = ?, capabilities_json = ?, last_seen_utc = ?, expires_at_utc = ?
                WHERE session_id = ?
                """,
                (status or "online", capabilities_json, utc_now(), expires_at, session_id),
            )
        return {"ok": True, "session_id": session_id, "status": status or "online", "expires_at_utc": expires_at}
    finally:
        conn.close()


def list_worker_sessions(db_path: Path, status: str | None = None, limit: int = 50) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        clauses = []
        params: list[Any] = []
        if status:
            clauses.append("status = ?")
            params.append(status)
        where = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        rows = conn.execute(
            f"""
            SELECT *
            FROM worker_sessions
            {where}
            ORDER BY last_seen_utc DESC
            LIMIT ?
            """,
            (*params, clamp_limit(limit),),
        ).fetchall()
        return {"workers": [worker_session_row(row) for row in rows]}
    finally:
        conn.close()


def submit_job(
    db_path: Path,
    job_type: str,
    request: dict[str, Any] | None = None,
    priority: int = 0,
    timeout_seconds: int = DEFAULT_JOB_TIMEOUT_SECONDS,
    max_retries: int = 1,
    trace_id: str | None = None,
) -> dict[str, Any]:
    if not job_type:
        raise SystemExit("job_type is required.")
    now = utc_now()
    job_id = new_id("job")
    resolved_trace_id = trace_id or stable_trace_id(f"{job_type}:{json_text(request or {})}:{now}")
    conn = connect(db_path)
    try:
        with conn:
            conn.execute(
                """
                INSERT INTO jobs(
                    job_id, job_type, state, priority, request_json, timeout_seconds,
                    max_retries, trace_id, created_at_utc, updated_at_utc
                ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    job_id,
                    job_type,
                    "queued",
                    int(priority),
                    json_text(request or {}),
                    max(1, int(timeout_seconds)),
                    max(0, int(max_retries)),
                    resolved_trace_id,
                    now,
                    now,
                ),
            )
            add_job_event(conn, job_id, "submitted", {"job_type": job_type, "trace_id": resolved_trace_id})
            row = conn.execute("SELECT * FROM jobs WHERE job_id = ?", (job_id,)).fetchone()
            return job_row(conn, row, include_events=True)
    finally:
        conn.close()


def list_jobs(db_path: Path, state: str | None = None, limit: int = 50, include_events: bool = False) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        clauses = []
        params: list[Any] = []
        if state:
            clauses.append("state = ?")
            params.append(state)
        where = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        rows = conn.execute(
            f"""
            SELECT *
            FROM jobs
            {where}
            ORDER BY created_at_utc DESC
            LIMIT ?
            """,
            (*params, clamp_limit(limit),),
        ).fetchall()
        return {"jobs": [job_row(conn, row, include_events=include_events) for row in rows]}
    finally:
        conn.close()


def get_job(db_path: Path, job_id: str, include_events: bool = True) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        row = conn.execute("SELECT * FROM jobs WHERE job_id = ?", (job_id,)).fetchone()
        if not row:
            raise SystemExit(f"Unknown job: {job_id}")
        return job_row(conn, row, include_events=include_events)
    finally:
        conn.close()


def recover_jobs(db_path: Path) -> dict[str, Any]:
    now = utc_now()
    conn = connect(db_path)
    try:
        stale_sessions = conn.execute(
            """
            SELECT session_id
            FROM worker_sessions
            WHERE status != 'stale' AND expires_at_utc < ?
            """,
            (now,),
        ).fetchall()
        expired_jobs = conn.execute(
            """
            SELECT *
            FROM jobs
            WHERE state IN ('assigned', 'running', 'partial') AND lease_until_utc != '' AND lease_until_utc < ?
            """,
            (now,),
        ).fetchall()
        with conn:
            for session in stale_sessions:
                conn.execute("UPDATE worker_sessions SET status = ? WHERE session_id = ?", ("stale", session["session_id"]))
            retried = 0
            failed = 0
            for row in expired_jobs:
                if row["retry_count"] < row["max_retries"]:
                    retried += 1
                    conn.execute(
                        """
                        UPDATE jobs
                        SET state = 'retry_wait',
                            retry_count = retry_count + 1,
                            assigned_worker_id = '',
                            assigned_session_id = '',
                            lease_until_utc = '',
                            updated_at_utc = ?
                        WHERE job_id = ?
                        """,
                        (now, row["job_id"]),
                    )
                    add_job_event(conn, row["job_id"], "lease_expired_retry", {"previous_session_id": row["assigned_session_id"]})
                else:
                    failed += 1
                    conn.execute(
                        """
                        UPDATE jobs
                        SET state = 'failed',
                            error_json = ?,
                            assigned_worker_id = '',
                            assigned_session_id = '',
                            lease_until_utc = '',
                            updated_at_utc = ?,
                            finished_at_utc = ?
                        WHERE job_id = ?
                        """,
                        (
                            json_text({"code": "UEPI_JOB_TIMEOUT", "message": "Job lease expired and retry budget was exhausted."}),
                            now,
                            now,
                            row["job_id"],
                        ),
                    )
                    add_job_event(conn, row["job_id"], "lease_expired_failed", {"previous_session_id": row["assigned_session_id"]})
        return {
            "ok": True,
            "stale_worker_sessions": len(stale_sessions),
            "retried_jobs": retried,
            "failed_jobs": failed,
        }
    finally:
        conn.close()


def poll_jobs(
    db_path: Path,
    session_id: str,
    session_token: str,
    limit: int = 1,
    wait_seconds: int = DEFAULT_LONG_POLL_SECONDS,
) -> dict[str, Any]:
    deadline = time.monotonic() + max(0, wait_seconds)
    while True:
        recover_jobs(db_path)
        conn = connect(db_path)
        try:
            with conn:
                session = get_worker_session(conn, session_id, session_token)
                rows = conn.execute(
                    """
                    SELECT *
                    FROM jobs
                    WHERE state IN ('queued', 'retry_wait')
                    ORDER BY priority DESC, created_at_utc ASC
                    LIMIT ?
                    """,
                    (max(1, min(limit, 20)),),
                ).fetchall()
                if rows:
                    assigned = []
                    for row in rows:
                        lease_until = seconds_from_now(row["timeout_seconds"])
                        started_at = row["started_at_utc"] or utc_now()
                        conn.execute(
                            """
                            UPDATE jobs
                            SET state = 'assigned',
                                assigned_worker_id = ?,
                                assigned_session_id = ?,
                                lease_until_utc = ?,
                                started_at_utc = ?,
                                updated_at_utc = ?
                            WHERE job_id = ?
                            """,
                            (session["worker_id"], session_id, lease_until, started_at, utc_now(), row["job_id"]),
                        )
                        add_job_event(conn, row["job_id"], "assigned", {"session_id": session_id, "worker_id": session["worker_id"]})
                        assigned_row = conn.execute("SELECT * FROM jobs WHERE job_id = ?", (row["job_id"],)).fetchone()
                        assigned.append(job_row(conn, assigned_row, include_events=True))
                    return {"jobs": assigned, "session_id": session_id}
        finally:
            conn.close()
        if time.monotonic() >= deadline:
            return {"jobs": [], "session_id": session_id}
        time.sleep(0.25)


def update_job(
    db_path: Path,
    session_id: str,
    session_token: str,
    job_id: str,
    state: str,
    result: dict[str, Any] | None = None,
    error: dict[str, Any] | None = None,
    artifacts: list[Any] | None = None,
) -> dict[str, Any]:
    if state not in JOB_MUTABLE_STATES and state not in JOB_TERMINAL_STATES:
        raise SystemExit(f"Unsupported job state: {state}")
    conn = connect(db_path)
    try:
        with conn:
            session = get_worker_session(conn, session_id, session_token)
            row = conn.execute("SELECT * FROM jobs WHERE job_id = ?", (job_id,)).fetchone()
            if not row:
                raise SystemExit(f"Unknown job: {job_id}")
            if row["assigned_session_id"] != session_id:
                raise SystemExit(f"Job {job_id} is not leased to session {session_id}.")
            if row["state"] in JOB_TERMINAL_STATES:
                raise SystemExit(f"Job {job_id} is already terminal: {row['state']}")
            now = utc_now()
            started_at = row["started_at_utc"] or now
            finished_at = now if state in JOB_TERMINAL_STATES else row["finished_at_utc"]
            result_json = json_text(result if result is not None else json.loads(row["result_json"]))
            error_json = json_text(error if error is not None else json.loads(row["error_json"]))
            artifacts_json = json_text(artifacts if artifacts is not None else json.loads(row["artifacts_json"]))
            conn.execute(
                """
                UPDATE jobs
                SET state = ?,
                    result_json = ?,
                    error_json = ?,
                    artifacts_json = ?,
                    started_at_utc = ?,
                    finished_at_utc = ?,
                    updated_at_utc = ?,
                    lease_until_utc = ?
                WHERE job_id = ?
                """,
                (
                    state,
                    result_json,
                    error_json,
                    artifacts_json,
                    started_at,
                    finished_at,
                    now,
                    "" if state in JOB_TERMINAL_STATES else seconds_from_now(row["timeout_seconds"]),
                    job_id,
                ),
            )
            add_job_event(conn, job_id, "updated", {"state": state, "session_id": session_id, "worker_id": session["worker_id"]})
            updated = conn.execute("SELECT * FROM jobs WHERE job_id = ?", (job_id,)).fetchone()
            return job_row(conn, updated, include_events=True)
    finally:
        conn.close()


def cancel_job(db_path: Path, job_id: str, reason: str = "cancelled by client") -> dict[str, Any]:
    conn = connect(db_path)
    try:
        with conn:
            row = conn.execute("SELECT * FROM jobs WHERE job_id = ?", (job_id,)).fetchone()
            if not row:
                raise SystemExit(f"Unknown job: {job_id}")
            if row["state"] in JOB_TERMINAL_STATES:
                return job_row(conn, row, include_events=True)
            now = utc_now()
            conn.execute(
                """
                UPDATE jobs
                SET state = 'cancelled',
                    cancel_reason = ?,
                    lease_until_utc = '',
                    updated_at_utc = ?,
                    finished_at_utc = ?
                WHERE job_id = ?
                """,
                (reason, now, now, job_id),
            )
            add_job_event(conn, job_id, "cancelled", {"reason": reason})
            updated = conn.execute("SELECT * FROM jobs WHERE job_id = ?", (job_id,)).fetchone()
            return job_row(conn, updated, include_events=True)
    finally:
        conn.close()


def safe_artifact_name(value: str) -> str:
    safe = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in value.strip())
    return safe or "artifact"


def upload_job_chunk(
    db_path: Path,
    session_id: str,
    session_token: str,
    job_id: str,
    artifact_id: str,
    ordinal: int,
    data_base64: str,
    expected_sha256: str | None = None,
) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        with conn:
            get_worker_session(conn, session_id, session_token)
            row = conn.execute("SELECT * FROM jobs WHERE job_id = ?", (job_id,)).fetchone()
            if not row:
                raise SystemExit(f"Unknown job: {job_id}")
            if row["assigned_session_id"] != session_id:
                raise SystemExit(f"Job {job_id} is not leased to session {session_id}.")
            data = base64.b64decode(data_base64.encode("ascii"), validate=True)
            actual_sha256 = hashlib.sha256(data).hexdigest()
            if expected_sha256 and actual_sha256 != expected_sha256:
                raise SystemExit("Chunk sha256 mismatch.")
            artifact_dir = db_path.parent / "job_artifacts" / safe_artifact_name(job_id) / safe_artifact_name(artifact_id)
            artifact_dir.mkdir(parents=True, exist_ok=True)
            payload_path = artifact_dir / f"{max(0, int(ordinal)):08d}.part"
            payload_path.write_bytes(data)
            conn.execute(
                """
                INSERT OR REPLACE INTO job_chunks(
                    job_id, artifact_id, ordinal, sha256, byte_count, payload_path, received_at_utc
                ) VALUES(?, ?, ?, ?, ?, ?, ?)
                """,
                (job_id, artifact_id, int(ordinal), actual_sha256, len(data), str(payload_path), utc_now()),
            )
            add_job_event(
                conn,
                job_id,
                "chunk_uploaded",
                {"artifact_id": artifact_id, "ordinal": int(ordinal), "sha256": actual_sha256, "byte_count": len(data)},
            )
            return {
                "ok": True,
                "job_id": job_id,
                "artifact_id": artifact_id,
                "ordinal": int(ordinal),
                "sha256": actual_sha256,
                "byte_count": len(data),
                "payload_path": str(payload_path),
            }
    finally:
        conn.close()


SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".cpp", ".cxx", ".cc", ".inl", ".cs", ".ini"}
UHT_MACRO_RE = re.compile(r"\b(UCLASS|USTRUCT|UENUM|UINTERFACE|UFUNCTION|UPROPERTY|UDELEGATE)\s*\((.*?)\)", re.DOTALL)
TYPE_DECL_RE = re.compile(r"\b(?:class|struct|enum\s+class|enum)\s+(?:\w+_API\s+)?([A-Za-z_][A-Za-z0-9_]*)")
FUNCTION_DECL_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_:<>,\s*&]+?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")
PROPERTY_DECL_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_:<>,\s*&]+?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|;)")
TYPE_CONTEXT_RE = re.compile(r"\b(?:class|struct)\s+(?:\w+_API\s+)?([A-Za-z_][A-Za-z0-9_]*)[^{;]*\{", re.MULTILINE)
UNREAL_PATH_RE = re.compile(r"['\"]((?:/(?:Game|Engine|Script|Plugin|[\w]+))/(?:[^'\"\s,;)]+))['\"]")
CONFIG_SECTION_RE = re.compile(r"^\s*\[([^\]]+)\]\s*$")
CONFIG_VALUE_RE = re.compile(r"^\s*([+\-.!]?)\s*([^=;\s][^=]*)=(.*)$")
ANIMATION_QUERY_KINDS = [
    "skeleton",
    "bone",
    "skeletal_mesh",
    "animation_sequence",
    "animation_track",
    "anim_notify",
    "blend_space",
    "blend_space_sample",
    "pose_asset",
    "pose_asset_pose",
    "pose_asset_curve",
    "ik_rig",
    "ik_retargeter",
    "physics_asset",
    "anim_blueprint",
    "anim_state_machine",
    "anim_state",
    "anim_transition",
    "anim_asset_player",
    "anim_cached_pose",
    "anim_slot",
    "control_rig_blueprint",
]
DATA_QUERY_KINDS = [
    "data_asset",
    "user_defined_struct",
    "user_defined_struct_field",
    "user_defined_enum",
    "user_defined_enum_entry",
    "string_table",
    "string_table_entry",
    "data_table",
    "data_table_column",
    "data_table_row",
    "curve_table",
    "curve_table_row",
    "curve",
    "curve_channel",
    "curve_key",
    "curve_linear_color_atlas",
    "curve_atlas_entry",
]


def source_project_root(conn: sqlite3.Connection, project: str | None) -> Path:
    if project:
        project_path = Path(project).expanduser()
        if project_path.suffix.lower() == ".uproject":
            return project_path.resolve().parent
        return project_path.resolve()
    row = conn.execute("SELECT project_file FROM scans ORDER BY ingested_at_utc DESC LIMIT 1").fetchone()
    if row and row["project_file"]:
        return Path(row["project_file"]).resolve().parent
    return Path.cwd().resolve()


def iter_source_files(project_root: Path) -> list[Path]:
    roots = [project_root / "Source", project_root / "Plugins", project_root / "Config"]
    files: list[Path] = []
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            if any(part in {"Intermediate", "Binaries", "Saved", ".git"} for part in path.parts):
                continue
            files.append(path)
    return sorted(files, key=lambda path: str(path).lower())


def line_number_for_offset(text: str, offset: int) -> int:
    return text.count("\n", 0, max(0, offset)) + 1


def parse_macro_metadata(raw: str) -> dict[str, Any]:
    raw = " ".join(raw.replace("\n", " ").split())
    values = [part.strip() for part in raw.split(",") if part.strip()]
    return {
        "raw": raw,
        "specifiers": values,
        "blueprint_callable": "BlueprintCallable" in raw,
        "blueprint_pure": "BlueprintPure" in raw,
        "blueprint_read_write": "BlueprintReadWrite" in raw,
        "blueprint_read_only": "BlueprintReadOnly" in raw,
        "authority_only": "BlueprintAuthorityOnly" in raw,
    }


def find_following_declaration(text: str, start: int, macro_name: str) -> tuple[str, str, str]:
    window = text[start : start + 1200]
    if macro_name in {"UCLASS", "USTRUCT", "UENUM", "UINTERFACE"}:
        match = TYPE_DECL_RE.search(window)
        if match:
            return match.group(1), match.group(0).strip(), "unreal_reflection_symbol"
    if macro_name in {"UFUNCTION", "UDELEGATE"}:
        match = FUNCTION_DECL_RE.search(window)
        if match:
            return match.group(2), match.group(0).strip(), "cpp_function"
    if macro_name == "UPROPERTY":
        match = PROPERTY_DECL_RE.search(window)
        if match:
            return match.group(2), match.group(0).strip(), "cpp_property"
    return "", "", "unreal_reflection_symbol"


def unreal_reflection_name(cpp_name: str) -> str:
    if len(cpp_name) > 1 and cpp_name[0] in {"A", "F", "I", "S", "U"} and cpp_name[1].isupper():
        return cpp_name[1:]
    return cpp_name


def find_enclosing_cpp_type(text: str, offset: int) -> str:
    matches = list(TYPE_CONTEXT_RE.finditer(text[: max(0, offset)]))
    if not matches:
        return ""
    return matches[-1].group(1)


def source_symbol_id(project_root: Path, kind: str, file_path: Path, line: int, name: str) -> str:
    payload = f"{project_root}|{kind}|{file_path}|{line}|{name}"
    return "source_symbol_" + hashlib.sha256(payload.encode("utf-8")).hexdigest()[:24]


def source_reference_id(project_root: Path, kind: str, file_path: Path, line: int, value: str) -> str:
    payload = f"{project_root}|{kind}|{file_path}|{line}|{value}"
    return "source_ref_" + hashlib.sha256(payload.encode("utf-8")).hexdigest()[:24]


def config_value_id(project_root: Path, file_path: Path, line: int, section: str, key: str, operator: str) -> str:
    payload = f"{project_root}|{file_path}|{line}|{section}|{key}|{operator}"
    return "config_value_" + hashlib.sha256(payload.encode("utf-8")).hexdigest()[:24]


def source_symbol_row(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "symbol_id": row["symbol_id"],
        "project_root": row["project_root"],
        "kind": row["kind"],
        "name": row["name"],
        "qualified_name": row["qualified_name"],
        "file_path": row["file_path"],
        "line": row["line"],
        "signature": row["signature"],
        "metadata": json.loads(row["metadata_json"]),
        "indexed_at_utc": row["indexed_at_utc"],
    }


def source_reference_row(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "reference_id": row["reference_id"],
        "project_root": row["project_root"],
        "kind": row["kind"],
        "value": row["value"],
        "file_path": row["file_path"],
        "line": row["line"],
        "context": row["context"],
        "indexed_at_utc": row["indexed_at_utc"],
    }


def strip_config_value(raw_value: str) -> str:
    value = raw_value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
        return value[1:-1]
    return value


def parse_config_values(project_root: Path, file_path: Path, text: str, indexed_at: str) -> list[tuple[Any, ...]]:
    rows: list[tuple[Any, ...]] = []
    section = ""
    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        stripped = raw_line.strip()
        if not stripped or stripped.startswith(";") or stripped.startswith("#"):
            continue
        section_match = CONFIG_SECTION_RE.match(stripped)
        if section_match:
            section = section_match.group(1).strip()
            continue
        value_match = CONFIG_VALUE_RE.match(raw_line)
        if not value_match:
            continue
        operator = value_match.group(1) or "="
        key = value_match.group(2).strip()
        value = strip_config_value(value_match.group(3))
        rows.append(
            (
                config_value_id(project_root, file_path, line_number, section, key, operator),
                str(project_root),
                str(file_path),
                line_number,
                section,
                key,
                key.lower(),
                operator,
                value,
                raw_line.strip(),
                indexed_at,
            )
        )
    return rows


def config_value_row(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "config_value_id": row["config_value_id"],
        "project_root": row["project_root"],
        "file_path": row["file_path"],
        "line": row["line"],
        "section": row["section"],
        "key": row["key"],
        "operator": row["operator"],
        "value": row["value"],
        "raw_line": row["raw_line"],
        "indexed_at_utc": row["indexed_at_utc"],
    }


def build_effective_config(rows: list[sqlite3.Row], include_history: bool = False, limit: int = 100) -> list[dict[str, Any]]:
    states: dict[tuple[str, str], dict[str, Any]] = {}
    for row in rows:
        identity = (row["section"], row["normalized_key"])
        state = states.setdefault(
            identity,
            {
                "section": row["section"],
                "key": row["key"],
                "values": [],
                "array_style": False,
                "last_file_path": "",
                "last_line": 0,
                "history": [],
            },
        )
        operator = row["operator"]
        value = row["value"]
        if operator == "!":
            state["values"] = []
            state["array_style"] = True
        elif operator == "-":
            state["values"] = [existing for existing in state["values"] if existing != value]
            state["array_style"] = True
        elif operator == "+":
            if value not in state["values"]:
                state["values"].append(value)
            state["array_style"] = True
        elif operator == ".":
            state["values"].append(value)
            state["array_style"] = True
        else:
            state["values"] = [value]
        state["key"] = row["key"]
        state["last_file_path"] = row["file_path"]
        state["last_line"] = row["line"]
        if include_history:
            state["history"].append(config_value_row(row))

    effective: list[dict[str, Any]] = []
    for state in states.values():
        values = list(state["values"])
        entry: dict[str, Any] = {
            "section": state["section"],
            "key": state["key"],
            "value": values if state["array_style"] else (values[-1] if values else ""),
            "values": values,
            "value_count": len(values),
            "is_array": bool(state["array_style"]),
            "source": {
                "file_path": state["last_file_path"],
                "line": state["last_line"],
            },
        }
        if include_history:
            entry["history"] = state["history"]
        effective.append(entry)

    effective.sort(key=lambda item: (item["section"].lower(), item["key"].lower()))
    return effective[:clamp_limit(limit)]


def index_source(db_path: Path, project: str | None = None, compile_database: str | None = None) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        project_root = source_project_root(conn, project)
        project_root = require_sandbox_path(project_root, "Project root", db_path, must_exist=True)
        files = iter_source_files(project_root)
        indexed_at = utc_now()
        symbol_rows: list[tuple[Any, ...]] = []
        reference_rows: list[tuple[Any, ...]] = []
        config_rows: list[tuple[Any, ...]] = []
        for file_path in files:
            try:
                text = file_path.read_text(encoding="utf-8-sig", errors="replace")
            except OSError:
                continue
            if file_path.suffix.lower() == ".ini":
                config_rows.extend(parse_config_values(project_root, file_path, text, indexed_at))
            for match in UHT_MACRO_RE.finditer(text):
                macro_name = match.group(1)
                symbol_name, signature, kind = find_following_declaration(text, match.end(), macro_name)
                if not symbol_name:
                    continue
                line = line_number_for_offset(text, match.start())
                metadata = parse_macro_metadata(match.group(2))
                metadata.update({"macro": macro_name, "relative_file": str(file_path.relative_to(project_root))})
                owner_cpp_name = ""
                if macro_name in {"UFUNCTION", "UPROPERTY", "UDELEGATE"}:
                    owner_cpp_name = find_enclosing_cpp_type(text, match.start())
                    if owner_cpp_name:
                        metadata.update(
                            {
                                "owner_cpp_name": owner_cpp_name,
                                "owner_unreal_name": unreal_reflection_name(owner_cpp_name),
                                "owner_qualified_name": f"{owner_cpp_name}::{symbol_name}",
                            }
                        )
                qualified_name = f"{owner_cpp_name}::{symbol_name}" if owner_cpp_name else symbol_name
                symbol_rows.append(
                    (
                        source_symbol_id(project_root, kind, file_path, line, symbol_name),
                        str(project_root),
                        kind,
                        symbol_name,
                        qualified_name,
                        str(file_path),
                        line,
                        signature,
                        json_text(metadata),
                        indexed_at,
                    )
                )
            for ref_match in UNREAL_PATH_RE.finditer(text):
                value = ref_match.group(1)
                line = line_number_for_offset(text, ref_match.start())
                line_text = text.splitlines()[line - 1].strip() if line - 1 < len(text.splitlines()) else value
                kind = "config_asset_reference" if file_path.suffix.lower() == ".ini" else "cpp_asset_reference"
                reference_rows.append(
                    (
                        source_reference_id(project_root, kind, file_path, line, value),
                        str(project_root),
                        kind,
                        value,
                        str(file_path),
                        line,
                        line_text[:500],
                        indexed_at,
                    )
                )
            if file_path.name.endswith(".Build.cs") or file_path.name.endswith(".Target.cs"):
                kind = "build_cs_file" if file_path.name.endswith(".Build.cs") else "target_cs_file"
                reference_rows.append(
                    (
                        source_reference_id(project_root, kind, file_path, 1, file_path.name),
                        str(project_root),
                        kind,
                        file_path.name,
                        str(file_path),
                        1,
                        str(file_path.relative_to(project_root)),
                        indexed_at,
                    )
                )
        compile_db_path = Path(compile_database).expanduser().resolve() if compile_database else project_root / "compile_commands.json"
        if compile_db_path.exists():
            compile_db_path = require_sandbox_path(compile_db_path, "Compile database", db_path, must_exist=True)
            reference_rows.append(
                (
                    source_reference_id(project_root, "compile_database", compile_db_path, 1, str(compile_db_path)),
                    str(project_root),
                    "compile_database",
                    str(compile_db_path),
                    str(compile_db_path),
                    1,
                    "compile_commands.json discovered for source indexing.",
                    indexed_at,
                )
            )
        with conn:
            conn.execute("DELETE FROM source_symbols WHERE project_root = ?", (str(project_root),))
            conn.execute("DELETE FROM source_references WHERE project_root = ?", (str(project_root),))
            conn.execute("DELETE FROM config_values WHERE project_root = ?", (str(project_root),))
            conn.executemany(
                """
                INSERT INTO source_symbols(
                    symbol_id, project_root, kind, name, qualified_name, file_path, line,
                    signature, metadata_json, indexed_at_utc
                ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                symbol_rows,
            )
            conn.executemany(
                """
                INSERT INTO source_references(
                    reference_id, project_root, kind, value, file_path, line, context, indexed_at_utc
                ) VALUES(?, ?, ?, ?, ?, ?, ?, ?)
                """,
                reference_rows,
            )
            conn.executemany(
                """
                INSERT INTO config_values(
                    config_value_id, project_root, file_path, line, section, key, normalized_key,
                    operator, value, raw_line, indexed_at_utc
                ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                config_rows,
            )
        return {
            "ok": True,
            "project_root": str(project_root),
            "file_count": len(files),
            "symbol_count": len(symbol_rows),
            "reference_count": len(reference_rows),
            "config_value_count": len(config_rows),
            "indexed_at_utc": indexed_at,
            "compile_database": str(compile_db_path) if compile_db_path.exists() else "",
        }
    finally:
        conn.close()


def source_symbols(db_path: Path, project: str | None = None, kind: str | None = None, query: str | None = None, limit: int = 100) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        project_root = str(source_project_root(conn, project))
        clauses = ["project_root = ?"]
        params: list[Any] = [project_root]
        if kind:
            clauses.append("kind = ?")
            params.append(kind)
        if query:
            clauses.append("(name LIKE ? OR qualified_name LIKE ? OR signature LIKE ? OR file_path LIKE ?)")
            like = f"%{query}%"
            params.extend([like, like, like, like])
        rows = conn.execute(
            f"""
            SELECT *
            FROM source_symbols
            WHERE {' AND '.join(clauses)}
            ORDER BY file_path, line
            LIMIT ?
            """,
            (*params, clamp_limit(limit),),
        ).fetchall()
        return {"project_root": project_root, "symbols": [source_symbol_row(row) for row in rows]}
    finally:
        conn.close()


def source_references(db_path: Path, project: str | None = None, kind: str | None = None, query: str | None = None, limit: int = 100) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        project_root = str(source_project_root(conn, project))
        clauses = ["project_root = ?"]
        params: list[Any] = [project_root]
        if kind:
            clauses.append("kind = ?")
            params.append(kind)
        if query:
            clauses.append("(value LIKE ? OR context LIKE ? OR file_path LIKE ?)")
            like = f"%{query}%"
            params.extend([like, like, like])
        rows = conn.execute(
            f"""
            SELECT *
            FROM source_references
            WHERE {' AND '.join(clauses)}
            ORDER BY file_path, line
            LIMIT ?
            """,
            (*params, clamp_limit(limit),),
        ).fetchall()
        return {"project_root": project_root, "references": [source_reference_row(row) for row in rows]}
    finally:
        conn.close()


def source_search(db_path: Path, query: str, project: str | None = None, limit: int = 50) -> dict[str, Any]:
    return {
        "symbols": source_symbols(db_path, project, None, query, limit)["symbols"],
        "references": source_references(db_path, project, None, query, limit)["references"],
    }


def normalize_source_owner_name(value: str) -> str:
    return unreal_reflection_name(value.strip()).lower()


def blueprint_cpp_link_id(scan_id: str, relation_id: str, symbol_id: str) -> str:
    digest = hashlib.sha256(f"{scan_id}|{relation_id}|{symbol_id}".encode("utf-8")).hexdigest()[:24]
    return f"bp_cpp_link_{digest}"


def blueprint_call_matches_query(link: dict[str, Any], query: str | None) -> bool:
    if not query:
        return True
    needle = query.lower()
    haystack = json_text(link).lower()
    return needle in haystack


def blueprint_cpp_links(
    db_path: Path,
    scan_id: str | None = None,
    project: str | None = None,
    query: str | None = None,
    limit: int = 100,
) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        resolved_scan_id = resolve_scan_id(conn, scan_id, "Target") if scan_id else latest_scan_id(conn)
        project_root = str(source_project_root(conn, project))
        symbol_rows = conn.execute(
            """
            SELECT *
            FROM source_symbols
            WHERE project_root = ? AND kind = 'cpp_function'
            ORDER BY file_path, line
            """,
            (project_root,),
        ).fetchall()
        symbols_by_name: dict[str, list[dict[str, Any]]] = {}
        for row in symbol_rows:
            symbol = source_symbol_row(row)
            symbols_by_name.setdefault(symbol["name"].lower(), []).append(symbol)

        call_rows = conn.execute(
            """
            SELECT
                r.*,
                node.display_name AS node_display_name,
                node.canonical_key AS node_canonical_key,
                fn.display_name AS function_display_name,
                fn.canonical_key AS function_canonical_key
            FROM relations r
            LEFT JOIN entities node ON node.scan_id = r.scan_id AND node.id = r.from_id
            LEFT JOIN entities fn ON fn.scan_id = r.scan_id AND fn.id = r.to_id
            WHERE r.scan_id = ? AND r.type = 'calls_function'
            ORDER BY r.id
            """,
            (resolved_scan_id,),
        ).fetchall()

        links: list[dict[str, Any]] = []
        unmatched: list[dict[str, Any]] = []
        for row in call_rows:
            attributes = json.loads(row["attributes_json"] or "{}")
            function_name = str(attributes.get("function_name") or row["function_display_name"] or "")
            owner_class_name = str(attributes.get("owner_class_name") or "")
            owner_class_path = str(attributes.get("owner_class") or "")
            candidates = symbols_by_name.get(function_name.lower(), [])
            if not candidates:
                unmatched.append({"relation_id": row["id"], "function_name": function_name, "owner_class_name": owner_class_name, "reason": "no_source_symbol_name_match"})
                continue

            normalized_owner = normalize_source_owner_name(owner_class_name)
            owner_matches = []
            for symbol in candidates:
                metadata = symbol.get("metadata", {})
                owner_names = {
                    normalize_source_owner_name(str(metadata.get("owner_cpp_name", ""))),
                    normalize_source_owner_name(str(metadata.get("owner_unreal_name", ""))),
                }
                if normalized_owner and normalized_owner in owner_names:
                    owner_matches.append(symbol)

            matched_symbol: dict[str, Any] | None = None
            match_reason = ""
            confidence = 0.0
            if len(owner_matches) == 1:
                matched_symbol = owner_matches[0]
                match_reason = "function_name_and_owner_class"
                confidence = 0.95
            elif len(candidates) == 1 and not owner_matches:
                matched_symbol = candidates[0]
                match_reason = "unique_function_name"
                confidence = 0.6
            elif owner_matches:
                unmatched.append(
                    {
                        "relation_id": row["id"],
                        "function_name": function_name,
                        "owner_class_name": owner_class_name,
                        "reason": "ambiguous_owner_match",
                        "candidate_count": len(owner_matches),
                    }
                )
                continue
            else:
                unmatched.append(
                    {
                        "relation_id": row["id"],
                        "function_name": function_name,
                        "owner_class_name": owner_class_name,
                        "reason": "ambiguous_function_name",
                        "candidate_count": len(candidates),
                    }
                )
                continue

            link_id = blueprint_cpp_link_id(resolved_scan_id, row["id"], matched_symbol["symbol_id"])
            link = {
                "link_id": link_id,
                "schema_version": "uepi.blueprint_cpp_link.v1",
                "scan_id": resolved_scan_id,
                "project_root": project_root,
                "match_reason": match_reason,
                "confidence": confidence,
                "blueprint_call": {
                    "relation_id": row["id"],
                    "node_id": row["from_id"],
                    "node_display_name": row["node_display_name"] or "",
                    "node_canonical_key": row["node_canonical_key"] or "",
                    "function_entity_id": row["to_id"],
                    "function_display_name": row["function_display_name"] or "",
                    "function_canonical_key": row["function_canonical_key"] or "",
                    "function_name": function_name,
                    "owner_class_name": owner_class_name,
                    "owner_class_path": owner_class_path,
                    "attributes": attributes,
                },
                "source_symbol": matched_symbol,
                "derived_relation": {
                    "id": link_id,
                    "type": "blueprint_calls_cpp_symbol",
                    "from_id": row["from_id"],
                    "to_id": matched_symbol["symbol_id"],
                    "source_layer": "Derived",
                    "confidence": confidence,
                    "attributes": {
                        "function_name": function_name,
                        "owner_class_name": owner_class_name,
                        "match_reason": match_reason,
                    },
                },
            }
            if blueprint_call_matches_query(link, query):
                links.append(link)

        limited_links = links[:clamp_limit(limit, 1000)]
        return {
            "schema_version": "uepi.blueprint_cpp_links.v1",
            "scan_id": resolved_scan_id,
            "project_root": project_root,
            "call_relation_count": len(call_rows),
            "source_symbol_count": len(symbol_rows),
            "link_count": len(links),
            "returned_count": len(limited_links),
            "links": limited_links,
            "unmatched_count": len(unmatched),
            "unmatched_preview": unmatched[:10],
        }
    finally:
        conn.close()


def config_values(
    db_path: Path,
    project: str | None = None,
    section: str | None = None,
    key: str | None = None,
    query: str | None = None,
    limit: int = 100,
    include_history: bool = False,
) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        project_root = str(source_project_root(conn, project))
        clauses = ["project_root = ?"]
        params: list[Any] = [project_root]
        if section is not None:
            clauses.append("section = ?")
            params.append(section)
        if key is not None:
            clauses.append("normalized_key = ?")
            params.append(key.lower())
        if query:
            clauses.append("(section LIKE ? OR key LIKE ? OR value LIKE ? OR raw_line LIKE ? OR file_path LIKE ?)")
            like = f"%{query}%"
            params.extend([like, like, like, like, like])
        rows = conn.execute(
            f"""
            SELECT *
            FROM config_values
            WHERE {' AND '.join(clauses)}
            ORDER BY file_path, line
            """,
            params,
        ).fetchall()
        return {
            "project_root": project_root,
            "effective": build_effective_config(rows, include_history, limit),
            "rows": [config_value_row(row) for row in rows[:clamp_limit(limit)]],
            "row_count": len(rows),
        }
    finally:
        conn.close()


def domain_manifest(
    db_path: Path,
    domain: str,
    kinds: list[str],
    scan_id: str | None = None,
    asset: str | None = None,
    limit: int = 100,
    include_snapshot: bool = False,
) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        resolved_scan_id = resolve_scan_id(conn, scan_id, "Target") if scan_id else latest_scan_id(conn)
        placeholders = ",".join("?" for _ in kinds)
        clauses = [f"scan_id = ? AND kind IN ({placeholders})"]
        params: list[Any] = [resolved_scan_id, *kinds]
        if asset:
            clauses.append("(canonical_key LIKE ? OR display_name LIKE ? OR attributes_json LIKE ?)")
            like = f"%{asset}%"
            params.extend([like, like, like])
        rows = conn.execute(
            f"""
            SELECT *
            FROM entities
            WHERE {' AND '.join(clauses)}
            ORDER BY kind, canonical_key
            LIMIT ?
            """,
            (*params, clamp_limit(limit, 1000),),
        ).fetchall()
        all_counts = conn.execute(
            f"""
            SELECT kind, COUNT(*) AS count
            FROM entities
            WHERE scan_id = ? AND kind IN ({placeholders})
            GROUP BY kind
            ORDER BY kind
            """,
            (resolved_scan_id, *kinds),
        ).fetchall()
        entities = [entity_row(row, include_snapshot=include_snapshot) for row in rows]
        omitted = Counter()
        covered = Counter()
        for row in rows:
            completeness = json.loads(row["completeness_json"])
            for value in completeness.get("omitted", []):
                omitted[str(value)] += 1
            for value in completeness.get("covered", []):
                covered[str(value)] += 1
        return {
            "domain": domain,
            "scan_id": resolved_scan_id,
            "asset_filter": asset or "",
            "kind_counts": {row["kind"]: row["count"] for row in all_counts},
            "returned_count": len(entities),
            "entities": entities,
            "covered": dict(sorted(covered.items())),
            "omitted": dict(sorted(omitted.items())),
            "artifact_policy": "Use token_budget/MCP artifacts or export-graph/artifact-range for large snapshots.",
        }
    finally:
        conn.close()


def animation_query(
    db_path: Path,
    scan_id: str | None = None,
    asset: str | None = None,
    limit: int = 100,
    include_snapshot: bool = False,
) -> dict[str, Any]:
    return domain_manifest(db_path, "animation", ANIMATION_QUERY_KINDS, scan_id, asset, limit, include_snapshot)


def data_query(
    db_path: Path,
    scan_id: str | None = None,
    asset: str | None = None,
    limit: int = 100,
    include_snapshot: bool = False,
) -> dict[str, Any]:
    return domain_manifest(db_path, "data", DATA_QUERY_KINDS, scan_id, asset, limit, include_snapshot)


def snapshot_collection(snapshot: dict[str, Any], collection: str) -> tuple[dict[str, Any], str, list[dict[str, Any]]]:
    candidates: list[tuple[str, dict[str, Any]]] = [("snapshot", snapshot)]
    for key in ("data_asset", "data_table", "curve_table", "string_table"):
        value = snapshot.get(key)
        if isinstance(value, dict):
            candidates.append((key, value))

    for container_name, container in candidates:
        items = container.get(collection)
        if isinstance(items, list):
            return container, container_name, [item for item in items if isinstance(item, dict)]
    raise SystemExit(f"Collection '{collection}' was not found in the entity snapshot.")


def write_data_collection_artifact(
    db_path: Path,
    scan_id: str,
    entity: str,
    collection: str,
    container_name: str,
    container_schema_version: str,
    items: list[dict[str, Any]],
) -> dict[str, Any]:
    payload = {
        "schema_version": "uepi.data_collection_artifact.v1",
        "scan_id": scan_id,
        "entity": entity,
        "collection": collection,
        "container": container_name,
        "container_schema_version": container_schema_version,
        "item_count": len(items),
        "items": items,
    }
    data = json.dumps(payload, ensure_ascii=False, sort_keys=True, indent=2).encode("utf-8")
    digest = hashlib.sha256(data).hexdigest()
    artifact_id = f"{safe_artifact_name(collection)}_{digest[:24]}"
    artifact_dir = db_path.parent / "data_artifacts"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    artifact_path = artifact_dir / f"{artifact_id}.json"
    if not artifact_path.exists():
        artifact_path.write_bytes(data)
    return {
        "schema_version": "uepi.data_collection_artifact_manifest.v1",
        "artifact_id": artifact_id,
        "artifact_uri": f"uepi://data-artifact/{artifact_id}",
        "path": str(artifact_path),
        "sha256": digest,
        "byte_count": len(data),
        "item_count": len(items),
        "encoding": "json",
        "collection": collection,
        "container": container_name,
    }


def data_snapshot_page(
    db_path: Path,
    entity: str,
    collection: str = "rows",
    scan_id: str | None = None,
    limit: int = 100,
    cursor: str | None = None,
    include_artifact: bool = False,
) -> dict[str, Any]:
    if collection not in {"rows", "columns", "entries", "bundles", "parent_tables"}:
        raise SystemExit("Collection must be one of: rows, columns, entries, bundles, parent_tables.")
    limit = clamp_limit(limit, 1000)
    cursor_value = decode_cursor(cursor)
    start_index = 0
    if cursor_value:
        if cursor_value.get("entity") != entity or cursor_value.get("collection") != collection:
            raise SystemExit("Cursor does not match the requested entity and collection.")
        start_index = int(cursor_value.get("index", -1)) + 1

    conn = connect(db_path)
    try:
        resolved_scan_id = resolve_scan_id(conn, scan_id, "Target") if scan_id else latest_scan_id(conn)
        rows = conn.execute(
            """
            SELECT id, kind, canonical_key, display_name, source_layer, attributes_json, snapshot_json
            FROM entities
            WHERE scan_id = ? AND (id = ? OR canonical_key = ?)
            ORDER BY CASE WHEN kind = 'asset' THEN 0 ELSE 1 END, kind ASC, id ASC
            """,
            (resolved_scan_id, entity, entity),
        ).fetchall()
        if not rows:
            raise SystemExit(f"Entity not found: {entity}")

        row = rows[0]
        container: dict[str, Any] | None = None
        container_name = ""
        items: list[dict[str, Any]] = []
        for candidate in rows:
            snapshot = json.loads(candidate["snapshot_json"] or "{}")
            try:
                container, container_name, items = snapshot_collection(snapshot, collection)
            except SystemExit:
                continue
            row = candidate
            break
        if container is None:
            raise SystemExit(f"Collection '{collection}' was not found in the entity snapshot.")

        page_items = items[start_index : start_index + limit]
        next_cursor = None
        if start_index + len(page_items) < len(items):
            next_cursor = encode_cursor(
                {
                    "entity": entity,
                    "collection": collection,
                    "index": start_index + len(page_items) - 1,
                }
            )
        result = paged_result(page_items, limit, next_cursor)
        result.update(
            {
                "scan_id": resolved_scan_id,
                "entity": entity_row(row),
                "collection": collection,
                "container": container_name,
                "container_schema_version": container.get("schema_version", ""),
                "total_count": len(items),
                "start_index": start_index,
                "end_index": start_index + len(page_items) - 1 if page_items else start_index - 1,
            }
        )
        if include_artifact:
            result["collection_artifact"] = write_data_collection_artifact(
                db_path,
                resolved_scan_id,
                entity,
                collection,
                container_name,
                container.get("schema_version", ""),
                items,
            )
        return result
    finally:
        conn.close()


def level_sequence_from_snapshot(snapshot: dict[str, Any]) -> dict[str, Any] | None:
    if not isinstance(snapshot, dict):
        return None
    nested = snapshot.get("level_sequence")
    if isinstance(nested, dict):
        return nested
    if snapshot.get("schema_version") == "uepi.level_sequence.v1":
        return snapshot
    return None


def level_sequence_identifiers(level_sequence: dict[str, Any]) -> set[str]:
    return {
        str(value)
        for value in (
            level_sequence.get("id"),
            level_sequence.get("sequence_path"),
            level_sequence.get("movie_scene_id"),
            level_sequence.get("movie_scene_path"),
        )
        if value not in (None, "")
    }


def find_level_sequence_snapshot(
    conn: sqlite3.Connection,
    scan_id: str,
    entity: str,
) -> tuple[sqlite3.Row, dict[str, Any]]:
    exact_rows = conn.execute(
        """
        SELECT id, kind, canonical_key, display_name, source_layer, attributes_json, snapshot_json
        FROM entities
        WHERE scan_id = ? AND (id = ? OR canonical_key = ?)
        ORDER BY CASE WHEN kind = 'asset' THEN 0 ELSE 1 END, kind ASC, id ASC
        """,
        (scan_id, entity, entity),
    ).fetchall()
    for row in exact_rows:
        snapshot = json.loads(row["snapshot_json"] or "{}")
        level_sequence = level_sequence_from_snapshot(snapshot)
        if level_sequence is not None:
            return row, level_sequence

    candidate_rows = conn.execute(
        """
        SELECT id, kind, canonical_key, display_name, source_layer, attributes_json, snapshot_json
        FROM entities
        WHERE scan_id = ? AND snapshot_json LIKE '%"level_sequence"%'
        ORDER BY CASE WHEN kind = 'asset' THEN 0 ELSE 1 END, kind ASC, id ASC
        """,
        (scan_id,),
    ).fetchall()
    for row in candidate_rows:
        snapshot = json.loads(row["snapshot_json"] or "{}")
        level_sequence = level_sequence_from_snapshot(snapshot)
        if level_sequence is None:
            continue
        identifiers = level_sequence_identifiers(level_sequence)
        identifiers.add(row["id"])
        identifiers.add(row["canonical_key"])
        if entity in identifiers:
            return row, level_sequence

    raise SystemExit(f"LevelSequence snapshot not found: {entity}")


def cinematics_filter_matches(filter_value: str | None, candidates: list[Any]) -> bool:
    if filter_value is None or filter_value == "":
        return True
    needle = str(filter_value).casefold()
    return any(str(candidate).casefold() == needle for candidate in candidates if candidate not in (None, ""))


def flatten_cinematics_keys(
    level_sequence: dict[str, Any],
    section: str | None = None,
    channel: str | None = None,
) -> list[dict[str, Any]]:
    items: list[dict[str, Any]] = []
    for section_value in json_list(level_sequence.get("sections")):
        if not isinstance(section_value, dict):
            continue
        if not cinematics_filter_matches(
            section,
            [
                section_value.get("id"),
                section_value.get("index"),
                section_value.get("track_id"),
                section_value.get("track_path"),
                section_value.get("section_path"),
                section_value.get("name"),
            ],
        ):
            continue
        for channel_value in json_list(section_value.get("channels")):
            if not isinstance(channel_value, dict):
                continue
            if not cinematics_filter_matches(
                channel,
                [
                    channel_value.get("id"),
                    channel_value.get("index"),
                    channel_value.get("channel_name"),
                    channel_value.get("display_name"),
                ],
            ):
                continue
            for key_value in json_list(channel_value.get("keys")):
                if not isinstance(key_value, dict):
                    continue
                row = dict(key_value)
                row.setdefault("sequence_id", level_sequence.get("id", ""))
                row.setdefault("sequence_path", level_sequence.get("sequence_path", ""))
                row.setdefault("track_id", section_value.get("track_id", ""))
                row.setdefault("track_path", section_value.get("track_path", ""))
                row.setdefault("section_id", section_value.get("id", ""))
                row.setdefault("section_index", section_value.get("index", 0))
                row.setdefault("section_path", section_value.get("section_path", ""))
                row.setdefault("section_name", section_value.get("name", ""))
                row.setdefault("channel_id", channel_value.get("id", ""))
                row.setdefault("channel_index", channel_value.get("index", 0))
                row.setdefault("channel_name", channel_value.get("channel_name", ""))
                row.setdefault("display_name", channel_value.get("display_name", ""))
                items.append(row)
    return items


def write_cinematics_key_artifact(
    db_path: Path,
    scan_id: str,
    entity: str,
    level_sequence: dict[str, Any],
    section: str | None,
    channel: str | None,
    items: list[dict[str, Any]],
) -> dict[str, Any]:
    if channel:
        scope_kind = "channel"
        scope_id = channel
    elif section:
        scope_kind = "section"
        scope_id = section
    else:
        scope_kind = "level_sequence"
        scope_id = str(level_sequence.get("id") or level_sequence.get("sequence_path") or entity)
    payload = {
        "schema_version": "uepi.cinematics_key_artifact.v1",
        "scan_id": scan_id,
        "entity": entity,
        "sequence_id": level_sequence.get("id", ""),
        "sequence_path": level_sequence.get("sequence_path", ""),
        "section_filter": section or "",
        "channel_filter": channel or "",
        "scope_kind": scope_kind,
        "scope_id": scope_id,
        "item_count": len(items),
        "items": items,
    }
    data = json.dumps(payload, ensure_ascii=False, sort_keys=True, indent=2).encode("utf-8")
    digest = hashlib.sha256(data).hexdigest()
    artifact_id = f"cinematics_keys_{digest[:24]}"
    artifact_dir = db_path.parent / "cinematics_artifacts"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    artifact_path = artifact_dir / f"{safe_artifact_name(artifact_id)}.json"
    if not artifact_path.exists():
        artifact_path.write_bytes(data)
    return {
        "schema_version": "uepi.cinematics_key_artifact_manifest.v1",
        "artifact_id": artifact_id,
        "artifact_uri": f"uepi://cinematics-key-artifact/{artifact_id}",
        "storage": "daemon_materialized_json",
        "scope_kind": scope_kind,
        "scope_id": scope_id,
        "path": str(artifact_path),
        "sha256": digest,
        "byte_count": len(data),
        "item_count": len(items),
        "encoding": "json",
    }


def cinematics_key_page(
    db_path: Path,
    entity: str,
    scan_id: str | None = None,
    limit: int = 100,
    cursor: str | None = None,
    section: str | None = None,
    channel: str | None = None,
    include_artifact: bool = False,
) -> dict[str, Any]:
    limit = clamp_limit(limit, 1000)
    cursor_value = decode_cursor(cursor)
    start_index = 0
    normalized_section = section or ""
    normalized_channel = channel or ""
    if cursor_value:
        if (
            cursor_value.get("entity") != entity
            or cursor_value.get("section", "") != normalized_section
            or cursor_value.get("channel", "") != normalized_channel
        ):
            raise SystemExit("Cursor does not match the requested entity and key filters.")
        start_index = int(cursor_value.get("index", -1)) + 1

    conn = connect(db_path)
    try:
        resolved_scan_id = resolve_scan_id(conn, scan_id, "Target") if scan_id else latest_scan_id(conn)
        row, level_sequence = find_level_sequence_snapshot(conn, resolved_scan_id, entity)
        items = flatten_cinematics_keys(level_sequence, section, channel)
        page_items = items[start_index : start_index + limit]
        next_cursor = None
        if start_index + len(page_items) < len(items):
            next_cursor = encode_cursor(
                {
                    "entity": entity,
                    "section": normalized_section,
                    "channel": normalized_channel,
                    "index": start_index + len(page_items) - 1,
                }
            )
        result = paged_result(page_items, limit, next_cursor)
        result.update(
            {
                "scan_id": resolved_scan_id,
                "entity": entity_row(row),
                "sequence_id": level_sequence.get("id", ""),
                "sequence_path": level_sequence.get("sequence_path", ""),
                "section_filter": normalized_section,
                "channel_filter": normalized_channel,
                "total_count": len(items),
                "start_index": start_index,
                "end_index": start_index + len(page_items) - 1 if page_items else start_index - 1,
            }
        )
        if include_artifact:
            result["key_artifact"] = write_cinematics_key_artifact(
                db_path,
                resolved_scan_id,
                entity,
                level_sequence,
                section,
                channel,
                items,
            )
        return result
    finally:
        conn.close()


def ingest(scan_path: Path, db_path: Path) -> dict[str, Any]:
    scan_path = require_sandbox_path(scan_path, "Scan artifact", db_path)
    scan_id, scan, _ = load_scan(scan_path)
    conn = connect(db_path)

    try:
        entities = scan.get("entities", [])
        relations = scan.get("relations", [])
        diagnostics = scan.get("diagnostics", [])

        with conn:
            conn.execute("DELETE FROM scans WHERE scan_id = ?", (scan_id,))
            conn.execute(
            """
            INSERT INTO scans(
                scan_id, schema_version, project_id, project_name, project_file, engine_version,
                started_at_utc, finished_at_utc, ingested_at_utc, scan_path,
                entity_count, relation_count, diagnostic_count, completeness_json, git_json
            ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                scan_id,
                scan.get("schema_version", ""),
                scan.get("project_id", ""),
                scan.get("project_name", ""),
                scan.get("project_file", ""),
                scan.get("engine_version", ""),
                scan.get("started_at_utc", ""),
                scan.get("finished_at_utc", ""),
                utc_now(),
                str(scan_path),
                len(entities),
                len(relations),
                len(diagnostics),
                json_text(scan.get("completeness", {})),
                json_text(git_metadata(scan.get("project_file", ""))),
            ),
        )

            conn.executemany(
            """
            INSERT INTO entities(
                scan_id, id, kind, canonical_key, display_name, source_layer,
                attributes_json, snapshot_json, completeness_json, diagnostics_json, evidence_json
            ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                (
                    scan_id,
                    entity.get("id", ""),
                    entity.get("kind", ""),
                    entity.get("canonical_key", ""),
                    entity.get("display_name", ""),
                    entity.get("source_layer", ""),
                    json_text(entity.get("attributes", {})),
                    json_text(entity.get("snapshot", {})),
                    json_text(entity.get("completeness", {})),
                    json_text(entity.get("diagnostics", [])),
                    json_text(entity.get("evidence", [])),
                )
                for entity in entities
            ],
        )

            conn.executemany(
            """
            INSERT INTO relations(
                scan_id, id, type, from_id, to_id, source_layer, derived, confidence,
                attributes_json, evidence_json
            ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                (
                    scan_id,
                    relation.get("id", ""),
                    relation.get("type", ""),
                    relation.get("from_id", ""),
                    relation.get("to_id", ""),
                    relation.get("source_layer", ""),
                    1 if relation.get("derived", False) else 0,
                    float(relation.get("confidence", 0.0)),
                    json_text(relation.get("attributes", {})),
                    json_text(relation.get("evidence", [])),
                )
                for relation in relations
            ],
        )

            conn.executemany(
            """
            INSERT INTO diagnostics(scan_id, ordinal, code, severity, message, context_json)
            VALUES(?, ?, ?, ?, ?, ?)
            """,
            [
                (
                    scan_id,
                    index,
                    diagnostic.get("code", ""),
                    diagnostic.get("severity", ""),
                    diagnostic.get("message", ""),
                    json_text(diagnostic.get("context", {})),
                )
                for index, diagnostic in enumerate(diagnostics)
            ],
        )

            if fts_available(conn):
                conn.execute("DELETE FROM entity_fts WHERE scan_id = ?", (scan_id,))
                conn.executemany(
                """
                INSERT INTO entity_fts(scan_id, id, kind, display_name, canonical_key, attributes)
                VALUES(?, ?, ?, ?, ?, ?)
                """,
                [
                    (
                        scan_id,
                        entity.get("id", ""),
                        entity.get("kind", ""),
                        entity.get("display_name", ""),
                        entity.get("canonical_key", ""),
                        json_text(entity.get("attributes", {})),
                    )
                    for entity in entities
                ],
                )

            record_asset_revisions(conn, scan_id, scan, entities, relations, str(scan_path))

        return summary_dict(conn, scan_id)
    finally:
        conn.close()


def record_asset_revisions(
    conn: sqlite3.Connection,
    scan_id: str,
    scan: dict[str, Any],
    entities: list[dict[str, Any]],
    relations: list[dict[str, Any]],
    scan_path: str,
) -> None:
    asset_entities = [entity for entity in entities if entity.get("kind") == "asset"]
    if not asset_entities:
        return

    relations_by_asset: dict[str, list[dict[str, Any]]] = {entity.get("id", ""): [] for entity in asset_entities}
    for relation in relations:
        for endpoint in (relation.get("from_id", ""), relation.get("to_id", "")):
            if endpoint in relations_by_asset:
                relations_by_asset[endpoint].append(relation)

    created_at = utc_now()
    revision_rows = []
    for entity in asset_entities:
        entity_id = entity.get("id", "")
        asset_key = entity.get("canonical_key", "")
        attributes = entity.get("attributes", {})
        asset_relations = sorted(
            relations_by_asset.get(entity_id, []),
            key=lambda relation: relation.get("id", ""),
        )
        payload = {
            "entity": {
                "id": entity_id,
                "kind": entity.get("kind", ""),
                "canonical_key": asset_key,
                "display_name": entity.get("display_name", ""),
                "source_layer": entity.get("source_layer", ""),
                "attributes": attributes,
                "snapshot": entity.get("snapshot", {}),
                "completeness": entity.get("completeness", {}),
                "diagnostics": entity.get("diagnostics", []),
                "evidence": entity.get("evidence", []),
            },
            "relations": [
                {
                    "id": relation.get("id", ""),
                    "type": relation.get("type", ""),
                    "from_id": relation.get("from_id", ""),
                    "to_id": relation.get("to_id", ""),
                    "source_layer": relation.get("source_layer", ""),
                    "derived": bool(relation.get("derived", False)),
                    "confidence": float(relation.get("confidence", 0.0)),
                    "attributes": relation.get("attributes", {}),
                    "evidence": relation.get("evidence", []),
                }
                for relation in asset_relations
            ],
        }
        canonical_hash = "sha256:" + hashlib.sha256(json_text(payload).encode("utf-8")).hexdigest()
        revision_id = hashlib.sha256(
            json_text(
                {
                    "kind": "asset_revision",
                    "asset_key": asset_key,
                    "scan_id": scan_id,
                    "canonical_hash": canonical_hash,
                }
            ).encode("utf-8")
        ).hexdigest()
        file_fingerprint = {
            "scan_path": scan_path,
            "package_name": attributes.get("package_name", ""),
            "package_path": attributes.get("package_path", ""),
            "object_path": attributes.get("object_path", asset_key),
            "asset_class_path": attributes.get("asset_class_path", ""),
            "object_class_path": attributes.get("object_class_path", ""),
            "collection_level": attributes.get("collection_level", ""),
            "package_file": package_file_fingerprint(scan.get("project_file", ""), attributes.get("package_name", "")),
        }

        conn.execute(
            """
            UPDATE asset_revisions
            SET is_current = 0,
                valid_to_scan = COALESCE(valid_to_scan, ?)
            WHERE asset_key = ? AND is_current = 1
            """,
            (scan_id, asset_key),
        )
        revision_rows.append(
            (
                revision_id,
                asset_key,
                entity_id,
                scan_id,
                scan.get("project_id", ""),
                scan.get("project_name", ""),
                attributes.get("asset_class_path", ""),
                attributes.get("object_path", asset_key),
                attributes.get("package_name", ""),
                attributes.get("package_path", ""),
                canonical_hash,
                json_text(file_fingerprint),
                scan_id,
                scan_id,
                None,
                created_at,
                1,
            )
        )

    conn.executemany(
        """
        INSERT OR REPLACE INTO asset_revisions(
            revision_id, asset_key, asset_entity_id, scan_id, project_id, project_name,
            asset_class_path, object_path, package_name, package_path,
            canonical_hash, file_fingerprint_json, snapshot_artifact_id,
            valid_from_scan, valid_to_scan, created_at_utc, is_current
        ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        revision_rows,
    )


def summary_dict(conn: sqlite3.Connection, scan_id: str | None = None) -> dict[str, Any]:
    scan_id = scan_id or latest_scan_id(conn)
    row = conn.execute("SELECT * FROM scans WHERE scan_id = ?", (scan_id,)).fetchone()
    if not row:
        raise SystemExit(f"Scan not found: {scan_id}")

    relation_types = conn.execute(
        """
        SELECT type, COUNT(*) AS count
        FROM relations
        WHERE scan_id = ?
        GROUP BY type
        ORDER BY count DESC, type ASC
        """,
        (scan_id,),
    ).fetchall()

    revision_counts = conn.execute(
        """
        SELECT
            COUNT(*) AS total_count,
            SUM(CASE WHEN scan_id = ? THEN 1 ELSE 0 END) AS scan_count,
            SUM(CASE WHEN is_current = 1 THEN 1 ELSE 0 END) AS current_count
        FROM asset_revisions
        """
        ,
        (scan_id,),
    ).fetchone()

    return {
        "scan_id": row["scan_id"],
        "schema_version": row["schema_version"],
        "project_id": row["project_id"],
        "project_name": row["project_name"],
        "engine_version": row["engine_version"],
        "entity_count": row["entity_count"],
        "relation_count": row["relation_count"],
        "diagnostic_count": row["diagnostic_count"],
        "fts5_available": fts_available(conn),
        "git": json.loads(row["git_json"]),
        "stale": scan_artifact_status(row),
        "package_stale": package_staleness_for_scan(conn, scan_id, 0),
        "asset_revisions": {
            "total_count": int(revision_counts["total_count"] or 0),
            "scan_count": int(revision_counts["scan_count"] or 0),
            "current_count": int(revision_counts["current_count"] or 0),
        },
        "relation_types": {item["type"]: item["count"] for item in relation_types},
    }


def scan_meta(conn: sqlite3.Connection, scan_id: str) -> dict[str, Any]:
    row = conn.execute("SELECT * FROM scans WHERE scan_id = ?", (scan_id,)).fetchone()
    if not row:
        raise SystemExit(f"Scan not found: {scan_id}")
    return {
        "scan_id": row["scan_id"],
        "short_scan_id": row["scan_id"][:12],
        "project_name": row["project_name"],
        "scan_path": row["scan_path"],
        "ingested_at_utc": row["ingested_at_utc"],
        "entity_count": row["entity_count"],
        "relation_count": row["relation_count"],
        "diagnostic_count": row["diagnostic_count"],
        "git": json.loads(row["git_json"]),
    }


def scan_artifact_status(row: sqlite3.Row) -> dict[str, Any]:
    scan_path = Path(row["scan_path"])
    status = {
        "scan_path": row["scan_path"],
        "exists": scan_path.exists(),
        "indexed_scan_id": row["scan_id"],
        "current_scan_id": None,
        "stale": False,
        "reason": "current",
    }
    if not scan_path.exists():
        status["stale"] = True
        status["reason"] = "scan_artifact_missing"
        return status

    current_scan_id = hashlib.sha256(scan_path.read_bytes()).hexdigest()
    status["current_scan_id"] = current_scan_id
    if current_scan_id != row["scan_id"]:
        status["stale"] = True
        status["reason"] = "scan_artifact_changed"
    return status


def compare_package_file_status(file_fingerprint: dict[str, Any]) -> dict[str, Any]:
    stored = file_fingerprint.get("package_file", {})
    status = {
        "stale": False,
        "status": "unknown",
        "reason": stored.get("reason", "missing_package_file_fingerprint"),
        "path": stored.get("path", ""),
        "stored": stored,
        "current": None,
    }
    if not stored.get("resolved"):
        return status

    path = Path(stored.get("path", ""))
    if not path.exists():
        status["stale"] = True
        status["status"] = "stale"
        status["reason"] = "package_file_missing"
        status["current"] = {
            "exists": False,
            "size": None,
            "mtime_ns": None,
            "sha256": "",
        }
        return status

    stat = path.stat()
    current = {
        "exists": True,
        "size": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "sha256": sha256_file(path),
    }
    status["current"] = current
    changed_fields = [
        field
        for field in ("size", "mtime_ns", "sha256")
        if stored.get(field) != current.get(field)
    ]
    if changed_fields:
        status["stale"] = True
        status["status"] = "stale"
        status["reason"] = "package_file_changed"
        status["changed_fields"] = changed_fields
    else:
        status["status"] = "current"
        status["reason"] = "current"
    return status


def package_staleness_for_scan(conn: sqlite3.Connection, scan_id: str, limit: int = 100) -> dict[str, Any]:
    rows = conn.execute(
        """
        SELECT revision_id, asset_key, scan_id, file_fingerprint_json, is_current
        FROM asset_revisions
        WHERE scan_id = ?
        ORDER BY asset_key ASC
        """,
        (scan_id,),
    ).fetchall()
    details = []
    counts = Counter()
    for row in rows:
        file_fingerprint = json.loads(row["file_fingerprint_json"])
        package_status = compare_package_file_status(file_fingerprint)
        counts[package_status["status"]] += 1
        if package_status["status"] != "current":
            details.append(
                {
                    "asset_key": row["asset_key"],
                    "revision_id": row["revision_id"],
                    "short_revision_id": row["revision_id"][:12],
                    "scan_id": row["scan_id"],
                    "is_current": bool(row["is_current"]),
                    "package": package_status,
                }
            )

    return {
        "checked_count": len(rows),
        "current_count": counts.get("current", 0),
        "stale_count": counts.get("stale", 0),
        "unknown_count": counts.get("unknown", 0),
        "ok": counts.get("stale", 0) == 0 and counts.get("unknown", 0) == 0,
        "issues": limited(details, limit),
        "truncated_issue_count": max(len(details) - max(limit, 0), 0),
    }


def staleness(db_path: Path, scan_id: str | None = None, limit: int = 100) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        scan_id = resolve_scan_id(conn, scan_id, "Target") if scan_id else latest_scan_id(conn)
        row = conn.execute("SELECT * FROM scans WHERE scan_id = ?", (scan_id,)).fetchone()
        if not row:
            raise SystemExit(f"Scan not found: {scan_id}")
        return {
            "scan": scan_meta(conn, scan_id),
            "artifact": scan_artifact_status(row),
            "packages": package_staleness_for_scan(conn, scan_id, limit),
        }
    finally:
        conn.close()


def db_integrity(db_path: Path) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        integrity_rows = [row[0] for row in conn.execute("PRAGMA integrity_check").fetchall()]
        foreign_key_rows = [dict(row) for row in conn.execute("PRAGMA foreign_key_check").fetchall()]
        counts = {
            "scans": conn.execute("SELECT COUNT(*) AS count FROM scans").fetchone()["count"],
            "entities": conn.execute("SELECT COUNT(*) AS count FROM entities").fetchone()["count"],
            "relations": conn.execute("SELECT COUNT(*) AS count FROM relations").fetchone()["count"],
            "diagnostics": conn.execute("SELECT COUNT(*) AS count FROM diagnostics").fetchone()["count"],
            "asset_revisions": conn.execute("SELECT COUNT(*) AS count FROM asset_revisions").fetchone()["count"],
        }
        return {
            "ok": integrity_rows == ["ok"] and not foreign_key_rows,
            "db_path": str(db_path),
            "integrity_check": integrity_rows,
            "foreign_key_issues": foreign_key_rows,
            "counts": counts,
        }
    finally:
        conn.close()


def db_recover(db_path: Path) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        conn.commit()
        checkpoint = [dict(row) for row in conn.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchall()]
        conn.execute("PRAGMA optimize")
    finally:
        conn.close()
    integrity = db_integrity(db_path)
    return {
        "ok": integrity["ok"],
        "db_path": str(db_path),
        "wal_checkpoint": checkpoint,
        "integrity": integrity,
    }


def asset_history(db_path: Path, asset: str | None, limit: int = 50) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        params: list[Any] = []
        where_clause = ""
        if asset:
            where_clause = """
            WHERE ar.asset_key = ?
               OR ar.asset_entity_id = ?
               OR ar.revision_id = ?
               OR ar.revision_id LIKE ?
            """
            params = [asset, asset, asset, f"{asset}%"]

        rows = conn.execute(
            f"""
            SELECT ar.*, s.scan_path, s.ingested_at_utc, s.entity_count, s.relation_count, s.diagnostic_count
            FROM asset_revisions ar
            JOIN scans s ON s.scan_id = ar.scan_id
            {where_clause}
            ORDER BY ar.asset_key ASC, ar.created_at_utc DESC, ar.revision_id ASC
            LIMIT ?
            """,
            (*params, max(limit, 0)),
        ).fetchall()

        if asset and not rows:
            rows = conn.execute(
                """
                SELECT ar.*, s.scan_path, s.ingested_at_utc, s.entity_count, s.relation_count, s.diagnostic_count
                FROM asset_revisions ar
                JOIN scans s ON s.scan_id = ar.scan_id
                WHERE ar.asset_key LIKE ?
                ORDER BY ar.asset_key ASC, ar.created_at_utc DESC, ar.revision_id ASC
                LIMIT ?
                """,
                (f"%{asset}%", max(limit, 0)),
            ).fetchall()

        return {
            "query": asset,
            "revision_count": len(rows),
            "revisions": [asset_revision_row(row) for row in rows],
        }
    finally:
        conn.close()


def asset_revision_row(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "revision_id": row["revision_id"],
        "short_revision_id": row["revision_id"][:12],
        "asset_key": row["asset_key"],
        "asset_entity_id": row["asset_entity_id"],
        "scan_id": row["scan_id"],
        "short_scan_id": row["scan_id"][:12],
        "project_name": row["project_name"],
        "asset_class_path": row["asset_class_path"],
        "object_path": row["object_path"],
        "package_name": row["package_name"],
        "package_path": row["package_path"],
        "canonical_hash": row["canonical_hash"],
        "file_fingerprint": json.loads(row["file_fingerprint_json"]),
        "snapshot_artifact_id": row["snapshot_artifact_id"],
        "valid_from_scan": row["valid_from_scan"],
        "valid_to_scan": row["valid_to_scan"],
        "created_at_utc": row["created_at_utc"],
        "is_current": bool(row["is_current"]),
        "scan_path": row["scan_path"],
        "ingested_at_utc": row["ingested_at_utc"],
        "scan_entity_count": row["entity_count"],
        "scan_relation_count": row["relation_count"],
        "scan_diagnostic_count": row["diagnostic_count"],
    }


def row_digest(row: sqlite3.Row, fields: list[str]) -> str:
    payload = {field: row[field] for field in fields}
    return hashlib.sha256(json_text(payload).encode("utf-8")).hexdigest()


def changed_fields(before: sqlite3.Row, after: sqlite3.Row, fields: list[str]) -> list[str]:
    return [field for field in fields if before[field] != after[field]]


def limited(items: list[dict[str, Any]], limit: int) -> list[dict[str, Any]]:
    return items[: max(limit, 0)]


def counter_dict(values: list[str]) -> dict[str, int]:
    return dict(sorted(Counter(values).items()))


def clamp_limit(limit: int, maximum: int = 500) -> int:
    return max(0, min(limit, maximum))


def encode_cursor(values: dict[str, Any] | None) -> str | None:
    if not values:
        return None
    payload = json_text(values).encode("utf-8")
    return base64.urlsafe_b64encode(payload).decode("ascii").rstrip("=")


def decode_cursor(cursor: str | None) -> dict[str, Any] | None:
    if not cursor:
        return None
    padding = "=" * (-len(cursor) % 4)
    try:
        payload = base64.urlsafe_b64decode((cursor + padding).encode("ascii"))
        value = json.loads(payload.decode("utf-8"))
    except (ValueError, json.JSONDecodeError) as exc:
        raise SystemExit(f"Invalid cursor: {cursor}") from exc
    if not isinstance(value, dict):
        raise SystemExit("Invalid cursor payload.")
    return value


def paged_result(items: list[dict[str, Any]], limit: int, next_cursor: str | None) -> dict[str, Any]:
    return {
        "items": items,
        "count": len(items),
        "limit": limit,
        "next_cursor": next_cursor,
        "has_more": bool(next_cursor),
    }


def list_scans(db_path: Path, limit: int = 50, cursor: str | None = None) -> dict[str, Any]:
    limit = clamp_limit(limit)
    cursor_value = decode_cursor(cursor)
    where_clause = ""
    params: list[Any] = []
    if cursor_value:
        where_clause = "WHERE (ingested_at_utc < ? OR (ingested_at_utc = ? AND scan_id < ?))"
        params.extend([
            cursor_value.get("ingested_at_utc", ""),
            cursor_value.get("ingested_at_utc", ""),
            cursor_value.get("scan_id", ""),
        ])

    conn = connect(db_path)
    try:
        rows = conn.execute(
            f"""
            SELECT * FROM scans
            {where_clause}
            ORDER BY ingested_at_utc DESC, scan_id DESC
            LIMIT ?
            """,
            (*params, limit + 1),
        ).fetchall()
        page_rows = rows[:limit]
        items = [
            {
                "scan_id": row["scan_id"],
                "short_scan_id": row["scan_id"][:12],
                "project_name": row["project_name"],
                "scan_path": row["scan_path"],
                "ingested_at_utc": row["ingested_at_utc"],
                "entity_count": row["entity_count"],
                "relation_count": row["relation_count"],
                "diagnostic_count": row["diagnostic_count"],
                "git": json.loads(row["git_json"]),
                "stale": scan_artifact_status(row),
            }
            for row in page_rows
        ]
        next_cursor = None
        if len(rows) > limit and page_rows:
            last = page_rows[-1]
            next_cursor = encode_cursor({"ingested_at_utc": last["ingested_at_utc"], "scan_id": last["scan_id"]})
        return paged_result(items, limit, next_cursor)
    finally:
        conn.close()


def list_entities(
    db_path: Path,
    scan_id: str | None = None,
    kind: str | None = None,
    limit: int = 100,
    cursor: str | None = None,
    include_snapshot: bool = False,
) -> dict[str, Any]:
    limit = clamp_limit(limit)
    cursor_value = decode_cursor(cursor)

    conn = connect(db_path)
    try:
        resolved_scan_id = resolve_scan_id(conn, scan_id, "Target") if scan_id else latest_scan_id(conn)
        clauses = ["scan_id = ?"]
        params: list[Any] = [resolved_scan_id]
        if kind:
            clauses.append("kind = ?")
            params.append(kind)
        if cursor_value:
            clauses.append("(canonical_key > ? OR (canonical_key = ? AND id > ?))")
            params.extend([
                cursor_value.get("canonical_key", ""),
                cursor_value.get("canonical_key", ""),
                cursor_value.get("id", ""),
            ])
        rows = conn.execute(
            f"""
            SELECT id, kind, canonical_key, display_name, source_layer, attributes_json, snapshot_json
            FROM entities
            WHERE {' AND '.join(clauses)}
            ORDER BY canonical_key ASC, id ASC
            LIMIT ?
            """,
            (*params, limit + 1),
        ).fetchall()
        page_rows = rows[:limit]
        items = [entity_row(row, include_snapshot=include_snapshot) for row in page_rows]
        next_cursor = None
        if len(rows) > limit and page_rows:
            last = page_rows[-1]
            next_cursor = encode_cursor({"canonical_key": last["canonical_key"], "id": last["id"]})
        result = paged_result(items, limit, next_cursor)
        result["scan_id"] = resolved_scan_id
        result["kind"] = kind
        return result
    finally:
        conn.close()


def list_relations(
    db_path: Path,
    scan_id: str | None = None,
    relation_type: str | None = None,
    limit: int = 100,
    cursor: str | None = None,
) -> dict[str, Any]:
    limit = clamp_limit(limit)
    cursor_value = decode_cursor(cursor)

    conn = connect(db_path)
    try:
        resolved_scan_id = resolve_scan_id(conn, scan_id, "Target") if scan_id else latest_scan_id(conn)
        clauses = ["r.scan_id = ?"]
        params: list[Any] = [resolved_scan_id]
        if relation_type:
            clauses.append("r.type = ?")
            params.append(relation_type)
        if cursor_value:
            clauses.append("(r.type > ? OR (r.type = ? AND r.id > ?))")
            params.extend([
                cursor_value.get("type", ""),
                cursor_value.get("type", ""),
                cursor_value.get("id", ""),
            ])

        rows = conn.execute(
            f"""
            SELECT r.id, r.type, r.from_id, r.to_id, r.source_layer, r.derived, r.confidence,
                   r.attributes_json,
                   from_e.canonical_key AS from_key,
                   to_e.canonical_key AS to_key
            FROM relations r
            LEFT JOIN entities from_e ON from_e.scan_id = r.scan_id AND from_e.id = r.from_id
            LEFT JOIN entities to_e ON to_e.scan_id = r.scan_id AND to_e.id = r.to_id
            WHERE {' AND '.join(clauses)}
            ORDER BY r.type ASC, r.id ASC
            LIMIT ?
            """,
            (*params, limit + 1),
        ).fetchall()
        page_rows = rows[:limit]
        items = [
            {
                "id": row["id"],
                "type": row["type"],
                "from_id": row["from_id"],
                "to_id": row["to_id"],
                "from_key": row["from_key"],
                "to_key": row["to_key"],
                "source_layer": row["source_layer"],
                "derived": bool(row["derived"]),
                "confidence": row["confidence"],
                "attributes": json.loads(row["attributes_json"]),
            }
            for row in page_rows
        ]
        next_cursor = None
        if len(rows) > limit and page_rows:
            last = page_rows[-1]
            next_cursor = encode_cursor({"type": last["type"], "id": last["id"]})
        result = paged_result(items, limit, next_cursor)
        result["scan_id"] = resolved_scan_id
        result["relation_type"] = relation_type
        return result
    finally:
        conn.close()


def load_entity_rows(conn: sqlite3.Connection, scan_id: str) -> dict[str, sqlite3.Row]:
    rows = conn.execute(
        """
        SELECT id, kind, canonical_key, display_name, source_layer,
               attributes_json, snapshot_json, completeness_json, diagnostics_json, evidence_json
        FROM entities
        WHERE scan_id = ?
        """,
        (scan_id,),
    ).fetchall()
    return {row["id"]: row for row in rows}


def load_relation_rows(conn: sqlite3.Connection, scan_id: str) -> dict[str, sqlite3.Row]:
    rows = conn.execute(
        """
        SELECT id, type, from_id, to_id, source_layer, derived, confidence, attributes_json, evidence_json
        FROM relations
        WHERE scan_id = ?
        """,
        (scan_id,),
    ).fetchall()
    return {row["id"]: row for row in rows}


def entity_diff_item(row: sqlite3.Row, changed: list[str] | None = None) -> dict[str, Any]:
    item = {
        "id": row["id"],
        "kind": row["kind"],
        "canonical_key": row["canonical_key"],
        "display_name": row["display_name"],
        "source_layer": row["source_layer"],
    }
    if changed is not None:
        item["changed_fields"] = changed
    return item


def relation_diff_item(
    row: sqlite3.Row,
    entity_rows: dict[str, sqlite3.Row],
    changed: list[str] | None = None,
) -> dict[str, Any]:
    from_entity = entity_rows.get(row["from_id"])
    to_entity = entity_rows.get(row["to_id"])
    item = {
        "id": row["id"],
        "type": row["type"],
        "from_id": row["from_id"],
        "to_id": row["to_id"],
        "from_key": from_entity["canonical_key"] if from_entity else None,
        "to_key": to_entity["canonical_key"] if to_entity else None,
        "source_layer": row["source_layer"],
        "derived": bool(row["derived"]),
        "confidence": row["confidence"],
    }
    if changed is not None:
        item["changed_fields"] = changed
    return item


def entity_attributes(row: sqlite3.Row) -> dict[str, Any]:
    return json.loads(row["attributes_json"])


def data_table_scope(row: sqlite3.Row) -> str:
    attributes = entity_attributes(row)
    return attributes.get("table_path") or row["canonical_key"].split(":row:", 1)[0].split(":column:", 1)[0]


def animation_manifest_scope(row: sqlite3.Row) -> str:
    attributes = entity_attributes(row)
    for key in (
        "sequence_path",
        "blend_space_path",
        "pose_asset_path",
        "anim_blueprint_path",
        "control_rig_blueprint_path",
        "physics_asset_path",
        "ik_rig_path",
        "ik_retargeter_path",
        "skeletal_mesh_path",
        "skeleton_path",
    ):
        value = attributes.get(key)
        if value:
            return value
    return row["canonical_key"]


def scoped_entity_diff_item(
    row: sqlite3.Row,
    scope: str,
    changed: list[str] | None = None,
) -> dict[str, Any]:
    item = entity_diff_item(row, changed)
    item["scope"] = scope
    return item


def domain_entity_diff(
    base_entities: dict[str, sqlite3.Row],
    compare_entities: dict[str, sqlite3.Row],
    added_entity_ids: list[str],
    removed_entity_ids: list[str],
    changed_entity_ids: list[str],
    entity_fields: list[str],
    kinds: set[str],
    scope_fn: Any,
    limit: int,
) -> dict[str, Any]:
    def matching_added() -> list[sqlite3.Row]:
        return [compare_entities[entity_id] for entity_id in added_entity_ids if compare_entities[entity_id]["kind"] in kinds]

    def matching_removed() -> list[sqlite3.Row]:
        return [base_entities[entity_id] for entity_id in removed_entity_ids if base_entities[entity_id]["kind"] in kinds]

    def matching_changed() -> list[tuple[sqlite3.Row, list[str]]]:
        return [
            (
                compare_entities[entity_id],
                changed_fields(base_entities[entity_id], compare_entities[entity_id], entity_fields),
            )
            for entity_id in changed_entity_ids
            if compare_entities[entity_id]["kind"] in kinds
        ]

    added = [scoped_entity_diff_item(row, scope_fn(row)) for row in matching_added()]
    removed = [scoped_entity_diff_item(row, scope_fn(row)) for row in matching_removed()]
    changed = [scoped_entity_diff_item(row, scope_fn(row), fields) for row, fields in matching_changed()]
    all_items = added + removed + changed

    return {
        "entity_added_count": len(added),
        "entity_removed_count": len(removed),
        "entity_changed_count": len(changed),
        "added_by_kind": counter_dict([item["kind"] for item in added]),
        "removed_by_kind": counter_dict([item["kind"] for item in removed]),
        "changed_by_kind": counter_dict([item["kind"] for item in changed]),
        "affected_scope_count": len({item["scope"] for item in all_items}),
        "affected_scopes": sorted({item["scope"] for item in all_items})[: max(limit, 0)],
        "added": limited(added, limit),
        "removed": limited(removed, limit),
        "changed": limited(changed, limit),
    }


def scan_diff(
    db_path: Path,
    base_scan_id: str | None = None,
    compare_scan_id: str | None = None,
    limit: int = 100,
) -> dict[str, Any]:
    entity_fields = [
        "kind",
        "canonical_key",
        "display_name",
        "source_layer",
        "attributes_json",
        "snapshot_json",
        "completeness_json",
        "diagnostics_json",
        "evidence_json",
    ]
    relation_fields = [
        "type",
        "from_id",
        "to_id",
        "source_layer",
        "derived",
        "confidence",
        "attributes_json",
        "evidence_json",
    ]

    conn = connect(db_path)
    try:
        if base_scan_id and compare_scan_id:
            base_scan_id = resolve_scan_id(conn, base_scan_id, "Base")
            compare_scan_id = resolve_scan_id(conn, compare_scan_id, "Compare")
        elif base_scan_id or compare_scan_id:
            raise SystemExit("Both --base and --compare must be supplied, or neither.")
        else:
            base_scan_id, compare_scan_id = latest_two_scan_ids(conn)

        base_entities = load_entity_rows(conn, base_scan_id)
        compare_entities = load_entity_rows(conn, compare_scan_id)
        base_relations = load_relation_rows(conn, base_scan_id)
        compare_relations = load_relation_rows(conn, compare_scan_id)

        added_entity_ids = sorted(set(compare_entities) - set(base_entities))
        removed_entity_ids = sorted(set(base_entities) - set(compare_entities))
        common_entity_ids = sorted(set(base_entities) & set(compare_entities))
        changed_entity_ids = [
            entity_id
            for entity_id in common_entity_ids
            if row_digest(base_entities[entity_id], entity_fields)
            != row_digest(compare_entities[entity_id], entity_fields)
        ]

        added_relation_ids = sorted(set(compare_relations) - set(base_relations))
        removed_relation_ids = sorted(set(base_relations) - set(compare_relations))
        common_relation_ids = sorted(set(base_relations) & set(compare_relations))
        changed_relation_ids = [
            relation_id
            for relation_id in common_relation_ids
            if row_digest(base_relations[relation_id], relation_fields)
            != row_digest(compare_relations[relation_id], relation_fields)
        ]

        impacted_entity_ids = set(added_entity_ids) | set(removed_entity_ids) | set(changed_entity_ids)
        for relation_id in added_relation_ids:
            relation = compare_relations[relation_id]
            impacted_entity_ids.update([relation["from_id"], relation["to_id"]])
        for relation_id in removed_relation_ids:
            relation = base_relations[relation_id]
            impacted_entity_ids.update([relation["from_id"], relation["to_id"]])
        for relation_id in changed_relation_ids:
            before = base_relations[relation_id]
            after = compare_relations[relation_id]
            impacted_entity_ids.update([before["from_id"], before["to_id"], after["from_id"], after["to_id"]])

        impacted_entities = []
        for entity_id in sorted(impacted_entity_ids):
            row = compare_entities.get(entity_id) or base_entities.get(entity_id)
            if row:
                impacted_entities.append(entity_diff_item(row))

        blueprint_entity_kinds = {"blueprint", "blueprint_graph", "blueprint_node", "blueprint_pin"}
        blueprint_relation_types = {
            "contains_graph",
            "contains_node",
            "has_pin",
            "connects_to",
            "exec_flows_to",
            "data_flows_to",
            "delegate_flows_to",
        }
        data_table_entity_kinds = {"data_table", "data_table_row", "data_table_column"}
        animation_manifest_entity_kinds = {
            "skeleton",
            "bone",
            "skeletal_mesh",
            "animation_sequence",
            "animation_track",
            "anim_notify",
            "blend_space",
            "blend_space_sample",
            "pose_asset",
            "pose_asset_pose",
            "pose_asset_curve",
            "ik_rig",
            "ik_rig_chain",
            "ik_rig_goal",
            "ik_rig_solver",
            "ik_retargeter",
            "ik_retarget_chain_map",
            "physics_asset",
            "physics_body",
            "physics_shape",
            "physics_constraint",
            "anim_blueprint",
            "anim_state_machine",
            "anim_state",
            "anim_transition",
            "anim_asset_player",
            "anim_cached_pose",
            "anim_slot",
            "anim_control_rig_node",
            "control_rig_blueprint",
            "control_rig_vm_graph",
            "control_rig_vm_node",
            "control_rig_vm_pin",
            "control_rig_vm_link",
            "control_rig_hierarchy_element",
        }

        added_entities = [entity_diff_item(compare_entities[entity_id]) for entity_id in added_entity_ids]
        removed_entities = [entity_diff_item(base_entities[entity_id]) for entity_id in removed_entity_ids]
        changed_entities = [
            entity_diff_item(
                compare_entities[entity_id],
                changed_fields(base_entities[entity_id], compare_entities[entity_id], entity_fields),
            )
            for entity_id in changed_entity_ids
        ]

        added_relations = [
            relation_diff_item(compare_relations[relation_id], compare_entities)
            for relation_id in added_relation_ids
        ]
        removed_relations = [
            relation_diff_item(removed_relation, base_entities)
            for removed_relation in (base_relations[relation_id] for relation_id in removed_relation_ids)
        ]
        changed_relations = [
            relation_diff_item(
                compare_relations[relation_id],
                compare_entities,
                changed_fields(base_relations[relation_id], compare_relations[relation_id], relation_fields),
            )
            for relation_id in changed_relation_ids
        ]
        data_table_diff = domain_entity_diff(
            base_entities,
            compare_entities,
            added_entity_ids,
            removed_entity_ids,
            changed_entity_ids,
            entity_fields,
            data_table_entity_kinds,
            data_table_scope,
            limit,
        )
        animation_manifest_diff = domain_entity_diff(
            base_entities,
            compare_entities,
            added_entity_ids,
            removed_entity_ids,
            changed_entity_ids,
            entity_fields,
            animation_manifest_entity_kinds,
            animation_manifest_scope,
            limit,
        )

        return {
            "base": scan_meta(conn, base_scan_id),
            "compare": scan_meta(conn, compare_scan_id),
            "entity_diff": {
                "added_count": len(added_entities),
                "removed_count": len(removed_entities),
                "changed_count": len(changed_entities),
                "added_by_kind": counter_dict([item["kind"] for item in added_entities]),
                "removed_by_kind": counter_dict([item["kind"] for item in removed_entities]),
                "changed_by_kind": counter_dict([item["kind"] for item in changed_entities]),
                "added": limited(added_entities, limit),
                "removed": limited(removed_entities, limit),
                "changed": limited(changed_entities, limit),
            },
            "relation_diff": {
                "added_count": len(added_relations),
                "removed_count": len(removed_relations),
                "changed_count": len(changed_relations),
                "added_by_type": counter_dict([item["type"] for item in added_relations]),
                "removed_by_type": counter_dict([item["type"] for item in removed_relations]),
                "changed_by_type": counter_dict([item["type"] for item in changed_relations]),
                "added": limited(added_relations, limit),
                "removed": limited(removed_relations, limit),
                "changed": limited(changed_relations, limit),
            },
            "blueprint_diff": {
                "entity_added_count": sum(1 for item in added_entities if item["kind"] in blueprint_entity_kinds),
                "entity_removed_count": sum(1 for item in removed_entities if item["kind"] in blueprint_entity_kinds),
                "entity_changed_count": sum(1 for item in changed_entities if item["kind"] in blueprint_entity_kinds),
                "relation_added_count": sum(1 for item in added_relations if item["type"] in blueprint_relation_types),
                "relation_removed_count": sum(1 for item in removed_relations if item["type"] in blueprint_relation_types),
                "relation_changed_count": sum(1 for item in changed_relations if item["type"] in blueprint_relation_types),
            },
            "data_table_diff": {
                **data_table_diff,
                "table_added_count": data_table_diff["added_by_kind"].get("data_table", 0),
                "table_removed_count": data_table_diff["removed_by_kind"].get("data_table", 0),
                "table_changed_count": data_table_diff["changed_by_kind"].get("data_table", 0),
                "row_added_count": data_table_diff["added_by_kind"].get("data_table_row", 0),
                "row_removed_count": data_table_diff["removed_by_kind"].get("data_table_row", 0),
                "row_changed_count": data_table_diff["changed_by_kind"].get("data_table_row", 0),
                "column_added_count": data_table_diff["added_by_kind"].get("data_table_column", 0),
                "column_removed_count": data_table_diff["removed_by_kind"].get("data_table_column", 0),
                "column_changed_count": data_table_diff["changed_by_kind"].get("data_table_column", 0),
            },
            "animation_manifest_diff": {
                **animation_manifest_diff,
                "sequence_added_count": animation_manifest_diff["added_by_kind"].get("animation_sequence", 0),
                "sequence_removed_count": animation_manifest_diff["removed_by_kind"].get("animation_sequence", 0),
                "sequence_changed_count": animation_manifest_diff["changed_by_kind"].get("animation_sequence", 0),
                "track_added_count": animation_manifest_diff["added_by_kind"].get("animation_track", 0),
                "track_removed_count": animation_manifest_diff["removed_by_kind"].get("animation_track", 0),
                "track_changed_count": animation_manifest_diff["changed_by_kind"].get("animation_track", 0),
                "notify_added_count": animation_manifest_diff["added_by_kind"].get("anim_notify", 0),
                "notify_removed_count": animation_manifest_diff["removed_by_kind"].get("anim_notify", 0),
                "notify_changed_count": animation_manifest_diff["changed_by_kind"].get("anim_notify", 0),
                "blend_sample_added_count": animation_manifest_diff["added_by_kind"].get("blend_space_sample", 0),
                "blend_sample_removed_count": animation_manifest_diff["removed_by_kind"].get("blend_space_sample", 0),
                "blend_sample_changed_count": animation_manifest_diff["changed_by_kind"].get("blend_space_sample", 0),
            },
            "impact": {
                "affected_entity_count": len(impacted_entities),
                "affected_by_kind": counter_dict([item["kind"] for item in impacted_entities]),
                "entities": limited(impacted_entities, limit),
            },
            "truncated": {
                "limit": max(limit, 0),
                "entity_added": max(len(added_entities) - max(limit, 0), 0),
                "entity_removed": max(len(removed_entities) - max(limit, 0), 0),
                "entity_changed": max(len(changed_entities) - max(limit, 0), 0),
                "relation_added": max(len(added_relations) - max(limit, 0), 0),
                "relation_removed": max(len(removed_relations) - max(limit, 0), 0),
                "relation_changed": max(len(changed_relations) - max(limit, 0), 0),
                "impact_entities": max(len(impacted_entities) - max(limit, 0), 0),
            },
        }
    finally:
        conn.close()


def search(db_path: Path, query: str, limit: int, include_snapshot: bool = False) -> list[dict[str, Any]]:
    conn = connect(db_path)
    try:
        scan_id = latest_scan_id(conn)

        if fts_available(conn):
            try:
                rows = conn.execute(
                    """
                    SELECT e.id, e.kind, e.canonical_key, e.display_name, e.source_layer, e.attributes_json, e.snapshot_json
                    FROM entity_fts f
                    JOIN entities e ON e.scan_id = f.scan_id AND e.id = f.id
                    WHERE f.scan_id = ? AND entity_fts MATCH ?
                    ORDER BY rank
                    LIMIT ?
                    """,
                    (scan_id, query, limit),
                ).fetchall()
            except sqlite3.OperationalError:
                rows = like_search_rows(conn, scan_id, query, limit)
        else:
            rows = like_search_rows(conn, scan_id, query, limit)

        return [entity_row(row, include_snapshot=include_snapshot) for row in rows]
    finally:
        conn.close()


def like_search_rows(
    conn: sqlite3.Connection, scan_id: str, query: str, limit: int
) -> list[sqlite3.Row]:
    like_query = f"%{query}%"
    return conn.execute(
        """
        SELECT id, kind, canonical_key, display_name, source_layer, attributes_json
               , snapshot_json
        FROM entities
        WHERE scan_id = ?
          AND (canonical_key LIKE ? OR display_name LIKE ? OR attributes_json LIKE ?)
        ORDER BY canonical_key
        LIMIT ?
        """,
        (scan_id, like_query, like_query, like_query, limit),
    ).fetchall()


def related(
    db_path: Path,
    entity: str,
    limit: int,
    include_snapshot: bool = False,
    scan_id: str | None = None,
) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        resolved_scan_id = resolve_scan_id(conn, scan_id, "Target") if scan_id else latest_scan_id(conn)
        entity_row_data = find_entity(conn, resolved_scan_id, entity)
        if not entity_row_data:
            raise SystemExit(f"Entity not found by id or canonical key: {entity}")

        entity_id = entity_row_data["id"]
        rows = conn.execute(
            """
            SELECT r.id, r.type, r.from_id, r.to_id, r.source_layer, r.derived, r.confidence,
                   r.attributes_json,
                   from_e.canonical_key AS from_key,
                   to_e.canonical_key AS to_key
            FROM relations r
            LEFT JOIN entities from_e ON from_e.scan_id = r.scan_id AND from_e.id = r.from_id
            LEFT JOIN entities to_e ON to_e.scan_id = r.scan_id AND to_e.id = r.to_id
            WHERE r.scan_id = ? AND (r.from_id = ? OR r.to_id = ?)
            ORDER BY r.type, to_key, from_key
            LIMIT ?
            """,
            (resolved_scan_id, entity_id, entity_id, limit),
        ).fetchall()

        return {
            "scan_id": resolved_scan_id,
            "entity": entity_row(entity_row_data, include_snapshot=include_snapshot),
            "relations": [
                {
                    "id": row["id"],
                    "type": row["type"],
                    "direction": "out" if row["from_id"] == entity_id else "in",
                    "from_id": row["from_id"],
                    "to_id": row["to_id"],
                    "from_key": row["from_key"],
                    "to_key": row["to_key"],
                    "source_layer": row["source_layer"],
                    "derived": bool(row["derived"]),
                    "confidence": row["confidence"],
                    "attributes": json.loads(row["attributes_json"]),
                }
                for row in rows
            ],
        }
    finally:
        conn.close()


def subgraph(
    db_path: Path,
    entity: str,
    depth: int,
    limit: int,
    relation_types: list[str] | None = None,
    scan_id: str | None = None,
) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        resolved_scan_id = resolve_scan_id(conn, scan_id, "Target") if scan_id else latest_scan_id(conn)
        root = find_entity(conn, resolved_scan_id, entity)
        if not root:
            raise SystemExit(f"Entity not found by id or canonical key: {entity}")

        relation_types = relation_types or []
        seen_node_ids = {root["id"]}
        frontier = {root["id"]}
        edge_rows: dict[str, sqlite3.Row] = {}

        for _ in range(max(depth, 0)):
            if not frontier or len(edge_rows) >= limit:
                break

            placeholders = ",".join("?" for _ in frontier)
            params: list[Any] = [resolved_scan_id, *sorted(frontier), *sorted(frontier)]
            type_clause = ""
            if relation_types:
                type_placeholders = ",".join("?" for _ in relation_types)
                type_clause = f" AND type IN ({type_placeholders})"
                params.extend(relation_types)

            rows = conn.execute(
                f"""
                SELECT id, type, from_id, to_id, source_layer, derived, confidence, attributes_json
                FROM relations
                WHERE scan_id = ?
                  AND (from_id IN ({placeholders}) OR to_id IN ({placeholders}))
                  {type_clause}
                ORDER BY type, id
                LIMIT ?
                """,
                (*params, max(limit - len(edge_rows), 0)),
            ).fetchall()

            next_frontier: set[str] = set()
            for row in rows:
                edge_rows[row["id"]] = row
                for node_id in (row["from_id"], row["to_id"]):
                    if node_id not in seen_node_ids:
                        seen_node_ids.add(node_id)
                        next_frontier.add(node_id)

            frontier = next_frontier

        nodes = fetch_entities_by_id(conn, resolved_scan_id, sorted(seen_node_ids))
        edges = fetch_edge_views(conn, resolved_scan_id, list(edge_rows.values()))

        return {
            "scan_id": resolved_scan_id,
            "root": entity_row(root),
            "depth": depth,
            "node_count": len(nodes),
            "edge_count": len(edges),
            "relation_types": relation_types,
            "nodes": nodes,
            "edges": edges,
        }
    finally:
        conn.close()


def graph_page(
    db_path: Path,
    entity: str,
    depth: int = 1,
    collection: str = "edges",
    limit: int = 100,
    cursor: str | None = None,
    relation_types: list[str] | None = None,
    graph_limit: int = 1000,
) -> dict[str, Any]:
    if collection not in {"nodes", "edges"}:
        raise SystemExit("Collection must be one of: nodes, edges.")
    relation_types = sorted(set(relation_types or []))
    page_limit = clamp_limit(limit, 1000)
    graph_limit = clamp_limit(graph_limit, 5000)
    cursor_value = decode_cursor(cursor)
    start_index = 0
    if cursor_value:
        if (
            cursor_value.get("entity") != entity
            or int(cursor_value.get("depth", depth)) != depth
            or cursor_value.get("collection") != collection
            or cursor_value.get("relation_types", []) != relation_types
            or int(cursor_value.get("graph_limit", graph_limit)) != graph_limit
        ):
            raise SystemExit("Cursor does not match the requested graph page.")
        start_index = int(cursor_value.get("index", -1)) + 1

    graph = subgraph(db_path, entity, depth, graph_limit, relation_types)
    items = graph[collection]
    page_items = items[start_index : start_index + page_limit]
    next_cursor = None
    if start_index + len(page_items) < len(items):
        next_cursor = encode_cursor(
            {
                "entity": entity,
                "depth": depth,
                "collection": collection,
                "relation_types": relation_types,
                "graph_limit": graph_limit,
                "index": start_index + len(page_items) - 1,
            }
        )
    result = paged_result(page_items, page_limit, next_cursor)
    result.update(
        {
            "root": graph["root"],
            "depth": depth,
            "relation_types": relation_types,
            "collection": collection,
            "graph_limit": graph_limit,
            "node_count": graph["node_count"],
            "edge_count": graph["edge_count"],
            "total_count": len(items),
            "start_index": start_index,
            "end_index": start_index + len(page_items) - 1 if page_items else start_index - 1,
        }
    )
    return result


def fetch_entities_by_id(conn: sqlite3.Connection, scan_id: str, entity_ids: list[str]) -> list[dict[str, Any]]:
    if not entity_ids:
        return []

    placeholders = ",".join("?" for _ in entity_ids)
    rows = conn.execute(
        f"""
        SELECT id, kind, canonical_key, display_name, source_layer, attributes_json, snapshot_json
        FROM entities
        WHERE scan_id = ? AND id IN ({placeholders})
        ORDER BY kind, canonical_key
        """,
        (scan_id, *entity_ids),
    ).fetchall()
    return [entity_row(row) for row in rows]


def fetch_edge_views(
    conn: sqlite3.Connection, scan_id: str, relation_rows: list[sqlite3.Row]
) -> list[dict[str, Any]]:
    if not relation_rows:
        return []

    relation_ids = [row["id"] for row in relation_rows]
    placeholders = ",".join("?" for _ in relation_ids)
    rows = conn.execute(
        f"""
        SELECT r.id, r.type, r.from_id, r.to_id, r.source_layer, r.derived, r.confidence,
               r.attributes_json,
               from_e.canonical_key AS from_key,
               to_e.canonical_key AS to_key
        FROM relations r
        LEFT JOIN entities from_e ON from_e.scan_id = r.scan_id AND from_e.id = r.from_id
        LEFT JOIN entities to_e ON to_e.scan_id = r.scan_id AND to_e.id = r.to_id
        WHERE r.scan_id = ? AND r.id IN ({placeholders})
        ORDER BY r.type, from_key, to_key
        """,
        (scan_id, *relation_ids),
    ).fetchall()

    return [
        {
            "id": row["id"],
            "type": row["type"],
            "from_id": row["from_id"],
            "to_id": row["to_id"],
            "from_key": row["from_key"],
            "to_key": row["to_key"],
            "source_layer": row["source_layer"],
            "derived": bool(row["derived"]),
            "confidence": row["confidence"],
            "attributes": json.loads(row["attributes_json"]),
        }
        for row in rows
    ]


def export_dot(
    db_path: Path,
    entity: str,
    depth: int,
    limit: int,
    relation_types: list[str] | None = None,
) -> str:
    graph = subgraph(db_path, entity, depth, limit, relation_types)
    return graph_to_dot(graph)


def graph_to_dot(graph: dict[str, Any]) -> str:
    node_by_id = {node["id"]: node for node in graph["nodes"]}
    lines = [
        "digraph UEPI {",
        "  graph [rankdir=LR];",
        "  node [shape=box, fontname=\"Segoe UI\"];",
        "  edge [fontname=\"Segoe UI\"];",
    ]

    for node in graph["nodes"]:
        label = node["display_name"] or node["canonical_key"]
        label = f"{label}\\n{node['kind']}"
        lines.append(f"  {dot_quote(node['id'])} [label={dot_quote(label)}];")

    for edge in graph["edges"]:
        if edge["from_id"] not in node_by_id or edge["to_id"] not in node_by_id:
            continue
        style = "dashed" if edge["derived"] else "solid"
        attributes = edge.get("attributes", {})
        detail = attributes.get("branch_label") or attributes.get("source_pin_name") or attributes.get("value_kind")
        edge_label = f"{edge['type']}\\n{detail}" if detail else edge["type"]
        lines.append(
            f"  {dot_quote(edge['from_id'])} -> {dot_quote(edge['to_id'])} "
            f"[label={dot_quote(edge_label)}, style={dot_quote(style)}];"
        )

    lines.append("}")
    return "\n".join(lines)


def mermaid_node_id(value: str) -> str:
    return "n_" + hashlib.sha1(value.encode("utf-8")).hexdigest()[:12]


def mermaid_label(value: Any) -> str:
    text = str(value).replace("\\", "\\\\").replace('"', '\\"')
    return text.replace("\r", " ").replace("\n", " ")[:160]


def graph_to_mermaid(graph: dict[str, Any]) -> str:
    id_map: dict[str, str] = {}
    lines = ["graph TD"]
    for node in graph["nodes"]:
        node_id = node["id"]
        rendered_id = mermaid_node_id(node_id)
        id_map[node_id] = rendered_id
        label = node["display_name"] or node["canonical_key"] or node_id
        lines.append(f'  {rendered_id}["{mermaid_label(label)}"]')

    for edge in graph["edges"]:
        if edge["from_id"] not in id_map or edge["to_id"] not in id_map:
            continue
        lines.append(f'  {id_map[edge["from_id"]]} -->|"{mermaid_label(edge["type"])}"| {id_map[edge["to_id"]]}')

    return "\n".join(lines)


def dot_quote(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def web_ui_index_path() -> Path:
    return Path(__file__).resolve().parents[2] / "Web" / "index.html"


def graphml_data(key: str, value: Any) -> str:
    if isinstance(value, (dict, list)):
        text = json_text(value)
    elif value is None:
        text = ""
    else:
        text = str(value)
    return f"      <data key=\"{html.escape(key, quote=True)}\">{html.escape(text)}</data>"


def graph_to_graphml(graph: dict[str, Any]) -> str:
    lines = [
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
        "<graphml xmlns=\"http://graphml.graphdrawing.org/xmlns\">",
        "  <key id=\"label\" for=\"node\" attr.name=\"label\" attr.type=\"string\"/>",
        "  <key id=\"kind\" for=\"node\" attr.name=\"kind\" attr.type=\"string\"/>",
        "  <key id=\"canonical_key\" for=\"node\" attr.name=\"canonical_key\" attr.type=\"string\"/>",
        "  <key id=\"source_layer\" for=\"node\" attr.name=\"source_layer\" attr.type=\"string\"/>",
        "  <key id=\"attributes\" for=\"node\" attr.name=\"attributes\" attr.type=\"string\"/>",
        "  <key id=\"type\" for=\"edge\" attr.name=\"type\" attr.type=\"string\"/>",
        "  <key id=\"derived\" for=\"edge\" attr.name=\"derived\" attr.type=\"boolean\"/>",
        "  <key id=\"confidence\" for=\"edge\" attr.name=\"confidence\" attr.type=\"double\"/>",
        "  <key id=\"edge_attributes\" for=\"edge\" attr.name=\"attributes\" attr.type=\"string\"/>",
        "  <graph id=\"UEPI\" edgedefault=\"directed\">",
    ]

    for node in graph["nodes"]:
        lines.append(f"    <node id=\"{html.escape(node['id'], quote=True)}\">")
        lines.append(graphml_data("label", node["display_name"] or node["canonical_key"]))
        lines.append(graphml_data("kind", node["kind"]))
        lines.append(graphml_data("canonical_key", node["canonical_key"]))
        lines.append(graphml_data("source_layer", node["source_layer"]))
        lines.append(graphml_data("attributes", node.get("attributes", {})))
        lines.append("    </node>")

    node_ids = {node["id"] for node in graph["nodes"]}
    for edge in graph["edges"]:
        if edge["from_id"] not in node_ids or edge["to_id"] not in node_ids:
            continue
        lines.append(
            f"    <edge id=\"{html.escape(edge['id'], quote=True)}\" "
            f"source=\"{html.escape(edge['from_id'], quote=True)}\" "
            f"target=\"{html.escape(edge['to_id'], quote=True)}\">"
        )
        lines.append(graphml_data("type", edge["type"]))
        lines.append(graphml_data("derived", str(bool(edge["derived"])).lower()))
        lines.append(graphml_data("confidence", edge["confidence"]))
        lines.append(graphml_data("edge_attributes", edge.get("attributes", {})))
        lines.append("    </edge>")

    lines.extend(["  </graph>", "</graphml>"])
    return "\n".join(lines)


def graph_to_cytoscape(graph: dict[str, Any]) -> dict[str, Any]:
    return {
        "format": "cytoscape",
        "root": graph["root"],
        "depth": graph["depth"],
        "relation_types": graph["relation_types"],
        "elements": {
            "nodes": [
                {
                    "data": {
                        "id": node["id"],
                        "label": node["display_name"] or node["canonical_key"],
                        "kind": node["kind"],
                        "canonical_key": node["canonical_key"],
                        "source_layer": node["source_layer"],
                        "attributes": node.get("attributes", {}),
                    }
                }
                for node in graph["nodes"]
            ],
            "edges": [
                {
                    "data": {
                        "id": edge["id"],
                        "source": edge["from_id"],
                        "target": edge["to_id"],
                        "type": edge["type"],
                        "from_key": edge["from_key"],
                        "to_key": edge["to_key"],
                        "source_layer": edge["source_layer"],
                        "derived": edge["derived"],
                        "confidence": edge["confidence"],
                        "attributes": edge.get("attributes", {}),
                    }
                }
                for edge in graph["edges"]
            ],
        },
    }


def graph_flat_records(graph: dict[str, Any]) -> list[dict[str, str]]:
    records: list[dict[str, str]] = []
    columns = [
        "record_type",
        "id",
        "kind",
        "canonical_key",
        "display_name",
        "relation_type",
        "from_id",
        "to_id",
        "from_key",
        "to_key",
        "source_layer",
        "derived",
        "confidence",
        "attributes_json",
    ]

    def normalized_record(values: dict[str, Any]) -> dict[str, str]:
        return {column: "" if values.get(column) is None else str(values.get(column, "")) for column in columns}

    for node in graph["nodes"]:
        records.append(
            normalized_record(
                {
                    "record_type": "node",
                    "id": node["id"],
                    "kind": node["kind"],
                    "canonical_key": node["canonical_key"],
                    "display_name": node["display_name"],
                    "source_layer": node["source_layer"],
                    "attributes_json": json_text(node.get("attributes", {})),
                }
            )
        )

    for edge in graph["edges"]:
        records.append(
            normalized_record(
                {
                    "record_type": "edge",
                    "id": edge["id"],
                    "relation_type": edge["type"],
                    "from_id": edge["from_id"],
                    "to_id": edge["to_id"],
                    "from_key": edge["from_key"],
                    "to_key": edge["to_key"],
                    "source_layer": edge["source_layer"],
                    "derived": str(bool(edge["derived"])).lower(),
                    "confidence": edge["confidence"],
                    "attributes_json": json_text(edge.get("attributes", {})),
                }
            )
        )

    return records


def graph_to_parquet_bytes(graph: dict[str, Any]) -> bytes:
    try:
        import pyarrow as pa  # type: ignore[import-not-found]
        import pyarrow.parquet as pq  # type: ignore[import-not-found]
    except ModuleNotFoundError as exc:
        raise SystemExit("Parquet export requires optional dependency: pyarrow.") from exc

    sink = pa.BufferOutputStream()
    pq.write_table(pa.Table.from_pylist(graph_flat_records(graph)), sink)
    return sink.getvalue().to_pybytes()


def export_graph(
    db_path: Path,
    entity: str,
    depth: int,
    limit: int,
    relation_types: list[str] | None,
    export_format: str,
) -> tuple[Any, str, bool]:
    graph = subgraph(db_path, entity, depth, limit, relation_types)
    normalized = export_format.lower().strip()
    if normalized == "json":
        return graph, "application/json; charset=utf-8", False
    if normalized == "dot":
        return graph_to_dot(graph), "text/vnd.graphviz; charset=utf-8", False
    if normalized == "mermaid":
        return graph_to_mermaid(graph), "text/vnd.mermaid; charset=utf-8", False
    if normalized == "graphml":
        return graph_to_graphml(graph), "application/graphml+xml; charset=utf-8", False
    if normalized == "cytoscape":
        return graph_to_cytoscape(graph), "application/json; charset=utf-8", False
    if normalized == "parquet":
        return graph_to_parquet_bytes(graph), "application/vnd.apache.parquet", True
    raise SystemExit(f"Unsupported graph export format: {export_format}")


def split_relation_filters(values: list[str]) -> list[str]:
    filters: list[str] = []
    for value in values:
        filters.extend(part.strip() for part in value.split(",") if part.strip())
    return filters


def parse_graph_dsl(expression: str) -> dict[str, Any]:
    tokens = shlex.split(expression)
    if not tokens:
        raise SystemExit("Graph DSL query is empty.")

    parsed: dict[str, Any] = {
        "entity": "",
        "depth": 1,
        "limit": 200,
        "relation_types": [],
    }
    index = 0
    while index < len(tokens):
        token = tokens[index]
        lower = token.lower()
        if lower in {"match", "graph"}:
            index += 1
            continue

        if "=" in token:
            key, value = token.split("=", 1)
            index += 1
        else:
            key = lower
            if key not in {"from", "root", "entity", "depth", "limit", "relation", "relation_type", "type"}:
                if not parsed["entity"]:
                    parsed["entity"] = token
                    index += 1
                    continue
                raise SystemExit(f"Unsupported graph DSL token: {token}")
            if index + 1 >= len(tokens):
                raise SystemExit(f"Graph DSL token requires a value: {token}")
            value = tokens[index + 1]
            index += 2

        key = key.lower().replace("-", "_")
        if key in {"from", "root", "entity"}:
            parsed["entity"] = value
        elif key == "depth":
            parsed["depth"] = int(value)
        elif key == "limit":
            parsed["limit"] = int(value)
        elif key in {"relation", "relation_type", "type"}:
            parsed["relation_types"].extend(split_relation_filters([value]))
        else:
            raise SystemExit(f"Unsupported graph DSL key: {key}")

    if not parsed["entity"]:
        raise SystemExit("Graph DSL query must include a root entity.")
    return parsed


def graph_query(db_path: Path, expression: str) -> dict[str, Any]:
    parsed = parse_graph_dsl(expression)
    graph = subgraph(
        db_path,
        parsed["entity"],
        parsed["depth"],
        parsed["limit"],
        parsed["relation_types"],
    )
    return {
        "query": expression,
        "parsed": parsed,
        "graph": graph,
    }


def artifact_range(
    db_path: Path,
    scan_id: str | None = None,
    offset: int = 0,
    length: int = 4096,
    encoding: str = "text",
) -> dict[str, Any]:
    if offset < 0:
        raise SystemExit("Artifact range offset must be non-negative.")
    if length < 0:
        raise SystemExit("Artifact range length must be non-negative.")

    read_length = min(length, ARTIFACT_RANGE_MAX_BYTES)
    conn = connect(db_path)
    try:
        resolved_scan_id = resolve_scan_id(conn, scan_id, "Target") if scan_id else latest_scan_id(conn)
        row = conn.execute("SELECT * FROM scans WHERE scan_id = ?", (resolved_scan_id,)).fetchone()
        if not row:
            raise SystemExit(f"Scan not found: {resolved_scan_id}")
        path = require_sandbox_path(Path(row["scan_path"]), "Scan artifact", db_path)
        size = path.stat().st_size
        with path.open("rb") as handle:
            handle.seek(min(offset, size))
            data = handle.read(read_length)

        normalized_encoding = encoding.lower().strip()
        if normalized_encoding == "base64":
            payload = base64.b64encode(data).decode("ascii")
        elif normalized_encoding == "text":
            payload = data.decode("utf-8-sig" if offset == 0 else "utf-8", errors="replace")
        else:
            raise SystemExit(f"Unsupported artifact range encoding: {encoding}")

        return {
            "scan": scan_meta(conn, resolved_scan_id),
            "artifact": "scan_json",
            "path": str(path),
            "size": size,
            "offset": offset,
            "requested_length": length,
            "max_length": ARTIFACT_RANGE_MAX_BYTES,
            "length": len(data),
            "encoding": normalized_encoding,
            "eof": offset + len(data) >= size,
            "data": payload,
        }
    finally:
        conn.close()


def markdown_table_row(values: list[Any]) -> str:
    return "| " + " | ".join(str(value).replace("|", "\\|") for value in values) + " |"


def markdown_report(db_path: Path, scan_id: str | None = None, limit: int = 20) -> str:
    conn = connect(db_path)
    try:
        resolved_scan_id = resolve_scan_id(conn, scan_id, "Target") if scan_id else latest_scan_id(conn)
        summary = summary_dict(conn, resolved_scan_id)
        entity_kinds = conn.execute(
            """
            SELECT kind, COUNT(*) AS count
            FROM entities
            WHERE scan_id = ?
            GROUP BY kind
            ORDER BY count DESC, kind ASC
            LIMIT ?
            """,
            (resolved_scan_id, max(limit, 0)),
        ).fetchall()
        relation_types = conn.execute(
            """
            SELECT type, COUNT(*) AS count
            FROM relations
            WHERE scan_id = ?
            GROUP BY type
            ORDER BY count DESC, type ASC
            LIMIT ?
            """,
            (resolved_scan_id, max(limit, 0)),
        ).fetchall()
        diagnostics = conn.execute(
            """
            SELECT severity, code, COUNT(*) AS count
            FROM diagnostics
            WHERE scan_id = ?
            GROUP BY severity, code
            ORDER BY count DESC, severity ASC, code ASC
            LIMIT ?
            """,
            (resolved_scan_id, max(limit, 0)),
        ).fetchall()

        lines = [
            "# UEPI Scan Report",
            "",
            f"- Scan: `{summary['scan_id']}`",
            f"- Project: {summary['project_name']}",
            f"- Engine: {summary['engine_version']}",
            f"- Entities: {summary['entity_count']}",
            f"- Relations: {summary['relation_count']}",
            f"- Diagnostics: {summary['diagnostic_count']}",
            f"- FTS5: {'available' if summary['fts5_available'] else 'unavailable'}",
            f"- Scan Artifact: {summary['stale']['reason']}",
            "",
            "## Entity Kinds",
            "",
            markdown_table_row(["Kind", "Count"]),
            markdown_table_row(["---", "---:"]),
        ]
        lines.extend(markdown_table_row([row["kind"], row["count"]]) for row in entity_kinds)
        lines.extend(
            [
                "",
                "## Relation Types",
                "",
                markdown_table_row(["Type", "Count"]),
                markdown_table_row(["---", "---:"]),
            ]
        )
        lines.extend(markdown_table_row([row["type"], row["count"]]) for row in relation_types)
        lines.extend(
            [
                "",
                "## Diagnostics",
                "",
                markdown_table_row(["Severity", "Code", "Count"]),
                markdown_table_row(["---", "---", "---:"]),
            ]
        )
        if diagnostics:
            lines.extend(markdown_table_row([row["severity"], row["code"], row["count"]]) for row in diagnostics)
        else:
            lines.append(markdown_table_row(["info", "none", 0]))
        lines.append("")
        return "\n".join(lines)
    finally:
        conn.close()


def api_document() -> dict[str, Any]:
    return {
        "openapi": "3.0.0",
        "info": {
            "title": "UE Project Intelligence Local Query API",
            "version": str(SCHEMA_VERSION),
        },
        "paths": {
            "/v1/health": {"get": {"summary": "Check daemon health."}},
            "/v1/summary": {"get": {"summary": "Return the latest scan summary."}},
            "/v1/scans": {"get": {"summary": "List ingested scans with cursor pagination."}},
            "/v1/entities": {"get": {"summary": "List entities with cursor pagination."}},
            "/v1/relations": {"get": {"summary": "List relations with cursor pagination."}},
            "/v1/search": {"get": {"summary": "Search entities in the latest scan."}},
            "/v1/related": {"get": {"summary": "Return direct relations for an entity."}},
            "/v1/subgraph": {"get": {"summary": "Return a bounded relation subgraph."}},
            "/v1/graph-page": {"get": {"summary": "Page nodes or edges from a bounded relation subgraph."}},
            "/v1/graph-query": {"get": {"summary": "Run a compact graph DSL query."}},
            "/v1/export-dot": {"get": {"summary": "Export a bounded subgraph as DOT."}},
            "/v1/export-graph": {"get": {"summary": "Export a bounded subgraph as JSON, DOT, Mermaid, GraphML, Cytoscape JSON, or optional Parquet."}},
            "/v1/artifact-range": {"get": {"summary": "Read a byte range from an ingested scan artifact."}},
            "/v1/report": {"get": {"summary": "Render a Markdown scan report."}},
            "/v1/integrity": {"get": {"summary": "Run SQLite integrity and foreign-key checks."}},
            "/v1/recover": {"post": {"summary": "Checkpoint WAL, optimize SQLite, and run integrity checks."}},
            "/v1/diff": {"get": {"summary": "Compare two ingested scans."}},
            "/v1/stale": {"get": {"summary": "Check scan and package staleness."}},
            "/v1/history": {"get": {"summary": "Return asset revision history."}},
            "/v1/ingest": {"post": {"summary": "Ingest an existing scan JSON artifact."}},
            "/v1/openapi": {"get": {"summary": "Return this API document."}},
            "/v1/ui": {"get": {"summary": "Serve the local UEPI web UI."}},
            "/v1/protocol": {"get": {"summary": "Return the UEPI worker agent protocol document."}},
            "/v1/workers": {"get": {"summary": "List worker sessions."}},
            "/v1/workers/register": {"post": {"summary": "Register a UE worker and issue a session token."}},
            "/v1/workers/heartbeat": {"post": {"summary": "Refresh a worker session lease."}},
            "/v1/jobs": {
                "get": {"summary": "List worker jobs."},
                "post": {"summary": "Submit a worker job."},
            },
            "/v1/jobs/{job_id}": {"get": {"summary": "Return a worker job with event history."}},
            "/v1/jobs/poll": {"post": {"summary": "Long-poll and lease queued jobs for a worker session."}},
            "/v1/jobs/update": {"post": {"summary": "Update a leased job state/result."}},
            "/v1/jobs/cancel": {"post": {"summary": "Cancel a queued or active job."}},
            "/v1/jobs/chunk": {"post": {"summary": "Upload a base64 artifact chunk for a leased job."}},
            "/v1/jobs/recover": {"post": {"summary": "Recover stale worker sessions and expired job leases."}},
            "/v1/source-index": {"post": {"summary": "Index C++/Config source symbols and Unreal path references."}},
            "/v1/source-symbols": {"get": {"summary": "List indexed Unreal reflection and C++ symbols."}},
            "/v1/source-references": {"get": {"summary": "List indexed C++/Config Unreal path references."}},
            "/v1/source-search": {"get": {"summary": "Search indexed source symbols and references."}},
            "/v1/blueprint-cpp-links": {"get": {"summary": "Link Blueprint call-function relations to indexed C++ source symbols."}},
            "/v1/config-values": {"get": {"summary": "Return indexed Unreal config rows and effective values."}},
            "/v1/animation-query": {"get": {"summary": "Query animation-domain manifest entities from an ingested scan."}},
            "/v1/data-query": {"get": {"summary": "Query data-domain manifest entities from an ingested scan."}},
            "/v1/data-page": {"get": {"summary": "Page a rows/columns/entries/bundles collection inside a data snapshot."}},
            "/v1/cinematics-key-page": {"get": {"summary": "Page Sequencer key time rows from a LevelSequence snapshot."}},
        },
        "x-uepi": {
            "cursor_endpoints": ["/v1/scans", "/v1/entities", "/v1/relations", "/v1/data-page", "/v1/graph-page", "/v1/cinematics-key-page"],
            "graph_export_formats": ["json", "dot", "mermaid", "graphml", "cytoscape", "parquet"],
            "graph_dsl": "from <entity> depth <n> relation <type[,type]> limit <n>",
            "artifact_range_max_bytes": ARTIFACT_RANGE_MAX_BYTES,
            "agent_protocol": agent_protocol_document(),
        },
    }


def first_query_value(query: dict[str, list[str]], key: str, default: str | None = None) -> str | None:
    values = query.get(key)
    if not values:
        return default
    return values[0]


def int_query_value(query: dict[str, list[str]], key: str, default: int) -> int:
    value = first_query_value(query, key)
    if value is None or value == "":
        return default
    try:
        return int(value)
    except ValueError as exc:
        raise ValueError(f"Query parameter '{key}' must be an integer.") from exc


def bool_query_value(query: dict[str, list[str]], key: str, default: bool = False) -> bool:
    value = first_query_value(query, key)
    if value is None or value == "":
        return default
    return value.lower() in {"1", "true", "yes", "on"}


def load_server_token(token: str | None, token_file: Path | None, db_path: Path) -> str:
    if token_file:
        token_path = require_sandbox_path(token_file, "Token file", db_path)
        return token_path.read_text(encoding="utf-8").strip()
    if token == "auto":
        token_value = secrets.token_urlsafe(32)
        token_path = db_path.parent / "uepi_http_token.txt"
        token_path.parent.mkdir(parents=True, exist_ok=True)
        token_path.write_text(token_value, encoding="utf-8")
        return token_value
    return token or ""


def api_path(path: str) -> str:
    normalized = path.rstrip("/") or "/"
    if normalized == "/v1":
        return "/"
    if normalized.startswith("/v1/"):
        return normalized[3:]
    return normalized


def serve(db_path: Path, host: str, port: int, token: str = "") -> None:
    class UEPIRequestHandler(BaseHTTPRequestHandler):
        server_version = "UEPIHTTP/0.1"

        def is_authorized(self, path: str, query: dict[str, list[str]]) -> bool:
            if not token:
                return True
            if path in {"/health", "/ui", "/ui/index.html"}:
                return True
            header_token = self.headers.get("X-UEPI-Token", "")
            auth_header = self.headers.get("Authorization", "")
            bearer = auth_header.removeprefix("Bearer ").strip() if auth_header.startswith("Bearer ") else ""
            query_token = first_query_value(query, "token", "") or ""
            return token in {header_token, bearer, query_token}

        def do_GET(self) -> None:
            parsed = urlparse(self.path)
            path = api_path(parsed.path)
            query = parse_qs(parsed.query)

            try:
                if not self.is_authorized(path, query):
                    self.write_json({"error": "Unauthorized"}, status=401)
                    return

                if path == "/":
                    self.write_json(
                        {
                            "service": "uepi_daemon",
                            "version": SCHEMA_VERSION,
                            "endpoints": [
                                "/health",
                                "/summary",
                                "/scans",
                                "/entities",
                                "/relations",
                                "/search",
                                "/related",
                                "/subgraph",
                                "/graph-query",
                                "/export-dot",
                                "/export-graph",
                                "/artifact-range",
                                "/report",
                                "/integrity",
                                "/recover",
                                "/diff",
                                "/stale",
                                "/history",
                                "/openapi",
                                "/ui",
                                "/protocol",
                                "/workers",
                                "/jobs",
                                "/source-symbols",
                                "/source-references",
                                "/source-search",
                                "/blueprint-cpp-links",
                                "/animation-query",
                                "/data-query",
                                "/data-page",
                                "/cinematics-key-page",
                            ],
                        }
                    )
                    return

                if path == "/health":
                    self.write_json({"ok": True, "db_path": str(db_path)})
                    return

                if path == "/openapi":
                    self.write_json(api_document())
                    return

                if path == "/protocol":
                    self.write_json(agent_protocol_document())
                    return

                if path in {"/ui", "/ui/index.html"}:
                    ui_path = web_ui_index_path()
                    if not ui_path.exists():
                        self.write_json({"error": f"Web UI not found: {ui_path}"}, status=404)
                        return
                    self.write_bytes(ui_path.read_bytes(), "text/html; charset=utf-8")
                    return

                if path == "/summary":
                    conn = connect(db_path)
                    try:
                        self.write_json(summary_dict(conn))
                    finally:
                        conn.close()
                    return

                if path == "/scans":
                    self.write_json(
                        list_scans(
                            db_path,
                            int_query_value(query, "limit", 50),
                            first_query_value(query, "cursor"),
                        )
                    )
                    return

                if path == "/entities":
                    self.write_json(
                        list_entities(
                            db_path,
                            first_query_value(query, "scan"),
                            first_query_value(query, "kind"),
                            int_query_value(query, "limit", 100),
                            first_query_value(query, "cursor"),
                            bool_query_value(query, "include_snapshot"),
                        )
                    )
                    return

                if path == "/relations":
                    self.write_json(
                        list_relations(
                            db_path,
                            first_query_value(query, "scan"),
                            first_query_value(query, "relation_type") or first_query_value(query, "type"),
                            int_query_value(query, "limit", 100),
                            first_query_value(query, "cursor"),
                        )
                    )
                    return

                if path == "/search":
                    search_query = first_query_value(query, "q") or first_query_value(query, "query")
                    if not search_query:
                        raise ValueError("Missing required query parameter: q")
                    self.write_json(
                        search(
                            db_path,
                            search_query,
                            int_query_value(query, "limit", 20),
                            bool_query_value(query, "include_snapshot"),
                        )
                    )
                    return

                if path == "/related":
                    entity = first_query_value(query, "entity")
                    if not entity:
                        raise ValueError("Missing required query parameter: entity")
                    self.write_json(
                        related(
                            db_path,
                            entity,
                            int_query_value(query, "limit", 50),
                            bool_query_value(query, "include_snapshot"),
                        )
                    )
                    return

                if path == "/subgraph":
                    entity = first_query_value(query, "entity")
                    if not entity:
                        raise ValueError("Missing required query parameter: entity")
                    self.write_json(
                        subgraph(
                            db_path,
                            entity,
                            int_query_value(query, "depth", 1),
                            int_query_value(query, "limit", 200),
                            query.get("relation_type", []),
                        )
                    )
                    return

                if path == "/graph-page":
                    entity = first_query_value(query, "entity")
                    if not entity:
                        raise ValueError("Missing required query parameter: entity")
                    self.write_json(
                        graph_page(
                            db_path,
                            entity,
                            int_query_value(query, "depth", 1),
                            first_query_value(query, "collection", "edges") or "edges",
                            int_query_value(query, "limit", 100),
                            first_query_value(query, "cursor"),
                            query.get("relation_type", []),
                            int_query_value(query, "graph_limit", 1000),
                        )
                    )
                    return

                if path == "/graph-query":
                    expression = first_query_value(query, "q") or first_query_value(query, "query") or first_query_value(query, "dsl")
                    if not expression:
                        raise ValueError("Missing required query parameter: q")
                    self.write_json(graph_query(db_path, expression))
                    return

                if path == "/export-dot":
                    entity = first_query_value(query, "entity")
                    if not entity:
                        raise ValueError("Missing required query parameter: entity")
                    dot = export_dot(
                        db_path,
                        entity,
                        int_query_value(query, "depth", 1),
                        int_query_value(query, "limit", 200),
                        query.get("relation_type", []),
                    )
                    self.write_text(dot, "text/vnd.graphviz; charset=utf-8")
                    return

                if path == "/export-graph":
                    entity = first_query_value(query, "entity")
                    if not entity:
                        raise ValueError("Missing required query parameter: entity")
                    payload, content_type, is_binary = export_graph(
                        db_path,
                        entity,
                        int_query_value(query, "depth", 1),
                        int_query_value(query, "limit", 200),
                        split_relation_filters(query.get("relation_type", []) + query.get("type", [])),
                        first_query_value(query, "format", "json") or "json",
                    )
                    if is_binary:
                        self.write_bytes(payload, content_type)
                    elif isinstance(payload, (dict, list)):
                        self.write_json(payload)
                    else:
                        self.write_text(payload, content_type)
                    return

                if path == "/artifact-range":
                    self.write_json(
                        artifact_range(
                            db_path,
                            first_query_value(query, "scan"),
                            int_query_value(query, "offset", 0),
                            int_query_value(query, "length", 4096),
                            first_query_value(query, "encoding", "text") or "text",
                        )
                    )
                    return

                if path == "/report":
                    markdown = markdown_report(
                        db_path,
                        first_query_value(query, "scan"),
                        int_query_value(query, "limit", 20),
                    )
                    self.write_text(markdown, "text/markdown; charset=utf-8")
                    return

                if path == "/integrity":
                    self.write_json(db_integrity(db_path))
                    return

                if path == "/diff":
                    self.write_json(
                        scan_diff(
                            db_path,
                            first_query_value(query, "base"),
                            first_query_value(query, "compare"),
                            int_query_value(query, "limit", 100),
                        )
                    )
                    return

                if path == "/stale":
                    self.write_json(
                        staleness(
                            db_path,
                            first_query_value(query, "scan"),
                            int_query_value(query, "limit", 100),
                        )
                    )
                    return

                if path == "/history":
                    self.write_json(
                        asset_history(
                            db_path,
                            first_query_value(query, "asset"),
                            int_query_value(query, "limit", 50),
                        )
                    )
                    return

                if path == "/workers":
                    self.write_json(
                        list_worker_sessions(
                            db_path,
                            first_query_value(query, "status"),
                            int_query_value(query, "limit", 50),
                        )
                    )
                    return

                if path == "/jobs":
                    self.write_json(
                        list_jobs(
                            db_path,
                            first_query_value(query, "state"),
                            int_query_value(query, "limit", 50),
                            bool_query_value(query, "include_events"),
                        )
                    )
                    return

                if path.startswith("/jobs/") and path not in {
                    "/jobs/poll",
                    "/jobs/update",
                    "/jobs/cancel",
                    "/jobs/chunk",
                    "/jobs/recover",
                }:
                    job_id = path.removeprefix("/jobs/").strip("/")
                    if not job_id:
                        raise ValueError("Missing job id.")
                    self.write_json(get_job(db_path, job_id, include_events=bool_query_value(query, "include_events", True)))
                    return

                if path == "/source-symbols":
                    self.write_json(
                        source_symbols(
                            db_path,
                            first_query_value(query, "project"),
                            first_query_value(query, "kind"),
                            first_query_value(query, "q") or first_query_value(query, "query"),
                            int_query_value(query, "limit", 100),
                        )
                    )
                    return

                if path == "/source-references":
                    self.write_json(
                        source_references(
                            db_path,
                            first_query_value(query, "project"),
                            first_query_value(query, "kind"),
                            first_query_value(query, "q") or first_query_value(query, "query"),
                            int_query_value(query, "limit", 100),
                        )
                    )
                    return

                if path == "/source-search":
                    search_query = first_query_value(query, "q") or first_query_value(query, "query")
                    if not search_query:
                        raise ValueError("Missing required query parameter: q")
                    self.write_json(
                        source_search(
                            db_path,
                            search_query,
                            first_query_value(query, "project"),
                            int_query_value(query, "limit", 50),
                        )
                    )
                    return

                if path == "/blueprint-cpp-links":
                    self.write_json(
                        blueprint_cpp_links(
                            db_path,
                            first_query_value(query, "scan"),
                            first_query_value(query, "project"),
                            first_query_value(query, "q") or first_query_value(query, "query"),
                            int_query_value(query, "limit", 100),
                        )
                    )
                    return

                if path == "/config-values":
                    self.write_json(
                        config_values(
                            db_path,
                            first_query_value(query, "project"),
                            first_query_value(query, "section"),
                            first_query_value(query, "key"),
                            first_query_value(query, "q") or first_query_value(query, "query"),
                            int_query_value(query, "limit", 100),
                            bool_query_value(query, "include_history"),
                        )
                    )
                    return

                if path == "/animation-query":
                    self.write_json(
                        animation_query(
                            db_path,
                            first_query_value(query, "scan"),
                            first_query_value(query, "asset"),
                            int_query_value(query, "limit", 100),
                            bool_query_value(query, "include_snapshot"),
                        )
                    )
                    return

                if path == "/data-query":
                    self.write_json(
                        data_query(
                            db_path,
                            first_query_value(query, "scan"),
                            first_query_value(query, "asset"),
                            int_query_value(query, "limit", 100),
                            bool_query_value(query, "include_snapshot"),
                        )
                    )
                    return

                if path == "/data-page":
                    entity = first_query_value(query, "entity")
                    if not entity:
                        raise SystemExit("Missing required query parameter: entity")
                    self.write_json(
                        data_snapshot_page(
                            db_path,
                            entity,
                            first_query_value(query, "collection", "rows") or "rows",
                            first_query_value(query, "scan"),
                            int_query_value(query, "limit", 100),
                            first_query_value(query, "cursor"),
                            bool_query_value(query, "include_artifact"),
                        )
                    )
                    return

                if path == "/cinematics-key-page":
                    entity = first_query_value(query, "entity")
                    if not entity:
                        raise SystemExit("Missing required query parameter: entity")
                    self.write_json(
                        cinematics_key_page(
                            db_path,
                            entity,
                            first_query_value(query, "scan"),
                            int_query_value(query, "limit", 100),
                            first_query_value(query, "cursor"),
                            first_query_value(query, "section"),
                            first_query_value(query, "channel"),
                            bool_query_value(query, "include_artifact"),
                        )
                    )
                    return

                self.write_json({"error": f"Unknown endpoint: {path}"}, status=404)
            except (SystemExit, ValueError) as exc:
                self.write_json({"error": str(exc)}, status=400)
            except Exception as exc:  # pragma: no cover - defensive boundary for long-running daemon.
                self.write_json({"error": type(exc).__name__, "message": str(exc)}, status=500)

        def do_OPTIONS(self) -> None:
            self.send_response(204)
            self.write_cors_headers()
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type, X-UEPI-Token, X-UEPI-Session-Token")
            self.end_headers()

        def do_POST(self) -> None:
            parsed = urlparse(self.path)
            path = api_path(parsed.path)
            query = parse_qs(parsed.query)

            try:
                if not self.is_authorized(path, query):
                    self.write_json({"error": "Unauthorized"}, status=401)
                    return

                body = self.rfile.read(int(self.headers.get("Content-Length", "0") or "0"))
                payload = json.loads(body.decode("utf-8")) if body else {}
                if not isinstance(payload, dict):
                    raise ValueError("POST body must be a JSON object.")
                session_id = payload.get("session_id") or first_query_value(query, "session_id") or ""
                session_token = (
                    payload.get("session_token")
                    or self.headers.get("X-UEPI-Session-Token", "")
                    or first_query_value(query, "session_token")
                    or ""
                )

                if path == "/recover":
                    self.write_json(db_recover(db_path))
                    return

                if path == "/workers/register":
                    self.write_json(
                        register_worker(
                            db_path,
                            str(payload.get("worker_id") or ""),
                            str(payload.get("worker_type") or "editor"),
                            json_object(payload.get("capabilities")),
                            int(payload.get("ttl_seconds", DEFAULT_WORKER_TTL_SECONDS)),
                        )
                    )
                    return

                if path == "/workers/heartbeat":
                    if not session_id or not session_token:
                        raise ValueError("Missing session_id or session token.")
                    self.write_json(
                        worker_heartbeat(
                            db_path,
                            session_id,
                            session_token,
                            str(payload.get("status") or "online"),
                            payload.get("capabilities") if isinstance(payload.get("capabilities"), dict) else None,
                            int(payload.get("ttl_seconds", DEFAULT_WORKER_TTL_SECONDS)),
                        )
                    )
                    return

                if path == "/jobs":
                    self.write_json(
                        submit_job(
                            db_path,
                            str(payload.get("job_type") or payload.get("type") or ""),
                            json_object(payload.get("request")),
                            int(payload.get("priority", 0)),
                            int(payload.get("timeout_seconds", DEFAULT_JOB_TIMEOUT_SECONDS)),
                            int(payload.get("max_retries", 1)),
                            payload.get("trace_id"),
                        )
                    )
                    return

                if path == "/jobs/poll":
                    if not session_id or not session_token:
                        raise ValueError("Missing session_id or session token.")
                    self.write_json(
                        poll_jobs(
                            db_path,
                            session_id,
                            session_token,
                            int(payload.get("limit", 1)),
                            int(payload.get("wait_seconds", DEFAULT_LONG_POLL_SECONDS)),
                        )
                    )
                    return

                if path == "/jobs/update":
                    if not session_id or not session_token:
                        raise ValueError("Missing session_id or session token.")
                    self.write_json(
                        update_job(
                            db_path,
                            session_id,
                            session_token,
                            str(payload.get("job_id") or ""),
                            str(payload.get("state") or ""),
                            payload.get("result") if isinstance(payload.get("result"), dict) else None,
                            payload.get("error") if isinstance(payload.get("error"), dict) else None,
                            payload.get("artifacts") if isinstance(payload.get("artifacts"), list) else None,
                        )
                    )
                    return

                if path == "/jobs/cancel":
                    self.write_json(cancel_job(db_path, str(payload.get("job_id") or ""), str(payload.get("reason") or "cancelled by client")))
                    return

                if path == "/jobs/chunk":
                    if not session_id or not session_token:
                        raise ValueError("Missing session_id or session token.")
                    self.write_json(
                        upload_job_chunk(
                            db_path,
                            session_id,
                            session_token,
                            str(payload.get("job_id") or ""),
                            str(payload.get("artifact_id") or "artifact"),
                            int(payload.get("ordinal", 0)),
                            str(payload.get("data_base64") or ""),
                            payload.get("sha256"),
                        )
                    )
                    return

                if path == "/jobs/recover":
                    self.write_json(recover_jobs(db_path))
                    return

                if path == "/source-index":
                    self.write_json(
                        index_source(
                            db_path,
                            payload.get("project") or payload.get("project_root") or first_query_value(query, "project"),
                            payload.get("compile_database") or first_query_value(query, "compile_database"),
                        )
                    )
                    return

                if path == "/ingest":
                    scan_path = (
                        payload.get("scan")
                        or payload.get("scan_path")
                        or first_query_value(query, "scan")
                        or first_query_value(query, "scan_path")
                    )
                    if not scan_path:
                        raise ValueError("Missing required scan path.")

                    self.write_json(ingest(Path(scan_path), db_path))
                    return

                self.write_json({"error": f"Unknown endpoint: {path}"}, status=404)
            except (json.JSONDecodeError, SystemExit, ValueError) as exc:
                self.write_json({"error": str(exc)}, status=400)
            except Exception as exc:  # pragma: no cover - defensive boundary for long-running daemon.
                self.write_json({"error": type(exc).__name__, "message": str(exc)}, status=500)

        def log_message(self, format: str, *args: Any) -> None:
            print(f"{self.address_string()} - {format % args}", file=sys.stderr)

        def write_json(self, value: Any, status: int = 200) -> None:
            data = json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.write_cors_headers()
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def write_bytes(self, value: bytes, content_type: str, status: int = 200) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.write_cors_headers()
            self.send_header("Content-Length", str(len(value)))
            self.end_headers()
            self.wfile.write(value)

        def write_text(self, value: str, content_type: str, status: int = 200) -> None:
            data = value.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.write_cors_headers()
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def write_cors_headers(self) -> None:
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Private-Network", "true")

    class UEPIHTTPServer(ThreadingHTTPServer):
        allow_reuse_address = True

    server = UEPIHTTPServer((host, port), UEPIRequestHandler)
    auth_text = " with token auth" if token else ""
    print(f"UEPI daemon serving http://{host}:{server.server_port} with db {db_path}{auth_text}", flush=True)
    server.serve_forever()


def find_entity(conn: sqlite3.Connection, scan_id: str, entity: str) -> sqlite3.Row | None:
    return conn.execute(
        """
        SELECT id, kind, canonical_key, display_name, source_layer, attributes_json
               , snapshot_json
        FROM entities
        WHERE scan_id = ? AND (id = ? OR canonical_key = ?)
        LIMIT 1
        """,
        (scan_id, entity, entity),
    ).fetchone()


def entity_row(row: sqlite3.Row, include_snapshot: bool = False) -> dict[str, Any]:
    result = {
        "id": row["id"],
        "kind": row["kind"],
        "canonical_key": row["canonical_key"],
        "display_name": row["display_name"],
        "source_layer": row["source_layer"],
        "attributes": json.loads(row["attributes_json"]),
    }
    if include_snapshot:
        result["snapshot"] = json.loads(row["snapshot_json"]) if "snapshot_json" in row.keys() else {}
    return result


def print_json(value: Any) -> None:
    print(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True))


def parse_json_arg(text: str | None, default: Any) -> Any:
    if text is None or text == "":
        return default
    value = json.loads(text)
    if isinstance(default, dict) and not isinstance(value, dict):
        raise SystemExit("Expected a JSON object.")
    if isinstance(default, list) and not isinstance(value, list):
        raise SystemExit("Expected a JSON array.")
    return value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="UEPI local SQLite ingest and query service.")
    parser.add_argument("--db", type=Path, default=Path(".uepi/index.sqlite3"), help="SQLite database path.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    ingest_parser = subparsers.add_parser("ingest", help="Ingest a UEPI scan JSON file.")
    ingest_parser.add_argument("--scan", type=Path, required=True, help="Path to last_scan.json.")

    subparsers.add_parser("summary", help="Print the latest scan summary.")
    subparsers.add_parser("health", help="Check database availability.")
    subparsers.add_parser("protocol", help="Print the UEPI worker agent protocol document.")

    worker_register_parser = subparsers.add_parser("worker-register", help="Register a UE worker session.")
    worker_register_parser.add_argument("--worker-id", default="")
    worker_register_parser.add_argument("--worker-type", default="editor")
    worker_register_parser.add_argument("--capabilities-json", default="{}")
    worker_register_parser.add_argument("--ttl-seconds", type=int, default=DEFAULT_WORKER_TTL_SECONDS)

    worker_heartbeat_parser = subparsers.add_parser("worker-heartbeat", help="Refresh a worker session lease.")
    worker_heartbeat_parser.add_argument("--session-id", required=True)
    worker_heartbeat_parser.add_argument("--session-token", required=True)
    worker_heartbeat_parser.add_argument("--status", default="online")
    worker_heartbeat_parser.add_argument("--capabilities-json")
    worker_heartbeat_parser.add_argument("--ttl-seconds", type=int, default=DEFAULT_WORKER_TTL_SECONDS)

    workers_parser = subparsers.add_parser("workers", help="List worker sessions.")
    workers_parser.add_argument("--status")
    workers_parser.add_argument("--limit", type=int, default=50)

    job_submit_parser = subparsers.add_parser("job-submit", help="Submit a worker job.")
    job_submit_parser.add_argument("--type", dest="job_type", required=True)
    job_submit_parser.add_argument("--request-json", default="{}")
    job_submit_parser.add_argument("--priority", type=int, default=0)
    job_submit_parser.add_argument("--timeout-seconds", type=int, default=DEFAULT_JOB_TIMEOUT_SECONDS)
    job_submit_parser.add_argument("--max-retries", type=int, default=1)
    job_submit_parser.add_argument("--trace-id")

    jobs_parser = subparsers.add_parser("jobs", help="List worker jobs.")
    jobs_parser.add_argument("--state")
    jobs_parser.add_argument("--limit", type=int, default=50)
    jobs_parser.add_argument("--include-events", action="store_true")

    job_parser = subparsers.add_parser("job", help="Show one worker job.")
    job_parser.add_argument("job_id")
    job_parser.add_argument("--no-events", action="store_true")

    job_poll_parser = subparsers.add_parser("job-poll", help="Long-poll and lease queued jobs.")
    job_poll_parser.add_argument("--session-id", required=True)
    job_poll_parser.add_argument("--session-token", required=True)
    job_poll_parser.add_argument("--limit", type=int, default=1)
    job_poll_parser.add_argument("--wait-seconds", type=int, default=DEFAULT_LONG_POLL_SECONDS)

    job_update_parser = subparsers.add_parser("job-update", help="Update a leased job state/result.")
    job_update_parser.add_argument("--session-id", required=True)
    job_update_parser.add_argument("--session-token", required=True)
    job_update_parser.add_argument("--job-id", required=True)
    job_update_parser.add_argument("--state", required=True)
    job_update_parser.add_argument("--result-json")
    job_update_parser.add_argument("--error-json")
    job_update_parser.add_argument("--artifacts-json")

    job_cancel_parser = subparsers.add_parser("job-cancel", help="Cancel a queued or active job.")
    job_cancel_parser.add_argument("--job-id", required=True)
    job_cancel_parser.add_argument("--reason", default="cancelled by client")

    job_chunk_parser = subparsers.add_parser("job-chunk", help="Upload one base64 artifact chunk for a leased job.")
    job_chunk_parser.add_argument("--session-id", required=True)
    job_chunk_parser.add_argument("--session-token", required=True)
    job_chunk_parser.add_argument("--job-id", required=True)
    job_chunk_parser.add_argument("--artifact-id", default="artifact")
    job_chunk_parser.add_argument("--ordinal", type=int, default=0)
    job_chunk_parser.add_argument("--data-base64", required=True)
    job_chunk_parser.add_argument("--sha256")

    subparsers.add_parser("jobs-recover", help="Recover stale worker sessions and expired job leases.")

    source_index_parser = subparsers.add_parser("source-index", help="Index C++/Config source symbols and Unreal path references.")
    source_index_parser.add_argument("--project", help="Project root or .uproject path. Defaults to the latest ingested scan project.")
    source_index_parser.add_argument("--compile-database", help="Optional compile_commands.json path.")

    source_symbols_parser = subparsers.add_parser("source-symbols", help="List indexed source symbols.")
    source_symbols_parser.add_argument("--project")
    source_symbols_parser.add_argument("--kind")
    source_symbols_parser.add_argument("--query")
    source_symbols_parser.add_argument("--limit", type=int, default=100)

    source_refs_parser = subparsers.add_parser("source-references", help="List indexed source references.")
    source_refs_parser.add_argument("--project")
    source_refs_parser.add_argument("--kind")
    source_refs_parser.add_argument("--query")
    source_refs_parser.add_argument("--limit", type=int, default=100)

    source_search_parser = subparsers.add_parser("source-search", help="Search indexed source symbols and references.")
    source_search_parser.add_argument("query")
    source_search_parser.add_argument("--project")
    source_search_parser.add_argument("--limit", type=int, default=50)

    blueprint_cpp_links_parser = subparsers.add_parser("blueprint-cpp-links", help="Link Blueprint call-function relations to indexed C++ symbols.")
    blueprint_cpp_links_parser.add_argument("--scan")
    blueprint_cpp_links_parser.add_argument("--project")
    blueprint_cpp_links_parser.add_argument("--query")
    blueprint_cpp_links_parser.add_argument("--limit", type=int, default=100)

    config_values_parser = subparsers.add_parser("config-values", help="List indexed Unreal config rows and effective values.")
    config_values_parser.add_argument("--project")
    config_values_parser.add_argument("--section")
    config_values_parser.add_argument("--key")
    config_values_parser.add_argument("--query")
    config_values_parser.add_argument("--limit", type=int, default=100)
    config_values_parser.add_argument("--include-history", action="store_true")

    animation_query_parser = subparsers.add_parser("animation-query", help="Query animation-domain manifest entities.")
    animation_query_parser.add_argument("--scan")
    animation_query_parser.add_argument("--asset")
    animation_query_parser.add_argument("--limit", type=int, default=100)
    animation_query_parser.add_argument("--include-snapshot", action="store_true")

    data_query_parser = subparsers.add_parser("data-query", help="Query data-domain manifest entities.")
    data_query_parser.add_argument("--scan")
    data_query_parser.add_argument("--asset")
    data_query_parser.add_argument("--limit", type=int, default=100)
    data_query_parser.add_argument("--include-snapshot", action="store_true")

    data_page_parser = subparsers.add_parser("data-page", help="Page rows/columns/entries/bundles inside a data snapshot.")
    data_page_parser.add_argument("entity", help="Asset entity id or canonical key with a data snapshot.")
    data_page_parser.add_argument("--scan")
    data_page_parser.add_argument("--collection", choices=["rows", "columns", "entries", "bundles", "parent_tables"], default="rows")
    data_page_parser.add_argument("--limit", type=int, default=100)
    data_page_parser.add_argument("--cursor")
    data_page_parser.add_argument("--include-artifact", action="store_true")

    cinematics_key_page_parser = subparsers.add_parser("cinematics-key-page", help="Page Sequencer key time rows inside a LevelSequence snapshot.")
    cinematics_key_page_parser.add_argument("entity", help="LevelSequence asset entity id, canonical key, sequence id, or movie scene id.")
    cinematics_key_page_parser.add_argument("--scan")
    cinematics_key_page_parser.add_argument("--section", help="Optional section id, path, name, or index filter.")
    cinematics_key_page_parser.add_argument("--channel", help="Optional channel id, name, display name, or index filter.")
    cinematics_key_page_parser.add_argument("--limit", type=int, default=100)
    cinematics_key_page_parser.add_argument("--cursor")
    cinematics_key_page_parser.add_argument("--include-artifact", action="store_true")

    scans_parser = subparsers.add_parser("scans", help="List ingested scans with cursor pagination.")
    scans_parser.add_argument("--limit", type=int, default=50)
    scans_parser.add_argument("--cursor")

    entities_parser = subparsers.add_parser("entities", help="List entities with cursor pagination.")
    entities_parser.add_argument("--scan", help="Scan id or unique prefix. Defaults to the latest scan.")
    entities_parser.add_argument("--kind")
    entities_parser.add_argument("--limit", type=int, default=100)
    entities_parser.add_argument("--cursor")
    entities_parser.add_argument("--include-snapshot", action="store_true")

    relations_parser = subparsers.add_parser("relations", help="List relations with cursor pagination.")
    relations_parser.add_argument("--scan", help="Scan id or unique prefix. Defaults to the latest scan.")
    relations_parser.add_argument("--relation-type")
    relations_parser.add_argument("--limit", type=int, default=100)
    relations_parser.add_argument("--cursor")

    search_parser = subparsers.add_parser("search", help="Search entities in the latest scan.")
    search_parser.add_argument("query", help="FTS or LIKE query.")
    search_parser.add_argument("--limit", type=int, default=20)
    search_parser.add_argument("--include-snapshot", action="store_true")

    related_parser = subparsers.add_parser("related", help="Show relations for an entity id or canonical key.")
    related_parser.add_argument("entity", help="Entity id or canonical key.")
    related_parser.add_argument("--limit", type=int, default=50)
    related_parser.add_argument("--include-snapshot", action="store_true")

    subgraph_parser = subparsers.add_parser("subgraph", help="Return a bounded relation subgraph.")
    subgraph_parser.add_argument("entity", help="Root entity id or canonical key.")
    subgraph_parser.add_argument("--depth", type=int, default=1)
    subgraph_parser.add_argument("--limit", type=int, default=200)
    subgraph_parser.add_argument("--relation-type", action="append", default=[])

    graph_page_parser = subparsers.add_parser("graph-page", help="Page nodes or edges from a bounded relation subgraph.")
    graph_page_parser.add_argument("entity", help="Root entity id or canonical key.")
    graph_page_parser.add_argument("--depth", type=int, default=1)
    graph_page_parser.add_argument("--collection", choices=["nodes", "edges"], default="edges")
    graph_page_parser.add_argument("--limit", type=int, default=100)
    graph_page_parser.add_argument("--cursor")
    graph_page_parser.add_argument("--graph-limit", type=int, default=1000)
    graph_page_parser.add_argument("--relation-type", action="append", default=[])

    graph_query_parser = subparsers.add_parser("graph-query", help="Run a compact graph DSL query.")
    graph_query_parser.add_argument("query", help="Example: from <entity> depth 2 relation contains_node limit 100")

    export_dot_parser = subparsers.add_parser("export-dot", help="Export a bounded relation subgraph as DOT.")
    export_dot_parser.add_argument("entity", help="Root entity id or canonical key.")
    export_dot_parser.add_argument("--depth", type=int, default=1)
    export_dot_parser.add_argument("--limit", type=int, default=200)
    export_dot_parser.add_argument("--relation-type", action="append", default=[])
    export_dot_parser.add_argument("--output", type=Path)

    export_graph_parser = subparsers.add_parser("export-graph", help="Export a bounded relation subgraph.")
    export_graph_parser.add_argument("entity", help="Root entity id or canonical key.")
    export_graph_parser.add_argument("--format", choices=["json", "dot", "mermaid", "graphml", "cytoscape", "parquet"], default="json")
    export_graph_parser.add_argument("--depth", type=int, default=1)
    export_graph_parser.add_argument("--limit", type=int, default=200)
    export_graph_parser.add_argument("--relation-type", action="append", default=[])
    export_graph_parser.add_argument("--output", type=Path)

    artifact_range_parser = subparsers.add_parser("artifact-range", help="Read a byte range from an ingested scan artifact.")
    artifact_range_parser.add_argument("--scan", help="Scan id or unique prefix. Defaults to the latest scan.")
    artifact_range_parser.add_argument("--offset", type=int, default=0)
    artifact_range_parser.add_argument("--length", type=int, default=4096)
    artifact_range_parser.add_argument("--encoding", choices=["text", "base64"], default="text")

    report_parser = subparsers.add_parser("report", help="Render a Markdown scan report.")
    report_parser.add_argument("--scan", help="Scan id or unique prefix. Defaults to the latest scan.")
    report_parser.add_argument("--limit", type=int, default=20)
    report_parser.add_argument("--output", type=Path)

    subparsers.add_parser("integrity", help="Run SQLite integrity and foreign-key checks.")
    subparsers.add_parser("recover", help="Checkpoint WAL, optimize SQLite, and run integrity checks.")

    diff_parser = subparsers.add_parser("diff", help="Compare two ingested scans and report structural impact.")
    diff_parser.add_argument("--base", help="Base scan id or unique prefix. Defaults to the previous ingested scan.")
    diff_parser.add_argument("--compare", help="Compare scan id or unique prefix. Defaults to the latest ingested scan.")
    diff_parser.add_argument("--limit", type=int, default=100, help="Maximum detailed rows per diff section.")

    stale_parser = subparsers.add_parser("stale", help="Check whether an ingested scan artifact is missing or changed.")
    stale_parser.add_argument("--scan", help="Scan id or unique prefix. Defaults to the latest ingested scan.")
    stale_parser.add_argument("--limit", type=int, default=100)

    history_parser = subparsers.add_parser("history", help="Show asset revision history.")
    history_parser.add_argument("asset", nargs="?", help="Asset canonical key, entity id, or revision id prefix.")
    history_parser.add_argument("--limit", type=int, default=50)

    subparsers.add_parser("api-docs", help="Print the local HTTP API document.")

    serve_parser = subparsers.add_parser("serve", help="Run the local UEPI HTTP query API.")
    serve_parser.add_argument("--host", default="127.0.0.1")
    serve_parser.add_argument("--port", type=int, default=8765)
    serve_parser.add_argument("--token", help="Optional HTTP token. Use 'auto' to generate one next to the database.")
    serve_parser.add_argument("--token-file", type=Path, help="Optional sandboxed file containing the HTTP token.")

    args = parser.parse_args(argv)

    if args.command == "ingest":
        print_json(ingest(args.scan, args.db))
        return 0

    if args.command == "summary":
        conn = connect(args.db)
        try:
            print_json(summary_dict(conn))
        finally:
            conn.close()
        return 0

    if args.command == "health":
        conn = connect(args.db)
        try:
            scan_count = conn.execute("SELECT COUNT(*) AS count FROM scans").fetchone()["count"]
            print_json({"ok": True, "db_path": str(args.db), "scan_count": scan_count})
        finally:
            conn.close()
        return 0

    if args.command == "protocol":
        print_json(agent_protocol_document())
        return 0

    if args.command == "worker-register":
        print_json(
            register_worker(
                args.db,
                args.worker_id,
                args.worker_type,
                parse_json_arg(args.capabilities_json, {}),
                args.ttl_seconds,
            )
        )
        return 0

    if args.command == "worker-heartbeat":
        print_json(
            worker_heartbeat(
                args.db,
                args.session_id,
                args.session_token,
                args.status,
                parse_json_arg(args.capabilities_json, {}) if args.capabilities_json is not None else None,
                args.ttl_seconds,
            )
        )
        return 0

    if args.command == "workers":
        print_json(list_worker_sessions(args.db, args.status, args.limit))
        return 0

    if args.command == "job-submit":
        print_json(
            submit_job(
                args.db,
                args.job_type,
                parse_json_arg(args.request_json, {}),
                args.priority,
                args.timeout_seconds,
                args.max_retries,
                args.trace_id,
            )
        )
        return 0

    if args.command == "jobs":
        print_json(list_jobs(args.db, args.state, args.limit, args.include_events))
        return 0

    if args.command == "job":
        print_json(get_job(args.db, args.job_id, include_events=not args.no_events))
        return 0

    if args.command == "job-poll":
        print_json(poll_jobs(args.db, args.session_id, args.session_token, args.limit, args.wait_seconds))
        return 0

    if args.command == "job-update":
        print_json(
            update_job(
                args.db,
                args.session_id,
                args.session_token,
                args.job_id,
                args.state,
                parse_json_arg(args.result_json, {}) if args.result_json is not None else None,
                parse_json_arg(args.error_json, {}) if args.error_json is not None else None,
                parse_json_arg(args.artifacts_json, []) if args.artifacts_json is not None else None,
            )
        )
        return 0

    if args.command == "job-cancel":
        print_json(cancel_job(args.db, args.job_id, args.reason))
        return 0

    if args.command == "job-chunk":
        print_json(
            upload_job_chunk(
                args.db,
                args.session_id,
                args.session_token,
                args.job_id,
                args.artifact_id,
                args.ordinal,
                args.data_base64,
                args.sha256,
            )
        )
        return 0

    if args.command == "jobs-recover":
        print_json(recover_jobs(args.db))
        return 0

    if args.command == "source-index":
        print_json(index_source(args.db, args.project, args.compile_database))
        return 0

    if args.command == "source-symbols":
        print_json(source_symbols(args.db, args.project, args.kind, args.query, args.limit))
        return 0

    if args.command == "source-references":
        print_json(source_references(args.db, args.project, args.kind, args.query, args.limit))
        return 0

    if args.command == "source-search":
        print_json(source_search(args.db, args.query, args.project, args.limit))
        return 0

    if args.command == "blueprint-cpp-links":
        print_json(blueprint_cpp_links(args.db, args.scan, args.project, args.query, args.limit))
        return 0

    if args.command == "config-values":
        print_json(config_values(args.db, args.project, args.section, args.key, args.query, args.limit, args.include_history))
        return 0

    if args.command == "animation-query":
        print_json(animation_query(args.db, args.scan, args.asset, args.limit, args.include_snapshot))
        return 0

    if args.command == "data-query":
        print_json(data_query(args.db, args.scan, args.asset, args.limit, args.include_snapshot))
        return 0

    if args.command == "data-page":
        print_json(data_snapshot_page(args.db, args.entity, args.collection, args.scan, args.limit, args.cursor, args.include_artifact))
        return 0

    if args.command == "cinematics-key-page":
        print_json(
            cinematics_key_page(
                args.db,
                args.entity,
                args.scan,
                args.limit,
                args.cursor,
                args.section,
                args.channel,
                args.include_artifact,
            )
        )
        return 0

    if args.command == "scans":
        print_json(list_scans(args.db, args.limit, args.cursor))
        return 0

    if args.command == "entities":
        print_json(list_entities(args.db, args.scan, args.kind, args.limit, args.cursor, args.include_snapshot))
        return 0

    if args.command == "relations":
        print_json(list_relations(args.db, args.scan, args.relation_type, args.limit, args.cursor))
        return 0

    if args.command == "search":
        print_json(search(args.db, args.query, args.limit, args.include_snapshot))
        return 0

    if args.command == "related":
        print_json(related(args.db, args.entity, args.limit, args.include_snapshot))
        return 0

    if args.command == "subgraph":
        print_json(subgraph(args.db, args.entity, args.depth, args.limit, split_relation_filters(args.relation_type)))
        return 0

    if args.command == "graph-page":
        print_json(
            graph_page(
                args.db,
                args.entity,
                args.depth,
                args.collection,
                args.limit,
                args.cursor,
                split_relation_filters(args.relation_type),
                args.graph_limit,
            )
        )
        return 0

    if args.command == "graph-query":
        print_json(graph_query(args.db, args.query))
        return 0

    if args.command == "export-dot":
        dot = export_dot(args.db, args.entity, args.depth, args.limit, split_relation_filters(args.relation_type))
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(dot, encoding="utf-8")
        else:
            print(dot)
        return 0

    if args.command == "export-graph":
        payload, _content_type, is_binary = export_graph(
            args.db,
            args.entity,
            args.depth,
            args.limit,
            split_relation_filters(args.relation_type),
            args.format,
        )
        if is_binary:
            if not args.output:
                raise SystemExit("Parquet export requires --output.")
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(payload)
        elif isinstance(payload, (dict, list)):
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8")
            else:
                print_json(payload)
        else:
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(payload, encoding="utf-8")
            else:
                print(payload)
        return 0

    if args.command == "artifact-range":
        print_json(artifact_range(args.db, args.scan, args.offset, args.length, args.encoding))
        return 0

    if args.command == "report":
        report = markdown_report(args.db, args.scan, args.limit)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(report, encoding="utf-8")
        else:
            print(report, end="")
        return 0

    if args.command == "integrity":
        print_json(db_integrity(args.db))
        return 0

    if args.command == "recover":
        print_json(db_recover(args.db))
        return 0

    if args.command == "diff":
        print_json(scan_diff(args.db, args.base, args.compare, args.limit))
        return 0

    if args.command == "stale":
        print_json(staleness(args.db, args.scan, args.limit))
        return 0

    if args.command == "history":
        print_json(asset_history(args.db, args.asset, args.limit))
        return 0

    if args.command == "api-docs":
        print_json(api_document())
        return 0

    if args.command == "serve":
        serve(args.db, args.host, args.port, load_server_token(args.token, args.token_file, args.db))
        return 0

    parser.error(f"Unknown command: {args.command}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
