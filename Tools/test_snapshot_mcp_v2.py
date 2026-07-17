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
sys.path.insert(0, str(PYTHONPATH))
sys.path.insert(0, str(ROOT / "Tools"))

from uepi.bridge_client import _read_token  # noqa: E402
from uepi.diff import build_transaction_diff  # noqa: E402
from uepi.edit import _apply_timeout_seconds, _asset_values, _budget_diagnostics, _refresh_timeout_seconds, _transaction_budgets  # noqa: E402
from uepi.plan import canonical_plan_hash, verify_plan_hash  # noqa: E402
from uepi.projections import apply_response_options  # noqa: E402
from uepi.runtime import _approved_subset, _matches_approved, _value_at_path  # noqa: E402
from uepi.result import envelope  # noqa: E402
from uepi.store import resolve_store_root  # noqa: E402
from uepi_doctor import _capability_settings, _pid_alive  # noqa: E402

READ_TOOLS = {
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
    "uepi_editor",
    "uepi_world",
    "uepi_refresh",
    "uepi_schema",
    "uepi_runtime",
}

EDIT_TOOLS = {
    "uepi_edit_discover",
    "uepi_edit_preview",
    "uepi_edit_apply",
    "uepi_edit_validate",
    "uepi_edit_rollback",
}

EXPECTED_TOOLS = READ_TOOLS | EDIT_TOOLS


def assert_error_evidence_contract() -> None:
    response = envelope(
        tool="uepi_edit_apply",
        operation="apply",
        project={"project_id": "test", "project_name": "Test"},
        state={"freshness": "current"},
        result={"apply": {"atomicity_restored": True}, "atomicity_restored": True},
        error={"code": "UEPI_EDIT_APPLY_FAILED", "message": "Injected failure.", "retryable": False, "candidates": []},
    )
    assert response["ok"] is False
    assert response["result"]["atomicity_restored"] is True
    assert response["error"]["code"] == "UEPI_EDIT_APPLY_FAILED"


def assert_doctor_contract() -> None:
    assert _pid_alive(os.getpid())
    legacy = _capability_settings({"write_enabled": True, "allow_save": True})
    assert legacy["write_enabled"] is True
    assert legacy["max_assets_per_transaction"] == 0
    advertised = _capability_settings({"capabilities": ["edit.apply"], "allow_save": True})
    assert advertised["write_enabled"] is True
    disabled = _capability_settings({"capabilities": ["edit.discover"], "allow_save": True})
    assert disabled["write_enabled"] is False
    configured = _capability_settings(
        {"capabilities": ["edit.apply"], "allow_save": True},
        {"max_operations_per_transaction": 96, "max_assets_per_transaction": 12},
    )
    assert configured["max_operations_per_transaction"] == 96
    assert configured["max_assets_per_transaction"] == 12


def assert_edit_asset_scope_contract() -> None:
    source = "/Game/Source.Source"
    destination = "/Game/Copy.Copy"
    assert _asset_values({"asset": {"$ref": "copy#asset"}}, ["asset"], {"copy": destination}) == [destination]
    plan = {
        "transaction_id": "uepi-tx:scope-test",
        "affected_assets": [destination],
        "dependency_assets": [source],
        "before_fingerprints": [
            {"asset": source, "exists": True},
            {"asset": destination, "exists": False},
        ],
        "operations": [{"operation_id": "copy", "type": "content.duplicate_asset"}],
    }
    apply_result = {
        "operations": [
            {
                "operation_id": "copy",
                "type": "content.duplicate_asset",
                "detail": {"source": {"path": source}, "asset": {"path": destination}},
            }
        ],
        "saved": True,
        "validation_ok": True,
    }
    diff = build_transaction_diff(plan, apply_result, [{"asset": destination, "exists": True}])
    assert diff["created_assets"] == [destination]
    assert diff["removed_assets"] == []


def assert_edit_transaction_budget_contract() -> None:
    defaults = _transaction_budgets(None)
    assert defaults == {
        "max_operations": 96,
        "max_assets": 12,
        "high_risk_operations": 64,
        "high_risk_assets": 12,
    }
    configured = _transaction_budgets({
        "settings": {
            "max_operations_per_transaction": 160,
            "max_assets_per_transaction": 32,
            "high_risk_operation_threshold": 64,
            "high_risk_asset_threshold": 12,
        }
    })
    assert configured["max_operations"] == 160
    assert configured["max_assets"] == 32
    assert _budget_diagnostics(64, 12, configured) == []
    large = _budget_diagnostics(65, 13, configured)
    assert [item["code"] for item in large] == ["UEPI_EDIT_LARGE_ATOMIC_TRANSACTION"]
    over_limit = _budget_diagnostics(161, 33, configured)
    assert {item["code"] for item in over_limit} == {
        "UEPI_EDIT_TOO_MANY_OPERATIONS",
        "UEPI_EDIT_TOO_MANY_ASSETS",
        "UEPI_EDIT_LARGE_ATOMIC_TRANSACTION",
    }
    small_plan = {"operations": [{}] * 8, "affected_assets": ["/Game/A.A"]}
    large_plan = {"operations": [{}] * 96, "affected_assets": [f"/Game/A{index}.A{index}" for index in range(12)]}
    assert _apply_timeout_seconds(small_plan) == 31.0
    assert _apply_timeout_seconds(large_plan) > _apply_timeout_seconds(small_plan)
    assert _refresh_timeout_seconds(large_plan) > _refresh_timeout_seconds(small_plan)


def assert_envelope(value: dict[str, Any]) -> None:
    assert value["schema_version"] == "uepi.mcp-envelope.v2"
    assert "ok" in value
    assert "tool" in value
    assert "operation" in value
    assert isinstance(value.get("project"), dict)
    assert isinstance(value.get("editor"), dict)
    assert isinstance(value.get("snapshot"), dict)
    assert isinstance(value.get("diagnostics"), list)
    assert isinstance(value.get("evidence"), list)
    assert isinstance(value.get("next_actions"), list)
    assert "truncation" in value
    assert "item_limit_hit" in value["truncation"]
    assert "byte_limit_hit" in value["truncation"]
    assert "continuation" in value


