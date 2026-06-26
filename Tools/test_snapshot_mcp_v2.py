from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any
from datetime import datetime, timezone


ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "Services" / "uepi" / "src" / "uepi" / "mcp_server.py"
PYTHONPATH = ROOT / "Services" / "uepi" / "src"

EXPECTED_TOOLS = {
    "uepi_status",
    "uepi_overview",
    "uepi_search",
    "uepi_context",
    "uepi_asset",
    "uepi_blueprint",
    "uepi_blueprint_trace",
    "uepi_animation",
    "uepi_impact",
    "uepi_diff",
}


def send_message(process: subprocess.Popen[bytes], message: dict[str, Any]) -> None:
    data = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    assert process.stdin is not None
    process.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    process.stdin.write(data)
    process.stdin.flush()


def read_message(process: subprocess.Popen[bytes]) -> dict[str, Any]:
    assert process.stdout is not None
    line = process.stdout.readline()
    while line in (b"\r\n", b"\n"):
        line = process.stdout.readline()
    if not line:
        stderr = process.stderr.read().decode("utf-8", errors="replace") if process.stderr else ""
        raise RuntimeError(f"MCP server closed stdout. stderr={stderr}")

    headers: dict[str, str] = {}
    while line not in (b"\r\n", b"\n", b""):
        decoded = line.decode("ascii", errors="replace")
        if ":" in decoded:
            key, value = decoded.split(":", 1)
            headers[key.lower()] = value.strip()
        line = process.stdout.readline()

    length = int(headers.get("content-length", "0"))
    if length <= 0:
        raise RuntimeError("MCP response did not include Content-Length.")
    return json.loads(process.stdout.read(length).decode("utf-8"))


def request(process: subprocess.Popen[bytes], request_id: int, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
    send_message(process, {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}})
    response = read_message(process)
    if "error" in response:
        raise RuntimeError(json.dumps(response["error"], indent=2))
    return response["result"]


