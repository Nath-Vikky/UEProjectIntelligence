from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any


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
    objects.mkdir(parents=True)
    manifests.mkdir(parents=True)

    asset_id = "asset-bp-hero"
    node_id = "node-beginplay"
    relation_id = "rel-contains-node"
    scan = {
        "schema_version": "uepi.scan.v1",
        "project_id": "project-fixture",
        "project_name": "FixtureProject",
        "project_file": str(root / "FixtureProject.uproject"),
        "engine_version": "5.3.2",
        "started_at_utc": "2026-06-26T00:00:00Z",
        "finished_at_utc": "2026-06-26T00:00:01Z",
        "completeness": {"state": "partial", "covered": ["blueprint_graphs"], "omitted": [], "warnings": []},
        "entities": [
            {
                "id": asset_id,
                "kind": "asset",
                "canonical_key": "/Game/BP_Hero.BP_Hero",
                "display_name": "BP_Hero",
                "source_layer": "asset_registry",
                "attributes": {"object_path": "/Game/BP_Hero.BP_Hero", "asset_name": "BP_Hero"},
                "completeness": {"state": "partial", "covered": ["asset_registry_metadata"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            },
            {
                "id": node_id,
                "kind": "blueprint_node",
                "canonical_key": "/Game/BP_Hero.BP_Hero::EventGraph::BeginPlay",
                "display_name": "Event BeginPlay",
                "source_layer": "editor_source_graph",
                "attributes": {"node_title": "Event BeginPlay"},
                "completeness": {"state": "partial", "covered": ["node"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            },
        ],
        "relations": [
            {
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
        ],
        "diagnostics": [],
    }
    object_path = objects / "aabbcc.json"
    object_path.write_text(json.dumps(scan, ensure_ascii=False), encoding="utf-8")

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
        "fragments": [{"kind": "project_scan", "schema_version": "uepi.scan.v1", "hash": "aabbcc", "path": str(object_path)}],
    }
    (manifests / "saved.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
    (manifests / "saved-1.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="uepi_snapshot_mcp_") as temp_dir:
        root = Path(temp_dir)
        write_fixture(root)
        env = os.environ.copy()
        env["PYTHONPATH"] = str(PYTHONPATH)
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
            tools = request(process, 2, "tools/list")["tools"]
            names = {tool["name"] for tool in tools}
            assert names == EXPECTED_TOOLS
            assert not any("worker" in name or "queue" in name or "daemon" in name for name in names)

            status = request(process, 3, "tools/call", {"name": "uepi_status", "arguments": {}})["structuredContent"]
            assert status["result"]["llm_readiness"]["requires_daemon"] is False
            search = request(process, 4, "tools/call", {"name": "uepi_search", "arguments": {"query": "BP_Hero"}})["structuredContent"]
            assert search["result"]["match_count"] >= 1
            blueprint = request(process, 5, "tools/call", {"name": "uepi_blueprint", "arguments": {"asset": "/Game/BP_Hero.BP_Hero"}})["structuredContent"]
            assert blueprint["result"]["blueprint_entities"][0]["kind"] == "blueprint_node"
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
