#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[3]
DAEMON_PATH = REPO_ROOT / "Plugins" / "UEProjectIntelligence" / "Services" / "uepi_daemon" / "uepi_daemon.py"
BLUEPRINT_SCAN = REPO_ROOT / "Saved" / "UEProjectIntelligence" / "l2_blueprint_scan.json"
BLUEPRINT_ENTITY = "/Game/ThirdPerson/Blueprints/BP_ThirdPersonGameMode.BP_ThirdPersonGameMode"


def load_daemon():
    spec = importlib.util.spec_from_file_location("uepi_daemon", DAEMON_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to load daemon module from {DAEMON_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if not BLUEPRINT_SCAN.exists():
        raise RuntimeError(f"Missing scan fixture: {BLUEPRINT_SCAN}")

    daemon = load_daemon()
    with tempfile.TemporaryDirectory(prefix="uepi_graph_page_") as temp_dir:
        db_path = Path(temp_dir) / "index.sqlite3"
        ingest = daemon.ingest(BLUEPRINT_SCAN, db_path)
        assert ingest["entity_count"] > 0

        first_page = daemon.graph_page(db_path, BLUEPRINT_ENTITY, depth=2, collection="edges", limit=2)
        assert first_page["root"]["canonical_key"] == BLUEPRINT_ENTITY
        assert first_page["collection"] == "edges"
        assert first_page["count"] == 2
        assert first_page["total_count"] >= 3
        assert first_page["has_more"]
        assert first_page["next_cursor"]

        second_page = daemon.graph_page(
            db_path,
            BLUEPRINT_ENTITY,
            depth=2,
            collection="edges",
            limit=2,
            cursor=first_page["next_cursor"],
        )
        assert second_page["start_index"] == 2
        assert second_page["items"][0]["id"] != first_page["items"][0]["id"]

        node_page = daemon.graph_page(db_path, BLUEPRINT_ENTITY, depth=1, collection="nodes", limit=1)
        assert node_page["collection"] == "nodes"
        assert node_page["count"] == 1
        assert node_page["items"][0]["id"]

    print("graph page assertions ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
