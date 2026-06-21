#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[3]
DAEMON_PATH = REPO_ROOT / "Plugins" / "UEProjectIntelligence" / "Services" / "uepi_daemon" / "uepi_daemon.py"
DATA_TABLE_SCAN = REPO_ROOT / "Saved" / "UEProjectIntelligence" / "l2_data_table_scan.json"
DATA_TABLE_ENTITY = "/DatasmithContent/Datasmith/AreaLightsTable.AreaLightsTable"


def load_daemon():
    spec = importlib.util.spec_from_file_location("uepi_daemon", DAEMON_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to load daemon module from {DAEMON_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if not DATA_TABLE_SCAN.exists():
        raise RuntimeError(f"Missing scan fixture: {DATA_TABLE_SCAN}")

    daemon = load_daemon()
    with tempfile.TemporaryDirectory(prefix="uepi_data_page_") as temp_dir:
        db_path = Path(temp_dir) / "index.sqlite3"
        ingest = daemon.ingest(DATA_TABLE_SCAN, db_path)
        assert ingest["entity_count"] > 0

        first_page = daemon.data_snapshot_page(db_path, DATA_TABLE_ENTITY, "rows", limit=2)
        assert first_page["container"] == "data_table"
        assert first_page["container_schema_version"] == "uepi.data_table.v1"
        assert first_page["count"] == 2
        assert first_page["total_count"] == 5
        assert first_page["items"][0]["index"] == 0
        assert first_page["has_more"]
        assert first_page["next_cursor"]

        second_page = daemon.data_snapshot_page(
            db_path,
            DATA_TABLE_ENTITY,
            "rows",
            limit=2,
            cursor=first_page["next_cursor"],
        )
        assert second_page["start_index"] == 2
        assert second_page["items"][0]["index"] == 2

        columns = daemon.data_snapshot_page(db_path, DATA_TABLE_ENTITY, "columns", limit=10)
        assert columns["count"] == 1
        assert columns["items"][0]["name"]

        artifact_page = daemon.data_snapshot_page(
            db_path,
            DATA_TABLE_ENTITY,
            "rows",
            limit=1,
            include_artifact=True,
        )
        artifact = artifact_page["collection_artifact"]
        artifact_path = Path(artifact["path"])
        assert artifact["artifact_uri"].startswith("uepi://data-artifact/")
        assert artifact["item_count"] == 5
        assert artifact["byte_count"] > 0
        assert artifact_path.exists()
        assert artifact_path.read_text(encoding="utf-8")

    print("data page assertions ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
