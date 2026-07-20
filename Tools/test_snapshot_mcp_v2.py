from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Any
from datetime import datetime, timezone
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "Services" / "uepi" / "src" / "uepi" / "mcp_server.py"
PYTHONPATH = ROOT / "Services" / "uepi" / "src"
sys.path.insert(0, str(PYTHONPATH))
sys.path.insert(0, str(ROOT / "Tools"))

from uepi.bridge_client import _read_token  # noqa: E402
from uepi.cache import cache_status, sync_cache  # noqa: E402
from uepi.context_routes import _best_domain_asset, _input_resolution, _normalized_asset_identity  # noqa: E402
from uepi.diff import build_transaction_diff  # noqa: E402
from uepi.edit import _apply_timeout_seconds, _asset_values, _authorization_decision, _budget_diagnostics, _refresh_timeout_seconds, _transaction_budgets, _transaction_report  # noqa: E402
from uepi.plan import canonical_plan_hash, verify_plan_hash  # noqa: E402
from uepi.projections import apply_response_options, enforce_response_budget  # noqa: E402
from uepi.query import make_engine  # noqa: E402
from uepi.recovery import inspect_recovery  # noqa: E402
from uepi.runtime import _approved_subset, _canonical_hash as runtime_plan_hash, _matches_approved, _ticket_from_plan, _unwrap_typed_value, _value_at_path  # noqa: E402
from uepi.result import envelope, tool_response  # noqa: E402
from uepi.store import SnapshotStore, resolve_store_root  # noqa: E402
from uepi.timing import attach_timing, begin_request, end_request, record  # noqa: E402
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
    "uepi_runtime_preview",
    "uepi_runtime_approve",
    "uepi_runtime",
    "uepi_recovery_inspect",
}

