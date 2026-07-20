from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
from types import SimpleNamespace
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Services" / "uepi" / "src"))

from uepi.mcp_server import UEPIMCPServer  # noqa: E402
from uepi.timing import TIMING_FIELDS  # noqa: E402


def _attribute(entity: dict[str, Any], key: str) -> Any:
    typed = entity.get("typed_attributes") if isinstance(entity.get("typed_attributes"), dict) else {}
    wrapped = typed.get(key)
    if isinstance(wrapped, dict) and "value" in wrapped:
        return wrapped["value"]
    if wrapped is not None and not isinstance(wrapped, dict):
        return wrapped
    attributes = entity.get("attributes") if isinstance(entity.get("attributes"), dict) else {}
    return attributes.get(key)


def _structured(server: UEPIMCPServer, tool: str, arguments: dict[str, Any]) -> dict[str, Any]:
    response = server.call_tool(tool, arguments)
    value = response.get("structuredContent")
    if not isinstance(value, dict):
        raise AssertionError(f"{tool} did not return structuredContent.")
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


def _value(server: UEPIMCPServer, tool: str, arguments: dict[str, Any]) -> dict[str, Any]:
    value = _structured(server, tool, arguments)
    if not value.get("ok"):
        raise AssertionError(f"{tool} failed: {value.get('error') or value.get('diagnostics')}")
    return value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Verify the current UEPI source against a live matching Editor bridge.")
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--viewport-width", type=int, default=640)
    parser.add_argument("--viewport-height", type=int, default=360)
    parser.add_argument("--warm-read-p95-ms", type=float, default=2000.0)
    parser.add_argument("--hard-scope-p95-ms", type=float, default=2000.0)
    parser.add_argument("--focused-node-p95-ms", type=float, default=500.0)
    parser.add_argument("--llmnpc-regression", action="store_true")
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
    assert status["result"]["cache"]["synced"] is True
    assert status["result"]["cache"]["generation"] == status["snapshot"]["view_generation"]
    assert status["result"]["plugin_version"]
    assert status["result"]["plugin_build_id"]
    assert status["result"]["catalog_hash"]
    assert status["result"]["service_version"]
    assert not any(item.get("code") == "UEPI_CACHE_SYNC_FAILED" for item in status["diagnostics"])

    discover_tool = next(tool for tool in server.tools() if tool["name"] == "uepi_edit_discover")
    assert {"cursor", "page_size", "max_payload_bytes", "fields", "exclude"}.issubset(discover_tool["inputSchema"]["properties"])

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

    warm_timings: list[float] = []
    for index in range(20):
        if index % 3 == 0:
            warm = _value(server, "uepi_status", guard)
        elif index % 3 == 1:
            warm = _value(
                server,
                "uepi_world",
                {**guard, "action": "read", "world": "editor", "include_components": False, "page_size": 4},
            )
        else:
            warm = _value(
                server,
                "uepi_schema",
                {**guard, "action": "edit_operation", "query": "content.create_asset"},
            )
        assert not any(item.get("code") == "UEPI_CACHE_SYNC_FAILED" for item in warm["diagnostics"])
        assert "WinError 5" not in json.dumps(warm, ensure_ascii=False)
        warm_timings.append(float(warm["timing"]["total_ms"]))
    sorted_timings = sorted(warm_timings)
    warm_p95_ms = sorted_timings[max(0, math.ceil(len(sorted_timings) * 0.95) - 1)]
    assert warm_p95_ms < args.warm_read_p95_ms, warm_timings

    llmnpc: dict[str, Any] | None = None
    if args.llmnpc_regression:
        slot = _value(
            server,
            "uepi_blueprint",
            {
                **guard,
                "asset": "/Game/LLMNPC/Animation/ABP_Manny1.ABP_Manny1",
                "graph": "AnimGraph",
                "node_classes": ["AnimGraphNode_Slot"],
            },
        )
        slot_nodes = slot["result"]["blueprint_entities"]
        assert any("AnimGraphNode_Slot_0" in str(item.get("canonical_key") or "") for item in slot_nodes), slot_nodes
        assert any(str(_attribute(item, "slot_name") or "") == "DefaultSlot" for item in slot_nodes)
        slot_node_guid = next((str(_attribute(item, "node_guid") or "") for item in slot_nodes if _attribute(item, "node_guid")), "")
        focused_timings: list[float] = []
        if slot_node_guid:
            for _ in range(20):
                focused = _value(
                    server,
                    "uepi_blueprint",
                    {
                        **guard,
                        "asset": "/Game/LLMNPC/Animation/ABP_Manny1.ABP_Manny1",
                        "node_guid": slot_node_guid,
                        "max_payload_bytes": 30000,
                    },
                )
                assert focused["result"]["focus"]["node_guid"] == slot_node_guid
                assert focused["payload_bytes"] < 30000
                assert focused["truncation"]["final_projection_complete"] is True, focused["truncation"]
                focused_timings.append(float(focused["timing"]["total_ms"]))
            focused_p95_ms = sorted(focused_timings)[max(0, math.ceil(len(focused_timings) * 0.95) - 1)]
            assert focused_p95_ms < args.focused_node_p95_ms, focused_timings
        else:
            focused_p95_ms = None

        no_match = _value(
            server,
            "uepi_blueprint",
            {
                **guard,
                "asset": "/Game/LLMNPC/Animation/ABP_Manny1.ABP_Manny1",
                "graph": "AnimGraph",
                "node_classes": ["DefinitelyMissingNodeClass"],
            },
        )
        assert no_match["result"]["blueprint_entities"] == []
        assert any(item.get("code") == "UEPI_BLUEPRINT_FILTER_NO_MATCH" for item in no_match["diagnostics"])
        assert not any(item.get("code") == "UEPI_REFRESH_REQUESTED" for item in no_match["diagnostics"])

        input_scope = [
            "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter",
            "/Game/LLMNPC/Blueprints/BP_LLMNPC_Manny.BP_LLMNPC_Manny",
        ]
        input_cases = [
            ("Trace key 3", "Three", [], "3"),
            ("Trace key 3, exclude LeftAlt", "Three", ["LeftAlt"], "3"),
            ("追踪按键 3 的效果，不要 LeftAlt", "Three", ["LeftAlt"], "3"),
            ("Trace key 3 and do not use LeftAlt", "Three", ["LeftAlt"], "3"),
            ("Trace LeftAlt, not Three", "LeftAlt", ["Three"], "leftalt"),
            ("Trace key 4", None, [], "4"),
            ("Trace keyboard key 4", None, [], "4"),
            ("Trace input Three and LeftAlt", None, [], ["Three", "LeftAlt"]),
            ("Trace gameplay input without a key", None, [], None),
        ]
        input_results: list[dict[str, Any]] = []
        for question, expected_key, excluded_keys, requested_input in input_cases:
            context = _structured(
                server,
                "uepi_context",
                {
                    **guard,
                    "question": question,
                    "route": "gameplay_input_to_effect",
                    "hard_scope": input_scope,
                    "max_items": 40,
                },
            )
            sections = context["result"].get("sections")
            assert isinstance(sections, dict), (question, sorted(context["result"]))
            assert sections["requested_input"] == requested_input, (question, sections)
            assert sections["resolved_input"] == expected_key, (question, sections)
            assert sections["excluded_input_keys"] == excluded_keys, (question, sections)
            if expected_key:
                assert context["ok"] is True, (question, context["error"])
                assert sections["selected_inputs"], (question, sections)
                assert all(item["input_key"] == expected_key for item in sections["cross_asset_paths"]), (question, sections)
                assert context["next_actions"], (question, context["next_actions"])
                assert all(item.get("arguments", {}).get("key") == expected_key for item in context["next_actions"]), (question, context["next_actions"])
            else:
                assert context["ok"] is (requested_input is None), (question, context["error"])
                assert sections["selected_inputs"] == [], (question, sections)
                assert sections["cross_asset_paths"] == [], (question, sections)
                assert sections["terminal_effects"] == [], (question, sections)
                assert context["result"]["matches"] == [], (question, context["result"]["matches"])
                assert context["result"]["relations"] == [], (question, context["result"]["relations"])
                assert context["next_actions"] == [], (question, context["next_actions"])
            input_results.append(
                {
                    "question": question,
                    "requested_input": sections["requested_input"],
                    "resolved_input": sections["resolved_input"],
                    "excluded_input_keys": sections["excluded_input_keys"],
                    "next_action_count": len(context["next_actions"]),
                }
            )

        hard_scope = [
            "/Game/LLMNPC/Blueprints/BP_LLMNPC_Manny",
            "/Game/LLMNPC/Animation/Waving",
            "/Game/LLMNPC/Animation/ABP_Manny1",
        ]
        animation_arguments = {
            **guard,
            "question": "Analyze the Waving animation sequence",
            "route": "animation_playback",
            "hard_scope": hard_scope,
            "max_items": 12,
            "fields": [
                "sections.animation_summary.asset",
                "sections.animation_summary.motion_summary",
                "sections.animation_summary.sequence.sequence_path",
                "sections.animation_summary.sequence.play_length_seconds",
            ],
            "max_payload_bytes": 131072,
        }
        hard_scope_timings: list[float] = []
        animation_context: dict[str, Any] | None = None
        for index in range(20):
            rotated = hard_scope[index % len(hard_scope) :] + hard_scope[: index % len(hard_scope)]
            animation_context = _value(server, "uepi_context", {**animation_arguments, "hard_scope": rotated})
            assert animation_context["result"]["query_source"] == "sqlite_cache"
            assert not any(item.get("code") == "UEPI_SLOW_OPERATION" for item in animation_context["diagnostics"])
            assert animation_context["result"]["sections"]["animation_summary"]["asset"]["canonical_key"] == "/Game/LLMNPC/Animation/Waving.Waving"
            assert animation_context["pre_projection_payload_bytes"] < 131072
            assert animation_context["truncation"]["final_projection_complete"] is True, animation_context["truncation"]
            hard_scope_timings.append(float(animation_context["timing"]["snapshot_query_ms"]))
        assert animation_context is not None
        hard_scope_p95_ms = sorted(hard_scope_timings)[max(0, math.ceil(len(hard_scope_timings) * 0.95) - 1)]
        assert hard_scope_p95_ms < args.hard_scope_p95_ms, hard_scope_timings
        animation_summary = animation_context["result"]["sections"]["animation_summary"]
        assert animation_summary["asset"]["canonical_key"] == "/Game/LLMNPC/Animation/Waving.Waving"
        assert animation_summary["motion_summary"] is not None
        assert animation_summary["sequence"] is not None
        llmnpc = {
            "slot_node_count": len(slot_nodes),
            "animation_asset": animation_summary["asset"]["canonical_key"],
            "input_cases": input_results,
            "hard_scope_p95_ms": hard_scope_p95_ms,
            "focused_node_p95_ms": focused_p95_ms,
        }

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
            "warm_read_p95_ms": warm_p95_ms,
        },
        "llmnpc": llmnpc,
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