def assert_response_budget_contract() -> None:
    base = envelope(
        tool="uepi_context",
        operation="context_route:auto",
        project={"project_id": "test", "project_name": "Test"},
        state={"freshness": "current", "saved_generation": 7},
        result={
            "matches": [{"id": f"asset-{index}", "canonical_key": f"/Game/A{index}.A{index}", "attributes": {"blob": "m" * 300}} for index in range(20)],
            "sections": {"asset_snapshot": {"entities": [{"blob": "s" * 1200} for _ in range(12)]}},
            "live_editor": {"output_log": ["log " + ("x" * 500) for _ in range(20)]},
        },
    )
    projected = apply_response_options(base, {"fields": ["matches.id"], "page_size": 4, "max_payload_bytes": 4096})
    assert set(projected["result"]) == {"matches"}
    assert len(projected["result"]["matches"]) <= 4
    assert projected["payload_bytes"] == len(json.dumps(projected, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
    assert projected["payload_bytes"] <= 4096
    assert projected["continuation"]["has_more"] is True

    schema_response = envelope(
        tool="uepi_schema",
        operation="asset_property",
        project={"project_id": "test", "project_name": "Test"},
        state={"freshness": "current", "saved_generation": 7},
        result={"properties": [{"name": f"P{index}", "schema": {"description": "p" * 400}} for index in range(30)]},
    )
    schema_page = apply_response_options(schema_response, {"page_size": 3, "max_payload_bytes": 4096})
    assert len(schema_page["result"]["properties"]) <= 3
    assert schema_page["continuation"]["has_more"] is True
    assert schema_page["payload_bytes"] <= 4096

    animation_response = envelope(
        tool="uepi_animation",
        operation="animation",
        project={"project_id": "test", "project_name": "Test"},
        state={"freshness": "current", "saved_generation": 7},
        result={
            "bone_motion_profile_manifest": {"artifact_uri": "uepi://animation-bone-motion-profile/test", "artifact_id": "test"},
            "bone_motion_profile": {"changed_bones": [{"bone_name": "hand_r", "samples": ["v" * 600 for _ in range(20)]}]},
        },
    )
    bounded_animation = apply_response_options(animation_response, {"fields": ["bone_motion_profile"], "max_payload_bytes": 4096})
    assert "bone_motion_profile_manifest" in bounded_animation["result"]
    assert "bone_motion_profile" not in bounded_animation["result"]
    assert bounded_animation["truncation"]["byte_limit_hit"] is True
    assert bounded_animation["payload_bytes"] <= 4096


def assert_plan_v2_contract() -> None:
    plan = {
        "schema_version": "uepi.edit_plan.v2",
        "transaction_id": "uepi-tx:test",
        "project": {"project_binding_id": "sha256:test", "project_file": "C:/Test/Test.uproject"},
        "editor": {"session_id": "test-session"},
        "base": {"catalog_hash": "sha256:catalog", "plugin_build_id": "test-build"},
        "operations": [],
        "operation_order": [],
        "predicted_touched_packages": [],
        "validation_plan": [],
        "save_policy": "after_validation",
        "risk": {"level": "low", "requires_user_approval": True},
        "approval": {"required": True, "nonce": "test-nonce"},
        "expires_at": "2099-01-01T00:00:00Z",
    }
    plan["plan_hash"] = canonical_plan_hash(plan)
    plan["approval"]["plan_hash"] = plan["plan_hash"]
    assert verify_plan_hash(plan)
    assert canonical_plan_hash(plan) == plan["plan_hash"]

    schema = json.loads((ROOT / "Schemas" / "edit-plan-v2.schema.json").read_text(encoding="utf-8"))
    assert {
        "project",
        "editor",
        "base",
        "operation_order",
        "predicted_touched_packages",
        "validation_plan",
        "save_policy",
        "risk",
        "approval",
    }.issubset(set(schema["required"]))


def assert_runtime_ticket_contract() -> None:
    target = {"class": "/Script/Test.MotionComponent"}
    approved = {
        "target": target,
        "function": "SubmitTemplate",
        "arguments": {"TemplateId": {"type": "name", "value": "gesture.wave"}},
    }
    requested = _approved_subset(
        {**approved, "ticket_id": "ignored", "timeout_seconds": 5},
        ("object_path", "target", "function", "arguments"),
    )
    assert _matches_approved(requested, [approved])
    assert not _matches_approved({**requested, "function": "OtherFunction"}, [approved])
    observed = {"outputs": {"return_value": {"fields": {"bPlaying": {"value": True}}}}}
    assert _value_at_path(observed, "outputs.return_value.fields.bPlaying.value") is True
    assert _value_at_path(observed, "outputs.return_value.fields.Missing.value") is None


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
    animation_motion_artifacts = store / "artifacts" / "animation_bone_motion"
    reconstruction_artifacts = store / "artifacts" / "animation_reconstruction"
    full_pose_artifacts = store / "artifacts" / "animation_full_pose_samples"
    manifests = store / "manifests"
    sessions = store / "sessions"
    objects.mkdir(parents=True)
    animation_motion_artifacts.mkdir(parents=True)
    reconstruction_artifacts.mkdir(parents=True)
    full_pose_artifacts.mkdir(parents=True)
    manifests.mkdir(parents=True)
    sessions.mkdir(parents=True)

    asset_id = "asset-bp-hero"
    animation_asset_id = "asset-waving"
    animation_sequence_id = "anim-sequence-waving"
    animation_artifact_id = "a" * 64
    reconstruction_artifact_id = "b" * 64
    full_pose_artifact_id = "c" * 64
    project_entity_id = "project-root"
    node_id = "node-beginplay"
    relation_id = "rel-contains-node"
    animation_relation_id = "rel-animation-asset-sequence"
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

    animation_profile_path = animation_motion_artifacts / f"{animation_artifact_id}.json"
    animation_profile = {
        "schema_version": "uepi.animation_bone_motion_profile.v1",
        "artifact_id": animation_artifact_id,
        "artifact_uri": f"uepi://animation-bone-motion-profile/{animation_artifact_id}",
        "intended_reader": "LLM",
        "sequence_path": "/Game/Animations/Waving.Waving",
        "sequence_name": "Waving",
        "skeleton_path": "/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin",
        "analysis_state": "ready",
        "coordinate_space_note": "local_transform is parent-relative; component_transform is skeleton component-space in Unreal centimeters.",
        "play_length_seconds": 1.0,
        "analysis_sample_count": 3,
        "bone_count": 2,
        "changed_bone_count": 1,
        "position_changed_bone_count": 1,
        "driver_bone_count": 1,
        "inherited_motion_bone_count": 0,
        "motion_intent_summary": "Primary local driver bones: hand_r. Use these before inherited end-effectors when generating procedural animation.",
        "sample_frames": [
            {"index": 0, "frame_number": 0, "time_seconds": 0.0, "normalized_time": 0.0},
            {"index": 1, "frame_number": 15, "time_seconds": 0.5, "normalized_time": 0.5},
            {"index": 2, "frame_number": 30, "time_seconds": 1.0, "normalized_time": 1.0},
        ],
        "initial_pose": [],
        "end_pose": [],
        "changed_bones": [
            {
                "bone_index": 1,
                "bone_name": "hand_r",
                "parent_index": 0,
                "parent_name": "upperarm_r",
                "has_direct_track": True,
                "track_index": 0,
                "position_changes": True,
                "rotation_changes": True,
                "motion_role": "direct_driver",
                "generation_priority": "primary_driver",
                "driver_score": 55.0,
                "component_motion_score": 72.55,
                "inherited_motion_score": 0.0,
                "change_channels": ["component_translation", "component_rotation", "local_rotation_track"],
                "component_translation_range": 24.0,
                "component_displacement_length": 4.0,
                "component_path_length": 48.0,
                "component_max_step": 24.0,
                "component_rotation_range_degrees": 55.0,
                "component_translation_delta_start_to_end": {
                    "vector": {"x": 0.0, "y": 4.0, "z": 0.0},
                    "length": 4.0,
                    "dominant_axis": "+Y",
                },
                "samples": [
                    {
                        "frame_number": 0,
                        "time_seconds": 0.0,
                        "normalized_time": 0.0,
                        "component_translation_delta_from_initial": {
                            "vector": {"x": 0.0, "y": 0.0, "z": 0.0},
                            "length": 0.0,
                            "dominant_axis": "none",
                        },
                    },
                    {
                        "frame_number": 15,
                        "time_seconds": 0.5,
                        "normalized_time": 0.5,
                        "component_translation_delta_from_initial": {
                            "vector": {"x": 0.0, "y": 24.0, "z": 0.0},
                            "length": 24.0,
                            "dominant_axis": "+Y",
                        },
                    },
                ],
                "llm_summary": "hand_r moves in a waving arc.",
            }
        ],
        "driver_bones": [
            {
                "rank": 0,
                "bone_index": 1,
                "bone_name": "hand_r",
                "parent_index": 0,
                "parent_name": "upperarm_r",
                "motion_role": "direct_driver",
                "generation_priority": "primary_driver",
                "driver_score": 55.0,
                "component_motion_score": 72.55,
                "inherited_motion_score": 0.0,
                "position_changes": True,
                "direct_local_motion": True,
                "inherited_component_motion": False,
                "intent_group": "right_arm_hand",
                "skeleton_chain": [
                    {"bone_index": 0, "bone_name": "upperarm_r"},
                    {"bone_index": 1, "bone_name": "hand_r"},
                ],
                "component_translation_range": 24.0,
                "component_path_length": 48.0,
                "component_rotation_range_degrees": 55.0,
                "local_translation_range": 0.0,
                "local_rotation_range_degrees": 55.0,
            }
        ],
        "inherited_motion_bones": [],
        "motion_intent_groups": [
            {
                "id": "right_arm_hand",
                "label": "Right arm and hand",
                "description": "Use these bones first for right-sided arm, hand, finger, or gesture motion.",
                "driver_count": 1,
                "inherited_motion_count": 0,
                "driver_score": 55.0,
                "component_motion_score": 72.55,
                "dominant_driver_bones": ["hand_r"],
                "dominant_inherited_bones": [],
            }
        ],
        "llm_generation_guidelines": [
            "Start procedural generation from driver_bones; they are locally keyed controls sorted by local motion strength.",
            "Use changed_bones samples as sparse keyframes.",
        ],
    }
    animation_profile_path.write_text(json.dumps(animation_profile, ensure_ascii=False), encoding="utf-8")

    full_pose_path = full_pose_artifacts / f"{full_pose_artifact_id}.json"
    full_pose_profile = {
        "schema_version": "uepi.animation_full_pose_samples.v1",
        "artifact_id": full_pose_artifact_id,
        "artifact_uri": f"uepi://animation-full-pose-samples/{full_pose_artifact_id}",
        "intended_reader": "tool_or_llm_when_high_fidelity_pose_data_is_needed",
        "sequence_path": "/Game/Animations/Waving.Waving",
        "sequence_name": "Waving",
        "skeleton_path": "/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin",
        "analysis_state": "ready",
        "sample_policy": "all_animation_keys",
        "source_key_count": 3,
        "source_frame_count": 2,
        "sample_count": 2,
        "bone_count": 2,
        "samples": [
            {
                "frame_number": 0,
                "time_seconds": 0.0,
                "normalized_time": 0.0,
                "bone_count": 2,
                "bones": [
                    {"index": 0, "bone_name": "upperarm_r", "local_transform": {}, "component_transform": {}},
                    {"index": 1, "bone_name": "hand_r", "local_transform": {}, "component_transform": {}},
                ],
            },
            {
                "frame_number": 30,
                "time_seconds": 1.0,
                "normalized_time": 1.0,
                "bone_count": 2,
                "bones": [
                    {"index": 0, "bone_name": "upperarm_r", "local_transform": {}, "component_transform": {}},
                    {"index": 1, "bone_name": "hand_r", "local_transform": {}, "component_transform": {}},
                ],
            },
        ],
    }
    full_pose_path.write_text(json.dumps(full_pose_profile, ensure_ascii=False), encoding="utf-8")
    full_pose_manifest = {
        "schema_version": "uepi.animation_full_pose_samples_manifest.v1",
        "profile_schema_version": "uepi.animation_full_pose_samples.v1",
        "artifact_id": full_pose_artifact_id,
        "artifact_uri": f"uepi://animation-full-pose-samples/{full_pose_artifact_id}",
        "storage": "snapshot_store_artifact_json",
        "path": str(full_pose_path),
        "sequence_path": "/Game/Animations/Waving.Waving",
        "skeleton_path": "/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin",
        "sample_policy": "all_animation_keys",
        "sample_count": 2,
        "bone_count": 2,
        "byte_count": full_pose_path.stat().st_size,
        "encoding": "json",
    }
    reconstruction_path = reconstruction_artifacts / f"{reconstruction_artifact_id}.json"
    reconstruction_profile = {
        "schema_version": "uepi.animation_reconstruction_profile.v1",
        "artifact_id": reconstruction_artifact_id,
        "artifact_uri": f"uepi://animation-reconstruction-profile/{reconstruction_artifact_id}",
        "intended_reader": "LLM_or_tool_generating_procedural_animation",
        "reconstruction_goal": "Recreate this animation by driving selected local FK bone tracks.",
        "sequence_path": "/Game/Animations/Waving.Waving",
        "sequence_name": "Waving",
        "skeleton_path": "/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin",
        "analysis_state": "ready",
        "play_length_seconds": 1.0,
        "sampling_frame_rate": "30/1",
        "source_key_count": 3,
        "source_frame_count": 2,
        "driver_curve_count": 1,
        "driver_key_count": 3,
        "driver_key_sample_policy": "all_animation_keys",
        "candidate_driver_bone_count": 1,
        "recommended_driver_bones": ["hand_r"],
        "motion_intent_groups": [{"id": "right_arm_hand", "dominant_driver_bones": ["hand_r"]}],
        "phase_estimates": [{"name": "main_motion", "normalized_start": 0.2, "normalized_end": 0.8}],
        "driver_track_curves": [
            {
                "rank": 0,
                "bone_name": "hand_r",
                "bone_index": 1,
                "parent_index": 0,
                "intent_group": "right_arm_hand",
                "track_index": 0,
                "driver_score": 55.0,
                "source_raw_key_count": 3,
                "key_count": 3,
                "local_translation_range": 0.0,
                "local_rotation_range_degrees": 55.0,
                "local_scale_range": 0.0,
                "rotation_extrema_count": 2,
                "curve_semantics": "oscillating_rotation",
                "programmatic_recipe_hint": "Use these local FK keys directly.",
                "skeleton_chain": [
                    {"bone_index": 0, "bone_name": "upperarm_r"},
                    {"bone_index": 1, "bone_name": "hand_r"},
                ],
                "keyframes": [
                    {"frame_number": 0, "time_seconds": 0.0, "normalized_time": 0.0, "local_transform": {}},
                    {"frame_number": 15, "time_seconds": 0.5, "normalized_time": 0.5, "local_transform": {}},
                    {"frame_number": 30, "time_seconds": 1.0, "normalized_time": 1.0, "local_transform": {}},
                ],
            }
        ],
        "reconstruction_guidelines": ["For programmatic recreation, drive driver_track_curves over normalized_time."],
        "full_pose_sample_artifact": full_pose_manifest,
    }
    reconstruction_path.write_text(json.dumps(reconstruction_profile, ensure_ascii=False), encoding="utf-8")
    reconstruction_manifest = {
        "schema_version": "uepi.animation_reconstruction_profile_manifest.v1",
        "profile_schema_version": "uepi.animation_reconstruction_profile.v1",
        "artifact_id": reconstruction_artifact_id,
        "artifact_uri": f"uepi://animation-reconstruction-profile/{reconstruction_artifact_id}",
        "storage": "snapshot_store_artifact_json",
        "path": str(reconstruction_path),
        "sequence_path": "/Game/Animations/Waving.Waving",
        "skeleton_path": "/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin",
        "driver_curve_count": 1,
        "driver_key_count": 3,
        "full_pose_artifact_uri": f"uepi://animation-full-pose-samples/{full_pose_artifact_id}",
        "full_pose_sample_count": 2,
        "byte_count": reconstruction_path.stat().st_size,
        "encoding": "json",
    }
    bone_motion_profile_manifest = {
        "schema_version": "uepi.animation_bone_motion_profile_manifest.v1",
        "profile_schema_version": "uepi.animation_bone_motion_profile.v1",
        "artifact_id": animation_artifact_id,
        "artifact_uri": f"uepi://animation-bone-motion-profile/{animation_artifact_id}",
        "storage": "snapshot_store_artifact_json",
        "path": str(animation_profile_path),
        "sequence_path": "/Game/Animations/Waving.Waving",
        "skeleton_path": "/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin",
        "analysis_sample_count": 3,
        "bone_count": 2,
        "changed_bone_count": 1,
        "position_changed_bone_count": 1,
        "driver_bone_count": 1,
        "inherited_motion_bone_count": 0,
        "byte_count": animation_profile_path.stat().st_size,
        "encoding": "json",
    }
    animation_asset_entity = {
        "id": animation_asset_id,
        "kind": "asset",
        "canonical_key": "/Game/Animations/Waving.Waving",
        "display_name": "Waving",
        "source_layer": "asset_registry",
        "attributes": {"object_path": "/Game/Animations/Waving.Waving", "asset_name": "Waving"},
        "completeness": {"state": "partial", "covered": ["asset_registry_metadata", "animation_sequence"], "omitted": [], "warnings": []},
        "diagnostics": [],
        "evidence": [],
        "snapshot": {
            "animation_sequence": {
                "schema_version": "uepi.anim_sequence.v1",
                "source_layer": "animation_data_model",
                "sequence_path": "/Game/Animations/Waving.Waving",
                "skeleton_path": "/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin",
                "play_length_seconds": 1.0,
                "sample_key_count": 31,
                "sampling_frame_rate": "30/1",
                "rate_scale": 1.0,
                "loop": False,
                "bone_track_count": 1,
                "float_curve_count": 0,
                "transform_curve_count": 0,
                "attribute_count": 0,
                "notify_count": 0,
                "sampled_pose_count": 0,
                "tracks": [
                    {
                        "id": "track-waving-hand-r",
                        "index": 0,
                        "bone_name": "hand_r",
                        "raw_local_key_count": 31,
                    }
                ],
                "motion_summary": {
                    "schema_version": "uepi.animation_motion_summary.v1",
                    "sequence_path": "/Game/Animations/Waving.Waving",
                    "track_count": 1,
                    "changing_bone_count": 1,
                    "static_bone_count": 0,
                    "changing_bones": [],
                },
                "bone_motion_profile": bone_motion_profile_manifest,
                "reconstruction_profile": reconstruction_manifest,
                "notifies": [],
                "pose_samples": [],
            }
        },
    }
    animation_sequence_entity = {
        "id": animation_sequence_id,
        "kind": "animation_sequence",
        "canonical_key": "/Game/Animations/Waving.Waving",
        "display_name": "Waving",
        "source_layer": "animation_data_model",
        "attributes": {
            "sequence_path": "/Game/Animations/Waving.Waving",
            "bone_motion_profile_artifact_uri": f"uepi://animation-bone-motion-profile/{animation_artifact_id}",
            "bone_motion_profile_driver_bone_count": "1",
            "bone_motion_profile_inherited_motion_bone_count": "0",
            "reconstruction_profile_artifact_uri": f"uepi://animation-reconstruction-profile/{reconstruction_artifact_id}",
            "reconstruction_profile_driver_curve_count": "1",
            "reconstruction_profile_driver_key_count": "3",
            "reconstruction_profile_full_pose_artifact_uri": f"uepi://animation-full-pose-samples/{full_pose_artifact_id}",
        },
        "completeness": {"state": "partial", "covered": ["motion_summary", "bone_motion_profile_artifact", "reconstruction_profile_artifact"], "omitted": [], "warnings": []},
        "diagnostics": [],
        "evidence": [],
    }
    animation_fragment = {
        "schema_version": "uepi.asset-fragment.v2",
        "project_id": "project-fixture",
        "project_name": "FixtureProject",
        "project_file": str(root / "FixtureProject.uproject"),
        "engine_version": "5.3.2",
        "source_scan_finished_at_utc": "2026-06-26T00:00:01Z",
        "asset": {"id": animation_asset_id, "canonical_key": "/Game/Animations/Waving.Waving", "display_name": "Waving", "kind": "asset"},
        "entities": [animation_asset_entity, animation_sequence_entity],
        "relations": [
            {
                "id": animation_relation_id,
                "type": "contains_animation_sequence",
                "from_id": animation_asset_id,
                "to_id": animation_sequence_id,
                "source_layer": "animation_data_model",
                "derived": False,
                "confidence": 1.0,
                "attributes": {},
                "evidence": [],
            }
        ],
        "diagnostics": [],
    }
    animation_fragment_path = objects / "aaanimationfragment.json"
    animation_fragment_path.write_text(json.dumps(animation_fragment, ensure_ascii=False), encoding="utf-8")

    manifest = {
        "schema_version": "uepi.snapshot-manifest.v2",
        "data_mode": "saved",
        "writer_mode": "test",
        "session_id": "",
        "generation": 1,
        "created_at_utc": "2026-06-26T00:00:02Z",
        "project": {"id": "project-fixture", "name": "FixtureProject", "project_file": str(root / "FixtureProject.uproject"), "engine_version": "5.3.2"},
        "counts": {"entities": 4, "relations": 2, "diagnostics": 0, "asset_entities": 2},
        "source": {},
        "completeness": {"state": "partial", "covered": ["blueprint_graphs"], "omitted": [], "warnings": []},
        "asset_entity_ids": [asset_id, animation_asset_id],
        "fragments": [
            {"kind": "project_fragment", "schema_version": "uepi.project-fragment.v2", "hash": "aaprojectfragment", "path": str(project_fragment_path)},
            {"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aaassetfragment", "path": str(asset_fragment_path), "asset_id": asset_id},
            {"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aaanimationfragment", "path": str(animation_fragment_path), "asset_id": animation_asset_id},
        ],
    }
    (manifests / "saved.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
    (manifests / "saved-1.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")

    live_node_id = "node-live-print"
    live_relation_id = "rel-live-contains-node"
    deleted_asset_id = "asset-bp-deleted"
    deleted_node_id = "node-deleted"
    renamed_old_asset_id = "asset-bp-old-name"
    renamed_new_asset_id = "asset-bp-new-name"
    metadata_only_asset_id = "asset-bp-metadata-only"
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
    renamed_old_fragment = {
        "schema_version": "uepi.asset-fragment.v2",
        "project_id": "project-fixture",
        "project_name": "FixtureProject",
        "project_file": str(root / "FixtureProject.uproject"),
        "engine_version": "5.3.2",
        "source_scan_finished_at_utc": "2026-06-26T00:00:01Z",
        "asset": {"id": renamed_old_asset_id, "canonical_key": "/Game/BP_OldName.BP_OldName", "display_name": "BP_OldName", "kind": "asset"},
        "entities": [
            {
                "id": renamed_old_asset_id,
                "kind": "asset",
                "canonical_key": "/Game/BP_OldName.BP_OldName",
                "display_name": "BP_OldName",
                "source_layer": "asset_registry",
                "attributes": {"object_path": "/Game/BP_OldName.BP_OldName", "asset_name": "BP_OldName"},
                "completeness": {"state": "partial", "covered": ["asset_registry_metadata"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            }
        ],
        "relations": [],
        "diagnostics": [],
    }
    renamed_old_path = objects / "aarenamedoldfragment.json"
    renamed_old_path.write_text(json.dumps(renamed_old_fragment, ensure_ascii=False), encoding="utf-8")
    renamed_tombstone = {
        "schema_version": "uepi.asset-tombstone.v2",
        "project_id": "project-fixture",
        "project_name": "FixtureProject",
        "project_file": str(root / "FixtureProject.uproject"),
        "engine_version": "5.3.2",
        "created_at_utc": "2026-06-26T00:00:05Z",
        "asset_key": "/Game/BP_OldName.BP_OldName",
        "asset_name": "BP_OldName",
        "asset_id": renamed_old_asset_id,
        "package_name": "/Game/BP_OldName",
        "class_path": "/Script/Engine.Blueprint",
        "reason": "asset_renamed_old_path",
        "event_type": "asset_renamed",
        "old_object_path": "/Game/BP_OldName.BP_OldName",
        "new_object_path": "/Game/BP_NewName.BP_NewName",
        "source_event_sequence": 4,
    }
    renamed_tombstone_path = objects / "aarenamedtombstone.json"
    renamed_tombstone_path.write_text(json.dumps(renamed_tombstone, ensure_ascii=False), encoding="utf-8")
    renamed_new_fragment = {
        "schema_version": "uepi.asset-fragment.v2",
        "project_id": "project-fixture",
        "project_name": "FixtureProject",
        "project_file": str(root / "FixtureProject.uproject"),
        "engine_version": "5.3.2",
        "source_scan_finished_at_utc": "2026-06-26T00:00:06Z",
        "asset": {"id": renamed_new_asset_id, "canonical_key": "/Game/BP_NewName.BP_NewName", "display_name": "BP_NewName", "kind": "asset"},
        "entities": [
            {
                "id": renamed_new_asset_id,
                "kind": "asset",
                "canonical_key": "/Game/BP_NewName.BP_NewName",
                "display_name": "BP_NewName",
                "source_layer": "asset_registry",
                "attributes": {"object_path": "/Game/BP_NewName.BP_NewName", "asset_name": "BP_NewName"},
                "completeness": {"state": "partial", "covered": ["asset_registry_metadata"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            }
        ],
        "relations": [],
        "diagnostics": [],
    }
    renamed_new_path = objects / "aarenamednewfragment.json"
    renamed_new_path.write_text(json.dumps(renamed_new_fragment, ensure_ascii=False), encoding="utf-8")
    metadata_only_fragment = {
        "schema_version": "uepi.asset-fragment.v2",
        "project_id": "project-fixture",
        "project_name": "FixtureProject",
        "project_file": str(root / "FixtureProject.uproject"),
        "engine_version": "5.3.2",
        "source_scan_finished_at_utc": "2026-06-26T00:00:01Z",
        "asset": {"id": metadata_only_asset_id, "canonical_key": "/Game/BP_MetadataOnly.BP_MetadataOnly", "display_name": "BP_MetadataOnly", "kind": "asset"},
        "entities": [
            {
                "id": metadata_only_asset_id,
                "kind": "asset",
                "canonical_key": "/Game/BP_MetadataOnly.BP_MetadataOnly",
                "display_name": "BP_MetadataOnly",
                "source_layer": "asset_registry",
                "attributes": {"object_path": "/Game/BP_MetadataOnly.BP_MetadataOnly", "asset_name": "BP_MetadataOnly"},
                "completeness": {"state": "metadata_only", "covered": ["asset_registry_metadata"], "omitted": ["loaded_uobject_properties"], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            }
        ],
        "relations": [
            {
                "id": "rel-metadata-only-hard-reference",
                "type": "hard_references",
                "from_id": metadata_only_asset_id,
                "to_id": asset_id,
                "source_layer": "asset_registry",
                "derived": False,
                "confidence": 1.0,
                "attributes": {},
                "evidence": [],
            }
        ],
        "diagnostics": [],
    }
    metadata_only_path = objects / "aametadataonlyfragment.json"
    metadata_only_path.write_text(json.dumps(metadata_only_fragment, ensure_ascii=False), encoding="utf-8")
    live_manifest = dict(manifest)
    manifest["fragments"].extend(
        [
            {"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aadeletedassetfragment", "path": str(deleted_object_path), "asset_id": deleted_asset_id},
            {"kind": "asset_tombstone", "schema_version": "uepi.asset-tombstone.v2", "hash": "aadeletedtombstone", "path": str(deleted_tombstone_path), "asset_id": deleted_asset_id, "asset_key": "/Game/BP_Deleted.BP_Deleted"},
            {"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aarenamedoldfragment", "path": str(renamed_old_path), "asset_id": renamed_old_asset_id},
            {"kind": "asset_tombstone", "schema_version": "uepi.asset-tombstone.v2", "hash": "aarenamedtombstone", "path": str(renamed_tombstone_path), "asset_id": renamed_old_asset_id, "asset_key": "/Game/BP_OldName.BP_OldName"},
            {"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aarenamednewfragment", "path": str(renamed_new_path), "asset_id": renamed_new_asset_id},
            {"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aametadataonlyfragment", "path": str(metadata_only_path), "asset_id": metadata_only_asset_id},
        ]
    )
    (manifests / "saved.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
    (manifests / "saved-1.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
    live_manifest.update(
        {
            "data_mode": "live",
            "writer_mode": "editor",
            "session_id": "session-live",
            "generation": 2,
            "base_saved_generation": 1,
            "is_overlay": True,
            "merge_strategy": "replace",
            "target_object_paths": ["/Game/BP_Hero.BP_Hero"],
            "fragments": [{"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aaliveassetfragment", "path": str(live_object_path), "asset_id": asset_id}],
        }
    )
    (manifests / "live.json").write_text(json.dumps(live_manifest, ensure_ascii=False), encoding="utf-8")
    (manifests / "live-2.json").write_text(json.dumps(live_manifest, ensure_ascii=False), encoding="utf-8")
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


def assert_bridge_token_and_registry_resolution(root: Path) -> None:
    sessions = root / "store" / "sessions"
    session_path = sessions / "editor-bridge.json"
    token_path = sessions / "editor-bridge-token.txt"
    token_path.write_text("secret-token", encoding="utf-8")
    assert _read_token({"token_path": "../../bad/editor-bridge-token.txt"}, session_path) == "secret-token"

    registry = root / "registry"
    registry.mkdir(parents=True)
    (registry / "FixtureProject-abcd1234.json").write_text(
        json.dumps(
            {
                "schema_version": "uepi.editor-bridge-session.v1",
                "active": True,
                "state": "active",
                "transport_ready": True,
                "project_name": "FixtureProject",
                "project_file": str(root.parent / "FixtureProject" / "FixtureProject.uproject"),
                "project_root": str(root.parent / "FixtureProject"),
                "store_root": str(root),
                "last_heartbeat": datetime.now(timezone.utc).isoformat(),
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    old_registry = os.environ.get("UEPI_SESSION_REGISTRY_DIR")
    os.environ["UEPI_SESSION_REGISTRY_DIR"] = str(registry)
    try:
        assert resolve_store_root(project="FixtureProject") != root.resolve()
        assert resolve_store_root(project=root.parent / "FixtureProject" / "FixtureProject.uproject") == root.resolve()
    finally:
        if old_registry is None:
            os.environ.pop("UEPI_SESSION_REGISTRY_DIR", None)
        else:
            os.environ["UEPI_SESSION_REGISTRY_DIR"] = old_registry


def main() -> int:
    assert_error_evidence_contract()
    assert_doctor_contract()
    assert_edit_asset_scope_contract()
    assert_edit_transaction_budget_contract()
    assert_plan_v2_contract()
    assert_runtime_ticket_contract()
    assert_response_budget_contract()
    with tempfile.TemporaryDirectory(prefix="uepi_snapshot_mcp_") as temp_dir:
        root = Path(temp_dir)
        write_fixture(root)
        assert_bridge_token_and_registry_resolution(root)
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
            assert_envelope(status)
            assert status["snapshot"]["view_generation"] == 2
            assert status["editor"]["connected"] is False
            assert status["result"]["llm_readiness"]["requires_daemon"] is False
            assert status["result"]["llm_readiness"]["bridge_ready"] is False
            assert status["result"]["doctor"]["project_bound"] is True
            assert status["result"]["cache"]["synced"] is True
            assert status["result"]["cache"]["schema_version"] == "uepi.sqlite-cache.v2.1"
            overview = request(process, 45, "tools/call", {"name": "uepi_overview", "arguments": {"limit": 10}})["structuredContent"]
            assert_envelope(overview)
            assert "cpp_symbols" in overview["result"]
            diff = request(process, 47, "tools/call", {"name": "uepi_diff", "arguments": {"from_generation": 1, "to_generation": 2}})["structuredContent"]
            assert_envelope(diff)
            assert diff["ok"] is True
            assert diff["result"]["from_data_mode"] == "saved"
            assert diff["result"]["to_data_mode"] == "live"
            assert diff["result"]["to_manifest_path"].endswith(("live.json", "live-2.json"))
            assert "node-live-print" in diff["result"]["entities"]["added"]
            search = request(process, 4, "tools/call", {"name": "uepi_search", "arguments": {"query": "BP_Hero"}})["structuredContent"]
            assert_envelope(search)
            assert search["result"]["match_count"] >= 1
            assert search["result"]["query_source"] == "sqlite_cache"
            assert "typed_attributes" in search["result"]["matches"][0]
            animation = request(
                process,
                48,
                "tools/call",
                {"name": "uepi_animation", "arguments": {"asset": "/Game/Animations/Waving.Waving", "include": ["summary", "bone_motion_profile"]}},
            )["structuredContent"]
            assert_envelope(animation)
            assert animation["ok"] is True
            assert animation["result"]["bone_motion_profile_manifest"]["artifact_id"] == "a" * 64
            assert animation["result"]["bone_motion_profile"]["schema_version"] == "uepi.animation_bone_motion_profile.v1"
            assert animation["result"]["bone_motion_profile"]["changed_bones"][0]["bone_name"] == "hand_r"
            assert animation["result"]["bone_motion_profile"]["driver_bones"][0]["bone_name"] == "hand_r"
            assert animation["result"]["bone_motion_profile"]["driver_bones"][0]["motion_role"] == "direct_driver"
            assert animation["result"]["bone_motion_profile"]["motion_intent_groups"][0]["id"] == "right_arm_hand"
            reconstruction = request(
                process,
                49,
                "tools/call",
                {"name": "uepi_animation", "arguments": {"asset": "/Game/Animations/Waving.Waving", "include": ["driver_track_curves", "full_pose_artifact"]}},
            )["structuredContent"]
            assert_envelope(reconstruction)
            assert reconstruction["ok"] is True
            assert reconstruction["result"]["reconstruction_profile_manifest"]["artifact_id"] == "b" * 64
            assert reconstruction["result"]["driver_track_curves"][0]["bone_name"] == "hand_r"
            assert reconstruction["result"]["driver_track_curves"][0]["curve_semantics"] == "oscillating_rotation"
            assert reconstruction["result"]["full_pose_artifact"]["schema_version"] == "uepi.animation_full_pose_samples.v1"
            assert reconstruction["result"]["full_pose_artifact"]["sample_count"] == 2
            context = request(
                process,
                44,
                "tools/call",
                {
                    "name": "uepi_context",
                    "arguments": {"question": "What does BP_Hero BeginPlay blueprint node do?", "route": "blueprint_behavior", "max_items": 20},
                },
            )["structuredContent"]
            assert_envelope(context)
            assert context["tool"] == "uepi_context"
            assert context["operation"] == "context_route:blueprint_behavior"
            assert context["result"]["route"] == "blueprint_behavior"
            assert "blueprint_semantic_summary" in context["result"]["sections"]
            scoped_context = request(
                process,
                51,
                "tools/call",
                {
                    "name": "uepi_context",
                    "arguments": {
                        "question": "Find unrelated AudioWidgets content",
                        "route": "blueprint_behavior",
                        "hard_scope": ["/Game/BP_Hero.BP_Hero"],
                        "max_items": 20,
                    },
                },
            )["structuredContent"]
            assert_envelope(scoped_context)
            assert scoped_context["ok"] is True
            assert scoped_context["result"]["matches"]
            assert all(str(item.get("canonical_key") or "").startswith("/Game/BP_Hero.BP_Hero") for item in scoped_context["result"]["matches"])

            first_page = request(
                process,
                52,
                "tools/call",
                {"name": "uepi_blueprint", "arguments": {"asset": "/Game/BP_Hero.BP_Hero", "page_size": 1, "compact": True}},
            )["structuredContent"]
            assert_envelope(first_page)
            assert first_page["continuation"]["has_more"] is True
            assert first_page["continuation"]["cursor"]
            second_page = request(
                process,
                53,
                "tools/call",
                {
                    "name": "uepi_blueprint",
                    "arguments": {
                        "asset": "/Game/BP_Hero.BP_Hero",
                        "page_size": 1,
                        "compact": True,
                        "cursor": first_page["continuation"]["cursor"],
                    },
                },
            )["structuredContent"]
            assert_envelope(second_page)
            assert first_page["result"]["blueprint_entities"] != second_page["result"]["blueprint_entities"]

            fuzzy_exact = request(
                process,
                54,
                "tools/call",
                {"name": "uepi_animation", "arguments": {"asset": "Waving"}},
            )["structuredContent"]
            assert fuzzy_exact["ok"] is False
            assert fuzzy_exact["error"]["code"] == "UEPI_ASSET_NOT_FOUND"

            wrong_project = request(
                process,
                55,
                "tools/call",
                {"name": "uepi_status", "arguments": {"expected_project_file": str(root / "WrongProject.uproject")}},
            )["structuredContent"]
            assert wrong_project["ok"] is False
            assert wrong_project["error"]["code"] == "UEPI_PROJECT_MISMATCH"
            editor_status = request(process, 56, "tools/call", {"name": "uepi_editor", "arguments": {"action": "status"}})["structuredContent"]
            assert_envelope(editor_status)
            assert editor_status["result"]["editor"]["connected"] is False
            world_read = request(process, 57, "tools/call", {"name": "uepi_world", "arguments": {"action": "read", "world": "editor"}})["structuredContent"]
            assert world_read["ok"] is False
            assert world_read["error"]["code"] in {"UEPI_BRIDGE_NOT_CONFIGURED", "UEPI_BRIDGE_NOT_READY", "UEPI_WORLD_READ_FAILED"}
            refresh_request = request(
                process,
                58,
                "tools/call",
                {
                    "name": "uepi_refresh",
                    "arguments": {
                        "action": "request",
                        "targets": ["/Game/BP_Hero.BP_Hero"],
                        "domains": ["blueprint"],
                    },
                },
            )["structuredContent"]
            assert_envelope(refresh_request)
            refresh_value = refresh_request["result"]["request"]
            assert refresh_value["schema_version"] == "uepi.refresh-request.v2"
            assert refresh_value["status"] in {"queued", "running"}
            refresh_status = request(
                process,
                59,
                "tools/call",
                {"name": "uepi_refresh", "arguments": {"action": "status", "request_id": refresh_value["request_id"]}},
            )["structuredContent"]
            assert_envelope(refresh_status)
            assert refresh_status["result"]["request"]["request_id"] == refresh_value["request_id"]
            live_context = request(
                process,
                46,
                "tools/call",
                {
                    "name": "uepi_context",
                    "arguments": {"question": "What is selected in the editor?", "live": True, "max_items": 20},
                },
            )["structuredContent"]
            assert_envelope(live_context)
            assert "live_editor" in live_context["result"]["sections"]
            assert any(item.get("code") == "UEPI_BRIDGE_LIVE_CONTEXT_UNAVAILABLE" for item in live_context["diagnostics"])
            deleted_search = request(process, 40, "tools/call", {"name": "uepi_search", "arguments": {"query": "BP_Deleted"}})["structuredContent"]
            assert deleted_search["result"]["match_count"] == 0
            deleted_asset = request(process, 41, "tools/call", {"name": "uepi_asset", "arguments": {"asset": "/Game/BP_Deleted.BP_Deleted"}})["structuredContent"]
            assert_envelope(deleted_asset)
            assert deleted_asset["ok"] is False
            assert deleted_asset["error"]["code"] == "UEPI_ASSET_TOMBSTONED"
            renamed_old = request(process, 42, "tools/call", {"name": "uepi_asset", "arguments": {"asset": "/Game/BP_OldName.BP_OldName"}})["structuredContent"]
            assert_envelope(renamed_old)
            assert renamed_old["error"]["code"] == "UEPI_ASSET_TOMBSTONED"
            renamed_new = request(process, 43, "tools/call", {"name": "uepi_asset", "arguments": {"asset": "/Game/BP_NewName.BP_NewName"}})["structuredContent"]
            assert_envelope(renamed_new)
            assert renamed_new["result"]["entity"]["display_name"] == "BP_NewName"
            blueprint = request(process, 5, "tools/call", {"name": "uepi_blueprint", "arguments": {"asset": "/Game/BP_Hero.BP_Hero"}})["structuredContent"]
            assert_envelope(blueprint)
            assert blueprint["result"]["query_source"] == "sqlite_cache"
            assert blueprint["snapshot"]["freshness"] == "refresh_requested"
            assert blueprint["diagnostics"][0]["code"] == "UEPI_REFRESH_REQUESTED"
            blueprint_names = {item["display_name"] for item in blueprint["result"]["blueprint_entities"]}
            assert "Event BeginPlay" in blueprint_names
            assert "Asset Fragment Node" in blueprint_names
            assert "Live Print String" in blueprint_names
            assert any((root / "store" / "requests").glob("*.json"))
            assert "semantic_summary" in blueprint["result"]
            assert blueprint["result"]["semantic_summary"]["schema_version"] == "uepi.blueprint-semantic-summary.v1"
            metadata_blueprint = request(process, 50, "tools/call", {"name": "uepi_blueprint", "arguments": {"asset": "/Game/BP_MetadataOnly.BP_MetadataOnly"}})["structuredContent"]
            assert_envelope(metadata_blueprint)
            assert metadata_blueprint["snapshot"]["freshness"] == "refresh_requested"
            assert metadata_blueprint["diagnostics"][0]["code"] == "UEPI_REFRESH_REQUESTED"
            assert metadata_blueprint["diagnostics"][0]["target_object_path"] == "/Game/BP_MetadataOnly.BP_MetadataOnly"
            assert "blueprint_graph_entities_not_present_in_snapshot" in metadata_blueprint["omissions"]
            assert metadata_blueprint["result"]["blueprint_entities"] == []
            assert metadata_blueprint["result"]["relations"] == []
            assert "BP_Hero" not in " ".join(metadata_blueprint["result"]["semantic_summary"]["summary_lines"])
            metadata_requests = [
                json.loads(path.read_text(encoding="utf-8"))
                for path in (root / "store" / "requests").glob("*.json")
            ]
            assert any("/Game/BP_MetadataOnly.BP_MetadataOnly" in request.get("target_object_paths", []) for request in metadata_requests)
            unknown = request(process, 6, "tools/call", {"name": "uepi_missing", "arguments": {}})["structuredContent"]
            assert_envelope(unknown)
            assert unknown["tool"] == "uepi_missing"
            assert unknown["error"]["code"] == "UEPI_UNKNOWN_TOOL"
            assert set(unknown["error"]["candidates"]) == EXPECTED_TOOLS
            discover = request(process, 62, "tools/call", {"name": "uepi_edit_discover", "arguments": {}})["structuredContent"]
            assert_envelope(discover)
            assert discover["tool"] == "uepi_edit_discover"
            assert discover["result"]["profile"] == "codex"
            assert discover["result"]["legacy_profile_alias"] == "codex_write_alpha"
            assert discover["result"]["default_enabled"] is True
            assert discover["result"]["apply_enabled"] is False
            assert any(item.get("code") == "UEPI_EDIT_CATALOG_UNAVAILABLE" for item in discover["diagnostics"])
            wrong_session_discover = request(
                process,
                66,
                "tools/call",
                {
                    "name": "uepi_edit_discover",
                    "arguments": {"expected_editor_session_id": "00000000-0000-0000-0000-000000000000"},
                },
            )["structuredContent"]
            assert_envelope(wrong_session_discover)
            assert wrong_session_discover["ok"] is False
            assert wrong_session_discover["error"]["code"] == "UEPI_EDITOR_SESSION_MISMATCH"
            assert wrong_session_discover["result"] is None
            wrong_project_discover = request(
                process,
                67,
                "tools/call",
                {
                    "name": "uepi_edit_discover",
                    "arguments": {"expected_project_file": str(root / "WrongProject.uproject")},
                },
            )["structuredContent"]
            assert_envelope(wrong_project_discover)
            assert wrong_project_discover["ok"] is False
            assert wrong_project_discover["error"]["code"] == "UEPI_PROJECT_MISMATCH"
            assert wrong_project_discover["result"] is None
            preview = request(
                process,
                63,
                "tools/call",
                {
                    "name": "uepi_edit_preview",
                    "arguments": {
                        "intent": "Add a safe test variable",
                        "operations": [
                            {
                                "type": "blueprint.add_variable",
                                "params": {
                                    "asset": "/Game/BP_Hero.BP_Hero",
                                    "name": "Health",
                                    "pin_type": "float",
                                },
                            }
                        ],
                    },
                },
            )["structuredContent"]
            assert_envelope(preview)
            assert preview["ok"] is False
            assert preview["error"]["code"] == "UEPI_EDIT_CATALOG_UNAVAILABLE"
            assert not any(item.get("tool") == "uepi_edit_apply" for item in preview["next_actions"])
            unrolled_countdown_ops: list[dict[str, Any]] = []
            for value in range(5, 0, -1):
                unrolled_countdown_ops.append(
                    {
                        "type": "blueprint.add_print_string_node",
                        "params": {
                            "asset": "/Game/BP_Hero.BP_Hero",
                            "graph": "EventGraph",
                            "message": str(value),
                        },
                    }
                )
                if value > 1:
                    unrolled_countdown_ops.append(
                        {
                            "type": "blueprint.add_function_call_node",
                            "params": {
                                "asset": "/Game/BP_Hero.BP_Hero",
                                "graph": "EventGraph",
                                "function_path": "/Script/Engine.KismetSystemLibrary:Delay",
                                "defaults": {"Duration": 1.0},
                            },
                        }
                    )
            unrolled_preview = request(
                process,
                65,
                "tools/call",
                {
                    "name": "uepi_edit_preview",
                    "arguments": {
                        "intent": "Add a countdown",
                        "operations": unrolled_countdown_ops,
                    },
                },
            )["structuredContent"]
            assert_envelope(unrolled_preview)
            assert unrolled_preview["error"]["code"] == "UEPI_EDIT_CATALOG_UNAVAILABLE"
            apply_without_bridge = request(
                process,
                64,
                "tools/call",
                {
                    "name": "uepi_edit_apply",
                    "arguments": {"transaction_id": "uepi-tx:missing", "plan_hash": "sha256:missing", "approval_nonce": "missing", "approved": True},
                },
            )["structuredContent"]
            assert_envelope(apply_without_bridge)
            assert apply_without_bridge["ok"] is False
            assert apply_without_bridge["error"]["code"] == "UEPI_EDIT_PLAN_NOT_FOUND"
        finally:
            if process.stdin:
                process.stdin.close()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)

        write_process = subprocess.Popen(
            [sys.executable, "-B", str(SERVER), "--store", str(root), "--tool-profile", "codex_write_alpha"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        try:
            write_init = request(
                write_process,
                60,
                "initialize",
                {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "snapshot-test", "version": "1"}},
            )
            assert "resources" not in write_init["capabilities"]
            write_tools = request(write_process, 61, "tools/list")["tools"]
            write_names = {tool["name"] for tool in write_tools}
            assert write_names == EXPECTED_TOOLS
            discover = request(write_process, 62, "tools/call", {"name": "uepi_edit_discover", "arguments": {}})["structuredContent"]
            assert_envelope(discover)
            assert discover["tool"] == "uepi_edit_discover"
            assert discover["result"]["profile"] == "codex"
            assert discover["result"]["legacy_profile_alias"] == "codex_write_alpha"
            assert discover["result"]["default_enabled"] is True
            assert discover["result"]["apply_enabled"] is False
            preview = request(
                write_process,
                63,
                "tools/call",
                {
                    "name": "uepi_edit_preview",
                    "arguments": {
                        "intent": "Add a safe test variable",
                        "operations": [
                            {
                                "type": "blueprint.add_variable",
                                "params": {
                                    "asset": "/Game/BP_Hero.BP_Hero",
                                    "name": "Health",
                                    "pin_type": "float",
                                },
                            }
                        ],
                    },
                },
            )["structuredContent"]
            assert_envelope(preview)
            assert preview["ok"] is False
            assert preview["error"]["code"] == "UEPI_EDIT_CATALOG_UNAVAILABLE"
            rejected = request(
                write_process,
                64,
                "tools/call",
                {"name": "uepi_edit_apply", "arguments": {"transaction_id": "uepi-tx:missing", "plan_hash": "sha256:missing", "approval_nonce": "missing", "approved": True}},
            )["structuredContent"]
            assert_envelope(rejected)
            assert rejected["ok"] is False
            assert rejected["error"]["code"] == "UEPI_EDIT_PLAN_NOT_FOUND"
        finally:
            if write_process.stdin:
                write_process.stdin.close()
            try:
                write_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                write_process.kill()
                write_process.wait(timeout=5)
    print("snapshot MCP v2 assertions ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
