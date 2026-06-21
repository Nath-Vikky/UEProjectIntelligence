#!/usr/bin/env python3
from __future__ import annotations

import base64
import hashlib
import importlib.util
from pathlib import Path
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[3]
DAEMON_PATH = REPO_ROOT / "Plugins" / "UEProjectIntelligence" / "Services" / "uepi_daemon" / "uepi_daemon.py"


def load_daemon():
    spec = importlib.util.spec_from_file_location("uepi_daemon", DAEMON_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to load daemon module from {DAEMON_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    daemon = load_daemon()
    with tempfile.TemporaryDirectory(prefix="uepi_worker_protocol_") as temp_dir:
        db_path = Path(temp_dir) / "index.sqlite3"

        protocol = daemon.agent_protocol_document()
        assert protocol["schema_version"] == "uepi.agent_protocol.v1"
        assert "queued" in protocol["job_states"]

        registration = daemon.register_worker(
            db_path,
            "editor-worker-a",
            "editor",
            {"engine_version": "5.3.2", "modes": ["live", "commandlet"]},
            ttl_seconds=30,
        )
        session_id = registration["session_id"]
        session_token = registration["session_token"]
        assert session_id.startswith("session_")
        assert session_token

        heartbeat = daemon.worker_heartbeat(db_path, session_id, session_token, status="idle", ttl_seconds=30)
        assert heartbeat["ok"] is True

        submitted = daemon.submit_job(
            db_path,
            "metadata_scan",
            {"asset": "/Game/Foo.Foo", "level": "L2"},
            priority=10,
            timeout_seconds=30,
            max_retries=1,
            trace_id="trace_test_worker_protocol",
        )
        job_id = submitted["job_id"]
        assert submitted["state"] == "queued"

        polled = daemon.poll_jobs(db_path, session_id, session_token, limit=1, wait_seconds=0)
        assert len(polled["jobs"]) == 1
        assert polled["jobs"][0]["job_id"] == job_id
        assert polled["jobs"][0]["state"] == "assigned"

        chunk = b'{"partial":true}'
        uploaded = daemon.upload_job_chunk(
            db_path,
            session_id,
            session_token,
            job_id,
            "scan-json",
            0,
            base64.b64encode(chunk).decode("ascii"),
            hashlib.sha256(chunk).hexdigest(),
        )
        assert uploaded["byte_count"] == len(chunk)

        completed = daemon.update_job(
            db_path,
            session_id,
            session_token,
            job_id,
            "succeeded",
            result={"scan_path": "Saved/UEProjectIntelligence/last_scan.json"},
            artifacts=[{"artifact_id": "scan-json", "chunks": 1}],
        )
        assert completed["state"] == "succeeded"
        assert completed["uploaded_artifacts"][0]["chunk_count"] == 1

        jobs = daemon.list_jobs(db_path, state="succeeded", include_events=True)
        assert len(jobs["jobs"]) == 1
        assert any(event["event_type"] == "chunk_uploaded" for event in jobs["jobs"][0]["events"])

        recovery = daemon.recover_jobs(db_path)
        assert recovery["ok"] is True

    print("worker protocol assertions ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
