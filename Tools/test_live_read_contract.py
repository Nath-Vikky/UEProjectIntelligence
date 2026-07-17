from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from types import SimpleNamespace
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Services" / "uepi" / "src"))

from uepi.mcp_server import UEPIMCPServer  # noqa: E402
from uepi.timing import TIMING_FIELDS  # noqa: E402


def _value(server: UEPIMCPServer, tool: str, arguments: dict[str, Any]) -> dict[str, Any]:
    response = server.call_tool(tool, arguments)
    value = response.get("structuredContent")
    if not isinstance(value, dict):
        raise AssertionError(f"{tool} did not return structuredContent.")
    if not value.get("ok"):
        raise AssertionError(f"{tool} failed: {value.get('error') or value.get('diagnostics')}")
    assert set(value.get("timing") or {}) == set(TIMING_FIELDS)
    assert all(isinstance(item, (int, float)) and item >= 0 for item in value["timing"].values())
    stage_total = sum(value["timing"][field] for field in value["timing"] if field != "total_ms")
    assert stage_total <= value["timing"]["total_ms"] + 1.0
    if arguments.get("inline_image"):
        images = [item for item in response.get("content") or [] if item.get("type") == "image"]
        assert len(images) == 1
        assert images[0].get("mimeType") == "image/png"
        assert images[0].get("data")
    return value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Verify the current UEPI source against a live matching Editor bridge.")
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--viewport-width", type=int, default=640)
    parser.add_argument("--viewport-height", type=int, default=360)
    args = parser.parse_args(argv)

    project_file = args.project.resolve()
    server = UEPIMCPServer(
        SimpleNamespace(
            project=project_file,
            store=None,
            db=None,
            token_budget=4000,
            tool_profile="codex",
            include_output_schema=False,
            trace_file=None,
        )
    )
    guard = {"expected_project_file": str(project_file), "max_payload_bytes": 32768}

    status = _value(server, "uepi_status", guard)
    assert status["editor"]["connected"] is True
    assert status["editor"]["active_map"]
    assert status["result"]["doctor"]["catalog_current"] is True
    assert isinstance(status["result"]["doctor"]["catalog_cache_current"], bool)

    world_page = _value(
        server,
        "uepi_world",
        {
            **guard,
            "action": "read",
            "world": "editor",
            "include_components": False,
            "fields": ["actors.path", "actors.label"],
            "page_size": 4,
        },
    )
    assert len(world_page["result"]["actors"]) == 4
    assert world_page["continuation"]["has_more"] is True
    assert world_page["payload_bytes"] <= guard["max_payload_bytes"]

    world = _value(
        server,
        "uepi_world",
        {
            **guard,
            "action": "read",
            "world": "editor",
            "filters": {"labels": ["PlayerStart"]},
            "include_components": True,
            "page_size": 4,
        },
    )
    actors = world["result"]["actors"]
    assert len(actors) == 1
    assert actors[0]["label"] == "PlayerStart"
    actor_path = actors[0]["path"]

    actor = _value(
        server,
        "uepi_world",
        {
            **guard,
            "action": "actor",
            "world": "editor",
            "actor": actor_path,
            "property_names": ["ActorLabel", "Tags"],
        },
    )
    assert actor["result"]["actor"]["path"] == actor_path
    components = actor["result"]["actor"]["components"]
    assert components

    component = _value(
        server,
        "uepi_world",
        {
            **guard,
            "action": "component",
            "world": "editor",
            "actor": actor_path,
            "component": components[0]["name"],
            "property_names": ["ComponentTags"],
        },
    )
    assert component["result"]["component"]["owner"] == actor_path

    operation = _value(
        server,
        "uepi_schema",
        {
            **guard,
            "action": "edit_operation",
            "query": "content.create_asset",
            "include_examples": True,
        },
    )
    descriptor = operation["result"].get("operation") or {}
    assert descriptor.get("name") == "content.create_asset"
    assert descriptor["contract_hash"]
    assert descriptor["input_schema"]["additionalProperties"] is False
    assert descriptor["input_schema"]["required"]
    assert descriptor["examples"]

    viewport = _value(
        server,
        "uepi_editor",
        {
            **guard,
            "action": "viewport_capture",
            "viewport": "level",
            "width": args.viewport_width,
            "height": args.viewport_height,
            "include_camera_metadata": True,
            "inline_image": True,
        },
    )
    capture = viewport["result"]
    assert capture["width"] == args.viewport_width
    assert capture["height"] == args.viewport_height
    assert capture["camera"]
    artifact_path = Path(capture["artifact_path"])
    assert artifact_path.is_absolute(), capture
    assert artifact_path.is_file() and artifact_path.stat().st_size > 0, capture

    summary = {
        "ok": True,
        "project": status["project"]["project_name"],
        "editor_session_id": status["editor"]["session_id"],
        "active_map": status["editor"]["active_map"],
        "catalog_current": status["result"]["doctor"]["catalog_current"],
        "world_actor": actor_path,
        "operation_contract": descriptor["name"],
        "viewport": {
            "width": capture["width"],
            "height": capture["height"],
            "artifact_path": str(artifact_path),
            "bytes": artifact_path.stat().st_size,
        },
        "timing": {
            "status": status["timing"],
            "world_page": world_page["timing"],
            "world": world["timing"],
            "component": component["timing"],
            "schema": operation["timing"],
            "viewport": viewport["timing"],
        },
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