EDIT_TOOLS = {
    "uepi_edit_discover",
    "uepi_edit_preview",
    "uepi_edit_apply",
    "uepi_edit_validate",
    "uepi_edit_rollback",
    "uepi_recovery_finalize",
    "uepi_recovery_discard",
    "uepi_recovery_rollback",
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

    disconnected = build_transaction_diff(
        {"transaction_id": "uepi-tx:disconnect", "affected_assets": [source], "operations": []},
        {
            "operations": [
                {
                    "operation_id": "disconnect",
                    "type": "blueprint.disconnect_pins",
                    "detail": {
                        "link_changes": [
                            {
                                "change": "disconnected",
                                "source_node_guid": "NODE-A",
                                "source_pin_guid": "PIN-A",
                                "target_node_guid": "NODE-B",
                                "target_pin_guid": "PIN-B",
                            }
                        ]
                    },
                }
            ]
        },
        [],
    )
    assert disconnected["link_changes"][0]["change"] == "disconnected"
    assert disconnected["link_changes"][0]["source_pin_guid"] == "PIN-A"


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


def assert_authorization_policy_contract() -> None:
    binding_id = "sha256:project"
    catalog = {
        "settings": {
            "write_authorization": {
                "mode": "TrustedProject",
                "trusted_project_binding_id": binding_id,
                "allowed_asset_roots": ["/Game/LLMNPC", "/Game/ThirdPerson"],
                "allowed_operation_domains": ["blueprint", "animation"],
                "maximum_risk_level": "high",
                "allow_asset_delete": False,
                "allow_asset_rename": True,
                "allow_runtime_control": True,
                "maximum_assets_per_transaction": 4,
                "always_create_backup": True,
                "always_report_after_apply": True,
            }
        }
    }
    operations = [{"type": "blueprint.add_node", "params": {}}]
    descriptors = {"blueprint.add_node": {"domain": "blueprint", "risk": "medium"}}
    decision, diagnostics = _authorization_decision(
        catalog,
        {"project_binding_id": binding_id},
        "editor-session",
        operations,
        ["/Game/LLMNPC/BP_Test.BP_Test"],
        descriptors,
        "medium",
    )
    assert diagnostics == []
    assert decision["automatically_authorized"] is True
    rejected, rejected_diagnostics = _authorization_decision(
        catalog,
        {"project_binding_id": binding_id},
        "editor-session",
        operations,
        ["/Game/Outside/BP_Test.BP_Test"],
        descriptors,
        "medium",
    )
    assert rejected["policy_decision"] == "rejected"
    assert rejected_diagnostics[0]["code"] == "UEPI_EDIT_TRUST_POLICY_REJECTED"
    assert _unwrap_typed_value({"type": "bool", "value": False}) is False


def assert_post_action_report_contract() -> None:
    plan = {
        "transaction_id": "uepi-tx:report",
        "intent": "Update one Blueprint safely",
        "operations": [{"operation_id": "compile", "type": "blueprint.compile", "params": {"asset": "/Game/BP_Test.BP_Test"}}],
        "affected_assets": ["/Game/BP_Test.BP_Test"],
        "before_fingerprints": [{"asset": "/Game/BP_Test.BP_Test", "sha256": "sha256:before"}],
        "verification_plan": {"verification_mode": "hybrid"},
    }
    result = {
        "operations": [{"operation_id": "compile", "type": "blueprint.compile", "ok": True}],
        "compile": [{"asset": "/Game/BP_Test.BP_Test", "ok": True}],
        "validation_ok": True,
        "saved": True,
        "saved_file_hashes": [{"file": "Content/BP_Test.uasset", "md5": "after"}],
        "backup_directory": "Saved/UEProjectIntelligence/backups/report",
        "journal_path": "Saved/UEProjectIntelligence/transactions/report.applied.json",
        "atomicity_state": "applied",
        "affected_assets": ["/Game/BP_Test.BP_Test"],
    }
    runtime_ticket = {
        "ticket_id": "uepi-runtime-ticket:report",
        "verification_mode": "hybrid",
        "technical_verification": "agent_objective",
        "visual_acceptance": "human_required",
        "visual_status": "unreviewed",
        "visual_reviewer": "user",
        "human_test_steps": ["Observe the action in PIE."],
    }
    semantic_diff = {"changed_assets": ["/Game/BP_Test.BP_Test"]}
    report = _transaction_report(plan, result, {"mode": "TrustedProject"}, transaction_diff=semantic_diff, runtime_ticket=runtime_ticket)
    required = {
        "transaction_id", "intent", "operations", "affected_assets", "compiled_assets", "validation_result",
        "saved_assets", "semantic_diff", "backup_path", "rollback_available", "runtime_verification_result",
        "human_visual_verification_required",
    }
    assert required <= set(report)
    assert report["compiled_assets"] == ["/Game/BP_Test.BP_Test"]
    assert report["saved_assets"] == ["/Game/BP_Test.BP_Test"]
    assert report["semantic_diff"] == semantic_diff
    assert report["rollback_available"] is True
    assert report["human_visual_verification_required"] is True
    assert report["runtime_verification_result"]["visual_reviewer"] == "user"


def assert_recovery_inspection_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="uepi_recovery_") as temp_dir:
        project_dir = Path(temp_dir) / "FixtureProject"
        root = project_dir / "Saved" / "UEProjectIntelligence"
        store_dir = root / "store"
        transactions = store_dir / "transactions"
        transactions.mkdir(parents=True)
        package = project_dir / "Content" / "BP_Test.uasset"
        backup = store_dir / "backups" / "BP_Test.uasset"
        package.parent.mkdir(parents=True)
        backup.parent.mkdir(parents=True)
        package.write_bytes(b"changed")
        backup.write_bytes(b"before")
        marker = transactions / "tx.prepared.json"
        marker.write_text(
            json.dumps(
                {
                    "transaction_id": "uepi-tx:test",
                    "phase": "prepared",
                    "affected_assets": ["/Game/BP_Test.BP_Test"],
                    "backups": [
                        {
                            "package_file": "../../../../../FixtureProject/Content/BP_Test.uasset",
                            "backup_file": "../../../../../FixtureProject/Saved/UEProjectIntelligence/store/backups/BP_Test.uasset",
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        store = SimpleNamespace(root=root, store_dir=store_dir)
        inspection = inspect_recovery(store)
        assert inspection[0]["recommended_action"] == "review_current_or_rollback"
        assert inspection[0]["files"][0]["package_file"] == str(package.resolve())
        assert inspection[0]["files"][0]["backup_file"] == str(backup.resolve())
        first_token = inspection[0]["discard_current_state"]["confirmation_token"]
        package.write_bytes(backup.read_bytes())
        restored = inspect_recovery(store)[0]
        assert restored["recommended_action"] == "finalize"
        assert restored["discard_current_state"]["confirmation_token"] != first_token
        (transactions / "tx.recovery_finalized.json").write_text("{}", encoding="utf-8")
        assert inspect_recovery(store) == []


def assert_blueprint_auto_refresh_retry_contract(engine: Any) -> None:
    original_domain_read = engine._domain_entities_for_asset
    original_force_refresh = engine._force_bridge_refresh_many
    original_active_session = engine.snapshot.active_editor_session
    calls = 0
    refreshed_node = {
        "id": "auto-refresh-node",
        "kind": "blueprint_node",
        "canonical_key": "/Game/BP_MetadataOnly.BP_MetadataOnly:EventGraph:AutoRefreshNode",
        "display_name": "AutoRefreshNode",
        "attributes": {"graph_name": "EventGraph", "node_guid": "auto-refresh-guid", "node_class": "K2Node_CallFunction"},
        "typed_attributes": {},
        "completeness": {"state": "complete", "covered": [], "omitted": [], "warnings": []},
        "diagnostics": [],
        "evidence": [],
    }

    def domain_read(*args: Any, **kwargs: Any) -> list[dict[str, Any]]:
        nonlocal calls
        calls += 1
        return [] if calls == 1 else [refreshed_node]

    def force_refresh(targets: list[str], **kwargs: Any) -> tuple[str, list[dict[str, Any]], dict[str, Any]]:
        return "current", [{"severity": "info", "blocking": False, "code": "UEPI_REFRESH_COMPLETED", "message": "fixture"}], {
            "requested_assets": targets,
            "refreshed_assets": targets,
            "failed_assets": [],
            "previous_generation": engine.state.generation,
            "new_generation": engine.state.generation + 1,
            "atomic": True,
        }

    try:
        engine._domain_entities_for_asset = domain_read
        engine._force_bridge_refresh_many = force_refresh
        engine.snapshot.active_editor_session = lambda: {"session_id": "fixture-editor"}
        response = engine.blueprint("/Game/BP_MetadataOnly.BP_MetadataOnly", refresh="auto")
        assert response["ok"] is True
        assert response["result"]["blueprint_entities"][0]["id"] == "auto-refresh-node"
        assert response["result"]["refresh"]["refreshed_assets"] == ["/Game/BP_MetadataOnly.BP_MetadataOnly"]
        assert any(item.get("code") == "UEPI_REFRESH_COMPLETED" for item in response["diagnostics"])
        assert calls == 2
    finally:
        engine._domain_entities_for_asset = original_domain_read
        engine._force_bridge_refresh_many = original_force_refresh
        engine.snapshot.active_editor_session = original_active_session


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
    assert set(value.get("timing", {})) == {
        "total_ms",
        "mcp_queue_ms",
        "snapshot_query_ms",
        "bridge_connect_ms",
        "bridge_wait_ms",
        "editor_dispatch_ms",
        "editor_execute_ms",
        "serialization_ms",
    }
    assert all(isinstance(item, (int, float)) and item >= 0 for item in value["timing"].values())
    assert value["timing"]["total_ms"] >= value["timing"]["serialization_ms"]
    stage_total = sum(value["timing"][field] for field in value["timing"] if field != "total_ms")
    assert stage_total <= value["timing"]["total_ms"] + 1.0


def assert_timing_contract() -> None:
    old_threshold = os.environ.get("UEPI_SLOW_OPERATION_MS")
    os.environ["UEPI_SLOW_OPERATION_MS"] = "1"
    token = begin_request()
    try:
        record("bridge_connect_ms", 0.1)
        record("bridge_wait_ms", 0.2)
        time.sleep(0.003)
        value = envelope(tool="uepi_status", operation="status", project={}, state={}, result={"ready": True})
        attach_timing(value)
        response = tool_response(value)
        assert response["structuredContent"] is value
        assert set(value["timing"]) == {
            "total_ms",
            "mcp_queue_ms",
            "snapshot_query_ms",
            "bridge_connect_ms",
            "bridge_wait_ms",
            "editor_dispatch_ms",
            "editor_execute_ms",
            "serialization_ms",
        }
        assert value["timing"]["bridge_connect_ms"] == 0.1
        assert value["timing"]["bridge_wait_ms"] == 0.2
        assert value["timing"]["serialization_ms"] >= 0
        assert any(item.get("code") == "UEPI_SLOW_OPERATION" for item in value["diagnostics"])
    finally:
        end_request(token)
        if old_threshold is None:
            os.environ.pop("UEPI_SLOW_OPERATION_MS", None)
        else:
            os.environ["UEPI_SLOW_OPERATION_MS"] = old_threshold


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
    assert set(projected["result"]) == {"matches", "sections"}
    assert len(projected["result"]["matches"]) <= 4
    assert projected["payload_bytes"] == len(json.dumps(projected, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
    assert projected["payload_bytes"] <= 4096
    assert projected["continuation"]["has_more"] is True
    projected["timing"] = {field: 123456.789 for field in (
        "total_ms",
        "mcp_queue_ms",
        "snapshot_query_ms",
        "bridge_connect_ms",
        "bridge_wait_ms",
        "editor_dispatch_ms",
        "editor_execute_ms",
        "serialization_ms",
    )}
    enforce_response_budget(projected, {"fields": ["matches.id"], "page_size": 4, "max_payload_bytes": 4096})
    assert projected["payload_bytes"] <= 4096

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
    assert bounded_animation["truncation"]["byte_limit_hit"] is False
    assert bounded_animation["truncation"]["pre_projection_byte_limit_hit"] is True
    assert bounded_animation["payload_bytes"] <= 4096


def assert_cross_asset_fragment_ownership_contract() -> None:
    def asset_fragment(asset_id: str, asset_path: str, node_id: str, shared_id: str, revision: str) -> dict[str, Any]:
        return {
            "schema_version": "uepi.asset-fragment.v2",
            "project_id": "project-fixture",
            "asset": {"id": asset_id, "canonical_key": asset_path, "kind": "asset"},
            "entities": [
                {"id": asset_id, "kind": "asset", "canonical_key": asset_path, "attributes": {"object_path": asset_path}},
                {"id": node_id, "kind": "blueprint_node", "canonical_key": f"{asset_path}:node:{revision}", "attributes": {"revision": revision}},
                {"id": shared_id, "kind": "blueprint_event", "canonical_key": "/Script/Test:SharedInput", "attributes": {}},
            ],
            "relations": [
                {"id": f"rel-{asset_id}-{revision}", "type": "contains_node", "from_id": asset_id, "to_id": node_id},
                {"id": f"rel-{node_id}-shared", "type": "exec_flows_to", "from_id": node_id, "to_id": shared_id},
            ],
            "diagnostics": [],
        }

    def tombstone(asset_id: str, asset_path: str) -> dict[str, Any]:
        return {
            "schema_version": "uepi.asset-tombstone.v2",
            "asset_id": asset_id,
            "asset_key": asset_path,
            "package_name": asset_path.split(".", 1)[0],
            "old_object_path": asset_path,
        }

    a1 = asset_fragment("asset-a", "/Game/A.A", "node-a-v1", "shared-input", "v1")
    a2 = asset_fragment("asset-a", "/Game/A.A", "node-a-v2", "shared-input", "v2")
    b1 = asset_fragment("asset-b", "/Game/B.B", "node-b-v1", "shared-input", "v1")
    b2 = asset_fragment("asset-b", "/Game/B.B", "node-b-v2", "shared-input", "v2")

    merged_ab = SnapshotStore._merge_project_scans([a1, b1, tombstone("asset-a", "/Game/A.A"), a2])
    ids_ab = {item["id"] for item in merged_ab["entities"]}
    assert {"asset-a", "node-a-v2", "asset-b", "node-b-v1", "shared-input"}.issubset(ids_ab)
    assert "node-a-v1" not in ids_ab

    merged_ba = SnapshotStore._merge_project_scans([b1, a1, tombstone("asset-b", "/Game/B.B"), b2])
    ids_ba = {item["id"] for item in merged_ba["entities"]}
    assert {"asset-b", "node-b-v2", "asset-a", "node-a-v1", "shared-input"}.issubset(ids_ba)
    assert "node-b-v1" not in ids_ba

    nodes = [
        {"id": "left-alt", "kind": "blueprint_node", "attributes": {"semantic_input_key": "LeftAlt"}},
        {"id": "three", "kind": "blueprint_node", "attributes": {"semantic_input_key": "Three"}},
    ]
    digit_resolution = _input_resolution("按 3 触发什么", nodes)
    assert digit_resolution["requested_input"] == "3"
    assert digit_resolution["resolved_input"] == "Three"
    assert digit_resolution["match_mode"] == "explicit_phrase"
    assert digit_resolution["available_inputs"] == ["LeftAlt", "Three"]
    assert digit_resolution["matched"] is True
    assert _input_resolution("Press LeftAlt", nodes)["resolved_input"] == "LeftAlt"

    discover_response = envelope(
        tool="uepi_edit_discover",
        operation="operation_catalog",
        project={"project_id": "test", "project_name": "Test"},
        state={"freshness": "current", "saved_generation": 7},
        result={
            "profile": "codex",
            "operations": [
                {"name": f"operation.{index}", "domain": "test", "description": "d" * 200}
                for index in range(8)
            ],
        },
    )
    discover_page = apply_response_options(
        discover_response,
        {"page_size": 2, "max_payload_bytes": 8192, "exclude": ["operations.description"]},
    )
    assert discover_page["continuation"]["has_more"] is True
    assert all("description" not in item for item in discover_page["result"]["operations"])
    continuation = next(item for item in discover_page["next_actions"] if item.get("tool") == "uepi_edit_discover")
    assert continuation["arguments"]["cursor"] == discover_page["continuation"]["cursor"]
    assert continuation["arguments"]["page_size"] == 2

    default_operations = [
        {"name": f"operation.default.{index}", "description": "d" * 300}
        for index in range(120)
    ]
    default_discover = apply_response_options(
        envelope(
            tool="uepi_edit_discover",
            operation="operation_catalog",
            project={"project_id": "test", "project_name": "Test"},
            state={"freshness": "current", "saved_generation": 7},
            result={"operations": default_operations},
        ),
        {"max_payload_bytes": 4096},
    )
    assert default_discover["continuation"]["has_more"] is True
    default_next = next(item for item in default_discover["next_actions"] if item.get("tool") == "uepi_edit_discover")
    continued_default = apply_response_options(
        envelope(
            tool="uepi_edit_discover",
            operation="operation_catalog",
            project={"project_id": "test", "project_name": "Test"},
            state={"freshness": "current", "saved_generation": 7},
            result={"operations": default_operations},
        ),
        default_next["arguments"],
    )
    assert continued_default["ok"] is True
    assert continued_default["error"] is None

    stale_response = envelope(
        tool="uepi_edit_discover",
        operation="operation_catalog",
        project={"project_id": "test", "project_name": "Test"},
        state={"freshness": "current", "saved_generation": 8},
        result={"operations": [{"name": "operation.new"}]},
    )
    stale_page = apply_response_options(
        stale_response,
        {
            "page_size": 2,
            "exclude": ["operations.description"],
            "cursor": discover_page["continuation"]["cursor"],
        },
    )
    assert stale_page["ok"] is False
    assert stale_page["error"]["code"] == "UEPI_CURSOR_STALE"
    assert stale_page["next_actions"][0]["tool"] == "uepi_edit_discover"
    assert "cursor" not in stale_page["next_actions"][0]["arguments"]


def assert_operation_machine_contract() -> None:
    registry_source = (ROOT / "Source" / "UEProjectIntelligence" / "Private" / "Edit" / "UEPIEditOperationRegistry.cpp").read_text(encoding="utf-8")
    registry_names = set(re.findall(r'Descriptor\(TEXT\("([^"]+)"\)', registry_source))
    payload = json.loads((ROOT / "Schemas" / "edit-operation-contracts.json").read_text(encoding="utf-8"))
    assert payload["schema_version"] == "uepi.edit-operation-contracts.v1"
    contracts = payload["operations"]
    assert set(contracts) == registry_names
    assert len(contracts) == 64
    for name, contract in contracts.items():
        schema = contract["input_schema"]
        assert schema["type"] == "object", name
        assert schema["additionalProperties"] is False, name
        assert isinstance(schema.get("properties"), dict) and schema["properties"], name
        assert isinstance(schema.get("required"), list), name
        assert isinstance(contract.get("contract_hash"), str) and contract["contract_hash"].startswith("sha256:"), name
        assert len(contract.get("examples") or []) >= 1, name
        example = contract["examples"][0]
        assert set(schema["required"]).issubset(example), name
        assert set(example).issubset(schema["properties"]), name
    create_asset = contracts["content.create_asset"]
    assert {"destination_asset", "asset_class"}.issubset(create_asset["input_schema"]["required"])
    writes = contracts["asset.set_properties"]["input_schema"]["properties"]["writes"]
    assert writes["items"]["required"] == ["path", "value"]
    add_node = contracts["blueprint.add_node"]["input_schema"]
    assert len(add_node["allOf"]) >= 5


def assert_context_identity_contract() -> None:
    assert _normalized_asset_identity("/Game/Animations/Waving") == "/game/animations/waving.waving"
    assert _normalized_asset_identity("/Game/Animations/Waving.Waving_C") == "/game/animations/waving.waving"
    matches = [
        {"kind": "asset", "canonical_key": "/Game/BP_Character.BP_Character", "display_name": "BP_Character", "attributes": {"asset_class": "Blueprint"}},
        {"kind": "animation_sequence", "canonical_key": "/Game/Animations/Waving.Waving::sequence", "display_name": "Waving", "attributes": {"asset_class": "AnimSequence"}},
    ]
    assert _best_domain_asset(matches, "Analyze the Waving animation", {"animation_sequence"}) == "/Game/Animations/Waving.Waving"


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
    with tempfile.TemporaryDirectory(prefix="uepi_runtime_ticket_") as temp_dir:
        store = SnapshotStore.from_paths(store=Path(temp_dir))
        engine = SimpleNamespace(store=store, identity={"project_binding_id": "sha256:runtime", "project_file": "C:/Test/Test.uproject"})
        plan = {
            "schema_version": "uepi.runtime-plan.v1",
            "runtime_plan_id": "uepi-runtime-plan:test",
            "project_binding_id": "sha256:runtime",
            "editor_session_id": "session-runtime",
            "map": "/Game/Maps/TestMap",
            "steps": [
                {"action": "start", "wait_until_running": True},
                {
                    "action": "input",
                    "delivery": "enhanced_input_action",
                    "input_action": "/Game/Input/IA_Test.IA_Test",
                    "value": True,
                    "event": "pressed",
                },
                {"action": "delay", "seconds": 0.1},
                {"action": "assert", "target": {"class": "/Script/Test.MotionComponent"}, "property": "bPlaying", "equals": True},
                {"action": "stop"},
            ],
            "timeout_seconds": 30,
        }
        plan["plan_hash"] = runtime_plan_hash(plan)
        ticket = _ticket_from_plan(engine, plan)
        assert "transaction_id" not in ticket
        assert ticket["runtime_plan_id"] == plan["runtime_plan_id"]
        assert ticket["allowed_deliveries"] == ["enhanced_input_action"]
        assert ticket["allowed_inputs"] == [
            {
                "event": "pressed",
                "delivery": "enhanced_input_action",
                "input_action": "/Game/Input/IA_Test.IA_Test",
                "value": True,
            }
        ]
        assert {"start", "input", "delay", "assert", "stop"}.issubset(set(ticket["allowed_actions"]))
        assert Path(temp_dir, "store", "runtime", ticket["ticket_id"].replace(":", "-") + ".json").is_file()


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
        "attributes": {
            "node_title": "Event BeginPlay",
            "graph_name": "EventGraph",
            "graph_role": "event_graph",
            "node_guid": "BEGIN-GUID",
            "node_class": "/Script/BlueprintGraph.K2Node_Event",
            "semantic_kind": "event",
        },
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
    typed_slot_node_id = "node-typed-slot"
    begin_pin_id = "pin-beginplay-then"
    fragment_pin_id = "pin-fragment-execute"
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
            },
            {
                "id": typed_slot_node_id,
                "kind": "anim_slot",
                "canonical_key": "/Game/BP_Hero.BP_Hero::AnimGraph::AnimGraphNode_Slot_0",
                "display_name": "AnimGraph Slot",
                "source_layer": "editor_source_graph",
                "attributes": {
                    "node_title": "Slot DefaultSlot",
                    "graph_path": "/Game/BP_Hero.BP_Hero:WrongGraph",
                    "graph_role": "wrong_role",
                    "node_guid": "WRONG-GUID",
                    "node_class": "/Script/BlueprintGraph.K2Node_CallFunction",
                    "semantic_kind": "wrong_semantic",
                    "slot_name": "DefaultSlot",
                },
                "typed_attributes": {
                    "graph_path": {"schema_version": "uepi.attribute-value.v2", "type": "string", "raw": "/Game/BP_Hero.BP_Hero:AnimGraph", "value": "/Game/BP_Hero.BP_Hero:AnimGraph"},
                    "graph_role": {"schema_version": "uepi.attribute-value.v2", "type": "string", "raw": "animation_graph", "value": "animation_graph"},
                    "node_guid": {"schema_version": "uepi.attribute-value.v2", "type": "string", "raw": "SLOT-GUID", "value": "SLOT-GUID"},
                    "node_class": {"schema_version": "uepi.attribute-value.v2", "type": "string", "raw": "/Script/AnimGraph.AnimGraphNode_Slot", "value": "/Script/AnimGraph.AnimGraphNode_Slot"},
                    "semantic_kind": {"schema_version": "uepi.attribute-value.v2", "type": "string", "raw": "slot", "value": "slot"},
                },
                "completeness": {"state": "partial", "covered": ["node"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            },
            {
                "id": begin_pin_id,
                "kind": "blueprint_pin",
                "canonical_key": "/Game/BP_Hero.BP_Hero::EventGraph::BeginPlay:pin:Then",
                "display_name": "Then",
                "source_layer": "editor_source_graph",
                "attributes": {"pin_id": "BEGIN-PIN-GUID", "pin_name": "Then", "direction": "output", "pin_category": "exec", "pin_container_type": "none"},
                "completeness": {"state": "complete", "covered": ["pin_metadata", "pin_links"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            },
            {
                "id": fragment_pin_id,
                "kind": "blueprint_pin",
                "canonical_key": "/Game/BP_Hero.BP_Hero::EventGraph::FragmentNode:pin:execute",
                "display_name": "execute",
                "source_layer": "editor_source_graph",
                "attributes": {"pin_id": "FRAGMENT-PIN-GUID", "pin_name": "execute", "direction": "input", "pin_category": "exec", "pin_container_type": "none"},
                "completeness": {"state": "complete", "covered": ["pin_metadata", "pin_links"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [],
            },
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
            },
            {
                "id": "rel-typed-slot-node",
                "type": "contains_node",
                "from_id": asset_id,
                "to_id": typed_slot_node_id,
                "source_layer": "editor_source_graph",
                "derived": False,
                "confidence": 1.0,
                "attributes": {},
                "evidence": [],
            },
            {"id": "rel-begin-pin", "type": "has_pin", "from_id": node_id, "to_id": begin_pin_id, "source_layer": "editor_source_graph", "derived": False, "confidence": 1.0, "attributes": {}, "evidence": []},
            {"id": "rel-fragment-pin", "type": "has_pin", "from_id": fragment_node_id, "to_id": fragment_pin_id, "source_layer": "editor_source_graph", "derived": False, "confidence": 1.0, "attributes": {}, "evidence": []},
            {"id": "rel-begin-fragment-pins", "type": "connects_to", "from_id": begin_pin_id, "to_id": fragment_pin_id, "source_layer": "editor_source_graph", "derived": False, "confidence": 1.0, "attributes": {"edge_kind": "canonical_pin_link"}, "evidence": []},
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
    anim_blueprint_asset_entity = {
        "id": "asset-abp-manny",
        "kind": "asset",
        "canonical_key": "/Game/Animations/ABP_Manny.ABP_Manny",
        "display_name": "ABP_Manny",
        "source_layer": "asset_registry",
        "attributes": {
            "object_path": "/Game/Animations/ABP_Manny.ABP_Manny",
            "asset_name": "ABP_Manny",
            "asset_class": "/Script/Engine.AnimBlueprint",
        },
        "completeness": {"state": "partial", "covered": ["asset_registry_metadata"], "omitted": [], "warnings": []},
        "diagnostics": [],
        "evidence": [],
    }
    anim_blueprint_state_entity = {
        "id": "anim-state-abp-manny-idle",
        "kind": "anim_state",
        "canonical_key": "/Game/Animations/ABP_Manny.ABP_Manny::AnimGraph::Idle",
        "display_name": "Idle",
        "source_layer": "editor_source_graph",
        "attributes": {"graph_name": "AnimGraph", "state_name": "Idle"},
        "completeness": {"state": "partial", "covered": ["anim_state"], "omitted": [], "warnings": []},
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
        "entities": [anim_blueprint_state_entity, anim_blueprint_asset_entity, animation_asset_entity, animation_sequence_entity],
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

    character_asset_id = "asset-third-person-character"
    npc_asset_id = "asset-llmnpc-manny"
    character_path = "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter"
    npc_path = "/Game/LLMNPC/Blueprints/BP_LLMNPC_Manny.BP_LLMNPC_Manny"

    def gameplay_entity(entity_id: str, kind: str, asset_path: str, suffix: str, title: str, attributes: dict[str, Any] | None = None) -> dict[str, Any]:
        return {
            "id": entity_id,
            "kind": kind,
            "canonical_key": f"{asset_path}::{suffix}" if suffix else asset_path,
            "display_name": title,
            "source_layer": "editor_source_graph" if kind != "asset" else "asset_registry",
            "attributes": {"object_path": asset_path, "asset_path": asset_path, "node_title": title, **(attributes or {})},
            "completeness": {"state": "complete", "covered": ["gameplay_fixture"], "omitted": [], "warnings": []},
            "diagnostics": [],
            "evidence": [],
        }

    character_entities = [
        gameplay_entity(character_asset_id, "asset", character_path, "", "BP_ThirdPersonCharacter", {"asset_name": "BP_ThirdPersonCharacter"}),
        gameplay_entity("node-character-three", "blueprint_node", character_path, "EventGraph::Three", "Three", {"node_guid": "CHAR-THREE", "node_class": "/Script/BlueprintGraph.K2Node_InputKey", "semantic_kind": "input_key", "input_key": "Three"}),
        gameplay_entity("node-character-left-alt", "blueprint_node", character_path, "EventGraph::LeftAlt", "Left Alt", {"node_guid": "CHAR-LEFT-ALT", "node_class": "/Script/BlueprintGraph.K2Node_InputKey", "semantic_kind": "input_key", "input_key": "LeftAlt"}),
        gameplay_entity("node-get-all-manny", "blueprint_node", character_path, "EventGraph::GetAllActorsOfClass", "Get All Actors Of Class", {"node_guid": "GET-ALL", "semantic_kind": "call_function", "semantic_function": "/Script/Engine.GameplayStatics:GetAllActorsOfClass"}),
        gameplay_entity("node-call-manny-333", "blueprint_node", character_path, "EventGraph::Call333", "333", {"node_guid": "CALL-333", "semantic_kind": "call_function", "semantic_function": "/Game/LLMNPC/Blueprints/BP_LLMNPC_Manny.SKEL_BP_LLMNPC_Manny_C:333"}),
        gameplay_entity("pin-get-all-outactors", "blueprint_pin", character_path, "EventGraph::GetAllActorsOfClass:pin:OutActors", "OutActors", {"pin_id": "PIN-OUT-ACTORS", "pin_name": "OutActors", "direction": "output", "pin_category": "object", "pin_subcategory_object": "/Game/LLMNPC/Blueprints/BP_LLMNPC_Manny.BP_LLMNPC_Manny_C", "pin_container_type": "array"}),
        gameplay_entity("pin-call-333-self", "blueprint_pin", character_path, "EventGraph::Call333:pin:self", "self", {"pin_id": "PIN-CALL-SELF", "pin_name": "self", "direction": "input", "pin_category": "object", "pin_subcategory_object": "/Game/LLMNPC/Blueprints/BP_LLMNPC_Manny.BP_LLMNPC_Manny_C", "pin_container_type": "none"}),
    ]
    character_relations = [
        {"id": "rel-character-three-getall", "type": "exec_flows_to", "from_id": "node-character-three", "to_id": "node-get-all-manny", "source_layer": "editor_source_graph", "derived": True, "confidence": 1.0, "attributes": {}, "evidence": []},
        {"id": "rel-character-getall-call", "type": "exec_flows_to", "from_id": "node-get-all-manny", "to_id": "node-call-manny-333", "source_layer": "editor_source_graph", "derived": True, "confidence": 1.0, "attributes": {}, "evidence": []},
        {"id": "rel-getall-pin", "type": "has_pin", "from_id": "node-get-all-manny", "to_id": "pin-get-all-outactors", "source_layer": "editor_source_graph", "derived": False, "confidence": 1.0, "attributes": {}, "evidence": []},
        {"id": "rel-call-self-pin", "type": "has_pin", "from_id": "node-call-manny-333", "to_id": "pin-call-333-self", "source_layer": "editor_source_graph", "derived": False, "confidence": 1.0, "attributes": {}, "evidence": []},
        {"id": "rel-raw-array-to-scalar", "type": "connects_to", "from_id": "pin-get-all-outactors", "to_id": "pin-call-333-self", "source_layer": "editor_source_graph", "derived": False, "confidence": 1.0, "attributes": {"projection_complete": False, "diagnostic_code": "UEPI_PIN_PROJECTION_INCOMPLETE"}, "evidence": []},
    ]
    character_fragment = {
        "schema_version": "uepi.asset-fragment.v2", "project_id": "project-fixture", "project_name": "FixtureProject", "project_file": str(root / "FixtureProject.uproject"), "engine_version": "5.3.2", "source_scan_finished_at_utc": "2026-06-26T00:00:01Z",
        "asset": {"id": character_asset_id, "canonical_key": character_path, "display_name": "BP_ThirdPersonCharacter", "kind": "asset"}, "entities": character_entities, "relations": character_relations, "diagnostics": [],
    }
    character_fragment_path = objects / "aacharactergameplayfragment.json"
    character_fragment_path.write_text(json.dumps(character_fragment, ensure_ascii=False), encoding="utf-8")

    npc_entities = [
        gameplay_entity(npc_asset_id, "asset", npc_path, "", "BP_LLMNPC_Manny", {"asset_name": "BP_LLMNPC_Manny"}),
        gameplay_entity("node-npc-three", "blueprint_node", npc_path, "EventGraph::Three", "Three", {"node_guid": "NPC-THREE", "node_class": "/Script/BlueprintGraph.K2Node_InputKey", "semantic_kind": "input_key", "input_key": "Three"}),
        gameplay_entity("node-npc-enable-input", "blueprint_node", npc_path, "EventGraph::EnableInput", "Enable Input", {"node_guid": "NPC-ENABLE", "semantic_kind": "call_function", "semantic_function": "/Script/Engine.Actor:EnableInput"}),
        gameplay_entity("node-npc-event-333", "blueprint_node", npc_path, "EventGraph::Event333", "333", {"node_guid": "EVENT-333", "node_class": "/Script/BlueprintGraph.K2Node_CustomEvent", "semantic_kind": "event", "semantic_event": "333", "event_name": "333", "semantic_id": f"{npc_path}:event:333"}),
        gameplay_entity("node-submit-template", "blueprint_node", npc_path, "EventGraph::SubmitPublishedTemplate", "Submit Published Template", {"node_guid": "SUBMIT-TEMPLATE", "semantic_kind": "call_function", "semantic_function": "/Script/LLMNPCActionLayer.LLMNPCMotionComponent:SubmitPublishedTemplate"}),
        gameplay_entity("node-dynamic-montage", "blueprint_node", npc_path, "EventGraph::DynamicMontage", "Play Slot Animation as Dynamic Montage", {"node_guid": "DYNAMIC-MONTAGE", "semantic_kind": "call_function", "semantic_function": "/Script/Engine.AnimInstance:PlaySlotAnimationAsDynamicMontage"}),
        gameplay_entity("pin-template-id", "blueprint_pin", npc_path, "EventGraph::SubmitPublishedTemplate:pin:TemplateId", "TemplateId", {"pin_id": "PIN-TEMPLATE-ID", "pin_name": "TemplateId", "direction": "input", "pin_category": "name", "pin_container_type": "none", "default_value": "gesture.wave.asset.manny.v1"}),
        gameplay_entity("pin-waving", "blueprint_pin", npc_path, "EventGraph::DynamicMontage:pin:Asset", "Asset", {"pin_id": "PIN-WAVING", "pin_name": "Asset", "direction": "input", "pin_category": "object", "pin_container_type": "none", "default_object": "/Game/Animations/Waving.Waving"}),
    ]
    npc_relations = [
        {"id": "rel-npc-local-three-submit", "type": "exec_flows_to", "from_id": "node-npc-three", "to_id": "node-submit-template", "source_layer": "editor_source_graph", "derived": True, "confidence": 1.0, "attributes": {}, "evidence": []},
        {"id": "rel-npc-event-submit", "type": "exec_flows_to", "from_id": "node-npc-event-333", "to_id": "node-submit-template", "source_layer": "editor_source_graph", "derived": True, "confidence": 1.0, "attributes": {}, "evidence": []},
        {"id": "rel-npc-submit-montage", "type": "exec_flows_to", "from_id": "node-submit-template", "to_id": "node-dynamic-montage", "source_layer": "editor_source_graph", "derived": True, "confidence": 1.0, "attributes": {}, "evidence": []},
        {"id": "rel-submit-template-pin", "type": "has_pin", "from_id": "node-submit-template", "to_id": "pin-template-id", "source_layer": "editor_source_graph", "derived": False, "confidence": 1.0, "attributes": {}, "evidence": []},
        {"id": "rel-dynamic-waving-pin", "type": "has_pin", "from_id": "node-dynamic-montage", "to_id": "pin-waving", "source_layer": "editor_source_graph", "derived": False, "confidence": 1.0, "attributes": {}, "evidence": []},
    ]
    npc_fragment = {
        "schema_version": "uepi.asset-fragment.v2", "project_id": "project-fixture", "project_name": "FixtureProject", "project_file": str(root / "FixtureProject.uproject"), "engine_version": "5.3.2", "source_scan_finished_at_utc": "2026-06-26T00:00:01Z",
        "asset": {"id": npc_asset_id, "canonical_key": npc_path, "display_name": "BP_LLMNPC_Manny", "kind": "asset"}, "entities": npc_entities, "relations": npc_relations, "diagnostics": [],
    }
    npc_fragment_path = objects / "aanpcgameplayfragment.json"
    npc_fragment_path.write_text(json.dumps(npc_fragment, ensure_ascii=False), encoding="utf-8")

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
        "asset_entity_ids": [asset_id, animation_asset_id, character_asset_id, npc_asset_id],
        "fragments": [
            {"kind": "project_fragment", "schema_version": "uepi.project-fragment.v2", "hash": "aaprojectfragment", "path": str(project_fragment_path)},
            {"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aaassetfragment", "path": str(asset_fragment_path), "asset_id": asset_id},
            {"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aaanimationfragment", "path": str(animation_fragment_path), "asset_id": animation_asset_id},
            {"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aacharactergameplayfragment", "path": str(character_fragment_path), "asset_id": character_asset_id},
            {"kind": "asset_fragment", "schema_version": "uepi.asset-fragment.v2", "hash": "aanpcgameplayfragment", "path": str(npc_fragment_path), "asset_id": npc_asset_id},
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


def assert_versioned_cache_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="uepi_cache_generation_") as temp_dir:
        root = Path(temp_dir)
        write_fixture(root)
        store = SnapshotStore.from_paths(store=root)
        first = sync_cache(store)
        first_path = Path(first["cache_path"])
        assert first_path.name.startswith("uepi-live-g2-")
        assert Path(first["pointer_path"]).is_file()

        old_engine = make_engine(store=root)
        try:
            assert old_engine.cache is not None
            old_counts = old_engine.cache.counts()
            assert_blueprint_auto_refresh_retry_contract(old_engine)

            live_path = root / "store" / "manifests" / "live.json"
            live_manifest = json.loads(live_path.read_text(encoding="utf-8"))
            live_manifest["generation"] = 3
            live_manifest["created_at_utc"] = "2026-06-26T00:00:05Z"
            live_path.write_text(json.dumps(live_manifest, ensure_ascii=False), encoding="utf-8")

            with ThreadPoolExecutor(max_workers=2) as executor:
                results = list(executor.map(lambda _: sync_cache(store), range(2)))
            published_paths = {item["cache_path"] for item in results}
            assert len(published_paths) == 1
            assert first_path not in {Path(item) for item in published_paths}
            assert old_engine.cache.counts() == old_counts

            status = cache_status(store)
            assert status["synced"] is True
            assert status["versioned"] is True
            assert status["generation"] == 3

            live_manifest["generation"] = 4
            live_manifest["created_at_utc"] = "2026-06-26T00:00:06Z"
            live_path.write_text(json.dumps(live_manifest, ensure_ascii=False), encoding="utf-8")
            env = os.environ.copy()
            env["PYTHONPATH"] = str(PYTHONPATH)
            processes = [
                subprocess.Popen(
                    [sys.executable, "-B", "-m", "uepi", "sync", "--store", str(root)],
                    cwd=ROOT,
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                for _ in range(2)
            ]
            process_results = []
            for process in processes:
                stdout, stderr = process.communicate(timeout=30)
                assert process.returncode == 0, stderr
                process_results.append(json.loads(stdout))
            assert len({item["cache_path"] for item in process_results}) == 1
            assert cache_status(store)["generation"] == 4

            before = set((root / "cache").glob("uepi-live-g*.sqlite3"))
            started = time.perf_counter()
            for _ in range(20):
                with make_engine(store=root) as engine:
                    assert engine.cache is not None
                    assert engine.status()["result"]["cache"]["synced"] is True
            assert time.perf_counter() - started < 2.0
            after = set((root / "cache").glob("uepi-live-g*.sqlite3"))
            assert after == before
        finally:
            old_engine.close()


def main() -> int:
    assert_error_evidence_contract()
    assert_timing_contract()
    assert_doctor_contract()
    assert_edit_asset_scope_contract()
    assert_edit_transaction_budget_contract()
    assert_authorization_policy_contract()
    assert_post_action_report_contract()
    assert_recovery_inspection_contract()
    assert_plan_v2_contract()
    assert_runtime_ticket_contract()
    assert_response_budget_contract()
    assert_cross_asset_fragment_ownership_contract()
    assert_operation_machine_contract()
    assert_context_identity_contract()
    assert_versioned_cache_contract()
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
            runtime_tool = next(tool for tool in tools if tool["name"] == "uepi_runtime")
            runtime_actions = runtime_tool["inputSchema"]["properties"]["action"]["enum"]
            assert "automation" not in runtime_actions
            assert "job_status" not in runtime_actions
            runtime_preview_tool = next(tool for tool in tools if tool["name"] == "uepi_runtime_preview")
            assert runtime_preview_tool["inputSchema"]["properties"]["steps"]["items"]["oneOf"]
            edit_preview_tool = next(tool for tool in tools if tool["name"] == "uepi_edit_preview")
            operation_variants = edit_preview_tool["inputSchema"]["properties"]["operations"]["items"]["oneOf"]
            assert operation_variants
            assert all({"type", "params"}.issubset(set(item["required"])) for item in operation_variants)
            world_tool = next(tool for tool in tools if tool["name"] == "uepi_world")
            assert world_tool["inputSchema"]["properties"]["filters"]["additionalProperties"] is False
            assert {"actor", "component", "property_names"}.issubset(world_tool["inputSchema"]["properties"])
            discover_tool = next(tool for tool in tools if tool["name"] == "uepi_edit_discover")
            assert {
                "cursor",
                "page_size",
                "max_payload_bytes",
                "fields",
                "exclude",
            }.issubset(discover_tool["inputSchema"]["properties"])

            status = request(process, 3, "tools/call", {"name": "uepi_status", "arguments": {}})["structuredContent"]
            assert_envelope(status)
            assert status["snapshot"]["view_generation"] == 2
            assert status["editor"]["connected"] is False
            assert status["result"]["llm_readiness"]["requires_daemon"] is False
            assert status["result"]["llm_readiness"]["bridge_ready"] is False
            assert status["result"]["doctor"]["project_bound"] is True
            assert status["result"]["doctor"]["catalog_current"] is False
            assert status["result"]["doctor"]["catalog_cache_current"] is False
            assert status["result"]["service_build_id"].startswith("uepi-service-")
            assert status["result"]["service_source_hash"].startswith("sha256:")
            assert status["result"]["service_process_start_time"]
            assert status["result"]["service_loaded_module_path"].endswith("__init__.py")
            assert status["result"]["editor"]["active_map"] is None
            assert status["result"]["editor"]["pie_state"] == "stopped"
            assert status["result"]["snapshot"]["generation_comparable"] is False
            assert status["result"]["snapshot"]["freshness"] == "current"
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
            package_scoped_context = request(
                process,
                68,
                "tools/call",
                {
                    "name": "uepi_context",
                    "arguments": {
                        "question": "Read this Blueprint only",
                        "route": "blueprint_behavior",
                        "hard_scope": ["/Game/BP_Hero"],
                        "max_items": 20,
                    },
                },
            )["structuredContent"]
            assert package_scoped_context["ok"] is True
            assert package_scoped_context["result"]["matches"]

            gameplay_started = time.perf_counter()
            gameplay_context = request(
                process,
                73,
                "tools/call",
                {
                    "name": "uepi_context",
                    "arguments": {
                        "question": "Trace key Three from the player to the Waving gameplay effect",
                        "route": "gameplay_input_to_effect",
                        "hard_scope": [
                            "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter",
                            "/Game/LLMNPC/Blueprints/BP_LLMNPC_Manny.BP_LLMNPC_Manny",
                            "/Game/Animations/Waving.Waving",
                        ],
                        "max_items": 40,
                        "max_payload_bytes": 30000,
                    },
                },
            )["structuredContent"]
            gameplay_elapsed = time.perf_counter() - gameplay_started
            assert_envelope(gameplay_context)
            gameplay_sections = gameplay_context["result"]["sections"]
            assert gameplay_context["result"]["query_source"] == "sqlite_cache"
            assert gameplay_sections["requested_input"] == "Three"
            assert gameplay_sections["resolved_input"] == "Three"
            assert gameplay_sections["match_mode"] == "explicit_phrase"
            assert gameplay_sections["input_owner"]["asset"].endswith("BP_ThirdPersonCharacter.BP_ThirdPersonCharacter")
            assert gameplay_sections["input_owner"]["owner_confidence"] > 0.5
            assert any(any(step.get("kind") == "cross_asset_call" and step.get("function_or_event") == "333" for step in path["steps"]) for path in gameplay_sections["cross_asset_paths"])
            assert any(any(pin.get("value") == "gesture.wave.asset.manny.v1" for pin in terminal.get("pin_defaults") or []) for terminal in gameplay_sections["terminal_effects"])
            assert any(item.get("diagnostic_code") == "UEPI_DUPLICATE_GAMEPLAY_INPUT_PATH" for item in gameplay_sections["duplicate_paths"])
            assert gameplay_elapsed < 2.0
            assert len(json.dumps(gameplay_context, ensure_ascii=False).encode("utf-8")) < 30000

            def gameplay_input_context(request_id: int, question: str, **arguments: Any) -> dict[str, Any]:
                value = request(
                    process,
                    request_id,
                    "tools/call",
                    {
                        "name": "uepi_context",
                        "arguments": {
                            "question": question,
                            "route": "gameplay_input_to_effect",
                            "hard_scope": [
                                "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter",
                                "/Game/LLMNPC/Blueprints/BP_LLMNPC_Manny.BP_LLMNPC_Manny",
                            ],
                            "max_items": 40,
                            **arguments,
                        },
                    },
                )["structuredContent"]
                assert_envelope(value)
                return value

            for request_id, question in (
                (730, "Trace key 3, exclude LeftAlt"),
                (731, "追踪按键 3 的效果，不要 LeftAlt"),
            ):
                resolved = gameplay_input_context(request_id, question)
                sections = resolved["result"]["sections"]
                assert sections["resolved_input"] == "Three"
                assert sections["excluded_input_keys"] == ["LeftAlt"]
                assert sections["available_inputs"] == ["LeftAlt", "Three"]
                assert all(item.get("id") != "node-character-left-alt" for item in resolved["result"]["matches"])
                assert resolved["next_actions"][0]["arguments"]["key"] == sections["resolved_input"]

            left_alt = gameplay_input_context(732, "Trace LeftAlt, not Three")
            left_alt_sections = left_alt["result"]["sections"]
            assert left_alt_sections["resolved_input"] == "LeftAlt"
            assert left_alt_sections["excluded_input_keys"] == ["Three"]
            assert left_alt["next_actions"][0]["arguments"]["key"] == "LeftAlt"

            structured_key = gameplay_input_context(733, "Trace the requested gameplay input", input_key="3", excluded_input_keys=["LeftAlt"])
            assert structured_key["result"]["sections"]["resolved_input"] == "Three"
            assert structured_key["result"]["sections"]["match_mode"] == "structured_exact"

            hinted_key = gameplay_input_context(734, "Trace the requested gameplay input", ranking_hints=["key=Three"])
            assert hinted_key["result"]["sections"]["resolved_input"] == "Three"
            assert hinted_key["result"]["sections"]["match_mode"] == "ranking_hint_exact"

            for request_id, question in (
                (735, "Trace key 4"),
                (736, "Trace keyboard key 4"),
            ):
                unmatched = gameplay_input_context(request_id, question)
                unmatched_sections = unmatched["result"]["sections"]
                assert unmatched_sections["requested_input"] == "4"
                assert unmatched_sections["resolved_input"] is None
                assert unmatched_sections["selected_inputs"] == []
                assert unmatched_sections["cross_asset_paths"] == []
                assert unmatched_sections["terminal_effects"] == []
                assert unmatched["result"]["matches"] == []
                assert unmatched["result"]["relations"] == []
                assert unmatched["next_actions"] == []
                assert any(item.get("code") == "UEPI_INPUT_KEY_UNMATCHED" and item.get("blocking") is True for item in unmatched["diagnostics"])

            gameplay_blueprint = request(
                process,
                74,
                "tools/call",
                {
                    "name": "uepi_blueprint",
                    "arguments": {
                        "asset": "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter",
                        "node_guid": "GET-ALL",
                        "limit": 100,
                    },
                },
            )["structuredContent"]
            out_actors = next(item for item in gameplay_blueprint["result"]["blueprint_entities"] if item.get("id") == "pin-get-all-outactors")
            assert out_actors["attributes"]["pin_container_type"] == "array"
            raw_link = next(item for item in gameplay_blueprint["result"]["relations"] if item.get("id") == "rel-raw-array-to-scalar")
            assert raw_link["type"] == "connects_to"
            assert raw_link["attributes"]["diagnostic_code"] == "UEPI_PIN_PROJECTION_INCOMPLETE"

            animation_scope = [
                "/Game/Animations/ABP_Manny",
                "/Game/Animations/Waving",
                "/Game/BP_Hero",
            ]
            scoped_animation = request(
                process,
                70,
                "tools/call",
                {
                    "name": "uepi_context",
                    "arguments": {
                        "question": "Analyze the Waving animation sequence",
                        "route": "animation_playback",
                        "hard_scope": animation_scope,
                        "max_items": 6,
                    },
                },
            )["structuredContent"]
            assert_envelope(scoped_animation)
            assert scoped_animation["result"]["query_source"] == "sqlite_cache"
            summary = scoped_animation["result"]["sections"]["animation_summary"]
            assert summary["asset"]["canonical_key"] == "/Game/Animations/Waving.Waving"
            assert summary["motion_summary"] is not None
            assert summary["sequence"] is not None
            root_paths = {
                _normalized_asset_identity(item.get("canonical_key"))
                for item in scoped_animation["result"]["matches"]
                if item.get("kind") == "asset"
            }
            assert {
                _normalized_asset_identity(scope)
                for scope in animation_scope
            }.issubset(root_paths)

            reversed_animation = request(
                process,
                71,
                "tools/call",
                {
                    "name": "uepi_context",
                    "arguments": {
                        "question": "Analyze the Waving animation sequence",
                        "route": "animation_playback",
                        "hard_scope": list(reversed(animation_scope)),
                        "max_items": 6,
                    },
                },
            )["structuredContent"]
            assert reversed_animation["result"]["sections"]["animation_summary"]["asset"]["canonical_key"] == "/Game/Animations/Waving.Waving"
            assert reversed_animation["result"]["query_source"] == "sqlite_cache"

            scoped_page = request(
                process,
                72,
                "tools/call",
                {
                    "name": "uepi_context",
                    "arguments": {
                        "question": "Analyze the Waving animation sequence",
                        "route": "animation_playback",
                        "hard_scope": animation_scope,
                        "fields": [
                            "hard_scope",
                            "matches",
                            "sections.animation_summary",
                        ],
                        "page_size": 1,
                        "max_payload_bytes": 30000,
                    },
                },
            )["structuredContent"]
            assert scoped_page["result"]["hard_scope"] == animation_scope
            if scoped_page["continuation"]["cursor"]:
                encoded_cursor = scoped_page["continuation"]["cursor"]
                cursor_payload = json.loads(base64.urlsafe_b64decode(encoded_cursor + "=" * (-len(encoded_cursor) % 4)))
                assert cursor_payload["sort_key"] == "matches"

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
            bad_world_filter = request(process, 69, "tools/call", {"name": "uepi_world", "arguments": {"action": "read", "filters": {"unknown": ["value"]}}})["structuredContent"]
            assert bad_world_filter["ok"] is False
            assert bad_world_filter["error"]["code"] == "UEPI_WORLD_FILTER_UNKNOWN"
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
            assert refresh_request["result"]["request_id"] == refresh_value["request_id"]
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
            assert blueprint["diagnostics"][0]["request_id"]
            assert blueprint["diagnostics"][0]["request"]["schema_version"] == "uepi.refresh-request.v2"
            assert blueprint["next_actions"][0]["tool"] == "uepi_refresh"
            assert blueprint["next_actions"][0]["arguments"]["request_id"] == blueprint["diagnostics"][0]["request_id"]
            blueprint_names = {item["display_name"] for item in blueprint["result"]["blueprint_entities"]}
            assert "Event BeginPlay" in blueprint_names
            assert "Asset Fragment Node" in blueprint_names
            assert "Live Print String" in blueprint_names
            assert any((root / "store" / "requests").glob("*.json"))
            assert "semantic_summary" in blueprint["result"]
            assert blueprint["result"]["semantic_summary"]["schema_version"] == "uepi.blueprint-semantic-summary.v1"

            typed_filter_cases = [
                {"graph": "AnimGraph"},
                {"graph_role": "animation_graph"},
                {"node_guid": "SLOT-GUID"},
                {"node_classes": ["AnimGraphNode_Slot"]},
                {"semantic_kinds": ["slot"]},
                {"graph": "AnimGraph", "node_classes": ["AnimGraphNode_Slot"]},
            ]
            for request_id, filters in enumerate(typed_filter_cases, start=72):
                filtered = request(
                    process,
                    request_id,
                    "tools/call",
                    {
                        "name": "uepi_blueprint",
                        "arguments": {"asset": "/Game/BP_Hero.BP_Hero", **filters},
                    },
                )["structuredContent"]
                assert filtered["ok"] is True
                assert {item["id"] for item in filtered["result"]["blueprint_entities"]} == {"node-typed-slot"}

            untyped_filter_cases = [
                {"graph": "EventGraph"},
                {"graph_role": "event_graph"},
                {"node_guid": "BEGIN-GUID"},
                {"node_classes": ["K2Node_Event"]},
                {"semantic_kinds": ["event"]},
            ]
            for request_id, filters in enumerate(untyped_filter_cases, start=80):
                filtered = request(
                    process,
                    request_id,
                    "tools/call",
                    {
                        "name": "uepi_blueprint",
                        "arguments": {"asset": "/Game/BP_Hero.BP_Hero", **filters},
                    },
                )["structuredContent"]
                assert filtered["ok"] is True
                assert "node-beginplay" in {item["id"] for item in filtered["result"]["blueprint_entities"]}

            focused = request(
                process,
                86,
                "tools/call",
                {"name": "uepi_blueprint", "arguments": {"asset": "/Game/BP_Hero.BP_Hero", "graph": "EventGraph", "node_guid": "BEGIN-GUID"}},
            )["structuredContent"]
            assert focused["operation"] == "focused_node_read"
            assert focused["result"]["focus"]["node"]["id"] == "node-beginplay"
            assert {item["id"] for item in focused["result"]["focus"]["pins"]} == {"pin-beginplay-then"}
            assert focused["result"]["focus"]["direct_links"][0]["type"] == "connects_to"
            assert focused["result"]["asset"].get("snapshot") is None

            focused_projected = request(
                process,
                106,
                "tools/call",
                {"name": "uepi_blueprint", "arguments": {"asset": "/Game/BP_Hero.BP_Hero", "node_guid": "BEGIN-GUID", "fields": ["blueprint_entities.id"]}},
            )["structuredContent"]
            assert focused_projected["result"]["focus"]["node"]["id"] == "node-beginplay"
            assert {item["id"] for item in focused_projected["result"]["focus"]["pins"]} == {"pin-beginplay-then"}
            assert focused_projected["result"]["query_source"] == "sqlite_cache"

            requests_before_no_match = {path.name for path in (root / "store" / "requests").glob("*.json")}
            no_match = request(
                process,
                85,
                "tools/call",
                {
                    "name": "uepi_blueprint",
                    "arguments": {
                        "asset": "/Game/BP_Hero.BP_Hero",
                        "node_classes": ["DefinitelyMissingNodeClass"],
                    },
                },
            )["structuredContent"]
            assert no_match["ok"] is True
            assert no_match["result"]["blueprint_entities"] == []
            assert no_match["result"]["unfiltered_entity_count"] > 0
            assert "blueprint_graph_entities_not_present_in_snapshot" not in no_match["omissions"]
            assert any(item.get("code") == "UEPI_BLUEPRINT_FILTER_NO_MATCH" for item in no_match["diagnostics"])
            assert {path.name for path in (root / "store" / "requests").glob("*.json")} == requests_before_no_match

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