def write_fixture(root: Path) -> None:
    store = root / "store"
    objects = store / "objects" / "aa"
    manifests = store / "manifests"
    sessions = store / "sessions"
    objects.mkdir(parents=True)
    manifests.mkdir(parents=True)
    sessions.mkdir(parents=True)

    asset_id = "asset-bp-hero"
    project_entity_id = "project-root"
    node_id = "node-beginplay"
    relation_id = "rel-contains-node"
    asset_entity = {
        "id": asset_id,
        "kind": "asset",
        "canonical_key": "/Game/BP_Hero.BP_Hero",
        "display_name": "BP_Hero",
        "source_layer": "asset_registry",
        "attributes": {"object_path": "/Game/BP_Hero.BP_Hero", "asset_name": "BP_Hero"},
        "completeness": {"state": "partial", "covered": ["asset_registry_metadata"], "omitted": [], "warnings": []},
        "diagnostics": [],
        "evidence": [],
    }
    node_entity = {
        "id": node_id,
        "kind": "blueprint_node",
        "canonical_key": "/Game/BP_Hero.BP_Hero::EventGraph::BeginPlay",
        "display_name": "Event BeginPlay",
        "source_layer": "editor_source_graph",
        "attributes": {"node_title": "Event BeginPlay"},
        "completeness": {"state": "partial", "covered": ["node"], "omitted": [], "warnings": []},
        "diagnostics": [],
        "evidence": [],
    }
    contains_node_relation = {
        "id": relation_id,
        "type": "contains_node",
        "from_id": asset_id,
        "to_id": node_id,
        "source_layer": "editor_source_graph",
        "derived": False,
        "confidence": 1.0,
        "attributes": {},
        "evidence": [],
    }
    project_fragment = {
        "schema_version": "uepi.project-fragment.v2",
        "project_id": "project-fixture",
        "project_name": "FixtureProject",
        "project_file": str(root / "FixtureProject.uproject"),
        "engine_version": "5.3.2",
        "started_at_utc": "2026-06-26T00:00:00Z",
        "finished_at_utc": "2026-06-26T00:00:01Z",
        "completeness": {"state": "partial", "covered": ["blueprint_graphs"], "omitted": [], "warnings": []},
        "entities": [
            {
                "id": project_entity_id,
                "kind": "project",
                "canonical_key": str(root / "FixtureProject.uproject"),
                "display_name": "FixtureProject",
                "source_layer": "filesystem",
                "attributes": {"project_name": "FixtureProject"},
                "completeness": {"state": "complete", "covered": ["project_descriptor"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            },
        ],
        "relations": [],
        "diagnostics": [],
    }
    project_fragment_path = objects / "aaprojectfragment.json"
    project_fragment_path.write_text(json.dumps(project_fragment, ensure_ascii=False), encoding="utf-8")

    fragment_node_id = "node-asset-fragment"
    asset_fragment = {
        "schema_version": "uepi.asset-fragment.v2",
        "project_id": "project-fixture",
        "project_name": "FixtureProject",
        "project_file": str(root / "FixtureProject.uproject"),
        "engine_version": "5.3.2",
        "source_scan_finished_at_utc": "2026-06-26T00:00:01Z",
        "asset": {"id": asset_id, "canonical_key": "/Game/BP_Hero.BP_Hero", "display_name": "BP_Hero", "kind": "asset"},
        "entities": [
            asset_entity,
            node_entity,
            {
                "id": fragment_node_id,
                "kind": "blueprint_node",
                "canonical_key": "/Game/BP_Hero.BP_Hero::EventGraph::FragmentNode",
                "display_name": "Asset Fragment Node",
                "source_layer": "editor_source_graph",
                "attributes": {"node_title": "Asset Fragment Node"},
                "completeness": {"state": "partial", "covered": ["node"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            }
        ],
        "relations": [
            contains_node_relation,
            {
                "id": "rel-asset-fragment-node",
                "type": "contains_node",
                "from_id": asset_id,
                "to_id": fragment_node_id,
                "source_layer": "editor_source_graph",
                "derived": False,
                "confidence": 1.0,
                "attributes": {},
                "evidence": [],
            }
        ],
        "diagnostics": [],
    }
    asset_fragment_path = objects / "aaassetfragment.json"
    asset_fragment_path.write_text(json.dumps(asset_fragment, ensure_ascii=False), encoding="utf-8")

    manifest = {
        "schema_version": "uepi.snapshot-manifest.v2",
        "data_mode": "saved",
        "writer_mode": "test",
        "session_id": "",
        "generation": 1,
        "created_at_utc": "2026-06-26T00:00:02Z",
        "project": {"id": "project-fixture", "name": "FixtureProject", "project_file": str(root / "FixtureProject.uproject"), "engine_version": "5.3.2"},
        "counts": {"entities": 2, "relations": 1, "diagnostics": 0, "asset_entities": 1},
        "source": {},
        "completeness": {"state": "partial", "covered": ["blueprint_graphs"], "omitted": [], "warnings": []},
        "asset_entity_ids": [asset_id],
        "fragments": [
            {"kind": "project_fragment", "schema_version": "uepi.project-fragment.v2", "hash": "aaprojectfragment", "path": str(project_fragment_path)},
            {"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aaassetfragment", "path": str(asset_fragment_path), "asset_id": asset_id},
        ],
    }
    (manifests / "saved.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
    (manifests / "saved-1.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")

    live_node_id = "node-live-print"
    live_relation_id = "rel-live-contains-node"
    deleted_asset_id = "asset-bp-deleted"
    deleted_node_id = "node-deleted"
    live_asset_entity = dict(asset_entity)
    live_asset_entity["attributes"] = {"object_path": "/Game/BP_Hero.BP_Hero", "asset_name": "BP_Hero", "live_marker": "true"}
    live_asset_fragment = {
        "schema_version": "uepi.asset-fragment.v2",
        "project_id": "project-fixture",
        "project_name": "FixtureProject",
        "project_file": str(root / "FixtureProject.uproject"),
        "engine_version": "5.3.2",
        "source_scan_finished_at_utc": "2026-06-26T00:00:03Z",
        "asset": {"id": asset_id, "canonical_key": "/Game/BP_Hero.BP_Hero", "display_name": "BP_Hero", "kind": "asset"},
        "entities": [
            live_asset_entity,
            {
                "id": live_node_id,
                "kind": "blueprint_node",
                "canonical_key": "/Game/BP_Hero.BP_Hero::EventGraph::PrintString",
                "display_name": "Live Print String",
                "source_layer": "editor_source_graph",
                "attributes": {"node_title": "Live Print String"},
                "completeness": {"state": "partial", "covered": ["node"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            },
        ],
        "relations": [
            {
                "id": live_relation_id,
                "type": "contains_node",
                "from_id": asset_id,
                "to_id": live_node_id,
                "source_layer": "editor_source_graph",
                "derived": False,
                "confidence": 1.0,
                "attributes": {},
                "evidence": [],
            }
        ],
        "diagnostics": [],
    }
    live_object_path = objects / "aaliveassetfragment.json"
    live_object_path.write_text(json.dumps(live_asset_fragment, ensure_ascii=False), encoding="utf-8")

    deleted_asset_fragment = {
        "schema_version": "uepi.asset-fragment.v2",
        "project_id": "project-fixture",
        "project_name": "FixtureProject",
        "project_file": str(root / "FixtureProject.uproject"),
        "engine_version": "5.3.2",
        "source_scan_finished_at_utc": "2026-06-26T00:00:01Z",
        "asset": {"id": deleted_asset_id, "canonical_key": "/Game/BP_Deleted.BP_Deleted", "display_name": "BP_Deleted", "kind": "asset"},
        "entities": [
            {
                "id": deleted_asset_id,
                "kind": "asset",
                "canonical_key": "/Game/BP_Deleted.BP_Deleted",
                "display_name": "BP_Deleted",
                "source_layer": "asset_registry",
                "attributes": {"object_path": "/Game/BP_Deleted.BP_Deleted", "asset_name": "BP_Deleted"},
                "completeness": {"state": "partial", "covered": ["asset_registry_metadata"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            },
            {
                "id": deleted_node_id,
                "kind": "blueprint_node",
                "canonical_key": "/Game/BP_Deleted.BP_Deleted::EventGraph::OldNode",
                "display_name": "Deleted Node",
                "source_layer": "editor_source_graph",
                "attributes": {"node_title": "Deleted Node"},
                "completeness": {"state": "partial", "covered": ["node"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            },
        ],
        "relations": [
            {
                "id": "rel-deleted-contains-node",
                "type": "contains_node",
                "from_id": deleted_asset_id,
                "to_id": deleted_node_id,
                "source_layer": "editor_source_graph",
                "derived": False,
                "confidence": 1.0,
                "attributes": {},
                "evidence": [],
            }
        ],
        "diagnostics": [],
    }
    deleted_object_path = objects / "aadeletedassetfragment.json"
    deleted_object_path.write_text(json.dumps(deleted_asset_fragment, ensure_ascii=False), encoding="utf-8")
    deleted_tombstone = {
        "schema_version": "uepi.asset-tombstone.v2",
        "project_id": "project-fixture",
        "project_name": "FixtureProject",
        "project_file": str(root / "FixtureProject.uproject"),
        "engine_version": "5.3.2",
        "created_at_utc": "2026-06-26T00:00:04Z",
        "asset_key": "/Game/BP_Deleted.BP_Deleted",
        "asset_name": "BP_Deleted",
        "asset_id": deleted_asset_id,
        "package_name": "/Game/BP_Deleted",
        "class_path": "/Script/Engine.Blueprint",
        "reason": "asset_removed",
        "event_type": "asset_removed",
        "old_object_path": "",
        "new_object_path": "",
        "source_event_sequence": 2,
    }
    deleted_tombstone_path = objects / "aadeletedtombstone.json"
    deleted_tombstone_path.write_text(json.dumps(deleted_tombstone, ensure_ascii=False), encoding="utf-8")
    live_manifest = dict(manifest)
    manifest["fragments"].extend(
        [
            {"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aadeletedassetfragment", "path": str(deleted_object_path), "asset_id": deleted_asset_id},
            {"kind": "asset_tombstone", "schema_version": "uepi.asset-tombstone.v2", "hash": "aadeletedtombstone", "path": str(deleted_tombstone_path), "asset_id": deleted_asset_id, "asset_key": "/Game/BP_Deleted.BP_Deleted"},
        ]
    )
    (manifests / "saved.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
    (manifests / "saved-1.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
    live_manifest.update(
        {
            "data_mode": "live",
            "writer_mode": "editor",
            "session_id": "session-live",
            "generation": 1,
            "base_saved_generation": 1,
            "is_overlay": True,
            "merge_strategy": "replace",
            "target_object_paths": ["/Game/BP_Hero.BP_Hero"],
            "fragments": [{"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aaliveassetfragment", "path": str(live_object_path), "asset_id": asset_id}],
        }
    )
    (manifests / "live.json").write_text(json.dumps(live_manifest, ensure_ascii=False), encoding="utf-8")
    (sessions / "editor-session.json").write_text(
        json.dumps(
            {
                "schema_version": "uepi.live-session.v2",
                "session_id": "session-live",
                "state": "active",
                "last_seen_at_utc": datetime.now(timezone.utc).isoformat(),
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    logs = store / "logs"
    logs.mkdir(parents=True)
    (logs / "incremental_events.jsonl").write_text(
        json.dumps(
            {
                "sequence": 3,
                "event_type": "asset_updated",
                "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                "asset_path": "/Game/BP_Hero.BP_Hero",
                "package_name": "/Game/BP_Hero",
                "class_path": "/Script/Engine.Blueprint",
                "old_object_path": "",
                "package_file_name": "",
            },
            ensure_ascii=False,
        )
        + "\n",
        encoding="utf-8",
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="uepi_snapshot_mcp_") as temp_dir:
        root = Path(temp_dir)
        write_fixture(root)
        env = os.environ.copy()
        env["PYTHONPATH"] = str(PYTHONPATH)
        sync = subprocess.run(
            [sys.executable, "-B", "-m", "uepi", "sync", "--store", str(root)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=True,
        )
        sync_result = json.loads(sync.stdout)
        assert sync_result["schema_version"] == "uepi.sqlite-cache.v2.1"
        assert sync_result["entity_count"] >= 3

        process = subprocess.Popen(
            [sys.executable, "-B", str(SERVER), "--store", str(root), "--tool-profile", "codex"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        try:
            init = request(
                process,
                1,
                "initialize",
                {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "snapshot-test", "version": "1"}},
            )
            assert init["serverInfo"]["name"] == "uepi-mcp"
            assert "instructions" in init
            assert "resources" not in init["capabilities"]
            tools = request(process, 2, "tools/list")["tools"]
            names = {tool["name"] for tool in tools}
            assert names == EXPECTED_TOOLS
            assert not any("worker" in name or "queue" in name or "daemon" in name for name in names)

            status = request(process, 3, "tools/call", {"name": "uepi_status", "arguments": {}})["structuredContent"]
            assert status["state"]["data_mode"] == "live"
            assert status["state"]["editor_connected"] is True
            assert status["result"]["llm_readiness"]["requires_daemon"] is False
            assert status["result"]["cache"]["synced"] is True
            assert status["result"]["cache"]["schema_version"] == "uepi.sqlite-cache.v2.1"
            search = request(process, 4, "tools/call", {"name": "uepi_search", "arguments": {"query": "BP_Hero"}})["structuredContent"]
            assert search["result"]["match_count"] >= 1
            assert search["result"]["query_source"] == "sqlite_cache"
            assert "typed_attributes" in search["result"]["matches"][0]
            deleted_search = request(process, 40, "tools/call", {"name": "uepi_search", "arguments": {"query": "BP_Deleted"}})["structuredContent"]
            assert deleted_search["result"]["match_count"] == 0
            deleted_asset = request(process, 41, "tools/call", {"name": "uepi_asset", "arguments": {"asset": "BP_Deleted"}})["structuredContent"]
            assert deleted_asset["error"]["code"] == "UEPI_ASSET_TOMBSTONED"
            blueprint = request(process, 5, "tools/call", {"name": "uepi_blueprint", "arguments": {"asset": "/Game/BP_Hero.BP_Hero"}})["structuredContent"]
            assert blueprint["result"]["query_source"] == "sqlite_cache"
            assert blueprint["state"]["freshness"] == "refresh_requested"
            assert blueprint["diagnostics"][0]["code"] == "UEPI_REFRESH_REQUESTED"
            blueprint_names = {item["display_name"] for item in blueprint["result"]["blueprint_entities"]}
            assert "Event BeginPlay" in blueprint_names
            assert "Asset Fragment Node" in blueprint_names
            assert "Live Print String" in blueprint_names
            assert any((root / "store" / "requests").glob("refresh-*.json"))
            unknown = request(process, 6, "tools/call", {"name": "uepi_missing", "arguments": {}})["structuredContent"]
            assert unknown["error"]["code"] == "UEPI_UNKNOWN_TOOL"
        finally:
            if process.stdin:
                process.stdin.close()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
    print("snapshot MCP v2 assertions ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
