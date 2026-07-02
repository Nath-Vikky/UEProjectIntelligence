#!/usr/bin/env python3
from __future__ import annotations

import argparse
from collections import Counter
import json
import re
import sys
from pathlib import Path
from typing import Any


HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
GUID_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
RELATION_TYPE_RE = re.compile(r"^[a-z][a-z0-9_]*$")
DIAGNOSTIC_CODE_RE = re.compile(r"^UEPI_[A-Z0-9_]+$")

SOURCE_LAYERS = {
    "filesystem",
    "asset_registry",
    "uobject_reflection",
    "editor_source_graph",
    "compiled_blueprint",
    "animation_data_model",
    "runtime_evaluation",
    "runtime_observed",
    "heuristic",
}
COMPLETENESS_STATES = {
    "complete",
    "partial",
    "metadata_only",
    "failed",
    "unsupported",
    "stale",
    "runtime_context_required",
}
DIAGNOSTIC_SEVERITIES = {"info", "warning", "error"}


class ScanValidator:
    def __init__(self, max_errors: int) -> None:
        self.max_errors = max_errors
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.entity_ids: set[str] = set()
        self.relation_ids: set[str] = set()

    def error(self, path: str, message: str) -> None:
        if len(self.errors) < self.max_errors:
            self.errors.append(f"{path}: {message}")

    def warning(self, path: str, message: str) -> None:
        self.warnings.append(f"{path}: {message}")

    def has_budget(self) -> bool:
        return len(self.errors) < self.max_errors

    def require_keys(self, value: Any, path: str, keys: list[str]) -> bool:
        if not isinstance(value, dict):
            self.error(path, "expected object")
            return False
        ok = True
        for key in keys:
            if key not in value:
                self.error(path, f"missing required key '{key}'")
                ok = False
        return ok

    def expect_string(self, value: Any, path: str, allow_empty: bool = True) -> bool:
        if not isinstance(value, str):
            self.error(path, "expected string")
            return False
        if not allow_empty and value == "":
            self.error(path, "expected non-empty string")
            return False
        return True

    def expect_string_map(self, value: Any, path: str) -> bool:
        if not isinstance(value, dict):
            self.error(path, "expected object with string values")
            return False
        ok = True
        for key, item in value.items():
            if not isinstance(key, str) or not isinstance(item, str):
                self.error(f"{path}.{key}", "expected string key and string value")
                ok = False
        return ok

    def expect_hex64(self, value: Any, path: str) -> bool:
        if not self.expect_string(value, path, allow_empty=False):
            return False
        if not HEX64_RE.match(value):
            self.error(path, "expected 64 lowercase hex characters")
            return False
        return True

    def expect_boolean(self, value: Any, path: str) -> bool:
        if not isinstance(value, bool):
            self.error(path, "expected boolean")
            return False
        return True

    def expect_integer(self, value: Any, path: str) -> bool:
        if not isinstance(value, int) or isinstance(value, bool):
            self.error(path, "expected integer")
            return False
        return True

    def expect_non_negative_integer(self, value: Any, path: str) -> bool:
        if not self.expect_integer(value, path):
            return False
        if value < 0:
            self.error(path, "expected non-negative integer")
            return False
        return True

    def expect_number(self, value: Any, path: str) -> bool:
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            self.error(path, "expected number")
            return False
        return True

    def validate_scan(self, scan: Any) -> None:
        if not self.require_keys(
            scan,
            "$",
            [
                "schema_version",
                "project_id",
                "project_name",
                "project_file",
                "engine_version",
                "started_at_utc",
                "finished_at_utc",
                "completeness",
                "entities",
                "relations",
                "diagnostics",
            ],
        ):
            return

        self.expect_string(scan.get("schema_version"), "$.schema_version", allow_empty=False)
        self.expect_hex64(scan.get("project_id"), "$.project_id")
        for key in ("project_name", "project_file", "engine_version", "started_at_utc", "finished_at_utc"):
            self.expect_string(scan.get(key), f"$.{key}")
        self.validate_completeness(scan.get("completeness"), "$.completeness")
        self.validate_diagnostics(scan.get("diagnostics"), "$.diagnostics")

        entities = scan.get("entities")
        relations = scan.get("relations")
        if not isinstance(entities, list):
            self.error("$.entities", "expected array")
            entities = []
        if not isinstance(relations, list):
            self.error("$.relations", "expected array")
            relations = []

        for index, entity in enumerate(entities):
            if not self.has_budget():
                break
            self.validate_entity(entity, f"$.entities[{index}]")

        for index, relation in enumerate(relations):
            if not self.has_budget():
                break
            self.validate_relation(relation, f"$.relations[{index}]")

    def validate_completeness(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["state", "covered", "omitted", "warnings"]):
            return
        state = value.get("state")
        if state not in COMPLETENESS_STATES:
            self.error(f"{path}.state", f"unknown completeness state '{state}'")
        for key in ("covered", "omitted", "warnings"):
            items = value.get(key)
            if not isinstance(items, list):
                self.error(f"{path}.{key}", "expected array")
                continue
            for index, item in enumerate(items):
                self.expect_string(item, f"{path}.{key}[{index}]")

    def validate_diagnostics(self, value: Any, path: str) -> None:
        if not isinstance(value, list):
            self.error(path, "expected array")
            return
        for index, diagnostic in enumerate(value):
            item_path = f"{path}[{index}]"
            if not self.require_keys(diagnostic, item_path, ["code", "severity", "message", "context"]):
                continue
            code = diagnostic.get("code")
            if not isinstance(code, str) or not DIAGNOSTIC_CODE_RE.match(code):
                self.error(f"{item_path}.code", "expected UEPI_* diagnostic code")
            severity = diagnostic.get("severity")
            if severity not in DIAGNOSTIC_SEVERITIES:
                self.error(f"{item_path}.severity", f"unknown diagnostic severity '{severity}'")
            self.expect_string(diagnostic.get("message"), f"{item_path}.message")
            self.expect_string_map(diagnostic.get("context"), f"{item_path}.context")

    def validate_evidence(self, value: Any, path: str) -> None:
        if not isinstance(value, list):
            self.error(path, "expected array")
            return
        for index, evidence in enumerate(value):
            item_path = f"{path}[{index}]"
            if not self.require_keys(evidence, item_path, ["source_layer", "path", "detail"]):
                continue
            layer = evidence.get("source_layer")
            if layer not in SOURCE_LAYERS:
                self.error(f"{item_path}.source_layer", f"unknown source layer '{layer}'")
            self.expect_string(evidence.get("path"), f"{item_path}.path")
            self.expect_string(evidence.get("detail"), f"{item_path}.detail")

    def validate_entity(self, entity: Any, path: str) -> None:
        if not self.require_keys(
            entity,
            path,
            ["id", "kind", "canonical_key", "source_layer", "attributes", "completeness", "evidence"],
        ):
            return

        entity_id = entity.get("id")
        if self.expect_hex64(entity_id, f"{path}.id"):
            if entity_id in self.entity_ids:
                self.error(f"{path}.id", f"duplicate entity id '{entity_id}'")
            self.entity_ids.add(entity_id)

        self.expect_string(entity.get("kind"), f"{path}.kind", allow_empty=False)
        self.expect_string(entity.get("canonical_key"), f"{path}.canonical_key", allow_empty=False)
        if "display_name" in entity:
            self.expect_string(entity.get("display_name"), f"{path}.display_name")
        layer = entity.get("source_layer")
        if layer not in SOURCE_LAYERS:
            self.error(f"{path}.source_layer", f"unknown source layer '{layer}'")
        self.expect_string_map(entity.get("attributes"), f"{path}.attributes")
        self.validate_completeness(entity.get("completeness"), f"{path}.completeness")
        self.validate_diagnostics(entity.get("diagnostics", []), f"{path}.diagnostics")
        self.validate_evidence(entity.get("evidence"), f"{path}.evidence")

        snapshot = entity.get("snapshot")
        if snapshot is not None:
            if not isinstance(snapshot, dict):
                self.error(f"{path}.snapshot", "expected object")
            else:
                if "blueprint_graphs" in snapshot:
                    self.validate_blueprint_graph_snapshot(snapshot["blueprint_graphs"], f"{path}.snapshot.blueprint_graphs")
                if "world" in snapshot:
                    self.validate_world_snapshot(snapshot["world"], f"{path}.snapshot.world")
                if "world_data_layers" in snapshot:
                    self.validate_world_data_layers_snapshot(snapshot["world_data_layers"], f"{path}.snapshot.world_data_layers")
                if "world_partition" in snapshot:
                    self.validate_world_partition_snapshot(snapshot["world_partition"], f"{path}.snapshot.world_partition")
                if "world_partition_actor_desc" in snapshot:
                    self.validate_world_partition_actor_desc_snapshot(
                        snapshot["world_partition_actor_desc"],
                        f"{path}.snapshot.world_partition_actor_desc",
                    )
                if "widget_blueprint" in snapshot:
                    self.validate_widget_blueprint_snapshot(snapshot["widget_blueprint"], f"{path}.snapshot.widget_blueprint")
                if "input_action" in snapshot:
                    self.validate_input_action_snapshot(snapshot["input_action"], f"{path}.snapshot.input_action")
                if "input_mapping_context" in snapshot:
                    self.validate_input_mapping_context_snapshot(
                        snapshot["input_mapping_context"],
                        f"{path}.snapshot.input_mapping_context",
                    )
                if "common_ui_input_data" in snapshot:
                    self.validate_common_ui_input_data_snapshot(
                        snapshot["common_ui_input_data"],
                        f"{path}.snapshot.common_ui_input_data",
                    )
                if "common_ui_hold_data" in snapshot:
                    self.validate_common_ui_hold_data_snapshot(
                        snapshot["common_ui_hold_data"],
                        f"{path}.snapshot.common_ui_hold_data",
                    )
                if "common_ui_input_action_table" in snapshot:
                    self.validate_common_ui_input_action_table_snapshot(
                        snapshot["common_ui_input_action_table"],
                        f"{path}.snapshot.common_ui_input_action_table",
                    )
                if "blackboard" in snapshot:
                    self.validate_blackboard_snapshot(snapshot["blackboard"], f"{path}.snapshot.blackboard")
                if "behavior_tree" in snapshot:
                    self.validate_behavior_tree_snapshot(snapshot["behavior_tree"], f"{path}.snapshot.behavior_tree")
                if "env_query" in snapshot:
                    self.validate_env_query_snapshot(snapshot["env_query"], f"{path}.snapshot.env_query")
                if "state_tree" in snapshot:
                    self.validate_state_tree_snapshot(snapshot["state_tree"], f"{path}.snapshot.state_tree")
                if "gameplay_ability" in snapshot:
                    self.validate_gameplay_ability_snapshot(snapshot["gameplay_ability"], f"{path}.snapshot.gameplay_ability")
                if "gameplay_effect" in snapshot:
                    self.validate_gameplay_effect_snapshot(snapshot["gameplay_effect"], f"{path}.snapshot.gameplay_effect")
                if "gameplay_cue_notify" in snapshot:
                    self.validate_gameplay_cue_notify_snapshot(
                        snapshot["gameplay_cue_notify"],
                        f"{path}.snapshot.gameplay_cue_notify",
                    )
                if "user_defined_struct" in snapshot:
                    self.validate_user_defined_struct_snapshot(snapshot["user_defined_struct"], f"{path}.snapshot.user_defined_struct")
                if "user_defined_enum" in snapshot:
                    self.validate_user_defined_enum_snapshot(snapshot["user_defined_enum"], f"{path}.snapshot.user_defined_enum")
                if "data_asset" in snapshot:
                    self.validate_data_asset_snapshot(snapshot["data_asset"], f"{path}.snapshot.data_asset")
                if "string_table" in snapshot:
                    self.validate_string_table_snapshot(snapshot["string_table"], f"{path}.snapshot.string_table")
                if "data_table" in snapshot:
                    self.validate_data_table_snapshot(snapshot["data_table"], f"{path}.snapshot.data_table")
                if "curve_table" in snapshot:
                    self.validate_curve_table_snapshot(snapshot["curve_table"], f"{path}.snapshot.curve_table")
                if "curve" in snapshot:
                    self.validate_curve_snapshot(snapshot["curve"], f"{path}.snapshot.curve")
                if "curve_linear_color_atlas" in snapshot:
                    self.validate_curve_linear_color_atlas_snapshot(
                        snapshot["curve_linear_color_atlas"],
                        f"{path}.snapshot.curve_linear_color_atlas",
                    )
                if "niagara_system" in snapshot:
                    self.validate_niagara_system_snapshot(snapshot["niagara_system"], f"{path}.snapshot.niagara_system")
                if "niagara_emitter" in snapshot:
                    self.validate_niagara_emitter_snapshot(snapshot["niagara_emitter"], f"{path}.snapshot.niagara_emitter")
                if "niagara_script" in snapshot:
                    self.validate_niagara_script_snapshot(snapshot["niagara_script"], f"{path}.snapshot.niagara_script")
                if "niagara_parameter_definitions" in snapshot:
                    self.validate_niagara_parameter_definitions_snapshot(
                        snapshot["niagara_parameter_definitions"],
                        f"{path}.snapshot.niagara_parameter_definitions",
                    )
                if "vector_field_static" in snapshot:
                    self.validate_vector_field_static_snapshot(snapshot["vector_field_static"], f"{path}.snapshot.vector_field_static")
                if "pcg_graph" in snapshot:
                    self.validate_pcg_graph_snapshot(snapshot["pcg_graph"], f"{path}.snapshot.pcg_graph")
                if "metasound" in snapshot:
                    self.validate_metasound_snapshot(snapshot["metasound"], f"{path}.snapshot.metasound")
                if "skeleton" in snapshot:
                    self.validate_skeleton_snapshot(snapshot["skeleton"], f"{path}.snapshot.skeleton")
                if "skeletal_mesh" in snapshot:
                    self.validate_skeletal_mesh_snapshot(snapshot["skeletal_mesh"], f"{path}.snapshot.skeletal_mesh")
                if "animation_sequence" in snapshot:
                    self.validate_animation_sequence_snapshot(snapshot["animation_sequence"], f"{path}.snapshot.animation_sequence")
                if "anim_montage" in snapshot:
                    self.validate_anim_montage_snapshot(snapshot["anim_montage"], f"{path}.snapshot.anim_montage")
                if "anim_composite" in snapshot:
                    self.validate_anim_composite_snapshot(snapshot["anim_composite"], f"{path}.snapshot.anim_composite")
                if "blend_space" in snapshot:
                    self.validate_blend_space_snapshot(snapshot["blend_space"], f"{path}.snapshot.blend_space")
                if "pose_asset" in snapshot:
                    self.validate_pose_asset_snapshot(snapshot["pose_asset"], f"{path}.snapshot.pose_asset")
                if "physics_asset" in snapshot:
                    self.validate_physics_asset_snapshot(snapshot["physics_asset"], f"{path}.snapshot.physics_asset")
                if "anim_blueprint" in snapshot:
                    self.validate_anim_blueprint_snapshot(snapshot["anim_blueprint"], f"{path}.snapshot.anim_blueprint")
                if "control_rig_blueprint" in snapshot:
                    self.validate_control_rig_blueprint_snapshot(
                        snapshot["control_rig_blueprint"],
                        f"{path}.snapshot.control_rig_blueprint",
                    )
                if "ik_rig" in snapshot:
                    self.validate_ik_rig_snapshot(snapshot["ik_rig"], f"{path}.snapshot.ik_rig")
                if "ik_retargeter" in snapshot:
                    self.validate_ik_retargeter_snapshot(snapshot["ik_retargeter"], f"{path}.snapshot.ik_retargeter")
                if "static_mesh" in snapshot:
                    self.validate_static_mesh_snapshot(snapshot["static_mesh"], f"{path}.snapshot.static_mesh")
                if "texture" in snapshot:
                    self.validate_texture_snapshot(snapshot["texture"], f"{path}.snapshot.texture")
                if "texture2d" in snapshot:
                    self.validate_texture2d_snapshot(snapshot["texture2d"], f"{path}.snapshot.texture2d")
                if "texture_cube" in snapshot:
                    self.validate_texture_cube_snapshot(snapshot["texture_cube"], f"{path}.snapshot.texture_cube")
                if "sound_submix" in snapshot:
                    self.validate_sound_submix_snapshot(snapshot["sound_submix"], f"{path}.snapshot.sound_submix")
                if "sound_submix_effect_preset" in snapshot:
                    self.validate_sound_submix_effect_preset_snapshot(snapshot["sound_submix_effect_preset"], f"{path}.snapshot.sound_submix_effect_preset")
                if "sound_cue" in snapshot:
                    self.validate_sound_cue_snapshot(snapshot["sound_cue"], f"{path}.snapshot.sound_cue")
                if "sound_wave" in snapshot:
                    self.validate_sound_wave_snapshot(snapshot["sound_wave"], f"{path}.snapshot.sound_wave")
                if "level_sequence" in snapshot:
                    self.validate_level_sequence_snapshot(snapshot["level_sequence"], f"{path}.snapshot.level_sequence")
                if "material" in snapshot:
                    self.validate_material_snapshot(snapshot["material"], f"{path}.snapshot.material")
                if "material_parameter_collection" in snapshot:
                    self.validate_material_parameter_collection_snapshot(
                        snapshot["material_parameter_collection"],
                        f"{path}.snapshot.material_parameter_collection",
                    )
                if "material_instance" in snapshot:
                    self.validate_material_instance_snapshot(snapshot["material_instance"], f"{path}.snapshot.material_instance")
                if "material_function" in snapshot:
                    self.validate_material_function_snapshot(snapshot["material_function"], f"{path}.snapshot.material_function")
                schema_version = snapshot.get("schema_version")
                if schema_version == "uepi.uobject_reflection.v1":
                    self.validate_uobject_reflection_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.skeleton.v1":
                    self.validate_skeleton_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.skeletal_mesh.v1":
                    self.validate_skeletal_mesh_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.anim_sequence.v1":
                    self.validate_animation_sequence_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.anim_montage.v1":
                    self.validate_anim_montage_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.anim_composite.v1":
                    self.validate_anim_composite_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.blend_space.v1":
                    self.validate_blend_space_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.pose_asset.v1":
                    self.validate_pose_asset_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.physics_asset.v1":
                    self.validate_physics_asset_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.anim_blueprint.v1":
                    self.validate_anim_blueprint_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.control_rig_blueprint.v1":
                    self.validate_control_rig_blueprint_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.ik_rig.v1":
                    self.validate_ik_rig_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.ik_retargeter.v1":
                    self.validate_ik_retargeter_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.static_mesh.v1":
                    self.validate_static_mesh_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.texture.v1":
                    self.validate_texture_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.texture2d.v1":
                    self.validate_texture2d_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.texture_cube.v1":
                    self.validate_texture_cube_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.sound_submix.v1":
                    self.validate_sound_submix_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.sound_submix_effect_preset.v1":
                    self.validate_sound_submix_effect_preset_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.sound_cue.v1":
                    self.validate_sound_cue_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.sound_wave.v1":
                    self.validate_sound_wave_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.level_sequence.v1":
                    self.validate_level_sequence_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.material.v1":
                    self.validate_material_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.material_parameter_collection.v1":
                    self.validate_material_parameter_collection_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.material_instance.v1":
                    self.validate_material_instance_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.material_function.v1":
                    self.validate_material_function_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.widget_blueprint.v1":
                    self.validate_widget_blueprint_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.common_ui_input_data.v1":
                    self.validate_common_ui_input_data_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.common_ui_hold_data.v1":
                    self.validate_common_ui_hold_data_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.common_ui_input_action_table.v1":
                    self.validate_common_ui_input_action_table_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.blackboard.v1":
                    self.validate_blackboard_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.behavior_tree.v1":
                    self.validate_behavior_tree_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.env_query.v1":
                    self.validate_env_query_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.state_tree.v1":
                    self.validate_state_tree_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.gameplay_ability.v1":
                    self.validate_gameplay_ability_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.gameplay_effect.v1":
                    self.validate_gameplay_effect_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.gameplay_cue_notify.v1":
                    self.validate_gameplay_cue_notify_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.user_defined_struct.v1":
                    self.validate_user_defined_struct_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.user_defined_enum.v1":
                    self.validate_user_defined_enum_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.data_asset.v1":
                    self.validate_data_asset_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.string_table.v1":
                    self.validate_string_table_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.data_table.v1":
                    self.validate_data_table_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.curve_table.v1":
                    self.validate_curve_table_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.curve.v1":
                    self.validate_curve_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.curve_linear_color_atlas.v1":
                    self.validate_curve_linear_color_atlas_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.niagara_system.v1":
                    self.validate_niagara_system_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.niagara_emitter.v1":
                    self.validate_niagara_emitter_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.niagara_script.v1":
                    self.validate_niagara_script_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.niagara_parameter_definitions.v1":
                    self.validate_niagara_parameter_definitions_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.vector_field_static.v1":
                    self.validate_vector_field_static_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.pcg_graph.v1":
                    self.validate_pcg_graph_snapshot(snapshot, f"{path}.snapshot")
                elif schema_version == "uepi.metasound.v1":
                    self.validate_metasound_snapshot(snapshot, f"{path}.snapshot")

    def validate_uobject_collection_artifact_manifest(
        self,
        value: Any,
        path: str,
        expected_item_count: int | None = None,
    ) -> None:
        required = ["schema_version", "artifact_id", "artifact_uri", "storage", "path", "item_count", "byte_count", "encoding"]
        if not self.require_keys(value, path, required):
            return
        if value.get("schema_version") != "uepi.uobject_collection_artifact_manifest.v1":
            self.error(f"{path}.schema_version", "expected uepi.uobject_collection_artifact_manifest.v1")
        for key in ("artifact_id", "artifact_uri", "storage", "path", "encoding"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)
        for key in ("item_count", "byte_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        if expected_item_count is not None and value.get("item_count") != expected_item_count:
            self.error(f"{path}.item_count", f"expected {expected_item_count}")
        if "write_error" in value:
            self.expect_string(value.get("write_error"), f"{path}.write_error", allow_empty=False)

    def validate_uobject_struct_value(self, value: Any, path: str, depth: int = 0) -> None:
        required = [
            "schema_version",
            "struct_path",
            "struct_name",
            "serializer",
            "depth",
            "max_depth",
            "field_count",
            "inline_field_count",
            "truncated",
            "fields",
        ]
        if not self.require_keys(value, path, required):
            return
        if value.get("schema_version") != "uepi.property_struct_value.v1":
            self.error(f"{path}.schema_version", "expected uepi.property_struct_value.v1")
        for key in ("struct_path", "struct_name", "serializer"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("depth", "max_depth", "field_count", "inline_field_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        self.expect_boolean(value.get("truncated"), f"{path}.truncated")
        if "normalized" in value and not isinstance(value.get("normalized"), dict):
            self.error(f"{path}.normalized", "expected object")

        fields = value.get("fields")
        if not isinstance(fields, list):
            self.error(f"{path}.fields", "expected array")
            fields = []
        if isinstance(value.get("inline_field_count"), int) and value.get("inline_field_count") != len(fields):
            self.error(f"{path}.inline_field_count", f"expected {len(fields)}")
        if isinstance(value.get("field_count"), int) and isinstance(value.get("inline_field_count"), int):
            if value["inline_field_count"] > value["field_count"]:
                self.error(f"{path}.inline_field_count", "expected <= field_count")
            expected_truncated = value["inline_field_count"] < value["field_count"]
            if isinstance(value.get("truncated"), bool) and value["truncated"] != expected_truncated:
                self.error(f"{path}.truncated", f"expected {expected_truncated}")
        if depth > 8:
            self.error(path, "struct value recursion exceeded validator depth")
            return
        for field_index, field in enumerate(fields):
            field_path = f"{path}.fields[{field_index}]"
            self.validate_uobject_property(field, field_path, depth + 1)
            if isinstance(field, dict) and "index" in field and field.get("index") != field_index:
                self.error(f"{field_path}.index", f"expected {field_index}")

    def validate_uobject_property(self, value: Any, path: str, depth: int = 0) -> None:
        required = ["name", "kind", "cpp_type", "source_layer", "value_text", "truncated"]
        if not self.require_keys(value, path, required):
            return
        for key in ("name", "kind", "cpp_type", "value_text"):
            self.expect_string(value.get(key), f"{path}.{key}")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_boolean(value.get("truncated"), f"{path}.truncated")
        if "index" in value:
            self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("declaring_struct_path", "object_path", "object_class_path", "cdo_value_text", "super_cdo_value_text", "declaring_class_path"):
            if key in value:
                self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("is_subobject_reference", "cycle_detected", "artifact_required", "differs_from_cdo", "is_inherited", "struct_depth_limited"):
            if key in value:
                self.expect_boolean(value.get(key), f"{path}.{key}")
        for key in ("reference_depth", "collection_count", "inline_limit"):
            if key in value:
                self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        if "collection_artifact" in value:
            expected_item_count = value.get("collection_count") if isinstance(value.get("collection_count"), int) else None
            self.validate_uobject_collection_artifact_manifest(value.get("collection_artifact"), f"{path}.collection_artifact", expected_item_count)
        if "struct_value" in value:
            self.validate_uobject_struct_value(value.get("struct_value"), f"{path}.struct_value", depth)

    def validate_uobject_reflection_snapshot(self, value: Any, path: str) -> None:
        required = ["schema_version", "source_layer", "object_path", "class_path", "property_count", "properties"]
        if not self.require_keys(value, path, required):
            return
        if value.get("schema_version") != "uepi.uobject_reflection.v1":
            self.error(f"{path}.schema_version", "expected uepi.uobject_reflection.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("object_path", "class_path", "outer_path", "class_default_object_path", "super_class_path", "cycle_guard"):
            if key in value:
                self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("property_count", "max_inline_collection_items", "max_reference_depth"):
            if key in value:
                self.expect_non_negative_integer(value.get(key), f"{path}.{key}")

        properties = value.get("properties")
        if not isinstance(properties, list):
            self.error(f"{path}.properties", "expected array")
            properties = []
        if value.get("property_count") != len(properties):
            self.error(f"{path}.property_count", f"expected {len(properties)}")
        for property_index, prop in enumerate(properties):
            self.validate_uobject_property(prop, f"{path}.properties[{property_index}]")

    def validate_relation(self, relation: Any, path: str) -> None:
        if not self.require_keys(
            relation,
            path,
            ["id", "type", "from_id", "to_id", "source_layer", "derived", "confidence", "evidence"],
        ):
            return

        relation_id = relation.get("id")
        if self.expect_hex64(relation_id, f"{path}.id"):
            if relation_id in self.relation_ids:
                self.error(f"{path}.id", f"duplicate relation id '{relation_id}'")
            self.relation_ids.add(relation_id)

        relation_type = relation.get("type")
        if not isinstance(relation_type, str) or not RELATION_TYPE_RE.match(relation_type):
            self.error(f"{path}.type", "expected snake_case relation type")

        for key in ("from_id", "to_id"):
            endpoint = relation.get(key)
            if self.expect_hex64(endpoint, f"{path}.{key}") and endpoint not in self.entity_ids:
                self.error(f"{path}.{key}", f"endpoint entity does not exist: {endpoint}")

        layer = relation.get("source_layer")
        if layer not in SOURCE_LAYERS:
            self.error(f"{path}.source_layer", f"unknown source layer '{layer}'")
        if not isinstance(relation.get("derived"), bool):
            self.error(f"{path}.derived", "expected boolean")
        confidence = relation.get("confidence")
        if not isinstance(confidence, (int, float)) or not 0 <= float(confidence) <= 1:
            self.error(f"{path}.confidence", "expected number between 0 and 1")
        self.expect_string_map(relation.get("attributes", {}), f"{path}.attributes")
        self.validate_evidence(relation.get("evidence"), f"{path}.evidence")

    def validate_blueprint_graph_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["schema_version", "source_layer", "blueprint_path", "graph_count", "graphs"]):
            return
        if value.get("schema_version") != "uepi.blueprint_graph.v1":
            self.error(f"{path}.schema_version", "expected uepi.blueprint_graph.v1")
        if value.get("source_layer") != "editor_source_graph":
            self.error(f"{path}.source_layer", "expected editor_source_graph")
        self.expect_string(value.get("blueprint_path"), f"{path}.blueprint_path", allow_empty=False)
        graphs = value.get("graphs")
        if not isinstance(graphs, list):
            self.error(f"{path}.graphs", "expected array")
            return
        if value.get("graph_count") != len(graphs):
            self.error(f"{path}.graph_count", f"expected {len(graphs)}")
        for graph_index, graph in enumerate(graphs):
            self.validate_blueprint_graph(graph, f"{path}.graphs[{graph_index}]")
        self.validate_counted_array(value, path, "component_template_count", "component_templates")

    def validate_blueprint_graph(self, graph: Any, path: str) -> None:
        if not self.require_keys(graph, path, ["id", "name", "guid", "path", "nodes"]):
            return
        self.expect_hex64(graph.get("id"), f"{path}.id")
        for key in ("name", "guid", "path"):
            self.expect_string(graph.get(key), f"{path}.{key}")

        nodes = graph.get("nodes")
        if not isinstance(nodes, list):
            self.error(f"{path}.nodes", "expected array")
            nodes = []
        for node_index, node in enumerate(nodes):
            self.validate_blueprint_node(node, f"{path}.nodes[{node_index}]")

        self.validate_counted_array(graph, path, "cfg_basic_block_count", "cfg_basic_blocks")
        self.validate_counted_array(graph, path, "dfg_value_count", "dfg_values")
        self.validate_counted_array(graph, path, "dfg_variable_access_count", "dfg_variable_accesses")

    def validate_blueprint_node(self, node: Any, path: str) -> None:
        if not self.require_keys(node, path, ["id", "name", "guid", "class", "title", "pins"]):
            return
        self.expect_hex64(node.get("id"), f"{path}.id")
        for key in ("name", "guid", "class", "title"):
            self.expect_string(node.get(key), f"{path}.{key}")
        pins = node.get("pins")
        if not isinstance(pins, list):
            self.error(f"{path}.pins", "expected array")
            return
        for pin_index, pin in enumerate(pins):
            pin_path = f"{path}.pins[{pin_index}]"
            if not self.require_keys(pin, pin_path, ["id", "pin_id", "name", "direction", "type", "linked_to"]):
                continue
            self.expect_hex64(pin.get("id"), f"{pin_path}.id")
            if pin.get("direction") not in {"input", "output", "unknown"}:
                self.error(f"{pin_path}.direction", "expected input/output/unknown")
            if not isinstance(pin.get("type"), dict):
                self.error(f"{pin_path}.type", "expected object")
            if not isinstance(pin.get("linked_to"), list):
                self.error(f"{pin_path}.linked_to", "expected array")

    def validate_counted_array(self, owner: dict[str, Any], path: str, count_key: str, array_key: str) -> None:
        if count_key not in owner and array_key not in owner:
            return
        values = owner.get(array_key, [])
        if not isinstance(values, list):
            self.error(f"{path}.{array_key}", "expected array")
            return
        if owner.get(count_key) != len(values):
            self.error(f"{path}.{count_key}", f"expected {len(values)}")

    def validate_world_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "world_path",
                "world_type",
                "level_count",
                "streaming_level_count",
                "actor_count",
                "component_count",
                "levels",
                "streaming_levels",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.world.v1":
            self.error(f"{path}.schema_version", "expected uepi.world.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_string(value.get("world_path"), f"{path}.world_path", allow_empty=False)
        self.expect_string(value.get("world_type"), f"{path}.world_type")

        levels = value.get("levels")
        if not isinstance(levels, list):
            self.error(f"{path}.levels", "expected array")
            return
        if value.get("level_count") != len(levels):
            self.error(f"{path}.level_count", f"expected {len(levels)}")

        streaming_levels = value.get("streaming_levels")
        if not isinstance(streaming_levels, list):
            self.error(f"{path}.streaming_levels", "expected array")
            streaming_levels = []
        elif value.get("streaming_level_count") != len(streaming_levels):
            self.error(f"{path}.streaming_level_count", f"expected {len(streaming_levels)}")

        streaming_states = {
            "removed",
            "unloaded",
            "failed_to_load",
            "loading",
            "loaded_not_visible",
            "making_visible",
            "loaded_visible",
            "making_invisible",
            "unknown",
        }
        for streaming_level_index, streaming_level in enumerate(streaming_levels):
            streaming_level_path = f"{path}.streaming_levels[{streaming_level_index}]"
            if not self.require_keys(
                streaming_level,
                streaming_level_path,
                [
                    "id",
                    "path",
                    "name",
                    "class",
                    "index",
                    "world_asset",
                    "world_asset_package",
                    "loaded_level_path",
                    "folder_path",
                    "should_be_loaded",
                    "should_be_visible",
                    "should_be_visible_flag",
                    "should_be_visible_in_editor",
                    "should_block_on_load",
                    "should_block_on_unload",
                    "should_be_always_loaded",
                    "is_level_loaded",
                    "is_level_visible",
                    "is_static",
                    "locked",
                    "level_lod_index",
                    "state",
                    "transform",
                ],
            ):
                continue
            self.expect_hex64(streaming_level.get("id"), f"{streaming_level_path}.id")
            for key in ("path", "name", "class", "world_asset", "world_asset_package", "loaded_level_path", "folder_path", "state"):
                self.expect_string(streaming_level.get(key), f"{streaming_level_path}.{key}")
            self.expect_integer(streaming_level.get("index"), f"{streaming_level_path}.index")
            self.expect_integer(streaming_level.get("level_lod_index"), f"{streaming_level_path}.level_lod_index")
            for key in (
                "should_be_loaded",
                "should_be_visible",
                "should_be_visible_flag",
                "should_be_visible_in_editor",
                "should_block_on_load",
                "should_block_on_unload",
                "should_be_always_loaded",
                "is_level_loaded",
                "is_level_visible",
                "is_static",
                "locked",
            ):
                self.expect_boolean(streaming_level.get(key), f"{streaming_level_path}.{key}")
            if streaming_level.get("state") not in streaming_states:
                self.error(f"{streaming_level_path}.state", "expected streaming state")
            self.validate_world_transform(streaming_level.get("transform"), f"{streaming_level_path}.transform")

        actor_count = 0
        component_count = 0
        for level_index, level in enumerate(levels):
            level_path = f"{path}.levels[{level_index}]"
            if not self.require_keys(
                level,
                level_path,
                [
                    "id",
                    "path",
                    "index",
                    "level_script_actor_id",
                    "level_script_actor_path",
                    "level_script_actor_class",
                    "actor_count",
                    "actors",
                ],
            ):
                continue
            self.expect_hex64(level.get("id"), f"{level_path}.id")
            self.expect_string(level.get("path"), f"{level_path}.path")
            self.expect_string(level.get("level_script_actor_id"), f"{level_path}.level_script_actor_id")
            if level.get("level_script_actor_id"):
                self.expect_hex64(level.get("level_script_actor_id"), f"{level_path}.level_script_actor_id")
            for key in ("level_script_actor_path", "level_script_actor_class"):
                self.expect_string(level.get(key), f"{level_path}.{key}")
            actors = level.get("actors")
            if not isinstance(actors, list):
                self.error(f"{level_path}.actors", "expected array")
                continue
            if level.get("actor_count") != len(actors):
                self.error(f"{level_path}.actor_count", f"expected {len(actors)}")
            actor_count += len(actors)
            for actor_index, actor in enumerate(actors):
                actor_path = f"{level_path}.actors[{actor_index}]"
                if not self.require_keys(
                    actor,
                    actor_path,
                    [
                        "id",
                        "path",
                        "name",
                        "label",
                        "class",
                        "actor_guid",
                        "actor_instance_guid",
                        "content_bundle_guid",
                        "folder_path",
                        "owner_id",
                        "owner_path",
                        "attach_parent_id",
                        "attach_parent_path",
                        "attach_parent_socket",
                        "tags",
                        "is_level_script_actor",
                        "is_level_instance",
                        "level_instance_guid",
                        "level_instance_world_asset",
                        "level_instance_world_package",
                        "level_instance_runtime_behavior",
                        "component_count",
                        "components",
                    ],
                ):
                    continue
                self.expect_hex64(actor.get("id"), f"{actor_path}.id")
                for key in (
                    "path",
                    "name",
                    "label",
                    "class",
                    "actor_guid",
                    "actor_instance_guid",
                    "content_bundle_guid",
                    "folder_path",
                    "owner_id",
                    "owner_path",
                    "attach_parent_id",
                    "attach_parent_path",
                    "attach_parent_socket",
                    "level_instance_guid",
                    "level_instance_world_asset",
                    "level_instance_world_package",
                    "level_instance_runtime_behavior",
                ):
                    self.expect_string(actor.get(key), f"{actor_path}.{key}")
                for key in ("owner_id", "attach_parent_id"):
                    if actor.get(key):
                        self.expect_hex64(actor.get(key), f"{actor_path}.{key}")
                for key in ("is_level_script_actor", "is_level_instance"):
                    self.expect_boolean(actor.get(key), f"{actor_path}.{key}")
                tags = actor.get("tags")
                if not isinstance(tags, list):
                    self.error(f"{actor_path}.tags", "expected array")
                else:
                    for tag_index, tag in enumerate(tags):
                        self.expect_string(tag, f"{actor_path}.tags[{tag_index}]")
                components = actor.get("components")
                if not isinstance(components, list):
                    self.error(f"{actor_path}.components", "expected array")
                    continue
                if actor.get("component_count") != len(components):
                    self.error(f"{actor_path}.component_count", f"expected {len(components)}")
                component_count += len(components)
                for component_index, component in enumerate(components):
                    component_path = f"{actor_path}.components[{component_index}]"
                    if not self.require_keys(component, component_path, ["id", "path", "name", "class"]):
                        continue
                    self.expect_hex64(component.get("id"), f"{component_path}.id")
                    for key in ("path", "name", "class"):
                        self.expect_string(component.get(key), f"{component_path}.{key}")

        if value.get("actor_count") != actor_count:
            self.error(f"{path}.actor_count", f"expected {actor_count}")
        if value.get("component_count") != component_count:
            self.error(f"{path}.component_count", f"expected {component_count}")

    def validate_world_data_layers_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "world_data_layers_path",
                "world_data_layers_class",
                "world_path",
                "level_path",
                "data_layer_count",
                "data_layers",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.world_data_layers.v1":
            self.error(f"{path}.schema_version", "expected uepi.world_data_layers.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("world_data_layers_path", "world_data_layers_class", "world_path", "level_path"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_non_negative_integer(value.get("data_layer_count"), f"{path}.data_layer_count")

        data_layers = value.get("data_layers")
        if not isinstance(data_layers, list):
            self.error(f"{path}.data_layers", "expected array")
            return
        if value.get("data_layer_count") != len(data_layers):
            self.error(f"{path}.data_layer_count", f"expected {len(data_layers)}")

        runtime_states = {"Unloaded", "Loaded", "Activated", "Invalid"}
        layer_types = {"runtime", "editor", "unknown"}
        for layer_index, layer in enumerate(data_layers):
            layer_path = f"{path}.data_layers[{layer_index}]"
            if not self.require_keys(
                layer,
                layer_path,
                [
                    "id",
                    "path",
                    "name",
                    "short_name",
                    "full_name",
                    "class",
                    "type",
                    "is_runtime",
                    "initial_runtime_state",
                    "runtime_state",
                    "effective_runtime_state",
                    "initially_visible",
                    "initially_loaded_in_editor",
                    "parent_id",
                    "parent_path",
                    "asset_path",
                    "debug_color",
                    "child_count",
                    "children",
                ],
            ):
                continue
            self.expect_hex64(layer.get("id"), f"{layer_path}.id")
            for key in ("path", "name", "short_name", "full_name", "class", "parent_id", "parent_path", "asset_path"):
                self.expect_string(layer.get(key), f"{layer_path}.{key}")
            if layer.get("type") not in layer_types:
                self.error(f"{layer_path}.type", "expected runtime/editor/unknown")
            for key in ("is_runtime", "initially_visible", "initially_loaded_in_editor"):
                self.expect_boolean(layer.get(key), f"{layer_path}.{key}")
            for key in ("initial_runtime_state", "runtime_state", "effective_runtime_state"):
                if layer.get(key) not in runtime_states:
                    self.error(f"{layer_path}.{key}", "expected Data Layer runtime state")

            debug_color = layer.get("debug_color")
            if not isinstance(debug_color, dict):
                self.error(f"{layer_path}.debug_color", "expected object")
            else:
                for channel in ("r", "g", "b", "a"):
                    if self.expect_integer(debug_color.get(channel), f"{layer_path}.debug_color.{channel}"):
                        if debug_color[channel] < 0 or debug_color[channel] > 255:
                            self.error(f"{layer_path}.debug_color.{channel}", "expected 0..255")

            children = layer.get("children")
            if not isinstance(children, list):
                self.error(f"{layer_path}.children", "expected array")
                continue
            if layer.get("child_count") != len(children):
                self.error(f"{layer_path}.child_count", f"expected {len(children)}")
            for child_index, child in enumerate(children):
                child_path = f"{layer_path}.children[{child_index}]"
                if not self.require_keys(child, child_path, ["id", "path"]):
                    continue
                self.expect_hex64(child.get("id"), f"{child_path}.id")
                self.expect_string(child.get("path"), f"{child_path}.path")

    def validate_guid_string(self, value: Any, path: str) -> None:
        if not self.expect_string(value, path, allow_empty=False):
            return
        if not GUID_RE.match(value):
            self.error(path, "expected lowercase GUID with hyphens")

    def validate_world_box(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["is_valid", "min", "max"]):
            return
        self.expect_boolean(value.get("is_valid"), f"{path}.is_valid")
        for key in ("min", "max"):
            vector = value.get(key)
            vector_path = f"{path}.{key}"
            if not self.require_keys(vector, vector_path, ["x", "y", "z"]):
                continue
            for axis in ("x", "y", "z"):
                self.expect_number(vector.get(axis), f"{vector_path}.{axis}")

    def validate_world_transform(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["location", "rotation", "scale"]):
            return
        for key in ("location", "scale"):
            vector = value.get(key)
            vector_path = f"{path}.{key}"
            if not self.require_keys(vector, vector_path, ["x", "y", "z"]):
                continue
            for axis in ("x", "y", "z"):
                self.expect_number(vector.get(axis), f"{vector_path}.{axis}")
        rotation = value.get("rotation")
        if self.require_keys(rotation, f"{path}.rotation", ["pitch", "yaw", "roll"]):
            for axis in ("pitch", "yaw", "roll"):
                self.expect_number(rotation.get(axis), f"{path}.rotation.{axis}")

    def validate_world_partition_actor_desc_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "guid",
                "actor_name",
                "actor_label",
                "actor_label_or_name",
                "actor_path",
                "actor_package",
                "native_class",
                "base_class",
                "display_class_name",
                "runtime_grid",
                "is_spatially_loaded",
                "is_spatially_loaded_raw",
                "actor_is_editor_only",
                "actor_is_runtime_only",
                "actor_is_hlod_relevant",
                "is_default_actor_desc",
                "is_container_instance",
                "hlod_layer",
                "folder_path",
                "folder_guid",
                "parent_actor_guid",
                "content_bundle_guid",
                "editor_bounds",
                "runtime_bounds",
                "data_layers",
                "data_layer_instance_names",
                "tags",
                "references",
                "reference_count",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key in ("guid", "folder_guid", "parent_actor_guid", "content_bundle_guid"):
            self.validate_guid_string(value.get(key), f"{path}.{key}")
        for key in (
            "actor_name",
            "actor_label",
            "actor_label_or_name",
            "actor_path",
            "actor_package",
            "native_class",
            "base_class",
            "display_class_name",
            "runtime_grid",
            "hlod_layer",
            "folder_path",
        ):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in (
            "is_spatially_loaded",
            "is_spatially_loaded_raw",
            "actor_is_editor_only",
            "actor_is_runtime_only",
            "actor_is_hlod_relevant",
            "is_default_actor_desc",
            "is_container_instance",
        ):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        self.validate_world_box(value.get("editor_bounds"), f"{path}.editor_bounds")
        self.validate_world_box(value.get("runtime_bounds"), f"{path}.runtime_bounds")

        for array_key in ("data_layers", "data_layer_instance_names", "tags", "references"):
            items = value.get(array_key)
            if not isinstance(items, list):
                self.error(f"{path}.{array_key}", "expected array")
                continue
            for item_index, item in enumerate(items):
                item_path = f"{path}.{array_key}[{item_index}]"
                if array_key == "references":
                    self.validate_guid_string(item, item_path)
                else:
                    self.expect_string(item, item_path)
        references = value.get("references")
        if isinstance(references, list) and value.get("reference_count") != len(references):
            self.error(f"{path}.reference_count", f"expected {len(references)}")

    def validate_world_partition_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "world_path",
                "world_partition_path",
                "world_partition_class",
                "actor_desc_container_count",
                "actor_desc_count",
                "actor_descs",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.world_partition.v1":
            self.error(f"{path}.schema_version", "expected uepi.world_partition.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("world_path", "world_partition_path", "world_partition_class"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("actor_desc_container_count", "actor_desc_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        actor_descs = value.get("actor_descs")
        if not isinstance(actor_descs, list):
            self.error(f"{path}.actor_descs", "expected array")
            return
        if value.get("actor_desc_count") != len(actor_descs):
            self.error(f"{path}.actor_desc_count", f"expected {len(actor_descs)}")
        for actor_desc_index, actor_desc in enumerate(actor_descs):
            self.validate_world_partition_actor_desc_snapshot(actor_desc, f"{path}.actor_descs[{actor_desc_index}]")

    def validate_widget_blueprint_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "widget_blueprint_path",
                "generated_class_path",
                "parent_class_path",
                "root_widget_id",
                "root_widget_name",
                "widget_count",
                "animation_count",
                "binding_count",
                "named_slot_count",
                "widgets",
                "animations",
                "bindings",
                "named_slots",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.widget_blueprint.v1":
            self.error(f"{path}.schema_version", "expected uepi.widget_blueprint.v1")
        if value.get("source_layer") != "editor_source_graph":
            self.error(f"{path}.source_layer", "expected editor_source_graph")
        for key in ("widget_blueprint_path", "generated_class_path", "parent_class_path", "root_widget_id", "root_widget_name"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key in {"root_widget_id", "root_widget_name"})
        for key in ("widget_count", "animation_count", "binding_count", "named_slot_count"):
            if not isinstance(value.get(key), int) or value.get(key) < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")

        widgets = value.get("widgets")
        if not isinstance(widgets, list):
            self.error(f"{path}.widgets", "expected array")
            widgets = []
        if value.get("widget_count") != len(widgets):
            self.error(f"{path}.widget_count", f"expected {len(widgets)}")
        widget_ids: set[str] = set()
        root_count = 0
        for widget_index, widget in enumerate(widgets):
            widget_path = f"{path}.widgets[{widget_index}]"
            if not self.require_keys(
                widget,
                widget_path,
                ["id", "index", "name", "path", "class", "parent_id", "parent_name", "is_root", "is_variable", "child_count", "visibility"],
            ):
                continue
            if self.expect_hex64(widget.get("id"), f"{widget_path}.id"):
                widget_ids.add(widget["id"])
            if widget.get("index") != widget_index:
                self.error(f"{widget_path}.index", f"expected {widget_index}")
            for key in ("name", "path", "class", "parent_id", "parent_name", "visibility"):
                self.expect_string(widget.get(key), f"{widget_path}.{key}")
            for key in ("is_root", "is_variable"):
                if not isinstance(widget.get(key), bool):
                    self.error(f"{widget_path}.{key}", "expected boolean")
            if widget.get("is_root"):
                root_count += 1
            if not isinstance(widget.get("child_count"), int) or widget.get("child_count") < 0:
                self.error(f"{widget_path}.child_count", "expected non-negative integer")
        root_widget_id = value.get("root_widget_id")
        if root_widget_id and root_widget_id not in widget_ids:
            self.error(f"{path}.root_widget_id", "root widget id not found in widgets")
        if widgets and root_count != 1:
            self.error(f"{path}.widgets", f"expected exactly one root widget, got {root_count}")

        animations = value.get("animations")
        if not isinstance(animations, list):
            self.error(f"{path}.animations", "expected array")
            animations = []
        if value.get("animation_count") != len(animations):
            self.error(f"{path}.animation_count", f"expected {len(animations)}")
        for animation_index, animation in enumerate(animations):
            animation_path = f"{path}.animations[{animation_index}]"
            if not self.require_keys(animation, animation_path, ["id", "index", "name", "path", "start_time", "end_time", "binding_count"]):
                continue
            self.expect_hex64(animation.get("id"), f"{animation_path}.id")
            if animation.get("index") != animation_index:
                self.error(f"{animation_path}.index", f"expected {animation_index}")
            for key in ("name", "path"):
                self.expect_string(animation.get(key), f"{animation_path}.{key}")
            for key in ("start_time", "end_time"):
                if not isinstance(animation.get(key), (int, float)):
                    self.error(f"{animation_path}.{key}", "expected number")
            if not isinstance(animation.get("binding_count"), int) or animation.get("binding_count") < 0:
                self.error(f"{animation_path}.binding_count", "expected non-negative integer")

        bindings = value.get("bindings")
        if not isinstance(bindings, list):
            self.error(f"{path}.bindings", "expected array")
            bindings = []
        if value.get("binding_count") != len(bindings):
            self.error(f"{path}.binding_count", f"expected {len(bindings)}")
        for binding_index, binding in enumerate(bindings):
            binding_path = f"{path}.bindings[{binding_index}]"
            if not self.require_keys(binding, binding_path, ["id", "index", "object_name", "property_name", "function_name", "source_property", "binding_kind"]):
                continue
            self.expect_hex64(binding.get("id"), f"{binding_path}.id")
            if binding.get("index") != binding_index:
                self.error(f"{binding_path}.index", f"expected {binding_index}")
            for key in ("object_name", "property_name", "function_name", "source_property", "binding_kind"):
                self.expect_string(binding.get(key), f"{binding_path}.{key}")
            if binding.get("binding_kind") not in {"function", "property", "unknown"}:
                self.error(f"{binding_path}.binding_kind", "expected function/property/unknown")

        named_slots = value.get("named_slots")
        if not isinstance(named_slots, list):
            self.error(f"{path}.named_slots", "expected array")
            named_slots = []
        if value.get("named_slot_count") != len(named_slots):
            self.error(f"{path}.named_slot_count", f"expected {len(named_slots)}")
        for slot_index, slot in enumerate(named_slots):
            slot_path = f"{path}.named_slots[{slot_index}]"
            if not self.require_keys(slot, slot_path, ["name", "content_widget_name"]):
                continue
            self.expect_string(slot.get("name"), f"{slot_path}.name")
            self.expect_string(slot.get("content_widget_name"), f"{slot_path}.content_widget_name")

    def validate_niagara_parameter(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            ["index", "scope", "name", "namespace", "type", "size_bytes", "has_default_value", "default_value"],
        ):
            return
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("scope", "name", "namespace", "type", "default_value"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_non_negative_integer(value.get("size_bytes"), f"{path}.size_bytes")
        self.expect_boolean(value.get("has_default_value"), f"{path}.has_default_value")

    def validate_niagara_parameter_definition(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "script_variable_path",
                "name",
                "namespace",
                "type",
                "size_bytes",
                "default_mode",
                "static_switch",
                "static_switch_default_value",
                "subscribed_to_parameter_definitions",
                "overrides_parameter_definitions_default_value",
                "change_id",
                "variable_guid",
                "description",
                "category_name",
                "display_unit",
                "advanced_display",
                "display_in_overview_stack",
                "inline_parameter_sort_priority",
                "editor_sort_priority",
                "inline_edit_condition_toggle",
                "parent_attribute",
                "alternate_alias_count",
                "property_metadata_count",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in (
            "script_variable_path",
            "name",
            "namespace",
            "type",
            "default_mode",
            "change_id",
            "variable_guid",
            "description",
            "category_name",
            "display_unit",
            "parent_attribute",
        ):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_non_negative_integer(value.get("size_bytes"), f"{path}.size_bytes")
        self.expect_integer(value.get("static_switch_default_value"), f"{path}.static_switch_default_value")
        for key in (
            "static_switch",
            "subscribed_to_parameter_definitions",
            "overrides_parameter_definitions_default_value",
            "advanced_display",
            "display_in_overview_stack",
            "inline_edit_condition_toggle",
        ):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        for key in ("inline_parameter_sort_priority", "editor_sort_priority"):
            self.expect_integer(value.get(key), f"{path}.{key}")
        for key in ("alternate_alias_count", "property_metadata_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")

    def validate_niagara_parameter_definitions_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "definitions_path",
                "definitions_class",
                "definitions_unique_id",
                "promote_to_top_in_add_menus",
                "menu_sort_order",
                "parameter_count",
                "parameters",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.niagara_parameter_definitions.v1":
            self.error(f"{path}.schema_version", "expected uepi.niagara_parameter_definitions.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("definitions_path", "definitions_class", "definitions_unique_id"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_boolean(value.get("promote_to_top_in_add_menus"), f"{path}.promote_to_top_in_add_menus")
        self.expect_integer(value.get("menu_sort_order"), f"{path}.menu_sort_order")
        self.expect_non_negative_integer(value.get("parameter_count"), f"{path}.parameter_count")

        parameters = value.get("parameters")
        if not isinstance(parameters, list):
            self.error(f"{path}.parameters", "expected array")
            return
        if value.get("parameter_count") != len(parameters):
            self.error(f"{path}.parameter_count", f"expected {len(parameters)}")
        for parameter_index, parameter in enumerate(parameters):
            parameter_path = f"{path}.parameters[{parameter_index}]"
            self.validate_niagara_parameter_definition(parameter, parameter_path)
            if isinstance(parameter, dict) and parameter.get("index") != parameter_index:
                self.error(f"{parameter_path}.index", f"expected {parameter_index}")

    def validate_niagara_script_ref(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["id", "index", "role", "script_path", "usage", "usage_id"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("role", "script_path", "usage", "usage_id"):
            self.expect_string(value.get(key), f"{path}.{key}")

    def validate_niagara_renderer(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            ["id", "index", "name", "class", "is_enabled", "is_active", "sort_order_hint", "source_mode"],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("name", "class"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("is_enabled", "is_active"):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        for key in ("sort_order_hint", "source_mode"):
            self.expect_integer(value.get(key), f"{path}.{key}")

    def validate_niagara_event_handler(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "source_event_name",
                "source_emitter_id",
                "execution_mode",
                "spawn_number",
                "max_events_per_frame",
                "random_spawn_number",
                "min_spawn_number",
                "update_attribute_initial_values",
                "script_usage_id",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("source_event_name", "source_emitter_id", "execution_mode", "script_usage_id"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("spawn_number", "max_events_per_frame", "min_spawn_number"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        for key in ("random_spawn_number", "update_attribute_initial_values"):
            self.expect_boolean(value.get(key), f"{path}.{key}")

    def validate_niagara_simulation_stage(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            ["id", "index", "name", "class", "is_enabled", "script_usage_id", "script_path"],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("name", "class", "script_usage_id", "script_path"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_boolean(value.get("is_enabled"), f"{path}.is_enabled")

    def validate_niagara_emitter_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "schema_version",
                "source_layer",
                "emitter_path",
                "index",
                "name",
                "handle_id",
                "unique_instance_name",
                "is_enabled",
                "version_guid",
                "version_major",
                "version_minor",
                "deprecated",
                "local_space",
                "deterministic",
                "random_seed",
                "interpolated_spawning",
                "requires_persistent_ids",
                "sim_target",
                "bounds_mode",
                "preallocation_count",
                "allocation_mode",
                "renderer_count",
                "event_handler_count",
                "simulation_stage_count",
                "script_count",
                "renderers",
                "event_handlers",
                "simulation_stages",
                "scripts",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        if value.get("schema_version") != "uepi.niagara_emitter.v1":
            self.error(f"{path}.schema_version", "expected uepi.niagara_emitter.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("emitter_path", "name", "handle_id", "unique_instance_name", "version_guid", "sim_target", "bounds_mode", "allocation_mode"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("index", "version_major", "version_minor", "preallocation_count", "renderer_count", "event_handler_count", "simulation_stage_count", "script_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        self.expect_integer(value.get("random_seed"), f"{path}.random_seed")
        for key in ("is_enabled", "deprecated", "local_space", "deterministic", "interpolated_spawning", "requires_persistent_ids"):
            self.expect_boolean(value.get(key), f"{path}.{key}")

        renderers = value.get("renderers")
        if not isinstance(renderers, list):
            self.error(f"{path}.renderers", "expected array")
            renderers = []
        if value.get("renderer_count") != len(renderers):
            self.error(f"{path}.renderer_count", f"expected {len(renderers)}")
        for renderer_index, renderer in enumerate(renderers):
            renderer_path = f"{path}.renderers[{renderer_index}]"
            self.validate_niagara_renderer(renderer, renderer_path)
            if isinstance(renderer, dict) and renderer.get("index") != renderer_index:
                self.error(f"{renderer_path}.index", f"expected {renderer_index}")

        event_handlers = value.get("event_handlers")
        if not isinstance(event_handlers, list):
            self.error(f"{path}.event_handlers", "expected array")
            event_handlers = []
        if value.get("event_handler_count") != len(event_handlers):
            self.error(f"{path}.event_handler_count", f"expected {len(event_handlers)}")
        for event_index, event_handler in enumerate(event_handlers):
            event_path = f"{path}.event_handlers[{event_index}]"
            self.validate_niagara_event_handler(event_handler, event_path)
            if isinstance(event_handler, dict) and event_handler.get("index") != event_index:
                self.error(f"{event_path}.index", f"expected {event_index}")

        simulation_stages = value.get("simulation_stages")
        if not isinstance(simulation_stages, list):
            self.error(f"{path}.simulation_stages", "expected array")
            simulation_stages = []
        if value.get("simulation_stage_count") != len(simulation_stages):
            self.error(f"{path}.simulation_stage_count", f"expected {len(simulation_stages)}")
        for stage_index, stage in enumerate(simulation_stages):
            stage_path = f"{path}.simulation_stages[{stage_index}]"
            self.validate_niagara_simulation_stage(stage, stage_path)
            if isinstance(stage, dict) and stage.get("index") != stage_index:
                self.error(f"{stage_path}.index", f"expected {stage_index}")

        scripts = value.get("scripts")
        if not isinstance(scripts, list):
            self.error(f"{path}.scripts", "expected array")
            return
        if value.get("script_count") != len(scripts):
            self.error(f"{path}.script_count", f"expected {len(scripts)}")
        for script_index, script in enumerate(scripts):
            script_path = f"{path}.scripts[{script_index}]"
            self.validate_niagara_script_ref(script, script_path)
            if isinstance(script, dict) and script.get("index") != script_index:
                self.error(f"{script_path}.index", f"expected {script_index}")

    def validate_niagara_script_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "schema_version",
                "source_layer",
                "script_path",
                "script_class",
                "usage",
                "usage_id",
                "compile_status",
                "vm_valid",
                "has_bytecode",
                "bytecode_length",
                "num_temp_registers",
                "num_user_pointers",
                "attribute_count",
                "data_interface_count",
                "uobject_reference_count",
                "called_vm_external_function_count",
                "read_dataset_count",
                "write_dataset_count",
                "simulation_stage_metadata_count",
                "last_op_count",
                "error_message",
                "rapid_iteration_parameter_count",
                "rapid_iteration_parameters",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        if value.get("schema_version") != "uepi.niagara_script.v1":
            self.error(f"{path}.schema_version", "expected uepi.niagara_script.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("script_path", "script_class", "usage", "usage_id", "compile_status", "error_message"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("vm_valid", "has_bytecode"):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        for key in (
            "bytecode_length",
            "num_temp_registers",
            "num_user_pointers",
            "attribute_count",
            "data_interface_count",
            "uobject_reference_count",
            "called_vm_external_function_count",
            "read_dataset_count",
            "write_dataset_count",
            "simulation_stage_metadata_count",
            "last_op_count",
            "rapid_iteration_parameter_count",
        ):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")

        parameters = value.get("rapid_iteration_parameters")
        if not isinstance(parameters, list):
            self.error(f"{path}.rapid_iteration_parameters", "expected array")
            return
        if value.get("rapid_iteration_parameter_count") != len(parameters):
            self.error(f"{path}.rapid_iteration_parameter_count", f"expected {len(parameters)}")
        for parameter_index, parameter in enumerate(parameters):
            parameter_path = f"{path}.rapid_iteration_parameters[{parameter_index}]"
            self.validate_niagara_parameter(parameter, parameter_path)
            if isinstance(parameter, dict) and parameter.get("index") != parameter_index:
                self.error(f"{parameter_path}.index", f"expected {parameter_index}")

    def validate_niagara_system_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "system_path",
                "system_class",
                "is_valid",
                "is_ready_to_run",
                "needs_warmup",
                "warmup_time",
                "warmup_tick_count",
                "warmup_tick_delta",
                "fixed_tick_delta",
                "fixed_tick_delta_time",
                "deterministic",
                "random_seed",
                "fixed_bounds",
                "effect_type_path",
                "emitter_count",
                "system_script_count",
                "scripts",
                "user_parameter_count",
                "user_parameters",
                "emitters",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.niagara_system.v1":
            self.error(f"{path}.schema_version", "expected uepi.niagara_system.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("system_path", "system_class", "effect_type_path"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "effect_type_path")
        for key in ("is_valid", "is_ready_to_run", "needs_warmup", "fixed_tick_delta", "deterministic", "fixed_bounds"):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        for key in ("warmup_time", "warmup_tick_delta", "fixed_tick_delta_time"):
            self.expect_number(value.get(key), f"{path}.{key}")
        for key in ("warmup_tick_count", "emitter_count", "system_script_count", "user_parameter_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        self.expect_integer(value.get("random_seed"), f"{path}.random_seed")

        scripts = value.get("scripts")
        if not isinstance(scripts, list):
            self.error(f"{path}.scripts", "expected array")
            scripts = []
        if value.get("system_script_count") != len(scripts):
            self.error(f"{path}.system_script_count", f"expected {len(scripts)}")
        for script_index, script in enumerate(scripts):
            script_path = f"{path}.scripts[{script_index}]"
            self.validate_niagara_script_ref(script, script_path)
            if isinstance(script, dict) and script.get("index") != script_index:
                self.error(f"{script_path}.index", f"expected {script_index}")

        parameters = value.get("user_parameters")
        if not isinstance(parameters, list):
            self.error(f"{path}.user_parameters", "expected array")
            parameters = []
        if value.get("user_parameter_count") != len(parameters):
            self.error(f"{path}.user_parameter_count", f"expected {len(parameters)}")
        for parameter_index, parameter in enumerate(parameters):
            parameter_path = f"{path}.user_parameters[{parameter_index}]"
            self.validate_niagara_parameter(parameter, parameter_path)
            if isinstance(parameter, dict) and parameter.get("index") != parameter_index:
                self.error(f"{parameter_path}.index", f"expected {parameter_index}")

        emitters = value.get("emitters")
        if not isinstance(emitters, list):
            self.error(f"{path}.emitters", "expected array")
            return
        if value.get("emitter_count") != len(emitters):
            self.error(f"{path}.emitter_count", f"expected {len(emitters)}")
        for emitter_index, emitter in enumerate(emitters):
            emitter_path = f"{path}.emitters[{emitter_index}]"
            self.validate_niagara_emitter_snapshot(emitter, emitter_path)
            if isinstance(emitter, dict) and emitter.get("index") != emitter_index:
                self.error(f"{emitter_path}.index", f"expected {emitter_index}")

    def validate_vector_field_static_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "vector_field_path",
                "vector_field_class",
                "size_x",
                "size_y",
                "size_z",
                "voxel_count",
                "intensity",
                "bounds_min",
                "bounds_max",
                "bounds_extent",
                "allow_cpu_access",
                "source_data_size_bytes",
                "has_cpu_data",
                "cpu_value_count",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.vector_field_static.v1":
            self.error(f"{path}.schema_version", "expected uepi.vector_field_static.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("vector_field_path", "vector_field_class"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)
        for key in ("size_x", "size_y", "size_z", "voxel_count", "source_data_size_bytes", "cpu_value_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        self.expect_number(value.get("intensity"), f"{path}.intensity")
        for key in ("bounds_min", "bounds_max", "bounds_extent"):
            self.validate_vector(value.get(key), f"{path}.{key}")
        for key in ("allow_cpu_access", "has_cpu_data"):
            self.expect_boolean(value.get(key), f"{path}.{key}")

        size_x = value.get("size_x")
        size_y = value.get("size_y")
        size_z = value.get("size_z")
        voxel_count = value.get("voxel_count")
        if all(isinstance(size, int) and not isinstance(size, bool) for size in (size_x, size_y, size_z, voxel_count)):
            expected_voxel_count = size_x * size_y * size_z
            if voxel_count != expected_voxel_count:
                self.error(f"{path}.voxel_count", f"expected {expected_voxel_count}")

    def validate_pcg_property(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["index", "name", "type", "value", "overridable"]):
            return
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("name", "type", "value"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_boolean(value.get("overridable"), f"{path}.overridable")

    def validate_pcg_settings(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "node_id",
                "settings_path",
                "settings_class",
                "settings_type",
                "enabled",
                "debug",
                "uses_seed",
                "seed",
                "filter_on_tag_count",
                "tags_applied_on_output_count",
                "filter_on_tags",
                "tags_applied_on_output",
                "property_count",
                "properties",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_hex64(value.get("node_id"), f"{path}.node_id")
        for key in ("settings_path", "settings_class", "settings_type"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "settings_type")
        for key in ("enabled", "debug", "uses_seed"):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        self.expect_integer(value.get("seed"), f"{path}.seed")
        for count_key, array_key in (
            ("filter_on_tag_count", "filter_on_tags"),
            ("tags_applied_on_output_count", "tags_applied_on_output"),
        ):
            self.expect_non_negative_integer(value.get(count_key), f"{path}.{count_key}")
            items = value.get(array_key)
            if not isinstance(items, list):
                self.error(f"{path}.{array_key}", "expected array")
                continue
            if value.get(count_key) != len(items):
                self.error(f"{path}.{count_key}", f"expected {len(items)}")
            for index, item in enumerate(items):
                self.expect_string(item, f"{path}.{array_key}[{index}]")

        properties = value.get("properties")
        if not isinstance(properties, list):
            self.error(f"{path}.properties", "expected array")
            properties = []
        if value.get("property_count") != len(properties):
            self.error(f"{path}.property_count", f"expected {len(properties)}")
        for index, item in enumerate(properties):
            item_path = f"{path}.properties[{index}]"
            self.validate_pcg_property(item, item_path)
            if isinstance(item, dict) and item.get("index") != index:
                self.error(f"{item_path}.index", f"expected {index}")

    def validate_pcg_node(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "node_path",
                "node_class",
                "title",
                "node_title",
                "settings_id",
                "settings_path",
                "position_x",
                "position_y",
                "input_pin_count",
                "output_pin_count",
                "has_inbound_edges",
                "inbound_edge_count",
                "comment",
                "comment_bubble_pinned",
                "comment_bubble_visible",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("node_path", "node_class", "title", "node_title", "settings_path", "comment"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key in {"node_title", "settings_path", "comment"})
        settings_id = value.get("settings_id")
        if settings_id:
            self.expect_hex64(settings_id, f"{path}.settings_id")
        else:
            self.expect_string(settings_id, f"{path}.settings_id")
        for key in ("position_x", "position_y"):
            self.expect_integer(value.get(key), f"{path}.{key}")
        for key in ("input_pin_count", "output_pin_count", "inbound_edge_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        for key in ("has_inbound_edges", "comment_bubble_pinned", "comment_bubble_visible"):
            self.expect_boolean(value.get(key), f"{path}.{key}")

    def validate_pcg_pin(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "node_id",
                "direction",
                "label",
                "allowed_types",
                "current_types",
                "allow_multiple_data",
                "allow_multiple_connections",
                "advanced_pin",
                "edge_count",
                "tooltip",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        self.expect_hex64(value.get("node_id"), f"{path}.node_id")
        if value.get("direction") not in {"input", "output"}:
            self.error(f"{path}.direction", "expected input or output")
        for key in ("label", "allowed_types", "current_types", "tooltip"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key in {"label", "tooltip"})
        for key in ("allow_multiple_data", "allow_multiple_connections", "advanced_pin"):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        self.expect_non_negative_integer(value.get("edge_count"), f"{path}.edge_count")

    def validate_pcg_edge(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "source_node_id",
                "source_pin_id",
                "source_pin_label",
                "target_node_id",
                "target_pin_id",
                "target_pin_label",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("source_node_id", "source_pin_id", "target_node_id", "target_pin_id"):
            self.expect_hex64(value.get(key), f"{path}.{key}")
        for key in ("source_pin_label", "target_pin_label"):
            self.expect_string(value.get(key), f"{path}.{key}")

    def validate_pcg_subgraph(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["id", "index", "node_id", "subgraph_path", "subgraph_class", "subgraph_name"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        self.expect_hex64(value.get("node_id"), f"{path}.node_id")
        for key in ("subgraph_path", "subgraph_class", "subgraph_name"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)

    def validate_universal_graph_ir(self, value: Any, path: str, expected_domain: str) -> None:
        if not self.require_keys(value, path, ["domain", "graph", "nodes", "ports", "links", "domain_semantics"]):
            return
        if value.get("domain") != expected_domain:
            self.error(f"{path}.domain", f"expected {expected_domain}")

        graph = value.get("graph")
        if self.require_keys(graph, f"{path}.graph", ["id", "path", "class", "name"]):
            self.expect_hex64(graph.get("id"), f"{path}.graph.id")
            for key in ("path", "class", "name"):
                self.expect_string(graph.get(key), f"{path}.graph.{key}", allow_empty=False)

        nodes = value.get("nodes")
        if not isinstance(nodes, list):
            self.error(f"{path}.nodes", "expected array")
            nodes = []
        for index, node in enumerate(nodes):
            node_path = f"{path}.nodes[{index}]"
            if not self.require_keys(node, node_path, ["id", "name", "class", "semantic_kind", "x", "y"]):
                continue
            self.expect_hex64(node.get("id"), f"{node_path}.id")
            for key in ("name", "class", "semantic_kind"):
                self.expect_string(node.get(key), f"{node_path}.{key}", allow_empty=key == "semantic_kind")
            for key in ("x", "y"):
                self.expect_number(node.get(key), f"{node_path}.{key}")

        ports = value.get("ports")
        if not isinstance(ports, list):
            self.error(f"{path}.ports", "expected array")
            ports = []
        for index, port in enumerate(ports):
            port_path = f"{path}.ports[{index}]"
            if not self.require_keys(port, port_path, ["id", "node_id", "name", "direction", "type", "domain_type"]):
                continue
            self.expect_hex64(port.get("id"), f"{port_path}.id")
            self.expect_hex64(port.get("node_id"), f"{port_path}.node_id")
            if port.get("direction") not in {"input", "output"}:
                self.error(f"{port_path}.direction", "expected input or output")
            for key in ("name", "type", "domain_type"):
                self.expect_string(port.get(key), f"{port_path}.{key}", allow_empty=key == "name")

        links = value.get("links")
        if not isinstance(links, list):
            self.error(f"{path}.links", "expected array")
            links = []
        for index, link in enumerate(links):
            link_path = f"{path}.links[{index}]"
            if not self.require_keys(link, link_path, ["id", "source_port_id", "target_port_id"]):
                continue
            self.expect_hex64(link.get("id"), f"{link_path}.id")
            self.expect_hex64(link.get("source_port_id"), f"{link_path}.source_port_id")
            self.expect_hex64(link.get("target_port_id"), f"{link_path}.target_port_id")

        if not isinstance(value.get("domain_semantics"), dict):
            self.error(f"{path}.domain_semantics", "expected object")

    def validate_pcg_graph_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "graph_path",
                "graph_class",
                "world_actor_resolution_state",
                "hierarchical_generation",
                "landscape_uses_metadata",
                "default_grid",
                "default_grid_size",
                "node_count",
                "pin_count",
                "edge_count",
                "settings_count",
                "subgraph_reference_count",
                "user_parameter_state",
                "nodes",
                "pins",
                "edges",
                "settings",
                "subgraphs",
                "universal_graph",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.pcg_graph.v1":
            self.error(f"{path}.schema_version", "expected uepi.pcg_graph.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("graph_path", "graph_class", "world_actor_resolution_state", "default_grid", "user_parameter_state"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "default_grid")
        for key in ("hierarchical_generation", "landscape_uses_metadata"):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        self.expect_number(value.get("default_grid_size"), f"{path}.default_grid_size")

        arrays = {
            "nodes": self.validate_pcg_node,
            "pins": self.validate_pcg_pin,
            "edges": self.validate_pcg_edge,
            "settings": self.validate_pcg_settings,
            "subgraphs": self.validate_pcg_subgraph,
        }
        count_keys = {
            "nodes": "node_count",
            "pins": "pin_count",
            "edges": "edge_count",
            "settings": "settings_count",
            "subgraphs": "subgraph_reference_count",
        }
        for array_key, validator in arrays.items():
            items = value.get(array_key)
            if not isinstance(items, list):
                self.error(f"{path}.{array_key}", "expected array")
                items = []
            count_key = count_keys[array_key]
            if value.get(count_key) != len(items):
                self.error(f"{path}.{count_key}", f"expected {len(items)}")
            for index, item in enumerate(items):
                item_path = f"{path}.{array_key}[{index}]"
                validator(item, item_path)
                if isinstance(item, dict) and array_key in {"nodes", "edges", "subgraphs"} and item.get("index") != index:
                    self.error(f"{item_path}.index", f"expected {index}")

        self.validate_universal_graph_ir(value.get("universal_graph"), f"{path}.universal_graph", "pcg")
        universal_graph = value.get("universal_graph")
        if isinstance(universal_graph, dict):
            universal_nodes = universal_graph.get("nodes")
            universal_ports = universal_graph.get("ports")
            universal_links = universal_graph.get("links")
            if isinstance(universal_nodes, list) and value.get("node_count") != len(universal_nodes):
                self.error(f"{path}.universal_graph.nodes", f"expected {value.get('node_count')} nodes")
            if isinstance(universal_ports, list) and value.get("pin_count") != len(universal_ports):
                self.error(f"{path}.universal_graph.ports", f"expected {value.get('pin_count')} ports")
            if isinstance(universal_links, list) and value.get("edge_count") != len(universal_links):
                self.error(f"{path}.universal_graph.links", f"expected {value.get('edge_count')} links")

    def validate_metasound_metadata(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "class_name",
                "class_type",
                "version_major",
                "version_minor",
                "deprecated",
                "display_name",
                "description",
                "author",
            ],
        ):
            return
        for key in ("class_name", "class_type", "display_name", "description", "author"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key in {"display_name", "description", "author"})
        for key in ("version_major", "version_minor"):
            if not isinstance(value.get(key), int) or value.get(key) < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")
        self.expect_boolean(value.get("deprecated"), f"{path}.deprecated")

    def validate_metasound_vertex_decl(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["index", "name", "type_name", "vertex_id", "node_id", "direction", "access_type"]):
            return
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("name", "type_name", "vertex_id", "node_id", "direction", "access_type"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key in {"vertex_id", "node_id"})
        if value.get("direction") not in {"input", "output"}:
            self.error(f"{path}.direction", "expected input or output")
        for key in ("default_literal_type", "default_literal_value"):
            if key in value:
                self.expect_string(value.get(key), f"{path}.{key}", allow_empty=True)

    def validate_metasound_graph(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "role",
                "graph_class_id",
                "metadata",
                "preset",
                "input_count",
                "output_count",
                "environment_count",
                "node_count",
                "edge_count",
                "variable_count",
                "inputs",
                "outputs",
                "environment",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("role", "graph_class_id"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "graph_class_id")
        self.expect_boolean(value.get("preset"), f"{path}.preset")
        self.validate_metasound_metadata(value.get("metadata"), f"{path}.metadata")

        for array_key, count_key in (
            ("inputs", "input_count"),
            ("outputs", "output_count"),
            ("environment", "environment_count"),
        ):
            items = value.get(array_key)
            if not isinstance(items, list):
                self.error(f"{path}.{array_key}", "expected array")
                items = []
            if value.get(count_key) != len(items):
                self.error(f"{path}.{count_key}", f"expected {len(items)}")
            for index, item in enumerate(items):
                item_path = f"{path}.{array_key}[{index}]"
                if array_key == "environment":
                    if not self.require_keys(item, item_path, ["index", "name", "type_name", "required"]):
                        continue
                    if item.get("index") != index:
                        self.error(f"{item_path}.index", f"expected {index}")
                    self.expect_string(item.get("name"), f"{item_path}.name")
                    self.expect_string(item.get("type_name"), f"{item_path}.type_name")
                    self.expect_boolean(item.get("required"), f"{item_path}.required")
                else:
                    self.validate_metasound_vertex_decl(item, item_path, index)

        for key in ("node_count", "edge_count", "variable_count"):
            if not isinstance(value.get(key), int) or value.get(key) < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")

    def validate_metasound_node(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "graph_id",
                "node_guid",
                "node_name",
                "class_id",
                "class_name",
                "class_type",
                "input_count",
                "output_count",
                "environment_count",
                "input_literal_count",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_hex64(value.get("graph_id"), f"{path}.graph_id")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("node_guid", "node_name", "class_id", "class_name", "class_type"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key in {"node_guid", "class_id", "class_name", "class_type"})
        for key in ("input_count", "output_count", "environment_count", "input_literal_count"):
            if not isinstance(value.get(key), int) or value.get(key) < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")

    def validate_metasound_vertex(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["id", "index", "node_id", "direction", "name", "type_name", "vertex_id"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_hex64(value.get("node_id"), f"{path}.node_id")
        if not isinstance(value.get("index"), int) or value.get("index") < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        if value.get("direction") not in {"input", "output"}:
            self.error(f"{path}.direction", "expected input or output")
        for key in ("name", "type_name", "vertex_id"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "vertex_id")

    def validate_metasound_edge(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "graph_id",
                "source_node_id",
                "source_vertex_id",
                "target_node_id",
                "target_vertex_id",
                "source_node_guid",
                "source_vertex_guid",
                "target_node_guid",
                "target_vertex_guid",
            ],
        ):
            return
        for key in ("id", "graph_id", "source_node_id", "source_vertex_id", "target_node_id", "target_vertex_id"):
            self.expect_hex64(value.get(key), f"{path}.{key}")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("source_node_guid", "source_vertex_guid", "target_node_guid", "target_vertex_guid"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=True)

    def validate_metasound_literal(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["id", "index", "graph_id", "node_id", "vertex_id", "literal_type", "literal_value"]):
            return
        for key in ("id", "graph_id", "node_id"):
            self.expect_hex64(value.get(key), f"{path}.{key}")
        if not isinstance(value.get("index"), int) or value.get("index") < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        for key in ("vertex_id", "literal_type", "literal_value"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=True)

    def validate_metasound_dependency(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["id", "index", "class_id", "metadata", "input_count", "output_count", "environment_count"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        self.expect_string(value.get("class_id"), f"{path}.class_id", allow_empty=True)
        self.validate_metasound_metadata(value.get("metadata"), f"{path}.metadata")
        for key in ("input_count", "output_count", "environment_count"):
            if not isinstance(value.get(key), int) or value.get(key) < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")

    def validate_metasound_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "asset_path",
                "asset_class",
                "base_metasound_class",
                "asset_type",
                "output_audio_format",
                "document_version_name",
                "document_version_major",
                "document_version_minor",
                "preset",
                "interface_count",
                "graph_count",
                "subgraph_count",
                "node_count",
                "vertex_count",
                "edge_count",
                "literal_count",
                "dependency_count",
                "interfaces",
                "graphs",
                "nodes",
                "vertices",
                "edges",
                "literals",
                "dependencies",
                "universal_graph",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.metasound.v1":
            self.error(f"{path}.schema_version", "expected uepi.metasound.v1")
        if value.get("source_layer") != "editor_source_graph":
            self.error(f"{path}.source_layer", "expected editor_source_graph")
        for key in ("asset_path", "asset_class", "base_metasound_class", "asset_type", "output_audio_format", "document_version_name"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key in {"output_audio_format", "document_version_name"})
        if value.get("asset_type") not in {"source", "patch"}:
            self.error(f"{path}.asset_type", "expected source or patch")
        self.expect_boolean(value.get("preset"), f"{path}.preset")
        for key in (
            "document_version_major",
            "document_version_minor",
            "interface_count",
            "graph_count",
            "subgraph_count",
            "node_count",
            "vertex_count",
            "edge_count",
            "literal_count",
            "dependency_count",
        ):
            if not isinstance(value.get(key), int) or value.get(key) < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")

        interfaces = value.get("interfaces")
        if not isinstance(interfaces, list):
            self.error(f"{path}.interfaces", "expected array")
            interfaces = []
        if value.get("interface_count") != len(interfaces):
            self.error(f"{path}.interface_count", f"expected {len(interfaces)}")
        for index, interface in enumerate(interfaces):
            interface_path = f"{path}.interfaces[{index}]"
            if not self.require_keys(interface, interface_path, ["index", "name", "major", "minor", "version"]):
                continue
            if interface.get("index") != index:
                self.error(f"{interface_path}.index", f"expected {index}")
            self.expect_string(interface.get("name"), f"{interface_path}.name", allow_empty=True)
            self.expect_string(interface.get("version"), f"{interface_path}.version")
            for key in ("major", "minor"):
                if not isinstance(interface.get(key), int) or interface.get(key) < 0:
                    self.error(f"{interface_path}.{key}", "expected non-negative integer")

        arrays = {
            "graphs": (self.validate_metasound_graph, "graph_count"),
            "nodes": (self.validate_metasound_node, "node_count"),
            "vertices": (self.validate_metasound_vertex, "vertex_count"),
            "edges": (self.validate_metasound_edge, "edge_count"),
            "literals": (self.validate_metasound_literal, "literal_count"),
            "dependencies": (self.validate_metasound_dependency, "dependency_count"),
        }
        for array_key, (validator, count_key) in arrays.items():
            items = value.get(array_key)
            if not isinstance(items, list):
                self.error(f"{path}.{array_key}", "expected array")
                items = []
            if value.get(count_key) != len(items):
                self.error(f"{path}.{count_key}", f"expected {len(items)}")
            for index, item in enumerate(items):
                if array_key in {"graphs", "nodes", "edges", "dependencies"}:
                    validator(item, f"{path}.{array_key}[{index}]", index)
                else:
                    validator(item, f"{path}.{array_key}[{index}]")

        self.validate_universal_graph_ir(value.get("universal_graph"), f"{path}.universal_graph", "metasound")
        universal_graph = value.get("universal_graph")
        if isinstance(universal_graph, dict):
            universal_nodes = universal_graph.get("nodes")
            universal_ports = universal_graph.get("ports")
            universal_links = universal_graph.get("links")
            if isinstance(universal_nodes, list) and value.get("node_count") != len(universal_nodes):
                self.error(f"{path}.universal_graph.nodes", f"expected {value.get('node_count')} nodes")
            if isinstance(universal_ports, list) and value.get("vertex_count") != len(universal_ports):
                self.error(f"{path}.universal_graph.ports", f"expected {value.get('vertex_count')} ports")
            if isinstance(universal_links, list) and value.get("edge_count") != len(universal_links):
                self.error(f"{path}.universal_graph.links", f"expected {value.get('edge_count')} links")

    def validate_input_action_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "action_path",
                "description",
                "value_type",
                "trigger_when_paused",
                "consume_input",
                "reserve_all_mappings",
                "trigger_count",
                "modifier_count",
                "triggers",
                "modifiers",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.input_action.v1":
            self.error(f"{path}.schema_version", "expected uepi.input_action.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("action_path", "description", "value_type"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("trigger_when_paused", "consume_input", "reserve_all_mappings"):
            if not isinstance(value.get(key), bool):
                self.error(f"{path}.{key}", "expected boolean")
        self.validate_input_object_class_array(value, path, "trigger_count", "triggers")
        self.validate_input_object_class_array(value, path, "modifier_count", "modifiers")

    def validate_input_object_class_array(self, owner: dict[str, Any], path: str, count_key: str, array_key: str) -> None:
        values = owner.get(array_key)
        if not isinstance(values, list):
            self.error(f"{path}.{array_key}", "expected array")
            return
        if owner.get(count_key) != len(values):
            self.error(f"{path}.{count_key}", f"expected {len(values)}")
        for index, item in enumerate(values):
            item_path = f"{path}.{array_key}[{index}]"
            if not self.require_keys(item, item_path, ["class", "path"]):
                continue
            self.expect_string(item.get("class"), f"{item_path}.class")
            self.expect_string(item.get("path"), f"{item_path}.path")

    def validate_input_mapping_context_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            ["schema_version", "source_layer", "context_path", "description", "mapping_count", "mappings"],
        ):
            return
        if value.get("schema_version") != "uepi.input_mapping_context.v1":
            self.error(f"{path}.schema_version", "expected uepi.input_mapping_context.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_string(value.get("context_path"), f"{path}.context_path", allow_empty=False)
        self.expect_string(value.get("description"), f"{path}.description")

        mappings = value.get("mappings")
        if not isinstance(mappings, list):
            self.error(f"{path}.mappings", "expected array")
            return
        if value.get("mapping_count") != len(mappings):
            self.error(f"{path}.mapping_count", f"expected {len(mappings)}")

        for mapping_index, mapping in enumerate(mappings):
            mapping_path = f"{path}.mappings[{mapping_index}]"
            if not self.require_keys(
                mapping,
                mapping_path,
                [
                    "id",
                    "index",
                    "key",
                    "action_path",
                    "action_name",
                    "player_mappable",
                    "trigger_count",
                    "modifier_count",
                ],
            ):
                continue
            self.expect_hex64(mapping.get("id"), f"{mapping_path}.id")
            if mapping.get("index") != mapping_index:
                self.error(f"{mapping_path}.index", f"expected {mapping_index}")
            for key in ("key", "action_path", "action_name"):
                self.expect_string(mapping.get(key), f"{mapping_path}.{key}")
            if not isinstance(mapping.get("player_mappable"), bool):
                self.error(f"{mapping_path}.player_mappable", "expected boolean")
            for key in ("trigger_count", "modifier_count"):
                value_count = mapping.get(key)
                if not isinstance(value_count, int) or value_count < 0:
                    self.error(f"{mapping_path}.{key}", "expected non-negative integer")

    def validate_common_ui_row_handle(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["table_path", "row_name"]):
            return
        self.expect_string(value.get("table_path"), f"{path}.table_path")
        self.expect_string(value.get("row_name"), f"{path}.row_name")

    def validate_common_ui_input_action_reference(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["path", "name"]):
            return
        self.expect_string(value.get("path"), f"{path}.path")
        self.expect_string(value.get("name"), f"{path}.name")

    def validate_common_ui_hold_data_value(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["hold_time", "hold_rollback_time"]):
            return
        for key in ("hold_time", "hold_rollback_time"):
            number = value.get(key)
            if not isinstance(number, (int, float)) or isinstance(number, bool) or number < 0:
                self.error(f"{path}.{key}", "expected non-negative number")

    def validate_common_ui_input_type_info(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            ["key", "override_state", "action_requires_hold", "hold_time", "hold_rollback_time", "override_brush_resource"],
        ):
            return
        self.expect_string(value.get("key"), f"{path}.key")
        override_state = value.get("override_state")
        if override_state not in {"enabled", "disabled", "hidden", "hidden_and_disabled", "unknown"}:
            self.error(f"{path}.override_state", "expected known CommonUI input action state")
        if not isinstance(value.get("action_requires_hold"), bool):
            self.error(f"{path}.action_requires_hold", "expected boolean")
        for key in ("hold_time", "hold_rollback_time"):
            number = value.get(key)
            if not isinstance(number, (int, float)) or isinstance(number, bool) or number < 0:
                self.error(f"{path}.{key}", "expected non-negative number")
        self.expect_string(value.get("override_brush_resource"), f"{path}.override_brush_resource")

    def validate_common_ui_input_data_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "id",
                "asset_path",
                "data_object_path",
                "data_class_path",
                "default_click_action",
                "default_back_action",
                "default_hold_data_class",
                "enhanced_input_click_action",
                "enhanced_input_back_action",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.common_ui_input_data.v1":
            self.error(f"{path}.schema_version", "expected uepi.common_ui_input_data.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key in ("asset_path", "data_object_path", "data_class_path", "default_hold_data_class"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.validate_common_ui_row_handle(value.get("default_click_action"), f"{path}.default_click_action")
        self.validate_common_ui_row_handle(value.get("default_back_action"), f"{path}.default_back_action")
        self.validate_common_ui_input_action_reference(value.get("enhanced_input_click_action"), f"{path}.enhanced_input_click_action")
        self.validate_common_ui_input_action_reference(value.get("enhanced_input_back_action"), f"{path}.enhanced_input_back_action")

    def validate_common_ui_hold_data_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "id",
                "asset_path",
                "data_object_path",
                "data_class_path",
                "keyboard_and_mouse",
                "gamepad",
                "touch",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.common_ui_hold_data.v1":
            self.error(f"{path}.schema_version", "expected uepi.common_ui_hold_data.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key in ("asset_path", "data_object_path", "data_class_path"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("keyboard_and_mouse", "gamepad", "touch"):
            self.validate_common_ui_hold_data_value(value.get(key), f"{path}.{key}")

    def validate_common_ui_input_action_row(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "row_name",
                "display_name",
                "hold_display_name",
                "nav_bar_priority",
                "has_hold_bindings",
                "keyboard",
                "gamepad",
                "touch",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("row_name", "display_name", "hold_display_name"):
            self.expect_string(value.get(key), f"{path}.{key}")
        if not isinstance(value.get("nav_bar_priority"), int) or isinstance(value.get("nav_bar_priority"), bool):
            self.error(f"{path}.nav_bar_priority", "expected integer")
        if not isinstance(value.get("has_hold_bindings"), bool):
            self.error(f"{path}.has_hold_bindings", "expected boolean")
        for key in ("keyboard", "gamepad", "touch"):
            self.validate_common_ui_input_type_info(value.get(key), f"{path}.{key}")

    def validate_common_ui_input_action_table_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            ["schema_version", "source_layer", "id", "table_path", "row_struct_path", "row_struct_name", "row_count", "rows"],
        ):
            return
        if value.get("schema_version") != "uepi.common_ui_input_action_table.v1":
            self.error(f"{path}.schema_version", "expected uepi.common_ui_input_action_table.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key in ("table_path", "row_struct_path", "row_struct_name"):
            self.expect_string(value.get(key), f"{path}.{key}")
        rows = value.get("rows")
        if not isinstance(rows, list):
            self.error(f"{path}.rows", "expected array")
            return
        if value.get("row_count") != len(rows):
            self.error(f"{path}.row_count", f"expected {len(rows)}")
        for row_index, row in enumerate(rows):
            self.validate_common_ui_input_action_row(row, f"{path}.rows[{row_index}]", row_index)

    def validate_blackboard_key(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["id", "index", "name", "key_type_class", "instance_synced", "description", "category"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("name", "key_type_class", "description", "category"):
            self.expect_string(value.get(key), f"{path}.{key}")
        if not isinstance(value.get("instance_synced"), bool):
            self.error(f"{path}.instance_synced", "expected boolean")

    def validate_blackboard_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            ["schema_version", "source_layer", "id", "blackboard_path", "parent_path", "has_synchronized_keys", "key_count", "keys"],
        ):
            return
        if value.get("schema_version") != "uepi.blackboard.v1":
            self.error(f"{path}.schema_version", "expected uepi.blackboard.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key in ("blackboard_path", "parent_path"):
            self.expect_string(value.get(key), f"{path}.{key}")
        if not isinstance(value.get("has_synchronized_keys"), bool):
            self.error(f"{path}.has_synchronized_keys", "expected boolean")
        keys = value.get("keys")
        if not isinstance(keys, list):
            self.error(f"{path}.keys", "expected array")
            return
        if value.get("key_count") != len(keys):
            self.error(f"{path}.key_count", f"expected {len(keys)}")
        for key_index, key in enumerate(keys):
            self.validate_blackboard_key(key, f"{path}.keys[{key_index}]", key_index)

    def validate_behavior_tree_node(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "node_path",
                "node_name",
                "node_role",
                "node_class_path",
                "execution_index",
                "tree_depth",
                "static_description",
                "child_count",
                "service_count",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("node_path", "node_name", "node_role", "node_class_path", "static_description"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("execution_index", "tree_depth", "child_count", "service_count"):
            count = value.get(key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")

    def validate_behavior_tree_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "id",
                "tree_path",
                "blackboard_path",
                "root_node_id",
                "root_decorator_count",
                "instance_memory_size",
                "node_count",
                "nodes",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.behavior_tree.v1":
            self.error(f"{path}.schema_version", "expected uepi.behavior_tree.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key in ("tree_path", "blackboard_path", "root_node_id"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("root_decorator_count", "instance_memory_size"):
            count = value.get(key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")
        nodes = value.get("nodes")
        if not isinstance(nodes, list):
            self.error(f"{path}.nodes", "expected array")
            return
        if value.get("node_count") != len(nodes):
            self.error(f"{path}.node_count", f"expected {len(nodes)}")
        for node_index, node in enumerate(nodes):
            self.validate_behavior_tree_node(node, f"{path}.nodes[{node_index}]", node_index)

    def validate_env_query_generator(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["id", "path", "class_path", "option_name", "item_type_class", "auto_sort_tests"]):
            return
        generator_id = value.get("id")
        if generator_id:
            self.expect_hex64(generator_id, f"{path}.id")
        elif not isinstance(generator_id, str):
            self.error(f"{path}.id", "expected string")
        for key in ("path", "class_path", "option_name", "item_type_class"):
            self.expect_string(value.get(key), f"{path}.{key}")
        if not isinstance(value.get("auto_sort_tests"), bool):
            self.error(f"{path}.auto_sort_tests", "expected boolean")

    def validate_env_query_test(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["id", "index", "test_order", "path", "class_path", "purpose", "filter_type", "scoring_equation", "cost", "comment"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        test_order = value.get("test_order")
        if not isinstance(test_order, int) or test_order < 0:
            self.error(f"{path}.test_order", "expected non-negative integer")
        for key in ("path", "class_path", "purpose", "filter_type", "scoring_equation", "cost", "comment"):
            self.expect_string(value.get(key), f"{path}.{key}")

    def validate_env_query_option(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["id", "index", "title", "details", "generator", "test_count", "tests"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("title", "details"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.validate_env_query_generator(value.get("generator"), f"{path}.generator")
        tests = value.get("tests")
        if not isinstance(tests, list):
            self.error(f"{path}.tests", "expected array")
            return
        if value.get("test_count") != len(tests):
            self.error(f"{path}.test_count", f"expected {len(tests)}")
        for test_index, test in enumerate(tests):
            self.validate_env_query_test(test, f"{path}.tests[{test_index}]", test_index)

    def validate_env_query_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["schema_version", "source_layer", "id", "query_path", "query_name", "option_count", "options"]):
            return
        if value.get("schema_version") != "uepi.env_query.v1":
            self.error(f"{path}.schema_version", "expected uepi.env_query.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key in ("query_path", "query_name"):
            self.expect_string(value.get(key), f"{path}.{key}")
        options = value.get("options")
        if not isinstance(options, list):
            self.error(f"{path}.options", "expected array")
            return
        if value.get("option_count") != len(options):
            self.error(f"{path}.option_count", f"expected {len(options)}")
        for option_index, option in enumerate(options):
            self.validate_env_query_option(option, f"{path}.options[{option_index}]", option_index)

    def validate_state_tree_state(self, value: Any, path: str, expected_index: int | None = None) -> None:
        required = [
            "id",
            "index",
            "depth",
            "state_path",
            "name",
            "guid",
            "parent_id",
            "parent_guid",
            "type",
            "selection_behavior",
            "linked_subtree_name",
            "linked_subtree_guid",
            "linked_subtree_type",
            "enabled",
            "child_count",
            "enter_condition_count",
            "task_count",
            "transition_count",
        ]
        if not self.require_keys(value, path, required):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in (
            "state_path",
            "name",
            "guid",
            "parent_id",
            "parent_guid",
            "type",
            "selection_behavior",
            "linked_subtree_name",
            "linked_subtree_guid",
            "linked_subtree_type",
        ):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_boolean(value.get("enabled"), f"{path}.enabled")
        for key in ("depth", "child_count", "enter_condition_count", "task_count", "transition_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")

    def validate_state_tree_node(self, value: Any, path: str, expected_index: int | None = None) -> None:
        required = [
            "id",
            "index",
            "guid",
            "owner_id",
            "node_kind",
            "name",
            "node_struct_path",
            "instance_struct_path",
            "instance_object_class_path",
            "condition_indent",
            "condition_operand",
        ]
        if not self.require_keys(value, path, required):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        for key in ("guid", "owner_id", "node_kind", "name", "node_struct_path", "instance_struct_path", "instance_object_class_path", "condition_operand"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_non_negative_integer(value.get("condition_indent"), f"{path}.condition_indent")

    def validate_state_tree_transition(self, value: Any, path: str, expected_index: int | None = None) -> None:
        required = [
            "id",
            "index",
            "guid",
            "state_id",
            "trigger",
            "event_tag",
            "target_state_name",
            "target_state_guid",
            "target_state_id",
            "target_link_type",
            "priority",
            "delay_transition",
            "delay_duration",
            "delay_random_variance",
            "condition_count",
            "enabled",
        ]
        if not self.require_keys(value, path, required):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("guid", "state_id", "trigger", "event_tag", "target_state_name", "target_state_guid", "target_state_id", "target_link_type", "priority"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_boolean(value.get("delay_transition"), f"{path}.delay_transition")
        self.expect_number(value.get("delay_duration"), f"{path}.delay_duration")
        self.expect_number(value.get("delay_random_variance"), f"{path}.delay_random_variance")
        self.expect_non_negative_integer(value.get("condition_count"), f"{path}.condition_count")
        self.expect_boolean(value.get("enabled"), f"{path}.enabled")

    def validate_state_tree_external_data(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["id", "scope", "index", "name", "struct_path", "requirement", "data_view_index", "guid"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("scope", "name", "struct_path", "requirement", "guid"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_integer(value.get("data_view_index"), f"{path}.data_view_index")

    def validate_state_tree_property_binding(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["index", "source_path", "target_path"]):
            return
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        self.expect_string(value.get("source_path"), f"{path}.source_path")
        self.expect_string(value.get("target_path"), f"{path}.target_path")

    def validate_state_tree_snapshot(self, value: Any, path: str) -> None:
        required = [
            "schema_version",
            "source_layer",
            "id",
            "tree_path",
            "schema_class_path",
            "ready_to_run",
            "num_data_views",
            "runtime_state_count",
            "runtime_transition_count",
            "runtime_node_count",
            "last_compiled_editor_data_hash",
            "property_binding_count",
            "property_bindings",
            "global_evaluator_count",
            "global_task_count",
            "editor_state_count",
            "states",
            "editor_node_count",
            "nodes",
            "editor_transition_count",
            "transitions",
            "external_data_count",
            "external_data",
            "context_data_count",
            "context_data",
        ]
        if not self.require_keys(value, path, required):
            return
        if value.get("schema_version") != "uepi.state_tree.v1":
            self.error(f"{path}.schema_version", "expected uepi.state_tree.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_string(value.get("tree_path"), f"{path}.tree_path")
        self.expect_string(value.get("schema_class_path"), f"{path}.schema_class_path")
        self.expect_boolean(value.get("ready_to_run"), f"{path}.ready_to_run")
        for key in (
            "num_data_views",
            "runtime_state_count",
            "runtime_transition_count",
            "runtime_node_count",
            "global_evaluator_count",
            "global_task_count",
        ):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        self.expect_number(value.get("last_compiled_editor_data_hash"), f"{path}.last_compiled_editor_data_hash")

        bindings = value.get("property_bindings")
        if not isinstance(bindings, list):
            self.error(f"{path}.property_bindings", "expected array")
            bindings = []
        if value.get("property_binding_count") != len(bindings):
            self.error(f"{path}.property_binding_count", f"expected {len(bindings)}")
        for binding_index, binding in enumerate(bindings):
            self.validate_state_tree_property_binding(binding, f"{path}.property_bindings[{binding_index}]", binding_index)

        states = value.get("states")
        if not isinstance(states, list):
            self.error(f"{path}.states", "expected array")
            states = []
        if value.get("editor_state_count") != len(states):
            self.error(f"{path}.editor_state_count", f"expected {len(states)}")
        for state_index, state in enumerate(states):
            self.validate_state_tree_state(state, f"{path}.states[{state_index}]", state_index)

        nodes = value.get("nodes")
        if not isinstance(nodes, list):
            self.error(f"{path}.nodes", "expected array")
            nodes = []
        if value.get("editor_node_count") != len(nodes):
            self.error(f"{path}.editor_node_count", f"expected {len(nodes)}")
        for node_index, node in enumerate(nodes):
            self.validate_state_tree_node(node, f"{path}.nodes[{node_index}]")

        transitions = value.get("transitions")
        if not isinstance(transitions, list):
            self.error(f"{path}.transitions", "expected array")
            transitions = []
        if value.get("editor_transition_count") != len(transitions):
            self.error(f"{path}.editor_transition_count", f"expected {len(transitions)}")
        for transition_index, transition in enumerate(transitions):
            self.validate_state_tree_transition(transition, f"{path}.transitions[{transition_index}]", transition_index)

        external_data = value.get("external_data")
        if not isinstance(external_data, list):
            self.error(f"{path}.external_data", "expected array")
            external_data = []
        if value.get("external_data_count") != len(external_data):
            self.error(f"{path}.external_data_count", f"expected {len(external_data)}")
        for external_index, item in enumerate(external_data):
            self.validate_state_tree_external_data(item, f"{path}.external_data[{external_index}]", external_index)

        context_data = value.get("context_data")
        if not isinstance(context_data, list):
            self.error(f"{path}.context_data", "expected array")
            context_data = []
        if value.get("context_data_count") != len(context_data):
            self.error(f"{path}.context_data_count", f"expected {len(context_data)}")
        for context_index, item in enumerate(context_data):
            self.validate_state_tree_external_data(item, f"{path}.context_data[{context_index}]", context_index)

    def validate_string_array(self, value: Any, path: str) -> None:
        if not isinstance(value, list):
            self.error(path, "expected array")
            return
        for index, item in enumerate(value):
            self.expect_string(item, f"{path}[{index}]")

    def validate_gas_scalable_float(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["value", "is_static", "curve_table_path", "curve_row_name"]):
            return
        self.expect_number(value.get("value"), f"{path}.value")
        self.expect_boolean(value.get("is_static"), f"{path}.is_static")
        self.expect_string(value.get("curve_table_path"), f"{path}.curve_table_path")
        self.expect_string(value.get("curve_row_name"), f"{path}.curve_row_name")

    def validate_gas_magnitude(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "calculation_type",
                "has_static_level_1_value",
                "static_level_1_value",
                "set_by_caller_name",
                "set_by_caller_tag",
                "custom_calculation_class",
            ],
        ):
            return
        self.expect_string(value.get("calculation_type"), f"{path}.calculation_type")
        self.expect_boolean(value.get("has_static_level_1_value"), f"{path}.has_static_level_1_value")
        self.expect_number(value.get("static_level_1_value"), f"{path}.static_level_1_value")
        for key in ("set_by_caller_name", "set_by_caller_tag", "custom_calculation_class"):
            self.expect_string(value.get(key), f"{path}.{key}")

    def validate_gameplay_ability_trigger(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["id", "index", "tag", "source"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("tag", "source"):
            self.expect_string(value.get(key), f"{path}.{key}")

    def validate_gameplay_ability_snapshot(self, value: Any, path: str) -> None:
        required = [
            "schema_version",
            "source_layer",
            "id",
            "asset_path",
            "ability_object_path",
            "ability_class_path",
            "instancing_policy",
            "replication_policy",
            "net_execution_policy",
            "net_security_policy",
            "replicate_input_directly",
            "ability_tags",
            "cancel_abilities_with_tags",
            "block_abilities_with_tags",
            "activation_owned_tags",
            "activation_required_tags",
            "activation_blocked_tags",
            "source_required_tags",
            "source_blocked_tags",
            "target_required_tags",
            "target_blocked_tags",
            "cost_gameplay_effect_class",
            "cooldown_gameplay_effect_class",
            "trigger_count",
            "triggers",
        ]
        if not self.require_keys(value, path, required):
            return
        if value.get("schema_version") != "uepi.gameplay_ability.v1":
            self.error(f"{path}.schema_version", "expected uepi.gameplay_ability.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key in (
            "asset_path",
            "ability_object_path",
            "ability_class_path",
            "instancing_policy",
            "replication_policy",
            "net_execution_policy",
            "net_security_policy",
            "cost_gameplay_effect_class",
            "cooldown_gameplay_effect_class",
        ):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_boolean(value.get("replicate_input_directly"), f"{path}.replicate_input_directly")
        for key in (
            "ability_tags",
            "cancel_abilities_with_tags",
            "block_abilities_with_tags",
            "activation_owned_tags",
            "activation_required_tags",
            "activation_blocked_tags",
            "source_required_tags",
            "source_blocked_tags",
            "target_required_tags",
            "target_blocked_tags",
        ):
            self.validate_string_array(value.get(key), f"{path}.{key}")
        triggers = value.get("triggers")
        if not isinstance(triggers, list):
            self.error(f"{path}.triggers", "expected array")
            return
        if value.get("trigger_count") != len(triggers):
            self.error(f"{path}.trigger_count", f"expected {len(triggers)}")
        for trigger_index, trigger in enumerate(triggers):
            self.validate_gameplay_ability_trigger(trigger, f"{path}.triggers[{trigger_index}]", trigger_index)

    def validate_gameplay_effect_modifier(self, value: Any, path: str, expected_index: int | None = None) -> None:
        required = [
            "id",
            "index",
            "attribute",
            "attribute_property_path",
            "modifier_op",
            "magnitude",
            "source_required_tags",
            "source_ignored_tags",
            "target_required_tags",
            "target_ignored_tags",
        ]
        if not self.require_keys(value, path, required):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("attribute", "attribute_property_path", "modifier_op"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.validate_gas_magnitude(value.get("magnitude"), f"{path}.magnitude")
        for key in ("source_required_tags", "source_ignored_tags", "target_required_tags", "target_ignored_tags"):
            self.validate_string_array(value.get(key), f"{path}.{key}")

    def validate_gameplay_effect_execution(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["index", "calculation_class", "passed_in_tag_requirement_count", "calculation_modifier_count"]):
            return
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        self.expect_string(value.get("calculation_class"), f"{path}.calculation_class")
        for key in ("passed_in_tag_requirement_count", "calculation_modifier_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")

    def validate_gameplay_effect_cue(self, value: Any, path: str, expected_index: int | None = None) -> None:
        required = ["index", "magnitude_attribute", "magnitude_attribute_property_path", "min_level", "max_level", "gameplay_cue_tags"]
        if not self.require_keys(value, path, required):
            return
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("magnitude_attribute", "magnitude_attribute_property_path"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_number(value.get("min_level"), f"{path}.min_level")
        self.expect_number(value.get("max_level"), f"{path}.max_level")
        self.validate_string_array(value.get("gameplay_cue_tags"), f"{path}.gameplay_cue_tags")

    def validate_gameplay_effect_indexed_class_ref(self, value: Any, path: str, class_key: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["index", class_key]):
            return
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        self.expect_string(value.get(class_key), f"{path}.{class_key}")

    def validate_gameplay_effect_granted_ability(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["index", "ability_class", "level", "input_id", "removal_policy"]):
            return
        index = value.get("index")
        if not isinstance(index, int) or index < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        elif expected_index is not None and index != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        self.expect_string(value.get("ability_class"), f"{path}.ability_class")
        self.validate_gas_scalable_float(value.get("level"), f"{path}.level")
        self.expect_integer(value.get("input_id"), f"{path}.input_id")
        self.expect_string(value.get("removal_policy"), f"{path}.removal_policy")

    def validate_gameplay_effect_snapshot(self, value: Any, path: str) -> None:
        required = [
            "schema_version",
            "source_layer",
            "id",
            "asset_path",
            "effect_object_path",
            "effect_class_path",
            "duration_policy",
            "duration_magnitude",
            "period",
            "execute_periodic_effect_on_application",
            "periodic_inhibition_policy",
            "deny_overflow_application",
            "clear_stack_on_overflow",
            "require_modifier_success_to_trigger_cues",
            "suppress_stacking_cues",
            "stacking_type",
            "stack_limit_count",
            "stack_duration_refresh_policy",
            "stack_period_reset_policy",
            "stack_expiration_policy",
            "asset_tags",
            "granted_tags",
            "blocked_ability_tags",
            "modifier_count",
            "modifiers",
            "execution_count",
            "executions",
            "gameplay_cue_count",
            "gameplay_cues",
            "overflow_effect_count",
            "overflow_effects",
            "granted_ability_count",
            "granted_abilities",
        ]
        if not self.require_keys(value, path, required):
            return
        if value.get("schema_version") != "uepi.gameplay_effect.v1":
            self.error(f"{path}.schema_version", "expected uepi.gameplay_effect.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key in (
            "asset_path",
            "effect_object_path",
            "effect_class_path",
            "duration_policy",
            "periodic_inhibition_policy",
            "stacking_type",
            "stack_duration_refresh_policy",
            "stack_period_reset_policy",
            "stack_expiration_policy",
        ):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.validate_gas_magnitude(value.get("duration_magnitude"), f"{path}.duration_magnitude")
        self.validate_gas_scalable_float(value.get("period"), f"{path}.period")
        for key in (
            "execute_periodic_effect_on_application",
            "deny_overflow_application",
            "clear_stack_on_overflow",
            "require_modifier_success_to_trigger_cues",
            "suppress_stacking_cues",
        ):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        self.expect_non_negative_integer(value.get("stack_limit_count"), f"{path}.stack_limit_count")
        for key in ("asset_tags", "granted_tags", "blocked_ability_tags"):
            self.validate_string_array(value.get(key), f"{path}.{key}")

        modifiers = value.get("modifiers")
        if not isinstance(modifiers, list):
            self.error(f"{path}.modifiers", "expected array")
            modifiers = []
        if value.get("modifier_count") != len(modifiers):
            self.error(f"{path}.modifier_count", f"expected {len(modifiers)}")
        for modifier_index, modifier in enumerate(modifiers):
            self.validate_gameplay_effect_modifier(modifier, f"{path}.modifiers[{modifier_index}]", modifier_index)

        executions = value.get("executions")
        if not isinstance(executions, list):
            self.error(f"{path}.executions", "expected array")
            executions = []
        if value.get("execution_count") != len(executions):
            self.error(f"{path}.execution_count", f"expected {len(executions)}")
        for execution_index, execution in enumerate(executions):
            self.validate_gameplay_effect_execution(execution, f"{path}.executions[{execution_index}]", execution_index)

        gameplay_cues = value.get("gameplay_cues")
        if not isinstance(gameplay_cues, list):
            self.error(f"{path}.gameplay_cues", "expected array")
            gameplay_cues = []
        if value.get("gameplay_cue_count") != len(gameplay_cues):
            self.error(f"{path}.gameplay_cue_count", f"expected {len(gameplay_cues)}")
        for cue_index, cue in enumerate(gameplay_cues):
            self.validate_gameplay_effect_cue(cue, f"{path}.gameplay_cues[{cue_index}]", cue_index)

        overflow_effects = value.get("overflow_effects")
        if not isinstance(overflow_effects, list):
            self.error(f"{path}.overflow_effects", "expected array")
            overflow_effects = []
        if value.get("overflow_effect_count") != len(overflow_effects):
            self.error(f"{path}.overflow_effect_count", f"expected {len(overflow_effects)}")
        for overflow_index, overflow_effect in enumerate(overflow_effects):
            self.validate_gameplay_effect_indexed_class_ref(overflow_effect, f"{path}.overflow_effects[{overflow_index}]", "effect_class", overflow_index)

        granted_abilities = value.get("granted_abilities")
        if not isinstance(granted_abilities, list):
            self.error(f"{path}.granted_abilities", "expected array")
            granted_abilities = []
        if value.get("granted_ability_count") != len(granted_abilities):
            self.error(f"{path}.granted_ability_count", f"expected {len(granted_abilities)}")
        for ability_index, granted_ability in enumerate(granted_abilities):
            self.validate_gameplay_effect_granted_ability(granted_ability, f"{path}.granted_abilities[{ability_index}]", ability_index)

    def validate_gameplay_cue_notify_snapshot(self, value: Any, path: str) -> None:
        required = [
            "schema_version",
            "source_layer",
            "id",
            "asset_path",
            "cue_object_path",
            "cue_class_path",
            "cue_type",
            "gameplay_cue_tag",
            "gameplay_cue_name",
            "is_override",
        ]
        if not self.require_keys(value, path, required):
            return
        if value.get("schema_version") != "uepi.gameplay_cue_notify.v1":
            self.error(f"{path}.schema_version", "expected uepi.gameplay_cue_notify.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key in ("asset_path", "cue_object_path", "cue_class_path", "cue_type", "gameplay_cue_tag", "gameplay_cue_name"):
            self.expect_string(value.get(key), f"{path}.{key}")
        if value.get("cue_type") not in {"static", "actor"}:
            self.error(f"{path}.cue_type", "expected static or actor")
        self.expect_boolean(value.get("is_override"), f"{path}.is_override")
        if value.get("cue_type") == "actor":
            actor_required = [
                "auto_destroy_on_remove",
                "auto_destroy_delay",
                "warn_if_timeline_is_still_running",
                "warn_if_latent_action_is_still_running",
                "auto_attach_to_owner",
                "unique_instance_per_instigator",
                "unique_instance_per_source_object",
                "allow_multiple_on_active_events",
                "allow_multiple_while_active_events",
                "num_preallocated_instances",
            ]
            if not self.require_keys(value, path, actor_required):
                return
            for key in (
                "auto_destroy_on_remove",
                "warn_if_timeline_is_still_running",
                "warn_if_latent_action_is_still_running",
                "auto_attach_to_owner",
                "unique_instance_per_instigator",
                "unique_instance_per_source_object",
                "allow_multiple_on_active_events",
                "allow_multiple_while_active_events",
            ):
                self.expect_boolean(value.get(key), f"{path}.{key}")
            self.expect_number(value.get("auto_destroy_delay"), f"{path}.auto_destroy_delay")
            self.expect_non_negative_integer(value.get("num_preallocated_instances"), f"{path}.num_preallocated_instances")

    def validate_user_defined_struct_field(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "name",
                "display_name",
                "cpp_type",
                "property_class",
                "array_dim",
                "property_flags",
                "offset_internal",
                "size",
                "min_alignment",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("name", "display_name", "cpp_type", "property_class", "property_flags"):
            self.expect_string(value.get(key), f"{path}.{key}")
        if not isinstance(value.get("array_dim"), int) or value.get("array_dim") < 1:
            self.error(f"{path}.array_dim", "expected positive integer")
        for key in ("offset_internal", "size", "min_alignment"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")

    def validate_user_defined_struct_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "struct_path",
                "struct_class",
                "guid",
                "status",
                "struct_size",
                "min_alignment",
                "field_count",
                "fields",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.user_defined_struct.v1":
            self.error(f"{path}.schema_version", "expected uepi.user_defined_struct.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("struct_path", "struct_class", "guid", "status"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "guid")
        for key in ("struct_size", "min_alignment", "field_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        fields = value.get("fields")
        if not isinstance(fields, list):
            self.error(f"{path}.fields", "expected array")
            return
        if value.get("field_count") != len(fields):
            self.error(f"{path}.field_count", f"expected {len(fields)}")
        for field_index, field in enumerate(fields):
            field_path = f"{path}.fields[{field_index}]"
            self.validate_user_defined_struct_field(field, field_path)
            if isinstance(field, dict) and field.get("index") != field_index:
                self.error(f"{field_path}.index", f"expected {field_index}")

    def validate_user_defined_enum_entry(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            ["id", "index", "name", "authored_name", "display_name", "tooltip", "value", "is_hidden", "is_max"],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("name", "authored_name", "display_name", "tooltip"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_integer(value.get("value"), f"{path}.value")
        for key in ("is_hidden", "is_max"):
            self.expect_boolean(value.get(key), f"{path}.{key}")

    def validate_user_defined_enum_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "enum_path",
                "enum_class",
                "cpp_form",
                "description",
                "entry_count",
                "entries",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.user_defined_enum.v1":
            self.error(f"{path}.schema_version", "expected uepi.user_defined_enum.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("enum_path", "enum_class", "cpp_form", "description"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "description")
        self.expect_non_negative_integer(value.get("entry_count"), f"{path}.entry_count")
        entries = value.get("entries")
        if not isinstance(entries, list):
            self.error(f"{path}.entries", "expected array")
            return
        if value.get("entry_count") != len(entries):
            self.error(f"{path}.entry_count", f"expected {len(entries)}")
        for entry_index, entry in enumerate(entries):
            entry_path = f"{path}.entries[{entry_index}]"
            self.validate_user_defined_enum_entry(entry, entry_path)
            if isinstance(entry, dict) and entry.get("index") != entry_index:
                self.error(f"{entry_path}.index", f"expected {entry_index}")

    def validate_data_table_column(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["id", "index", "name", "display_name", "cpp_type", "property_class", "array_dim"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        if not isinstance(value.get("index"), int) or value.get("index") < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        for key in ("name", "display_name", "cpp_type", "property_class"):
            self.expect_string(value.get(key), f"{path}.{key}")
        if not isinstance(value.get("array_dim"), int) or value.get("array_dim") < 1:
            self.error(f"{path}.array_dim", "expected positive integer")

    def validate_data_asset_bundle(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["name", "asset_count", "asset_paths"]):
            return
        self.expect_string(value.get("name"), f"{path}.name", allow_empty=False)
        self.expect_non_negative_integer(value.get("asset_count"), f"{path}.asset_count")
        asset_paths = value.get("asset_paths")
        if not isinstance(asset_paths, list):
            self.error(f"{path}.asset_paths", "expected array")
            return
        if value.get("asset_count") != len(asset_paths):
            self.error(f"{path}.asset_count", f"expected {len(asset_paths)}")
        for asset_index, asset_path in enumerate(asset_paths):
            self.expect_string(asset_path, f"{path}.asset_paths[{asset_index}]", allow_empty=False)

    def validate_data_asset_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "id",
                "data_asset_path",
                "data_asset_class",
                "is_primary_data_asset",
                "primary_asset_id",
                "primary_asset_type",
                "primary_asset_name",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.data_asset.v1":
            self.error(f"{path}.schema_version", "expected uepi.data_asset.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key in ("data_asset_path", "data_asset_class", "primary_asset_id", "primary_asset_type", "primary_asset_name"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key.startswith("primary_asset"))
        if not isinstance(value.get("is_primary_data_asset"), bool):
            self.error(f"{path}.is_primary_data_asset", "expected boolean")
        rules = value.get("asset_manager_rules")
        if rules is not None:
            if not isinstance(rules, dict):
                self.error(f"{path}.asset_manager_rules", "expected object")
            else:
                for key in ("priority", "chunk_id"):
                    if not isinstance(rules.get(key), int):
                        self.error(f"{path}.asset_manager_rules.{key}", "expected integer")
                self.expect_string(rules.get("cook_rule"), f"{path}.asset_manager_rules.cook_rule", allow_empty=False)

        bundles = value.get("bundles")
        if bundles is not None:
            if not isinstance(bundles, list):
                self.error(f"{path}.bundles", "expected array")
            else:
                if isinstance(value.get("bundle_count"), int) and value.get("bundle_count") != len(bundles):
                    self.error(f"{path}.bundle_count", f"expected {len(bundles)}")
                bundled_asset_count = 0
                for bundle_index, bundle in enumerate(bundles):
                    bundle_path = f"{path}.bundles[{bundle_index}]"
                    self.validate_data_asset_bundle(bundle, bundle_path)
                    if isinstance(bundle, dict) and isinstance(bundle.get("asset_paths"), list):
                        bundled_asset_count += len(bundle["asset_paths"])
                if isinstance(value.get("bundled_asset_count"), int) and value.get("bundled_asset_count") != bundled_asset_count:
                    self.error(f"{path}.bundled_asset_count", f"expected {bundled_asset_count}")

    def validate_string_table_entry(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["id", "index", "key", "source_string", "metadata_count", "metadata"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        self.expect_string(value.get("key"), f"{path}.key", allow_empty=False)
        self.expect_string(value.get("source_string"), f"{path}.source_string")
        self.expect_non_negative_integer(value.get("metadata_count"), f"{path}.metadata_count")
        metadata = value.get("metadata")
        if self.expect_string_map(metadata, f"{path}.metadata"):
            if value.get("metadata_count") != len(metadata):
                self.error(f"{path}.metadata_count", f"expected {len(metadata)}")

    def validate_string_table_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "string_table_path",
                "string_table_class",
                "table_id",
                "namespace",
                "is_loaded",
                "entry_count",
                "metadata_count",
                "entries",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.string_table.v1":
            self.error(f"{path}.schema_version", "expected uepi.string_table.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("string_table_path", "string_table_class", "table_id", "namespace"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "namespace")
        if not isinstance(value.get("is_loaded"), bool):
            self.error(f"{path}.is_loaded", "expected boolean")
        self.expect_non_negative_integer(value.get("entry_count"), f"{path}.entry_count")
        self.expect_non_negative_integer(value.get("metadata_count"), f"{path}.metadata_count")
        entries = value.get("entries")
        if not isinstance(entries, list):
            self.error(f"{path}.entries", "expected array")
            return
        if value.get("entry_count") != len(entries):
            self.error(f"{path}.entry_count", f"expected {len(entries)}")
        metadata_count = 0
        for entry_index, entry in enumerate(entries):
            entry_path = f"{path}.entries[{entry_index}]"
            self.validate_string_table_entry(entry, entry_path)
            if isinstance(entry, dict):
                if entry.get("index") != entry_index:
                    self.error(f"{entry_path}.index", f"expected {entry_index}")
                if isinstance(entry.get("metadata"), dict):
                    metadata_count += len(entry["metadata"])
        if value.get("metadata_count") != metadata_count:
            self.error(f"{path}.metadata_count", f"expected {metadata_count}")

    def validate_data_table_row(self, value: Any, path: str) -> int:
        if not self.require_keys(value, path, ["id", "index", "name", "field_count", "fields"]):
            return 0
        self.expect_hex64(value.get("id"), f"{path}.id")
        if not isinstance(value.get("index"), int) or value.get("index") < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        self.expect_string(value.get("name"), f"{path}.name", allow_empty=False)
        if not isinstance(value.get("field_count"), int) or value.get("field_count") < 0:
            self.error(f"{path}.field_count", "expected non-negative integer")
        fields = value.get("fields")
        if not isinstance(fields, list):
            self.error(f"{path}.fields", "expected array")
            return 0
        if value.get("field_count") != len(fields):
            self.error(f"{path}.field_count", f"expected {len(fields)}")
        for field_index, field in enumerate(fields):
            field_path = f"{path}.fields[{field_index}]"
            if not self.require_keys(field, field_path, ["column_name", "value"]):
                continue
            self.expect_string(field.get("column_name"), f"{field_path}.column_name", allow_empty=False)
            self.expect_string(field.get("value"), f"{field_path}.value")
        return len(fields)

    def validate_data_table_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "table_path",
                "table_class",
                "row_struct_path",
                "row_struct_name",
                "row_count",
                "column_count",
                "strip_from_client_builds",
                "ignore_extra_fields",
                "ignore_missing_fields",
                "import_key_field",
                "columns",
                "rows",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.data_table.v1":
            self.error(f"{path}.schema_version", "expected uepi.data_table.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("table_path", "table_class", "row_struct_path", "row_struct_name", "import_key_field"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "import_key_field")
        for key in ("row_count", "column_count"):
            if not isinstance(value.get(key), int) or value.get(key) < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")
        for key in ("strip_from_client_builds", "ignore_extra_fields", "ignore_missing_fields"):
            if not isinstance(value.get(key), bool):
                self.error(f"{path}.{key}", "expected boolean")

        columns = value.get("columns")
        if not isinstance(columns, list):
            self.error(f"{path}.columns", "expected array")
            columns = []
        if value.get("column_count") != len(columns):
            self.error(f"{path}.column_count", f"expected {len(columns)}")
        for column_index, column in enumerate(columns):
            column_path = f"{path}.columns[{column_index}]"
            self.validate_data_table_column(column, column_path)
            if isinstance(column, dict) and column.get("index") != column_index:
                self.error(f"{column_path}.index", f"expected {column_index}")

        rows = value.get("rows")
        if not isinstance(rows, list):
            self.error(f"{path}.rows", "expected array")
            return
        if value.get("row_count") != len(rows):
            self.error(f"{path}.row_count", f"expected {len(rows)}")
        for row_index, row in enumerate(rows):
            row_path = f"{path}.rows[{row_index}]"
            field_count = self.validate_data_table_row(row, row_path)
            if isinstance(row, dict) and row.get("index") != row_index:
                self.error(f"{row_path}.index", f"expected {row_index}")
            if isinstance(value.get("column_count"), int) and field_count != value.get("column_count"):
                self.error(f"{row_path}.field_count", f"expected {value.get('column_count')}")
        parent_tables = value.get("parent_tables", [])
        if parent_tables is not None:
            if not isinstance(parent_tables, list):
                self.error(f"{path}.parent_tables", "expected array")
            else:
                if isinstance(value.get("parent_table_count"), int) and value.get("parent_table_count") != len(parent_tables):
                    self.error(f"{path}.parent_table_count", f"expected {len(parent_tables)}")
                for parent_index, parent in enumerate(parent_tables):
                    parent_path = f"{path}.parent_tables[{parent_index}]"
                    if not self.require_keys(parent, parent_path, ["id", "index", "table_path", "row_struct_path", "row_count"]):
                        continue
                    self.expect_hex64(parent.get("id"), f"{parent_path}.id")
                    if parent.get("index") != parent_index:
                        self.error(f"{parent_path}.index", f"expected {parent_index}")
                    for key in ("table_path", "row_struct_path"):
                        self.expect_string(parent.get(key), f"{parent_path}.{key}")
                    if not isinstance(parent.get("row_count"), int) or parent.get("row_count") < 0:
                        self.error(f"{parent_path}.row_count", "expected non-negative integer")

    def validate_curve_table_row(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["id", "index", "name", "key_count", "min_time", "max_time", "min_value", "max_value"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        if not isinstance(value.get("index"), int) or value.get("index") < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        self.expect_string(value.get("name"), f"{path}.name", allow_empty=False)
        if not isinstance(value.get("key_count"), int) or value.get("key_count") < 0:
            self.error(f"{path}.key_count", "expected non-negative integer")
        for key in ("min_time", "max_time", "min_value", "max_value"):
            if not isinstance(value.get(key), (int, float)):
                self.error(f"{path}.{key}", "expected number")

    def validate_curve_table_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["schema_version", "source_layer", "curve_table_path", "curve_table_class", "row_count", "rows"]):
            return
        if value.get("schema_version") != "uepi.curve_table.v1":
            self.error(f"{path}.schema_version", "expected uepi.curve_table.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("curve_table_path", "curve_table_class"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)
        if not isinstance(value.get("row_count"), int) or value.get("row_count") < 0:
            self.error(f"{path}.row_count", "expected non-negative integer")
        rows = value.get("rows")
        if not isinstance(rows, list):
            self.error(f"{path}.rows", "expected array")
            return
        if value.get("row_count") != len(rows):
            self.error(f"{path}.row_count", f"expected {len(rows)}")
        for row_index, row in enumerate(rows):
            row_path = f"{path}.rows[{row_index}]"
            self.validate_curve_table_row(row, row_path)
            if isinstance(row, dict) and row.get("index") != row_index:
                self.error(f"{row_path}.index", f"expected {row_index}")

    def validate_curve_key(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "time",
                "value",
                "interp_mode",
                "tangent_mode",
                "tangent_weight_mode",
                "arrive_tangent",
                "arrive_tangent_weight",
                "leave_tangent",
                "leave_tangent_weight",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        if not isinstance(value.get("index"), int) or value.get("index") < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        for key in ("time", "value", "arrive_tangent", "arrive_tangent_weight", "leave_tangent", "leave_tangent_weight"):
            if not isinstance(value.get(key), (int, float)):
                self.error(f"{path}.{key}", "expected number")
        for key in ("interp_mode", "tangent_mode", "tangent_weight_mode"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)

    def validate_curve_channel(self, value: Any, path: str) -> int:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "name",
                "key_count",
                "has_default_value",
                "default_value",
                "pre_infinity_extrap",
                "post_infinity_extrap",
                "min_time",
                "max_time",
                "min_value",
                "max_value",
                "keys",
            ],
        ):
            return 0
        self.expect_hex64(value.get("id"), f"{path}.id")
        if not isinstance(value.get("index"), int) or value.get("index") < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        self.expect_string(value.get("name"), f"{path}.name")
        if not isinstance(value.get("key_count"), int) or value.get("key_count") < 0:
            self.error(f"{path}.key_count", "expected non-negative integer")
        if not isinstance(value.get("has_default_value"), bool):
            self.error(f"{path}.has_default_value", "expected boolean")
        for key in ("default_value", "min_time", "max_time", "min_value", "max_value"):
            if not isinstance(value.get(key), (int, float)):
                self.error(f"{path}.{key}", "expected number")
        for key in ("pre_infinity_extrap", "post_infinity_extrap"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)

        keys = value.get("keys")
        if not isinstance(keys, list):
            self.error(f"{path}.keys", "expected array")
            return 0
        if value.get("key_count") != len(keys):
            self.error(f"{path}.key_count", f"expected {len(keys)}")
        for key_index, key in enumerate(keys):
            key_path = f"{path}.keys[{key_index}]"
            self.validate_curve_key(key, key_path)
            if isinstance(key, dict) and key.get("index") != key_index:
                self.error(f"{key_path}.index", f"expected {key_index}")
        return len(keys)

    def validate_curve_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "curve_path",
                "curve_class",
                "curve_kind",
                "channel_count",
                "key_count",
                "min_time",
                "max_time",
                "min_value",
                "max_value",
                "channels",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.curve.v1":
            self.error(f"{path}.schema_version", "expected uepi.curve.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("curve_path", "curve_class", "curve_kind"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)
        for key in ("channel_count", "key_count"):
            if not isinstance(value.get(key), int) or value.get(key) < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")
        for key in ("min_time", "max_time", "min_value", "max_value"):
            if not isinstance(value.get(key), (int, float)):
                self.error(f"{path}.{key}", "expected number")

        channels = value.get("channels")
        if not isinstance(channels, list):
            self.error(f"{path}.channels", "expected array")
            return
        if value.get("channel_count") != len(channels):
            self.error(f"{path}.channel_count", f"expected {len(channels)}")
        total_keys = 0
        for channel_index, channel in enumerate(channels):
            channel_path = f"{path}.channels[{channel_index}]"
            total_keys += self.validate_curve_channel(channel, channel_path)
            if isinstance(channel, dict) and channel.get("index") != channel_index:
                self.error(f"{channel_path}.index", f"expected {channel_index}")
        if value.get("key_count") != total_keys:
            self.error(f"{path}.key_count", f"expected {total_keys}")

    def validate_curve_atlas_entry(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            ["id", "index", "normalized_position", "is_null", "curve_path", "curve_class", "curve_name"],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        self.expect_number(value.get("normalized_position"), f"{path}.normalized_position")
        self.expect_boolean(value.get("is_null"), f"{path}.is_null")
        for key in ("curve_path", "curve_class", "curve_name"):
            self.expect_string(value.get(key), f"{path}.{key}")
        if value.get("is_null") is False and not value.get("curve_path"):
            self.error(f"{path}.curve_path", "expected non-empty curve path for non-null atlas entry")

    def validate_curve_linear_color_atlas_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "atlas_path",
                "atlas_class",
                "texture_size",
                "texture_height",
                "square_resolution",
                "disable_all_adjustments",
                "dirty",
                "has_dirty_textures",
                "gradient_curve_count",
                "gradient_curves",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.curve_linear_color_atlas.v1":
            self.error(f"{path}.schema_version", "expected uepi.curve_linear_color_atlas.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("atlas_path", "atlas_class"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)
        for key in ("texture_size", "texture_height", "gradient_curve_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        for key in ("square_resolution", "disable_all_adjustments", "dirty", "has_dirty_textures"):
            self.expect_boolean(value.get(key), f"{path}.{key}")

        entries = value.get("gradient_curves")
        if not isinstance(entries, list):
            self.error(f"{path}.gradient_curves", "expected array")
            return
        if value.get("gradient_curve_count") != len(entries):
            self.error(f"{path}.gradient_curve_count", f"expected {len(entries)}")
        for entry_index, entry in enumerate(entries):
            entry_path = f"{path}.gradient_curves[{entry_index}]"
            self.validate_curve_atlas_entry(entry, entry_path)
            if isinstance(entry, dict) and entry.get("index") != entry_index:
                self.error(f"{entry_path}.index", f"expected {entry_index}")

    def validate_skeleton_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["schema_version", "source_layer", "skeleton_path", "bone_count", "raw_bone_count", "bones"]):
            return
        if value.get("schema_version") != "uepi.skeleton.v1":
            self.error(f"{path}.schema_version", "expected uepi.skeleton.v1")
        if value.get("source_layer") != "animation_data_model":
            self.error(f"{path}.source_layer", "expected animation_data_model")
        self.expect_string(value.get("skeleton_path"), f"{path}.skeleton_path", allow_empty=False)
        bones = value.get("bones")
        if not isinstance(bones, list):
            self.error(f"{path}.bones", "expected array")
            return
        if value.get("bone_count") != len(bones):
            self.error(f"{path}.bone_count", f"expected {len(bones)}")
        raw_bone_count = value.get("raw_bone_count")
        if not isinstance(raw_bone_count, int) or raw_bone_count < 0:
            self.error(f"{path}.raw_bone_count", "expected non-negative integer")
        for bone_index, bone in enumerate(bones):
            bone_path = f"{path}.bones[{bone_index}]"
            if not self.require_keys(bone, bone_path, ["id", "index", "name", "parent_index", "parent_name"]):
                continue
            self.expect_hex64(bone.get("id"), f"{bone_path}.id")
            if bone.get("index") != bone_index:
                self.error(f"{bone_path}.index", f"expected {bone_index}")
            self.expect_string(bone.get("name"), f"{bone_path}.name", allow_empty=False)
            if not isinstance(bone.get("parent_index"), int):
                self.error(f"{bone_path}.parent_index", "expected integer")
            self.expect_string(bone.get("parent_name"), f"{bone_path}.parent_name")
            if "ref_local" in bone:
                self.validate_transform(bone.get("ref_local"), f"{bone_path}.ref_local")
            if "ref_component" in bone:
                self.validate_transform(bone.get("ref_component"), f"{bone_path}.ref_component")

        if "virtual_bone_count" in value or "virtual_bones" in value:
            virtual_bones = value.get("virtual_bones")
            if not isinstance(virtual_bones, list):
                self.error(f"{path}.virtual_bones", "expected array")
            else:
                if value.get("virtual_bone_count") != len(virtual_bones):
                    self.error(f"{path}.virtual_bone_count", f"expected {len(virtual_bones)}")
                for virtual_bone_index, virtual_bone in enumerate(virtual_bones):
                    self.validate_virtual_bone(virtual_bone, f"{path}.virtual_bones[{virtual_bone_index}]", virtual_bone_index)

        if "socket_count" in value or "sockets" in value:
            sockets = value.get("sockets")
            if not isinstance(sockets, list):
                self.error(f"{path}.sockets", "expected array")
            else:
                if value.get("socket_count") != len(sockets):
                    self.error(f"{path}.socket_count", f"expected {len(sockets)}")
                for socket_index, socket in enumerate(sockets):
                    self.validate_skeletal_socket(socket, f"{path}.sockets[{socket_index}]", socket_index)

    def validate_skeletal_mesh_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "mesh_path",
                "skeleton_path",
                "bone_count",
                "lod_count",
                "material_count",
                "morph_target_count",
                "imported_bounds",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.skeletal_mesh.v1":
            self.error(f"{path}.schema_version", "expected uepi.skeletal_mesh.v1")
        if value.get("source_layer") != "animation_data_model":
            self.error(f"{path}.source_layer", "expected animation_data_model")
        self.expect_string(value.get("mesh_path"), f"{path}.mesh_path", allow_empty=False)
        self.expect_string(value.get("skeleton_path"), f"{path}.skeleton_path")
        for key in ("bone_count", "lod_count", "material_count", "morph_target_count"):
            count = value.get(key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")
        self.validate_bounds(value.get("imported_bounds"), f"{path}.imported_bounds")
        for count_key, array_key in (
            ("mesh_socket_count", "mesh_sockets"),
            ("active_socket_count", "active_sockets"),
        ):
            if count_key in value or array_key in value:
                sockets = value.get(array_key)
                if not isinstance(sockets, list):
                    self.error(f"{path}.{array_key}", "expected array")
                    continue
                if value.get(count_key) != len(sockets):
                    self.error(f"{path}.{count_key}", f"expected {len(sockets)}")
                for socket_index, socket in enumerate(sockets):
                    self.validate_skeletal_socket(socket, f"{path}.{array_key}[{socket_index}]", socket_index)
        if "socket_count" in value:
            self.expect_non_negative_integer(value.get("socket_count"), f"{path}.socket_count")

    def validate_static_mesh_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "mesh_path",
                "lod_count",
                "material_count",
                "bounds",
                "lods",
                "materials",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.static_mesh.v1":
            self.error(f"{path}.schema_version", "expected uepi.static_mesh.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_string(value.get("mesh_path"), f"{path}.mesh_path", allow_empty=False)
        self.validate_bounds(value.get("bounds"), f"{path}.bounds")

        lods = value.get("lods")
        if not isinstance(lods, list):
            self.error(f"{path}.lods", "expected array")
            lods = []
        if value.get("lod_count") != len(lods):
            self.error(f"{path}.lod_count", f"expected {len(lods)}")
        for lod_index, lod in enumerate(lods):
            lod_path = f"{path}.lods[{lod_index}]"
            if not self.require_keys(lod, lod_path, ["id", "index", "vertex_count", "triangle_count", "section_count"]):
                continue
            self.expect_hex64(lod.get("id"), f"{lod_path}.id")
            if lod.get("index") != lod_index:
                self.error(f"{lod_path}.index", f"expected {lod_index}")
            for key in ("vertex_count", "triangle_count", "section_count"):
                count = lod.get(key)
                if not isinstance(count, int) or count < 0:
                    self.error(f"{lod_path}.{key}", "expected non-negative integer")

        materials = value.get("materials")
        if not isinstance(materials, list):
            self.error(f"{path}.materials", "expected array")
            materials = []
        if value.get("material_count") != len(materials):
            self.error(f"{path}.material_count", f"expected {len(materials)}")
        for material_index, material in enumerate(materials):
            material_path = f"{path}.materials[{material_index}]"
            if not self.require_keys(material, material_path, ["slot_index", "slot_name", "imported_slot_name", "material_path"]):
                continue
            if material.get("slot_index") != material_index:
                self.error(f"{material_path}.slot_index", f"expected {material_index}")
            for key in ("slot_name", "imported_slot_name", "material_path"):
                self.expect_string(material.get(key), f"{material_path}.{key}")

    def validate_texture2d_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "texture_path",
                "dimension_source",
                "size_x",
                "size_y",
                "mip_count",
                "pixel_format",
                "compression_settings",
                "address_x",
                "address_y",
                "srgb",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.texture2d.v1":
            self.error(f"{path}.schema_version", "expected uepi.texture2d.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("texture_path", "dimension_source", "pixel_format", "compression_settings", "address_x", "address_y"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)
        for key in ("size_x", "size_y", "mip_count"):
            count = value.get(key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")
        if not isinstance(value.get("srgb"), bool):
            self.error(f"{path}.srgb", "expected boolean")

    def validate_texture_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "texture_path",
                "texture_class",
                "texture_classification",
                "material_type",
                "dimension_source",
                "surface_width",
                "surface_height",
                "surface_depth",
                "surface_array_size",
                "source_size_x",
                "source_size_y",
                "source_slice_count",
                "source_mip_count",
                "source_format",
                "source_is_long_lat",
                "compression_settings",
                "lod_group",
                "srgb",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.texture.v1":
            self.error(f"{path}.schema_version", "expected uepi.texture.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("texture_path", "texture_class", "texture_classification", "material_type", "dimension_source", "source_format", "compression_settings", "lod_group"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=(key == "source_format"))
        for key in ("surface_width", "surface_height", "surface_depth", "surface_array_size"):
            number = value.get(key)
            if not isinstance(number, (int, float)) or number < 0:
                self.error(f"{path}.{key}", "expected non-negative number")
        for key in ("source_size_x", "source_size_y", "source_slice_count", "source_mip_count"):
            count = value.get(key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")
        for key in ("source_is_long_lat", "srgb"):
            if not isinstance(value.get(key), bool):
                self.error(f"{path}.{key}", "expected boolean")

    def validate_texture_cube_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "texture_path",
                "texture_class",
                "dimension_source",
                "size_x",
                "size_y",
                "face_count",
                "source_slice_count",
                "mip_count",
                "pixel_format",
                "compression_settings",
                "source_format",
                "source_is_long_lat",
                "srgb",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.texture_cube.v1":
            self.error(f"{path}.schema_version", "expected uepi.texture_cube.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("texture_path", "texture_class", "dimension_source", "pixel_format", "compression_settings", "source_format"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=(key == "source_format"))
        for key in ("size_x", "size_y", "source_slice_count", "mip_count"):
            count = value.get(key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")
        if value.get("face_count") != 6:
            self.error(f"{path}.face_count", "expected 6")
        for key in ("source_is_long_lat", "srgb"):
            if not isinstance(value.get(key), bool):
                self.error(f"{path}.{key}", "expected boolean")

    def validate_audio_named_path(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["index", "path", "class_path"]):
            return
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        self.expect_string(value.get("path"), f"{path}.path")
        self.expect_string(value.get("class_path"), f"{path}.class_path")

    def validate_sound_cue_child(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["index", "id", "node_path", "node_class"]):
            return
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("id", "node_path", "node_class"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "id")

    def validate_sound_node_snapshot(self, value: Any, path: str, expected_index: int | None = None) -> None:
        required = [
            "id",
            "index",
            "node_path",
            "node_class",
            "semantic_kind",
            "child_count",
            "max_child_count",
            "min_child_count",
            "duration",
            "graph_pos_x",
            "graph_pos_y",
            "children",
        ]
        if not self.require_keys(value, path, required):
            return
        self.expect_string(value.get("id"), f"{path}.id", allow_empty=False)
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("node_path", "node_class", "semantic_kind"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)
        for key in ("child_count", "max_child_count", "min_child_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        self.expect_number(value.get("duration"), f"{path}.duration")
        self.expect_integer(value.get("graph_pos_x"), f"{path}.graph_pos_x")
        self.expect_integer(value.get("graph_pos_y"), f"{path}.graph_pos_y")
        children = value.get("children")
        if not isinstance(children, list):
            self.error(f"{path}.children", "expected array")
            children = []
        if value.get("child_count") != len(children):
            self.error(f"{path}.child_count", f"expected {len(children)}")
        for child_index, child in enumerate(children):
            child_path = f"{path}.children[{child_index}]"
            self.validate_sound_cue_child(child, child_path)
            if isinstance(child, dict) and child.get("index") != child_index:
                self.error(f"{child_path}.index", f"expected {child_index}")
        if value.get("semantic_kind") == "wave_player":
            for key in ("sound_wave_id", "sound_wave_path"):
                self.expect_string(value.get(key), f"{path}.{key}")
            self.expect_boolean(value.get("looping"), f"{path}.looping")

    def validate_sound_cue_snapshot(self, value: Any, path: str) -> None:
        required = [
            "schema_version",
            "source_layer",
            "id",
            "sound_cue_path",
            "sound_cue_class",
            "first_node_id",
            "first_node_path",
            "duration",
            "volume_multiplier",
            "pitch_multiplier",
            "sound_class_path",
            "attenuation_settings_path",
            "sound_submix_path",
            "source_effect_chain_path",
            "override_attenuation",
            "override_concurrency",
            "prime_on_load",
            "has_editor_graph",
            "node_count",
            "concurrency_count",
            "nodes",
            "concurrency",
        ]
        if not self.require_keys(value, path, required):
            return
        if value.get("schema_version") != "uepi.sound_cue.v1":
            self.error(f"{path}.schema_version", "expected uepi.sound_cue.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in (
            "id",
            "sound_cue_path",
            "sound_cue_class",
            "first_node_id",
            "first_node_path",
            "sound_class_path",
            "attenuation_settings_path",
            "sound_submix_path",
            "source_effect_chain_path",
        ):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key not in {"id", "sound_cue_path", "sound_cue_class"})
        for key in ("duration", "volume_multiplier", "pitch_multiplier"):
            self.expect_number(value.get(key), f"{path}.{key}")
        for key in ("override_attenuation", "override_concurrency", "prime_on_load", "has_editor_graph"):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        for key in ("node_count", "concurrency_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        nodes = value.get("nodes")
        if not isinstance(nodes, list):
            self.error(f"{path}.nodes", "expected array")
            nodes = []
        if value.get("node_count") != len(nodes):
            self.error(f"{path}.node_count", f"expected {len(nodes)}")
        for node_index, node in enumerate(nodes):
            self.validate_sound_node_snapshot(node, f"{path}.nodes[{node_index}]", node_index)
        concurrency = value.get("concurrency")
        if not isinstance(concurrency, list):
            self.error(f"{path}.concurrency", "expected array")
            concurrency = []
        if value.get("concurrency_count") != len(concurrency):
            self.error(f"{path}.concurrency_count", f"expected {len(concurrency)}")
        for concurrency_index, item in enumerate(concurrency):
            item_path = f"{path}.concurrency[{concurrency_index}]"
            self.validate_audio_named_path(item, item_path)
            if isinstance(item, dict) and item.get("index") != concurrency_index:
                self.error(f"{item_path}.index", f"expected {concurrency_index}")

    def validate_sound_wave_cue_point(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["index", "cue_point_id", "label", "frame_position", "frame_length"]):
            return
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        self.expect_integer(value.get("cue_point_id"), f"{path}.cue_point_id")
        self.expect_string(value.get("label"), f"{path}.label")
        for key in ("frame_position", "frame_length"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")

    def validate_sound_wave_subtitle(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["index", "time", "text"]):
            return
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        self.expect_number(value.get("time"), f"{path}.time")
        self.expect_string(value.get("text"), f"{path}.text")

    def validate_sound_wave_snapshot(self, value: Any, path: str) -> None:
        required = [
            "schema_version",
            "source_layer",
            "id",
            "sound_wave_path",
            "sound_wave_class",
            "duration",
            "sample_rate",
            "imported_sample_rate",
            "num_channels",
            "volume",
            "pitch",
            "compression_quality",
            "compression_type",
            "loading_behavior",
            "streaming",
            "looping",
            "mature",
            "manual_word_wrap",
            "single_line",
            "is_ambisonics",
            "sound_class_path",
            "attenuation_settings_path",
            "sound_submix_path",
            "source_effect_chain_path",
            "cue_point_count",
            "subtitle_count",
            "concurrency_count",
            "cue_points",
            "subtitles",
            "concurrency",
        ]
        if not self.require_keys(value, path, required):
            return
        if value.get("schema_version") != "uepi.sound_wave.v1":
            self.error(f"{path}.schema_version", "expected uepi.sound_wave.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in (
            "id",
            "sound_wave_path",
            "sound_wave_class",
            "compression_type",
            "loading_behavior",
            "sound_class_path",
            "attenuation_settings_path",
            "sound_submix_path",
            "source_effect_chain_path",
        ):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key not in {"id", "sound_wave_path", "sound_wave_class"})
        for key in ("duration", "volume", "pitch"):
            self.expect_number(value.get(key), f"{path}.{key}")
        for key in ("sample_rate", "imported_sample_rate", "num_channels", "cue_point_count", "subtitle_count", "concurrency_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        self.expect_integer(value.get("compression_quality"), f"{path}.compression_quality")
        for key in ("streaming", "looping", "mature", "manual_word_wrap", "single_line", "is_ambisonics"):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        cue_points = value.get("cue_points")
        if not isinstance(cue_points, list):
            self.error(f"{path}.cue_points", "expected array")
            cue_points = []
        if value.get("cue_point_count") != len(cue_points):
            self.error(f"{path}.cue_point_count", f"expected {len(cue_points)}")
        for cue_point_index, cue_point in enumerate(cue_points):
            self.validate_sound_wave_cue_point(cue_point, f"{path}.cue_points[{cue_point_index}]", cue_point_index)
        subtitles = value.get("subtitles")
        if not isinstance(subtitles, list):
            self.error(f"{path}.subtitles", "expected array")
            subtitles = []
        if value.get("subtitle_count") != len(subtitles):
            self.error(f"{path}.subtitle_count", f"expected {len(subtitles)}")
        for subtitle_index, subtitle in enumerate(subtitles):
            self.validate_sound_wave_subtitle(subtitle, f"{path}.subtitles[{subtitle_index}]", subtitle_index)
        concurrency = value.get("concurrency")
        if not isinstance(concurrency, list):
            self.error(f"{path}.concurrency", "expected array")
            concurrency = []
        if value.get("concurrency_count") != len(concurrency):
            self.error(f"{path}.concurrency_count", f"expected {len(concurrency)}")
        for concurrency_index, item in enumerate(concurrency):
            item_path = f"{path}.concurrency[{concurrency_index}]"
            self.validate_audio_named_path(item, item_path)
            if isinstance(item, dict) and item.get("index") != concurrency_index:
                self.error(f"{item_path}.index", f"expected {concurrency_index}")

    def validate_cinematics_frame_rate(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["numerator", "denominator", "text"]):
            return
        self.expect_integer(value.get("numerator"), f"{path}.numerator")
        denominator = value.get("denominator")
        if not self.expect_integer(denominator, f"{path}.denominator"):
            return
        if denominator == 0:
            self.error(f"{path}.denominator", "expected non-zero integer")
        self.expect_string(value.get("text"), f"{path}.text")

    def validate_cinematics_frame_range(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["empty", "lower_bound_type", "lower", "upper_bound_type", "upper", "text"]):
            return
        self.expect_boolean(value.get("empty"), f"{path}.empty")
        for key in ("lower_bound_type", "upper_bound_type", "text"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("lower", "upper"):
            self.expect_integer(value.get(key), f"{path}.{key}")

    def validate_cinematics_key_artifact_manifest(
        self,
        value: Any,
        path: str,
        expected_item_count: int | None = None,
        expected_scope_kind: str | None = None,
    ) -> None:
        required = ["schema_version", "artifact_id", "artifact_uri", "storage", "scope_kind", "scope_id", "item_count", "encoding"]
        if not self.require_keys(value, path, required):
            return
        if value.get("schema_version") != "uepi.cinematics_key_artifact_manifest.v1":
            self.error(f"{path}.schema_version", "expected uepi.cinematics_key_artifact_manifest.v1")
        for key in ("artifact_id", "artifact_uri", "storage", "scope_kind", "scope_id", "encoding"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)
        self.expect_non_negative_integer(value.get("item_count"), f"{path}.item_count")
        if expected_item_count is not None and value.get("item_count") != expected_item_count:
            self.error(f"{path}.item_count", f"expected {expected_item_count}")
        if expected_scope_kind is not None and value.get("scope_kind") != expected_scope_kind:
            self.error(f"{path}.scope_kind", f"expected {expected_scope_kind}")
        if "path" in value:
            self.expect_string(value.get("path"), f"{path}.path", allow_empty=False)
        if "sha256" in value:
            self.expect_hex64(value.get("sha256"), f"{path}.sha256")
        if "byte_count" in value:
            self.expect_non_negative_integer(value.get("byte_count"), f"{path}.byte_count")

    def validate_movie_scene_binding_reference(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["binding_id", "guid", "relative_sequence_id", "fixed_binding"]):
            return
        self.expect_string(value.get("binding_id"), f"{path}.binding_id")
        self.expect_string(value.get("guid"), f"{path}.guid")
        self.expect_integer(value.get("relative_sequence_id"), f"{path}.relative_sequence_id")
        self.expect_boolean(value.get("fixed_binding"), f"{path}.fixed_binding")

    def validate_level_sequence_spawnable(self, value: Any, path: str, expected_index: int | None = None) -> None:
        required = ["id", "index", "guid", "name", "template_path", "template_class_path", "child_possessable_count", "child_possessable_guids"]
        if not self.require_keys(value, path, required):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("guid", "name", "template_path", "template_class_path"):
            self.expect_string(value.get(key), f"{path}.{key}")
        children = value.get("child_possessable_guids")
        if not isinstance(children, list):
            self.error(f"{path}.child_possessable_guids", "expected array")
            children = []
        if value.get("child_possessable_count") != len(children):
            self.error(f"{path}.child_possessable_count", f"expected {len(children)}")
        for child_index, child_guid in enumerate(children):
            self.expect_string(child_guid, f"{path}.child_possessable_guids[{child_index}]")

    def validate_level_sequence_possessable(self, value: Any, path: str, expected_index: int | None = None) -> None:
        required = ["id", "index", "guid", "name", "possessed_object_class_path", "parent_guid", "spawnable_binding_id", "world_actor_resolution_state"]
        if not self.require_keys(value, path, required):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("guid", "name", "possessed_object_class_path", "parent_guid", "spawnable_binding_id", "world_actor_resolution_state"):
            self.expect_string(value.get(key), f"{path}.{key}")

    def validate_level_sequence_binding(self, value: Any, path: str, expected_index: int | None = None) -> None:
        required = ["id", "index", "guid", "name", "track_count", "world_actor_resolution_state"]
        if not self.require_keys(value, path, required):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("guid", "name", "world_actor_resolution_state"):
            self.expect_string(value.get(key), f"{path}.{key}")
        self.expect_non_negative_integer(value.get("track_count"), f"{path}.track_count")

    def validate_level_sequence_track(self, value: Any, path: str, expected_index: int | None = None) -> None:
        required = [
            "id",
            "index",
            "track_path",
            "track_class",
            "track_name",
            "display_name",
            "semantic_kind",
            "owner_id",
            "owner_kind",
            "supports_multiple_rows",
            "is_empty",
            "is_eval_disabled",
            "section_count",
        ]
        if not self.require_keys(value, path, required):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("track_path", "track_class", "track_name", "display_name", "semantic_kind", "owner_id", "owner_kind"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key in {"track_name", "owner_id"})
        for key in ("supports_multiple_rows", "is_empty", "is_eval_disabled"):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        self.expect_non_negative_integer(value.get("section_count"), f"{path}.section_count")
        for optional_bool in ("fire_events_when_forwards", "fire_events_when_backwards"):
            if optional_bool in value:
                self.expect_boolean(value.get(optional_bool), f"{path}.{optional_bool}")
        if "event_position" in value:
            self.expect_string(value.get("event_position"), f"{path}.event_position")

    def validate_level_sequence_channel_key(
        self,
        value: Any,
        path: str,
        expected_index: int | None = None,
        expected_channel_id: str | None = None,
    ) -> None:
        required = [
            "id",
            "index",
            "track_id",
            "section_id",
            "channel_id",
            "channel_index",
            "channel_name",
            "display_name",
            "frame_number",
            "time_seconds",
        ]
        if not self.require_keys(value, path, required):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("track_id", "section_id", "channel_id", "channel_name", "display_name"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key in {"channel_name", "display_name"})
        self.expect_non_negative_integer(value.get("channel_index"), f"{path}.channel_index")
        self.expect_integer(value.get("frame_number"), f"{path}.frame_number")
        self.expect_number(value.get("time_seconds"), f"{path}.time_seconds")
        if expected_channel_id is not None and value.get("channel_id") != expected_channel_id:
            self.error(f"{path}.channel_id", f"expected {expected_channel_id}")

    def validate_level_sequence_channel(self, value: Any, path: str, expected_index: int | None = None) -> None:
        required = [
            "id",
            "index",
            "channel_name",
            "display_name",
            "group",
            "intent_name",
            "enabled",
            "can_collapse_to_track",
            "sort_order",
            "key_count",
            "effective_range",
        ]
        if not self.require_keys(value, path, required):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("channel_name", "display_name", "group", "intent_name"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("enabled", "can_collapse_to_track"):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        self.expect_number(value.get("sort_order"), f"{path}.sort_order")
        self.expect_non_negative_integer(value.get("key_count"), f"{path}.key_count")
        self.validate_cinematics_frame_range(value.get("effective_range"), f"{path}.effective_range")
        if "key_time_count" in value:
            self.expect_non_negative_integer(value.get("key_time_count"), f"{path}.key_time_count")
        if "keys" in value:
            keys = value.get("keys")
            if not isinstance(keys, list):
                self.error(f"{path}.keys", "expected array")
                keys = []
            if isinstance(value.get("key_count"), int) and value.get("key_count") != len(keys):
                self.error(f"{path}.key_count", f"expected {len(keys)}")
            if isinstance(value.get("key_time_count"), int) and value.get("key_time_count") != len(keys):
                self.error(f"{path}.key_time_count", f"expected {len(keys)}")
            channel_id = value.get("id") if isinstance(value.get("id"), str) else None
            for key_index, key_value in enumerate(keys):
                self.validate_level_sequence_channel_key(key_value, f"{path}.keys[{key_index}]", key_index, channel_id)
        if "key_artifact" in value:
            expected_item_count = value.get("key_count") if isinstance(value.get("key_count"), int) else None
            self.validate_cinematics_key_artifact_manifest(value.get("key_artifact"), f"{path}.key_artifact", expected_item_count, "channel")

    def validate_level_sequence_section(self, value: Any, path: str, expected_index: int | None = None) -> None:
        required = [
            "id",
            "index",
            "track_id",
            "track_path",
            "section_path",
            "section_class",
            "semantic_kind",
            "name",
            "range",
            "effective_range",
            "row_index",
            "active",
            "locked",
            "completion_mode",
            "blend_type",
            "pre_roll_frames",
            "post_roll_frames",
            "channel_count",
            "key_count",
            "key_storage",
            "channels",
        ]
        if not self.require_keys(value, path, required):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("track_id", "track_path", "section_path", "section_class", "semantic_kind", "name", "completion_mode", "blend_type", "key_storage"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key in {"name", "blend_type"})
        self.validate_cinematics_frame_range(value.get("range"), f"{path}.range")
        self.validate_cinematics_frame_range(value.get("effective_range"), f"{path}.effective_range")
        for key in ("row_index", "pre_roll_frames", "post_roll_frames"):
            self.expect_integer(value.get(key), f"{path}.{key}")
        for key in ("active", "locked"):
            self.expect_boolean(value.get(key), f"{path}.{key}")
        for key in ("channel_count", "key_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        channels = value.get("channels")
        if not isinstance(channels, list):
            self.error(f"{path}.channels", "expected array")
            channels = []
        if value.get("channel_count") != len(channels):
            self.error(f"{path}.channel_count", f"expected {len(channels)}")
        key_count = 0
        for channel_index, channel in enumerate(channels):
            self.validate_level_sequence_channel(channel, f"{path}.channels[{channel_index}]", channel_index)
            if isinstance(channel, dict) and isinstance(channel.get("key_count"), int):
                key_count += channel["key_count"]
        if value.get("key_count") != key_count:
            self.error(f"{path}.key_count", f"expected {key_count}")
        if "key_artifact" in value:
            expected_item_count = value.get("key_count") if isinstance(value.get("key_count"), int) else None
            self.validate_cinematics_key_artifact_manifest(value.get("key_artifact"), f"{path}.key_artifact", expected_item_count, "section")
        if "camera_binding" in value:
            self.validate_movie_scene_binding_reference(value.get("camera_binding"), f"{path}.camera_binding")
        for key in ("camera_binding_guid", "sub_sequence_path", "sub_sequence_class", "sound_path", "sound_class", "attenuation_settings_path", "animation_path", "animation_class", "animation_slot_name"):
            if key in value:
                self.expect_string(value.get(key), f"{path}.{key}")
        if "sub_sequence_playback_range" in value:
            self.validate_cinematics_frame_range(value.get("sub_sequence_playback_range"), f"{path}.sub_sequence_playback_range")
        for key in (
            "sub_sequence_start_frame_offset",
            "sub_sequence_end_frame_offset",
            "sub_sequence_first_loop_start_frame_offset",
            "sub_sequence_hierarchical_bias",
            "audio_start_frame_offset",
            "animation_first_loop_start_frame_offset",
            "animation_start_frame_offset",
            "animation_end_frame_offset",
        ):
            if key in value:
                self.expect_integer(value.get(key), f"{path}.{key}")
        for key in ("sub_sequence_time_scale", "animation_play_rate"):
            if key in value:
                self.expect_number(value.get(key), f"{path}.{key}")
        for key in ("sub_sequence_can_loop", "animation_reverse"):
            if key in value:
                self.expect_boolean(value.get(key), f"{path}.{key}")

    def validate_level_sequence_binding_tag(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["index", "tag", "binding_count", "bindings"]):
            return
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        self.expect_string(value.get("tag"), f"{path}.tag")
        bindings = value.get("bindings")
        if not isinstance(bindings, list):
            self.error(f"{path}.bindings", "expected array")
            bindings = []
        if value.get("binding_count") != len(bindings):
            self.error(f"{path}.binding_count", f"expected {len(bindings)}")
        for binding_index, binding in enumerate(bindings):
            self.validate_movie_scene_binding_reference(binding, f"{path}.bindings[{binding_index}]")

    def validate_level_sequence_marked_frame(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["index", "frame_number", "label"]):
            return
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        self.expect_integer(value.get("frame_number"), f"{path}.frame_number")
        self.expect_string(value.get("label"), f"{path}.label")

    def validate_level_sequence_snapshot(self, value: Any, path: str) -> None:
        required = [
            "schema_version",
            "source_layer",
            "id",
            "sequence_path",
            "sequence_class",
            "movie_scene_id",
            "movie_scene_path",
            "movie_scene_class",
            "director_blueprint_path",
            "director_blueprint_name",
            "director_class_path",
            "display_rate",
            "tick_resolution",
            "playback_range",
            "selection_range",
            "world_actor_resolution_state",
            "key_storage",
            "camera_cut_track_id",
            "spawnable_count",
            "possessable_count",
            "binding_count",
            "track_count",
            "section_count",
            "channel_count",
            "key_count",
            "binding_tag_count",
            "marked_frame_count",
            "root_folder_count",
            "node_group_count",
            "spawnables",
            "possessables",
            "bindings",
            "tracks",
            "sections",
            "binding_tags",
            "marked_frames",
        ]
        if not self.require_keys(value, path, required):
            return
        if value.get("schema_version") != "uepi.level_sequence.v1":
            self.error(f"{path}.schema_version", "expected uepi.level_sequence.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key in (
            "sequence_path",
            "sequence_class",
            "movie_scene_id",
            "movie_scene_path",
            "movie_scene_class",
            "director_blueprint_path",
            "director_blueprint_name",
            "director_class_path",
            "world_actor_resolution_state",
            "key_storage",
            "camera_cut_track_id",
        ):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key in {"movie_scene_id", "movie_scene_path", "movie_scene_class", "director_blueprint_path", "director_blueprint_name", "director_class_path", "camera_cut_track_id"})
        self.validate_cinematics_frame_rate(value.get("display_rate"), f"{path}.display_rate")
        self.validate_cinematics_frame_rate(value.get("tick_resolution"), f"{path}.tick_resolution")
        self.validate_cinematics_frame_range(value.get("playback_range"), f"{path}.playback_range")
        self.validate_cinematics_frame_range(value.get("selection_range"), f"{path}.selection_range")

        array_specs = [
            ("spawnables", "spawnable_count", self.validate_level_sequence_spawnable),
            ("possessables", "possessable_count", self.validate_level_sequence_possessable),
            ("bindings", "binding_count", self.validate_level_sequence_binding),
            ("tracks", "track_count", self.validate_level_sequence_track),
            ("sections", "section_count", self.validate_level_sequence_section),
            ("binding_tags", "binding_tag_count", self.validate_level_sequence_binding_tag),
            ("marked_frames", "marked_frame_count", self.validate_level_sequence_marked_frame),
        ]
        for _, count_key, _ in array_specs:
            self.expect_non_negative_integer(value.get(count_key), f"{path}.{count_key}")
        for key in ("channel_count", "key_count", "root_folder_count", "node_group_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")

        section_channel_count = 0
        section_key_count = 0
        for array_key, count_key, validator in array_specs:
            items = value.get(array_key)
            if not isinstance(items, list):
                self.error(f"{path}.{array_key}", "expected array")
                items = []
            if value.get(count_key) != len(items):
                self.error(f"{path}.{count_key}", f"expected {len(items)}")
            for item_index, item in enumerate(items):
                validator(item, f"{path}.{array_key}[{item_index}]", item_index)
                if array_key == "sections" and isinstance(item, dict):
                    if isinstance(item.get("channel_count"), int):
                        section_channel_count += item["channel_count"]
                    if isinstance(item.get("key_count"), int):
                        section_key_count += item["key_count"]
        if value.get("channel_count") != section_channel_count:
            self.error(f"{path}.channel_count", f"expected {section_channel_count}")
        if value.get("key_count") != section_key_count:
            self.error(f"{path}.key_count", f"expected {section_key_count}")
        if "key_artifact" in value:
            expected_item_count = value.get("key_count") if isinstance(value.get("key_count"), int) else None
            self.validate_cinematics_key_artifact_manifest(value.get("key_artifact"), f"{path}.key_artifact", expected_item_count, "level_sequence")

    def validate_sound_submix_reference(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["id", "index", "submix_path", "submix_class"]):
            return
        self.expect_string(value.get("id"), f"{path}.id")
        self.expect_string(value.get("submix_path"), f"{path}.submix_path")
        self.expect_string(value.get("submix_class"), f"{path}.submix_class")
        if not isinstance(value.get("index"), int) or value.get("index") < 0:
            self.error(f"{path}.index", "expected non-negative integer")

    def validate_sound_submix_effect(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["id", "index", "effect_path", "effect_class"]):
            return
        self.expect_string(value.get("id"), f"{path}.id")
        self.expect_string(value.get("effect_path"), f"{path}.effect_path")
        self.expect_string(value.get("effect_class"), f"{path}.effect_class")
        if not isinstance(value.get("index"), int) or value.get("index") < 0:
            self.error(f"{path}.index", "expected non-negative integer")

    def validate_audio_color(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["r", "g", "b", "a"]):
            return
        for key in ("r", "g", "b", "a"):
            channel = value.get(key)
            if not isinstance(channel, int) or channel < 0 or channel > 255:
                self.error(f"{path}.{key}", "expected integer in [0, 255]")

    def validate_audio_setting_property(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["index", "name", "type", "value"]):
            return
        if not isinstance(value.get("index"), int) or value.get("index") < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        for key in ("name", "type", "value"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=(key == "value"))

    def validate_sound_submix_effect_preset_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "effect_path",
                "effect_class",
                "preset_color",
                "setting_property_count",
                "setting_properties",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.sound_submix_effect_preset.v1":
            self.error(f"{path}.schema_version", "expected uepi.sound_submix_effect_preset.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("effect_path", "effect_class"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)
        self.validate_audio_color(value.get("preset_color"), f"{path}.preset_color")
        setting_properties = value.get("setting_properties")
        if not isinstance(setting_properties, list):
            self.error(f"{path}.setting_properties", "expected array")
            setting_properties = []
        if value.get("setting_property_count") != len(setting_properties):
            self.error(f"{path}.setting_property_count", f"expected {len(setting_properties)}")
        for setting_index, setting in enumerate(setting_properties):
            setting_path = f"{path}.setting_properties[{setting_index}]"
            self.validate_audio_setting_property(setting, setting_path)
            if isinstance(setting, dict) and setting.get("index") != setting_index:
                self.error(f"{setting_path}.index", f"expected {setting_index}")

    def validate_sound_submix_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "sound_submix_path",
                "sound_submix_class",
                "parent_submix_id",
                "parent_submix_path",
                "ambisonics_plugin_settings_path",
                "auto_disable",
                "auto_disable_time",
                "mute_when_backgrounded",
                "envelope_follower_attack_time",
                "envelope_follower_release_time",
                "has_editor_graph",
                "child_submix_count",
                "effect_count",
                "child_submixes",
                "effects",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.sound_submix.v1":
            self.error(f"{path}.schema_version", "expected uepi.sound_submix.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("sound_submix_path", "sound_submix_class", "parent_submix_id", "parent_submix_path", "ambisonics_plugin_settings_path"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key != "sound_submix_path" and key != "sound_submix_class")
        for key in ("auto_disable", "mute_when_backgrounded", "has_editor_graph"):
            if not isinstance(value.get(key), bool):
                self.error(f"{path}.{key}", "expected boolean")
        number = value.get("auto_disable_time")
        if not isinstance(number, (int, float)) or number < 0:
            self.error(f"{path}.auto_disable_time", "expected non-negative number")
        for key in ("envelope_follower_attack_time", "envelope_follower_release_time", "child_submix_count", "effect_count"):
            count = value.get(key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")
        child_submixes = value.get("child_submixes")
        if not isinstance(child_submixes, list):
            self.error(f"{path}.child_submixes", "expected array")
            child_submixes = []
        if value.get("child_submix_count") != len(child_submixes):
            self.error(f"{path}.child_submix_count", f"expected {len(child_submixes)}")
        for child_index, child in enumerate(child_submixes):
            child_path = f"{path}.child_submixes[{child_index}]"
            self.validate_sound_submix_reference(child, child_path)
            if isinstance(child, dict) and child.get("index") != child_index:
                self.error(f"{child_path}.index", f"expected {child_index}")
        effects = value.get("effects")
        if not isinstance(effects, list):
            self.error(f"{path}.effects", "expected array")
            effects = []
        if value.get("effect_count") != len(effects):
            self.error(f"{path}.effect_count", f"expected {len(effects)}")
        for effect_index, effect in enumerate(effects):
            effect_path = f"{path}.effects[{effect_index}]"
            self.validate_sound_submix_effect(effect, effect_path)
            if isinstance(effect, dict) and effect.get("index") != effect_index:
                self.error(f"{effect_path}.index", f"expected {effect_index}")

    def validate_material_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "material_path",
                "material_class",
                "material_domain",
                "blend_mode",
                "shading_model_field",
                "two_sided",
                "expression_count",
                "comment_count",
                "expressions",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.material.v1":
            self.error(f"{path}.schema_version", "expected uepi.material.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("material_path", "material_class", "material_domain", "blend_mode", "shading_model_field"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)
        if not isinstance(value.get("two_sided"), bool):
            self.error(f"{path}.two_sided", "expected boolean")
        if not isinstance(value.get("comment_count"), int) or value.get("comment_count") < 0:
            self.error(f"{path}.comment_count", "expected non-negative integer")
        self.validate_material_expressions(value, path)

    def validate_material_collection_parameter(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["id", "index", "name", "type", "guid", "default_scalar", "default_vector"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("name", "type", "guid"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "guid")
        if value.get("type") not in {"scalar", "vector"}:
            self.error(f"{path}.type", "expected scalar/vector")
        self.expect_number(value.get("default_scalar"), f"{path}.default_scalar")
        vector = value.get("default_vector")
        if self.require_keys(vector, f"{path}.default_vector", ["r", "g", "b", "a"]):
            for channel in ("r", "g", "b", "a"):
                self.expect_number(vector.get(channel), f"{path}.default_vector.{channel}")

    def validate_material_parameter_collection_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "collection_path",
                "collection_class",
                "state_id",
                "scalar_parameter_count",
                "vector_parameter_count",
                "parameter_count",
                "parameters",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.material_parameter_collection.v1":
            self.error(f"{path}.schema_version", "expected uepi.material_parameter_collection.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("collection_path", "collection_class", "state_id"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "state_id")
        for key in ("scalar_parameter_count", "vector_parameter_count", "parameter_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        parameters = value.get("parameters")
        if not isinstance(parameters, list):
            self.error(f"{path}.parameters", "expected array")
            return
        if value.get("parameter_count") != len(parameters):
            self.error(f"{path}.parameter_count", f"expected {len(parameters)}")
        scalar_count = 0
        vector_count = 0
        for parameter_index, parameter in enumerate(parameters):
            parameter_path = f"{path}.parameters[{parameter_index}]"
            self.validate_material_collection_parameter(parameter, parameter_path)
            if isinstance(parameter, dict):
                if parameter.get("index") != parameter_index:
                    self.error(f"{parameter_path}.index", f"expected {parameter_index}")
                if parameter.get("type") == "scalar":
                    scalar_count += 1
                elif parameter.get("type") == "vector":
                    vector_count += 1
        if value.get("scalar_parameter_count") != scalar_count:
            self.error(f"{path}.scalar_parameter_count", f"expected {scalar_count}")
        if value.get("vector_parameter_count") != vector_count:
            self.error(f"{path}.vector_parameter_count", f"expected {vector_count}")

    def validate_material_function_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            ["schema_version", "source_layer", "function_path", "function_class", "usage", "expression_count", "expressions"],
        ):
            return
        if value.get("schema_version") != "uepi.material_function.v1":
            self.error(f"{path}.schema_version", "expected uepi.material_function.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("function_path", "function_class", "usage"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)
        self.validate_material_expressions(value, path)

    def validate_material_instance_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "instance_path",
                "instance_class",
                "parent_path",
                "blend_mode",
                "shading_model_field",
                "two_sided",
                "parameter_override_count",
                "parameters",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.material_instance.v1":
            self.error(f"{path}.schema_version", "expected uepi.material_instance.v1")
        if value.get("source_layer") != "uobject_reflection":
            self.error(f"{path}.source_layer", "expected uobject_reflection")
        for key in ("instance_path", "instance_class", "parent_path", "blend_mode", "shading_model_field"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "parent_path")
        if not isinstance(value.get("two_sided"), bool):
            self.error(f"{path}.two_sided", "expected boolean")
        parameters = value.get("parameters")
        if not isinstance(parameters, list):
            self.error(f"{path}.parameters", "expected array")
            parameters = []
        if value.get("parameter_override_count") != len(parameters):
            self.error(f"{path}.parameter_override_count", f"expected {len(parameters)}")
        for parameter_index, parameter in enumerate(parameters):
            self.validate_material_parameter(parameter, f"{path}.parameters[{parameter_index}]")

    def validate_material_expressions(self, owner: dict[str, Any], path: str) -> None:
        expressions = owner.get("expressions")
        if not isinstance(expressions, list):
            self.error(f"{path}.expressions", "expected array")
            expressions = []
        if owner.get("expression_count") != len(expressions):
            self.error(f"{path}.expression_count", f"expected {len(expressions)}")
        for expression_index, expression in enumerate(expressions):
            self.validate_material_expression(expression, f"{path}.expressions[{expression_index}]", expression_index)

    def validate_material_expression(self, value: Any, path: str, expected_index: int) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "name",
                "class",
                "guid",
                "caption",
                "description",
                "editor_x",
                "editor_y",
                "parameter_name",
                "referenced_texture_path",
                "material_function_path",
                "input_count",
                "output_count",
                "inputs",
                "outputs",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        if value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("name", "class", "guid", "caption", "description", "parameter_name", "referenced_texture_path", "material_function_path"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("editor_x", "editor_y"):
            if not isinstance(value.get(key), int):
                self.error(f"{path}.{key}", "expected integer")
        self.validate_material_ports(value, path, "input_count", "inputs", is_input=True)
        self.validate_material_ports(value, path, "output_count", "outputs", is_input=False)

    def validate_material_ports(self, owner: dict[str, Any], path: str, count_key: str, array_key: str, is_input: bool) -> None:
        ports = owner.get(array_key)
        if not isinstance(ports, list):
            self.error(f"{path}.{array_key}", "expected array")
            ports = []
        if owner.get(count_key) != len(ports):
            self.error(f"{path}.{count_key}", f"expected {len(ports)}")
        for port_index, port in enumerate(ports):
            port_path = f"{path}.{array_key}[{port_index}]"
            required = ["index", "name", "mask", "mask_r", "mask_g", "mask_b", "mask_a"]
            if is_input:
                required += ["connected_expression_id", "connected_output_index"]
            if not self.require_keys(port, port_path, required):
                continue
            if port.get("index") != port_index:
                self.error(f"{port_path}.index", f"expected {port_index}")
            self.expect_string(port.get("name"), f"{port_path}.name")
            for key in ("mask", "mask_r", "mask_g", "mask_b", "mask_a"):
                if not isinstance(port.get(key), int):
                    self.error(f"{port_path}.{key}", "expected integer")
            if is_input:
                connected_id = port.get("connected_expression_id")
                if connected_id != "":
                    self.expect_hex64(connected_id, f"{port_path}.connected_expression_id")
                if not isinstance(port.get("connected_output_index"), int):
                    self.error(f"{port_path}.connected_output_index", "expected integer")

    def validate_material_parameter(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["type", "parameter_info", "name", "expression_guid"]):
            return
        for key in ("type", "name", "expression_guid"):
            self.expect_string(value.get(key), f"{path}.{key}")
        info = value.get("parameter_info")
        if self.require_keys(info, f"{path}.parameter_info", ["name", "association", "index"]):
            self.expect_string(info.get("name"), f"{path}.parameter_info.name")
            for key in ("association", "index"):
                if not isinstance(info.get(key), int):
                    self.error(f"{path}.parameter_info.{key}", "expected integer")

    def validate_bounds(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["origin", "box_extent", "sphere_radius"]):
            return
        self.validate_vector(value.get("origin"), f"{path}.origin")
        self.validate_vector(value.get("box_extent"), f"{path}.box_extent")
        if not isinstance(value.get("sphere_radius"), (int, float)):
            self.error(f"{path}.sphere_radius", "expected number")

    def validate_vector(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["x", "y", "z"]):
            return
        for key in ("x", "y", "z"):
            if not isinstance(value.get(key), (int, float)):
                self.error(f"{path}.{key}", "expected number")

    def validate_quat(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["x", "y", "z", "w"]):
            return
        for key in ("x", "y", "z", "w"):
            if not isinstance(value.get(key), (int, float)):
                self.error(f"{path}.{key}", "expected number")

    def validate_rotator(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["pitch", "yaw", "roll"]):
            return
        for key in ("pitch", "yaw", "roll"):
            if not isinstance(value.get(key), (int, float)):
                self.error(f"{path}.{key}", "expected number")

    def validate_transform(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["translation", "rotation", "scale3d"]):
            return
        self.validate_vector(value.get("translation"), f"{path}.translation")
        self.validate_quat(value.get("rotation"), f"{path}.rotation")
        self.validate_vector(value.get("scale3d"), f"{path}.scale3d")

    def validate_virtual_bone(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["id", "index", "name", "source_bone", "target_bone"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        self.expect_string(value.get("name"), f"{path}.name", allow_empty=False)
        self.expect_string(value.get("source_bone"), f"{path}.source_bone", allow_empty=False)
        self.expect_string(value.get("target_bone"), f"{path}.target_bone", allow_empty=False)

    def validate_skeletal_socket(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "id",
                "index",
                "name",
                "bone_name",
                "source",
                "relative_location",
                "relative_rotation",
                "relative_scale",
                "force_always_animated",
            ],
        ):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        for key in ("name", "bone_name", "source"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "bone_name")
        self.validate_vector(value.get("relative_location"), f"{path}.relative_location")
        self.validate_rotator(value.get("relative_rotation"), f"{path}.relative_rotation")
        self.validate_vector(value.get("relative_scale"), f"{path}.relative_scale")
        self.expect_boolean(value.get("force_always_animated"), f"{path}.force_always_animated")
        if "local_transform" in value:
            self.validate_transform(value.get("local_transform"), f"{path}.local_transform")

    def validate_animation_frame_transform_sample(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["frame_number", "time_seconds", "transform"]):
            return
        self.expect_non_negative_integer(value.get("frame_number"), f"{path}.frame_number")
        if not isinstance(value.get("time_seconds"), (int, float)) or value.get("time_seconds") < 0:
            self.error(f"{path}.time_seconds", "expected non-negative number")
        self.validate_transform(value.get("transform"), f"{path}.transform")

    def validate_animation_transform_entry(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["index", "bone_name", "transform"]):
            return
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        else:
            self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        self.expect_string(value.get("bone_name"), f"{path}.bone_name", allow_empty=False)
        self.validate_transform(value.get("transform"), f"{path}.transform")

    def validate_animation_pose_sample(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "frame_number",
                "time_seconds",
                "local_track_count",
                "component_bone_count",
                "local_tracks",
                "component_bones",
            ],
        ):
            return
        self.expect_non_negative_integer(value.get("frame_number"), f"{path}.frame_number")
        if not isinstance(value.get("time_seconds"), (int, float)) or value.get("time_seconds") < 0:
            self.error(f"{path}.time_seconds", "expected non-negative number")
        for count_key, array_key in (
            ("local_track_count", "local_tracks"),
            ("component_bone_count", "component_bones"),
        ):
            entries = value.get(array_key)
            if not isinstance(entries, list):
                self.error(f"{path}.{array_key}", "expected array")
                continue
            if value.get(count_key) != len(entries):
                self.error(f"{path}.{count_key}", f"expected {len(entries)}")
            for entry_index, entry in enumerate(entries):
                self.validate_animation_transform_entry(entry, f"{path}.{array_key}[{entry_index}]", entry_index)

    def validate_animation_bone_motion_profile_manifest(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "profile_schema_version",
                "artifact_id",
                "artifact_uri",
                "storage",
                "path",
                "sequence_path",
                "skeleton_path",
                "analysis_sample_count",
                "bone_count",
                "changed_bone_count",
                "position_changed_bone_count",
                "byte_count",
                "encoding",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.animation_bone_motion_profile_manifest.v1":
            self.error(f"{path}.schema_version", "expected uepi.animation_bone_motion_profile_manifest.v1")
        if value.get("profile_schema_version") != "uepi.animation_bone_motion_profile.v1":
            self.error(f"{path}.profile_schema_version", "expected uepi.animation_bone_motion_profile.v1")
        self.expect_hex64(value.get("artifact_id"), f"{path}.artifact_id")
        self.expect_string(value.get("artifact_uri"), f"{path}.artifact_uri", allow_empty=False)
        self.expect_string(value.get("storage"), f"{path}.storage", allow_empty=False)
        self.expect_string(value.get("path"), f"{path}.path", allow_empty=False)
        self.expect_string(value.get("sequence_path"), f"{path}.sequence_path", allow_empty=False)
        self.expect_string(value.get("skeleton_path"), f"{path}.skeleton_path")
        for key in ("analysis_sample_count", "bone_count", "changed_bone_count", "position_changed_bone_count", "byte_count"):
            self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        for key in ("driver_bone_count", "inherited_motion_bone_count"):
            if key in value:
                self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        self.expect_string(value.get("encoding"), f"{path}.encoding", allow_empty=False)

    def validate_animation_sequence_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "sequence_path",
                "skeleton_path",
                "play_length_seconds",
                "sample_key_count",
                "sampling_frame_rate",
                "rate_scale",
                "loop",
                "bone_track_count",
                "float_curve_count",
                "transform_curve_count",
                "attribute_count",
                "notify_count",
                "tracks",
                "notifies",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.anim_sequence.v1":
            self.error(f"{path}.schema_version", "expected uepi.anim_sequence.v1")
        if value.get("source_layer") != "animation_data_model":
            self.error(f"{path}.source_layer", "expected animation_data_model")
        self.expect_string(value.get("sequence_path"), f"{path}.sequence_path", allow_empty=False)
        self.expect_string(value.get("skeleton_path"), f"{path}.skeleton_path")
        for key in ("play_length_seconds", "rate_scale"):
            if not isinstance(value.get(key), (int, float)):
                self.error(f"{path}.{key}", "expected number")
        if not isinstance(value.get("sample_key_count"), int) or value.get("sample_key_count") < 0:
            self.error(f"{path}.sample_key_count", "expected non-negative integer")
        self.expect_string(value.get("sampling_frame_rate"), f"{path}.sampling_frame_rate", allow_empty=False)
        if not isinstance(value.get("loop"), bool):
            self.error(f"{path}.loop", "expected boolean")
        for key in ("float_curve_count", "transform_curve_count", "attribute_count"):
            count = value.get(key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")
        for key in ("frame_count", "data_model_key_count", "sampled_pose_count"):
            if key in value:
                self.expect_non_negative_integer(value.get(key), f"{path}.{key}")
        for key in ("additive_anim_type", "additive_base_pose_type", "additive_ref_pose_sequence_path", "root_motion_root_lock"):
            if key in value:
                self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "additive_ref_pose_sequence_path")
        if "additive_ref_frame_index" in value:
            self.expect_non_negative_integer(value.get("additive_ref_frame_index"), f"{path}.additive_ref_frame_index")
        for key in (
            "valid_additive",
            "root_motion_enabled",
            "force_root_lock",
            "use_normalized_root_motion_scale",
            "root_motion_settings_copied_from_montage",
        ):
            if key in value:
                self.expect_boolean(value.get(key), f"{path}.{key}")
        if "root_motion_full_range" in value:
            self.validate_transform(value.get("root_motion_full_range"), f"{path}.root_motion_full_range")

        tracks = value.get("tracks")
        if not isinstance(tracks, list):
            self.error(f"{path}.tracks", "expected array")
            tracks = []
        if value.get("bone_track_count") != len(tracks):
            self.error(f"{path}.bone_track_count", f"expected {len(tracks)}")
        for track_index, track in enumerate(tracks):
            track_path = f"{path}.tracks[{track_index}]"
            if not self.require_keys(track, track_path, ["id", "index", "bone_name"]):
                continue
            self.expect_hex64(track.get("id"), f"{track_path}.id")
            if track.get("index") != track_index:
                self.error(f"{track_path}.index", f"expected {track_index}")
            self.expect_string(track.get("bone_name"), f"{track_path}.bone_name", allow_empty=False)
            if "raw_local_key_count" in track:
                self.expect_non_negative_integer(track.get("raw_local_key_count"), f"{track_path}.raw_local_key_count")
            if "raw_local_first" in track:
                self.validate_transform(track.get("raw_local_first"), f"{track_path}.raw_local_first")
            if "raw_local_last" in track:
                self.validate_transform(track.get("raw_local_last"), f"{track_path}.raw_local_last")
            if "raw_local_samples" in track:
                samples = track.get("raw_local_samples")
                if not isinstance(samples, list):
                    self.error(f"{track_path}.raw_local_samples", "expected array")
                else:
                    for sample_index, sample in enumerate(samples):
                        self.validate_animation_frame_transform_sample(sample, f"{track_path}.raw_local_samples[{sample_index}]")

        notifies = value.get("notifies")
        if not isinstance(notifies, list):
            self.error(f"{path}.notifies", "expected array")
            notifies = []
        if value.get("notify_count") != len(notifies):
            self.error(f"{path}.notify_count", f"expected {len(notifies)}")
        for notify_index, notify in enumerate(notifies):
            notify_path = f"{path}.notifies[{notify_index}]"
            if not self.require_keys(notify, notify_path, ["id", "index", "name", "time", "trigger_time", "duration", "track_index", "notify_class", "notify_state_class"]):
                continue
            self.expect_hex64(notify.get("id"), f"{notify_path}.id")
            if notify.get("index") != notify_index:
                self.error(f"{notify_path}.index", f"expected {notify_index}")
            self.expect_string(notify.get("name"), f"{notify_path}.name")
            for key in ("time", "trigger_time", "duration"):
                if not isinstance(notify.get(key), (int, float)):
                    self.error(f"{notify_path}.{key}", "expected number")
            if not isinstance(notify.get("track_index"), int):
                self.error(f"{notify_path}.track_index", "expected integer")
            self.expect_string(notify.get("notify_class"), f"{notify_path}.notify_class")
            self.expect_string(notify.get("notify_state_class"), f"{notify_path}.notify_state_class")

        if "sampled_pose_count" in value or "pose_samples" in value:
            pose_samples = value.get("pose_samples")
            if not isinstance(pose_samples, list):
                self.error(f"{path}.pose_samples", "expected array")
            else:
                if value.get("sampled_pose_count") != len(pose_samples):
                    self.error(f"{path}.sampled_pose_count", f"expected {len(pose_samples)}")
                for sample_index, sample in enumerate(pose_samples):
                    self.validate_animation_pose_sample(sample, f"{path}.pose_samples[{sample_index}]")
        if "bone_motion_profile" in value:
            self.validate_animation_bone_motion_profile_manifest(value.get("bone_motion_profile"), f"{path}.bone_motion_profile")

    def validate_animation_segment(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "index",
                "animation_path",
                "animation_class",
                "track_start_time",
                "track_end_time",
                "length",
                "source_start_time",
                "source_end_time",
                "play_rate",
                "valid_play_rate",
                "looping_count",
                "valid",
            ],
        ):
            return
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        else:
            self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        for key in ("animation_path", "animation_class"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=True)
        for key in ("track_start_time", "track_end_time", "length", "source_start_time", "source_end_time", "play_rate", "valid_play_rate"):
            if not isinstance(value.get(key), (int, float)):
                self.error(f"{path}.{key}", "expected number")
        self.expect_non_negative_integer(value.get("looping_count"), f"{path}.looping_count")
        self.expect_boolean(value.get("valid"), f"{path}.valid")

    def validate_montage_section(self, value: Any, path: str, expected_index: int | None = None) -> None:
        if not self.require_keys(value, path, ["index", "name", "start_time", "next_section", "slot_index", "segment_index", "link_method", "metadata_count"]):
            return
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        else:
            self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        self.expect_string(value.get("name"), f"{path}.name")
        self.expect_string(value.get("next_section"), f"{path}.next_section")
        self.expect_string(value.get("link_method"), f"{path}.link_method", allow_empty=False)
        if not isinstance(value.get("start_time"), (int, float)):
            self.error(f"{path}.start_time", "expected number")
        self.expect_non_negative_integer(value.get("slot_index"), f"{path}.slot_index")
        if not isinstance(value.get("segment_index"), int) or value.get("segment_index") < -1:
            self.error(f"{path}.segment_index", "expected integer >= -1")
        self.expect_non_negative_integer(value.get("metadata_count"), f"{path}.metadata_count")

    def validate_montage_slot(self, value: Any, path: str, expected_index: int | None = None) -> int:
        if not self.require_keys(value, path, ["index", "slot_name", "segment_count", "segments"]):
            return 0
        if expected_index is not None and value.get("index") != expected_index:
            self.error(f"{path}.index", f"expected {expected_index}")
        else:
            self.expect_non_negative_integer(value.get("index"), f"{path}.index")
        self.expect_string(value.get("slot_name"), f"{path}.slot_name")
        segments = value.get("segments")
        if not isinstance(segments, list):
            self.error(f"{path}.segments", "expected array")
            return 0
        if value.get("segment_count") != len(segments):
            self.error(f"{path}.segment_count", f"expected {len(segments)}")
        for segment_index, segment in enumerate(segments):
            self.validate_animation_segment(segment, f"{path}.segments[{segment_index}]", segment_index)
        return len(segments)

    def validate_anim_montage_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "montage_path",
                "skeleton_path",
                "play_length_seconds",
                "sampling_frame_rate",
                "rate_scale",
                "blend_in_time",
                "blend_out_time",
                "blend_out_trigger_time",
                "sync_group",
                "sync_slot_index",
                "auto_blend_out",
                "root_motion_root_lock",
                "section_count",
                "slot_count",
                "segment_count",
                "sections",
                "slots",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.anim_montage.v1":
            self.error(f"{path}.schema_version", "expected uepi.anim_montage.v1")
        if value.get("source_layer") != "animation_data_model":
            self.error(f"{path}.source_layer", "expected animation_data_model")
        for key in ("montage_path", "skeleton_path", "sampling_frame_rate", "sync_group", "root_motion_root_lock"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key in {"skeleton_path", "sync_group"})
        for key in ("play_length_seconds", "rate_scale", "blend_in_time", "blend_out_time", "blend_out_trigger_time"):
            if not isinstance(value.get(key), (int, float)):
                self.error(f"{path}.{key}", "expected number")
        self.expect_non_negative_integer(value.get("sync_slot_index"), f"{path}.sync_slot_index")
        self.expect_boolean(value.get("auto_blend_out"), f"{path}.auto_blend_out")
        sections = value.get("sections")
        if not isinstance(sections, list):
            self.error(f"{path}.sections", "expected array")
            sections = []
        if value.get("section_count") != len(sections):
            self.error(f"{path}.section_count", f"expected {len(sections)}")
        for section_index, section in enumerate(sections):
            self.validate_montage_section(section, f"{path}.sections[{section_index}]", section_index)
        slots = value.get("slots")
        total_segments = 0
        if not isinstance(slots, list):
            self.error(f"{path}.slots", "expected array")
            slots = []
        if value.get("slot_count") != len(slots):
            self.error(f"{path}.slot_count", f"expected {len(slots)}")
        for slot_index, slot in enumerate(slots):
            total_segments += self.validate_montage_slot(slot, f"{path}.slots[{slot_index}]", slot_index)
        if value.get("segment_count") != total_segments:
            self.error(f"{path}.segment_count", f"expected {total_segments}")

    def validate_anim_composite_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "composite_path",
                "skeleton_path",
                "play_length_seconds",
                "sampling_frame_rate",
                "rate_scale",
                "additive_anim_type",
                "valid_additive",
                "root_motion",
                "segment_count",
                "segments",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.anim_composite.v1":
            self.error(f"{path}.schema_version", "expected uepi.anim_composite.v1")
        if value.get("source_layer") != "animation_data_model":
            self.error(f"{path}.source_layer", "expected animation_data_model")
        for key in ("composite_path", "skeleton_path", "sampling_frame_rate", "additive_anim_type"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "skeleton_path")
        for key in ("play_length_seconds", "rate_scale"):
            if not isinstance(value.get(key), (int, float)):
                self.error(f"{path}.{key}", "expected number")
        self.expect_boolean(value.get("valid_additive"), f"{path}.valid_additive")
        self.expect_boolean(value.get("root_motion"), f"{path}.root_motion")
        segments = value.get("segments")
        if not isinstance(segments, list):
            self.error(f"{path}.segments", "expected array")
            return
        if value.get("segment_count") != len(segments):
            self.error(f"{path}.segment_count", f"expected {len(segments)}")
        for segment_index, segment in enumerate(segments):
            self.validate_animation_segment(segment, f"{path}.segments[{segment_index}]", segment_index)

    def validate_blend_space_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "blend_space_path",
                "blend_space_class",
                "skeleton_path",
                "dimension_count",
                "axis_count",
                "sample_count",
                "loop",
                "interpolate_using_grid",
                "target_weight_interpolation_speed",
                "target_weight_interpolation_ease_in_out",
                "allow_mesh_space_blending",
                "notify_trigger_mode",
                "preferred_triangulation_direction",
                "preview_base_pose_path",
                "anim_length",
                "axes",
                "samples",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.blend_space.v1":
            self.error(f"{path}.schema_version", "expected uepi.blend_space.v1")
        if value.get("source_layer") != "animation_data_model":
            self.error(f"{path}.source_layer", "expected animation_data_model")
        for key in ("blend_space_path", "blend_space_class"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=False)
        for key in ("skeleton_path", "notify_trigger_mode", "preferred_triangulation_direction", "preview_base_pose_path"):
            self.expect_string(value.get(key), f"{path}.{key}")
        for key in ("dimension_count", "axis_count", "sample_count"):
            count = value.get(key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")
        for key in ("loop", "interpolate_using_grid", "target_weight_interpolation_ease_in_out", "allow_mesh_space_blending"):
            if not isinstance(value.get(key), bool):
                self.error(f"{path}.{key}", "expected boolean")
        for key in ("target_weight_interpolation_speed", "anim_length"):
            if not isinstance(value.get(key), (int, float)) or value.get(key) < 0:
                self.error(f"{path}.{key}", "expected non-negative number")

        axes = value.get("axes")
        if not isinstance(axes, list):
            self.error(f"{path}.axes", "expected array")
            axes = []
        if value.get("axis_count") != len(axes):
            self.error(f"{path}.axis_count", f"expected {len(axes)}")
        if value.get("dimension_count") != len(axes):
            self.error(f"{path}.dimension_count", f"expected {len(axes)}")
        for axis_index, axis in enumerate(axes):
            axis_path = f"{path}.axes[{axis_index}]"
            if not self.require_keys(axis, axis_path, ["index", "name", "min", "max", "grid_num", "snap_to_grid", "wrap_input"]):
                continue
            if axis.get("index") != axis_index:
                self.error(f"{axis_path}.index", f"expected {axis_index}")
            self.expect_string(axis.get("name"), f"{axis_path}.name")
            for key in ("min", "max"):
                if not isinstance(axis.get(key), (int, float)):
                    self.error(f"{axis_path}.{key}", "expected number")
            if not isinstance(axis.get("grid_num"), int) or axis.get("grid_num") < 1:
                self.error(f"{axis_path}.grid_num", "expected positive integer")
            for key in ("snap_to_grid", "wrap_input"):
                if not isinstance(axis.get(key), bool):
                    self.error(f"{axis_path}.{key}", "expected boolean")

        samples = value.get("samples")
        if not isinstance(samples, list):
            self.error(f"{path}.samples", "expected array")
            samples = []
        if value.get("sample_count") != len(samples):
            self.error(f"{path}.sample_count", f"expected {len(samples)}")
        for sample_index, sample in enumerate(samples):
            sample_path = f"{path}.samples[{sample_index}]"
            if not self.require_keys(sample, sample_path, ["id", "index", "animation_path", "sample_value", "rate_scale", "sample_valid"]):
                continue
            self.expect_hex64(sample.get("id"), f"{sample_path}.id")
            if sample.get("index") != sample_index:
                self.error(f"{sample_path}.index", f"expected {sample_index}")
            self.expect_string(sample.get("animation_path"), f"{sample_path}.animation_path")
            self.validate_vector(sample.get("sample_value"), f"{sample_path}.sample_value")
            if not isinstance(sample.get("rate_scale"), (int, float)):
                self.error(f"{sample_path}.rate_scale", "expected number")
            if not isinstance(sample.get("sample_valid"), bool):
                self.error(f"{sample_path}.sample_valid", "expected boolean")

    def validate_pose_asset_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "pose_asset_path",
                "pose_asset_class",
                "skeleton_path",
                "additive",
                "base_pose_index",
                "base_pose_name",
                "retarget_source",
                "source_animation_path",
                "source_animation_raw_data_guid",
                "pose_count",
                "curve_count",
                "track_count",
                "poses",
                "curves",
                "tracks",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.pose_asset.v1":
            self.error(f"{path}.schema_version", "expected uepi.pose_asset.v1")
        if value.get("source_layer") != "animation_data_model":
            self.error(f"{path}.source_layer", "expected animation_data_model")
        for key in (
            "pose_asset_path",
            "pose_asset_class",
            "skeleton_path",
            "base_pose_name",
            "retarget_source",
            "source_animation_path",
            "source_animation_raw_data_guid",
        ):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key != "pose_asset_path" and key != "pose_asset_class")
        if not isinstance(value.get("additive"), bool):
            self.error(f"{path}.additive", "expected boolean")
        if not isinstance(value.get("base_pose_index"), int) or value.get("base_pose_index") < -1:
            self.error(f"{path}.base_pose_index", "expected integer >= -1")
        for key in ("pose_count", "curve_count", "track_count"):
            count = value.get(key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")

        poses = value.get("poses")
        if not isinstance(poses, list):
            self.error(f"{path}.poses", "expected array")
            poses = []
        if value.get("pose_count") != len(poses):
            self.error(f"{path}.pose_count", f"expected {len(poses)}")
        for pose_index, pose in enumerate(poses):
            pose_path = f"{path}.poses[{pose_index}]"
            if not self.require_keys(pose, pose_path, ["id", "index", "name", "curve_value_count", "curve_values"]):
                continue
            self.expect_hex64(pose.get("id"), f"{pose_path}.id")
            if pose.get("index") != pose_index:
                self.error(f"{pose_path}.index", f"expected {pose_index}")
            self.expect_string(pose.get("name"), f"{pose_path}.name", allow_empty=False)
            curve_values = pose.get("curve_values")
            if not isinstance(curve_values, list):
                self.error(f"{pose_path}.curve_values", "expected array")
                curve_values = []
            if pose.get("curve_value_count") != len(curve_values):
                self.error(f"{pose_path}.curve_value_count", f"expected {len(curve_values)}")
            for curve_value_index, curve_value in enumerate(curve_values):
                curve_value_path = f"{pose_path}.curve_values[{curve_value_index}]"
                if not self.require_keys(curve_value, curve_value_path, ["index", "curve_name", "value"]):
                    continue
                if curve_value.get("index") != curve_value_index:
                    self.error(f"{curve_value_path}.index", f"expected {curve_value_index}")
                self.expect_string(curve_value.get("curve_name"), f"{curve_value_path}.curve_name")
                if not isinstance(curve_value.get("value"), (int, float)):
                    self.error(f"{curve_value_path}.value", "expected number")

        curves = value.get("curves")
        if not isinstance(curves, list):
            self.error(f"{path}.curves", "expected array")
            curves = []
        if value.get("curve_count") != len(curves):
            self.error(f"{path}.curve_count", f"expected {len(curves)}")
        for curve_index, curve in enumerate(curves):
            curve_path = f"{path}.curves[{curve_index}]"
            if not self.require_keys(curve, curve_path, ["id", "index", "name"]):
                continue
            self.expect_hex64(curve.get("id"), f"{curve_path}.id")
            if curve.get("index") != curve_index:
                self.error(f"{curve_path}.index", f"expected {curve_index}")
            self.expect_string(curve.get("name"), f"{curve_path}.name", allow_empty=False)

        tracks = value.get("tracks")
        if not isinstance(tracks, list):
            self.error(f"{path}.tracks", "expected array")
            tracks = []
        if value.get("track_count") != len(tracks):
            self.error(f"{path}.track_count", f"expected {len(tracks)}")
        for track_index, track in enumerate(tracks):
            track_path = f"{path}.tracks[{track_index}]"
            if not self.require_keys(track, track_path, ["index", "bone_name"]):
                continue
            if track.get("index") != track_index:
                self.error(f"{track_path}.index", f"expected {track_index}")
            self.expect_string(track.get("bone_name"), f"{track_path}.bone_name", allow_empty=False)

    def validate_physics_shape(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["id", "index", "type"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        if not isinstance(value.get("index"), int) or value.get("index") < 0:
            self.error(f"{path}.index", "expected non-negative integer")
        self.expect_string(value.get("type"), f"{path}.type", allow_empty=False)
        if "center" in value:
            self.validate_vector(value.get("center"), f"{path}.center")
        if "rotation" in value:
            self.validate_rotator(value.get("rotation"), f"{path}.rotation")
        if "extent" in value:
            self.validate_vector(value.get("extent"), f"{path}.extent")
        for key in ("radius", "radius0", "radius1", "length"):
            if key in value and not isinstance(value.get(key), (int, float)):
                self.error(f"{path}.{key}", "expected number")
        for key in ("vertex_count", "index_count"):
            if key in value and (not isinstance(value.get(key), int) or value.get(key) < 0):
                self.error(f"{path}.{key}", "expected non-negative integer")

    def validate_physics_asset_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "physics_asset_path",
                "physics_asset_class",
                "preview_skeletal_mesh_path",
                "body_count",
                "constraint_count",
                "shape_count",
                "bounds_body_count",
                "physical_animation_profile_count",
                "constraint_profile_count",
                "not_for_dedicated_server",
                "solver_settings",
                "bodies",
                "constraints",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.physics_asset.v1":
            self.error(f"{path}.schema_version", "expected uepi.physics_asset.v1")
        if value.get("source_layer") != "animation_data_model":
            self.error(f"{path}.source_layer", "expected animation_data_model")
        for key in ("physics_asset_path", "physics_asset_class", "preview_skeletal_mesh_path"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "preview_skeletal_mesh_path")
        for key in (
            "body_count",
            "constraint_count",
            "shape_count",
            "bounds_body_count",
            "physical_animation_profile_count",
            "constraint_profile_count",
        ):
            count = value.get(key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")
        if not isinstance(value.get("not_for_dedicated_server"), bool):
            self.error(f"{path}.not_for_dedicated_server", "expected boolean")

        solver_settings = value.get("solver_settings")
        if self.require_keys(
            solver_settings,
            f"{path}.solver_settings",
            [
                "solver_type",
                "position_iterations",
                "velocity_iterations",
                "projection_iterations",
                "cull_distance",
                "max_depenetration_velocity",
                "fixed_time_step",
                "use_linear_joint_solver",
            ],
        ):
            self.expect_string(solver_settings.get("solver_type"), f"{path}.solver_settings.solver_type", allow_empty=False)
            for key in ("position_iterations", "velocity_iterations", "projection_iterations"):
                if not isinstance(solver_settings.get(key), int) or solver_settings.get(key) < 0:
                    self.error(f"{path}.solver_settings.{key}", "expected non-negative integer")
            for key in ("cull_distance", "max_depenetration_velocity", "fixed_time_step"):
                if not isinstance(solver_settings.get(key), (int, float)):
                    self.error(f"{path}.solver_settings.{key}", "expected number")
            if not isinstance(solver_settings.get("use_linear_joint_solver"), bool):
                self.error(f"{path}.solver_settings.use_linear_joint_solver", "expected boolean")

        bodies = value.get("bodies")
        if not isinstance(bodies, list):
            self.error(f"{path}.bodies", "expected array")
            bodies = []
        if value.get("body_count") != len(bodies):
            self.error(f"{path}.body_count", f"expected {len(bodies)}")
        total_shape_count = 0
        for body_index, body in enumerate(bodies):
            body_path = f"{path}.bodies[{body_index}]"
            if not self.require_keys(
                body,
                body_path,
                [
                    "id",
                    "index",
                    "bone_name",
                    "physics_type",
                    "collision_trace_flag",
                    "collision_response",
                    "consider_for_bounds",
                    "skip_scale_from_animation",
                    "physical_animation_profile_count",
                    "shape_count",
                    "shapes",
                ],
            ):
                continue
            self.expect_hex64(body.get("id"), f"{body_path}.id")
            if body.get("index") != body_index:
                self.error(f"{body_path}.index", f"expected {body_index}")
            for key in ("bone_name", "physics_type", "collision_trace_flag", "collision_response"):
                self.expect_string(body.get(key), f"{body_path}.{key}", allow_empty=key != "bone_name")
            for key in ("consider_for_bounds", "skip_scale_from_animation"):
                if not isinstance(body.get(key), bool):
                    self.error(f"{body_path}.{key}", "expected boolean")
            for key in ("physical_animation_profile_count", "shape_count"):
                if not isinstance(body.get(key), int) or body.get(key) < 0:
                    self.error(f"{body_path}.{key}", "expected non-negative integer")
            shapes = body.get("shapes")
            if not isinstance(shapes, list):
                self.error(f"{body_path}.shapes", "expected array")
                shapes = []
            if body.get("shape_count") != len(shapes):
                self.error(f"{body_path}.shape_count", f"expected {len(shapes)}")
            total_shape_count += len(shapes)
            for shape_index, shape in enumerate(shapes):
                self.validate_physics_shape(shape, f"{body_path}.shapes[{shape_index}]")
        if value.get("shape_count") != total_shape_count:
            self.error(f"{path}.shape_count", f"expected {total_shape_count}")

        constraints = value.get("constraints")
        if not isinstance(constraints, list):
            self.error(f"{path}.constraints", "expected array")
            constraints = []
        if value.get("constraint_count") != len(constraints):
            self.error(f"{path}.constraint_count", f"expected {len(constraints)}")
        for constraint_index, constraint in enumerate(constraints):
            constraint_path = f"{path}.constraints[{constraint_index}]"
            if not self.require_keys(
                constraint,
                constraint_path,
                [
                    "id",
                    "index",
                    "joint_name",
                    "child_bone_name",
                    "parent_bone_name",
                    "disable_collision",
                    "parent_dominates",
                    "linear_x_motion",
                    "linear_y_motion",
                    "linear_z_motion",
                    "linear_limit",
                    "swing1_motion",
                    "swing2_motion",
                    "twist_motion",
                    "swing1_limit_degrees",
                    "swing2_limit_degrees",
                    "twist_limit_degrees",
                    "constraint_profile_count",
                ],
            ):
                continue
            self.expect_hex64(constraint.get("id"), f"{constraint_path}.id")
            if constraint.get("index") != constraint_index:
                self.error(f"{constraint_path}.index", f"expected {constraint_index}")
            for key in (
                "joint_name",
                "child_bone_name",
                "parent_bone_name",
                "linear_x_motion",
                "linear_y_motion",
                "linear_z_motion",
                "swing1_motion",
                "swing2_motion",
                "twist_motion",
            ):
                self.expect_string(constraint.get(key), f"{constraint_path}.{key}", allow_empty=key == "joint_name")
            for key in ("disable_collision", "parent_dominates"):
                if not isinstance(constraint.get(key), bool):
                    self.error(f"{constraint_path}.{key}", "expected boolean")
            for key in ("linear_limit", "swing1_limit_degrees", "swing2_limit_degrees", "twist_limit_degrees"):
                if not isinstance(constraint.get(key), (int, float)):
                    self.error(f"{constraint_path}.{key}", "expected number")
            if not isinstance(constraint.get("constraint_profile_count"), int) or constraint.get("constraint_profile_count") < 0:
                self.error(f"{constraint_path}.constraint_profile_count", "expected non-negative integer")

    def validate_anim_blueprint_static_item(self, value: Any, path: str) -> None:
        if not isinstance(value, dict):
            self.error(path, "expected object")
            return
        if not self.require_keys(value, path, ["id"]):
            return
        self.expect_hex64(value.get("id"), f"{path}.id")
        for key, item in value.items():
            if item is not None and not isinstance(item, (str, int, float, bool)):
                self.error(f"{path}.{key}", "expected scalar")

    def validate_anim_blueprint_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "anim_blueprint_path",
                "state_machine_count",
                "state_count",
                "transition_count",
                "asset_player_count",
                "cached_pose_count",
                "slot_count",
                "control_rig_node_count",
                "state_machines",
                "states",
                "transitions",
                "asset_players",
                "cached_poses",
                "slots",
                "control_rig_nodes",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.anim_blueprint.v1":
            self.error(f"{path}.schema_version", "expected uepi.anim_blueprint.v1")
        if value.get("source_layer") != "editor_source_graph":
            self.error(f"{path}.source_layer", "expected editor_source_graph")
        for key in ("anim_blueprint_path", "target_skeleton_path"):
            if key in value:
                self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "target_skeleton_path")
        for key in ("is_template", "use_multi_threaded_animation_update", "warn_about_blueprint_usage"):
            if key in value and not isinstance(value.get(key), bool):
                self.error(f"{path}.{key}", "expected boolean")
        for count_key in (
            "sync_group_count",
            "state_machine_count",
            "state_count",
            "transition_count",
            "asset_player_count",
            "cached_pose_count",
            "slot_count",
            "control_rig_node_count",
        ):
            if count_key in value:
                count = value.get(count_key)
                if not isinstance(count, int) or count < 0:
                    self.error(f"{path}.{count_key}", "expected non-negative integer")

        for array_key, count_key in (
            ("state_machines", "state_machine_count"),
            ("states", "state_count"),
            ("transitions", "transition_count"),
            ("asset_players", "asset_player_count"),
            ("cached_poses", "cached_pose_count"),
            ("slots", "slot_count"),
            ("control_rig_nodes", "control_rig_node_count"),
        ):
            items = value.get(array_key)
            if not isinstance(items, list):
                self.error(f"{path}.{array_key}", "expected array")
                continue
            expected_count = value.get(count_key)
            if isinstance(expected_count, int) and len(items) != expected_count:
                self.error(f"{path}.{array_key}", f"expected {expected_count} items")
            for index, item in enumerate(items):
                self.validate_anim_blueprint_static_item(item, f"{path}.{array_key}[{index}]")

    def validate_control_rig_static_item(self, value: Any, path: str) -> None:
        self.validate_anim_blueprint_static_item(value, path)

    def validate_control_rig_blueprint_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "control_rig_blueprint_path",
                "rigvm_graph_count",
                "rigvm_node_count",
                "rigvm_pin_count",
                "rigvm_link_count",
                "hierarchy_element_count",
                "rigvm_graphs",
                "rigvm_nodes",
                "rigvm_pins",
                "rigvm_links",
                "hierarchy_elements",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.control_rig_blueprint.v1":
            self.error(f"{path}.schema_version", "expected uepi.control_rig_blueprint.v1")
        if value.get("source_layer") != "editor_source_graph":
            self.error(f"{path}.source_layer", "expected editor_source_graph")
        for key in ("control_rig_blueprint_path", "preview_skeletal_mesh_path"):
            if key in value:
                self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key == "preview_skeletal_mesh_path")

        for count_key in (
            "rigvm_graph_count",
            "rigvm_node_count",
            "rigvm_pin_count",
            "rigvm_link_count",
            "hierarchy_element_count",
        ):
            count = value.get(count_key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{count_key}", "expected non-negative integer")

        for array_key, count_key in (
            ("rigvm_graphs", "rigvm_graph_count"),
            ("rigvm_nodes", "rigvm_node_count"),
            ("rigvm_pins", "rigvm_pin_count"),
            ("rigvm_links", "rigvm_link_count"),
            ("hierarchy_elements", "hierarchy_element_count"),
        ):
            items = value.get(array_key)
            if not isinstance(items, list):
                self.error(f"{path}.{array_key}", "expected array")
                continue
            expected_count = value.get(count_key)
            if isinstance(expected_count, int) and len(items) != expected_count:
                self.error(f"{path}.{array_key}", f"expected {expected_count} items")
            for index, item in enumerate(items):
                self.validate_control_rig_static_item(item, f"{path}.{array_key}[{index}]")

    def validate_ik_rig_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "ik_rig_path",
                "preview_skeletal_mesh_path",
                "retarget_root",
                "bone_count",
                "excluded_bone_count",
                "goal_count",
                "solver_count",
                "chain_count",
                "excluded_bones",
                "goals",
                "solvers",
                "chains",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.ik_rig.v1":
            self.error(f"{path}.schema_version", "expected uepi.ik_rig.v1")
        if value.get("source_layer") != "animation_data_model":
            self.error(f"{path}.source_layer", "expected animation_data_model")
        for key in ("ik_rig_path", "preview_skeletal_mesh_path", "retarget_root"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key != "ik_rig_path")
        for key in ("bone_count", "excluded_bone_count", "goal_count", "solver_count", "chain_count"):
            count = value.get(key)
            if not isinstance(count, int) or count < 0:
                self.error(f"{path}.{key}", "expected non-negative integer")

        excluded_bones = value.get("excluded_bones")
        if not isinstance(excluded_bones, list):
            self.error(f"{path}.excluded_bones", "expected array")
            excluded_bones = []
        if value.get("excluded_bone_count") != len(excluded_bones):
            self.error(f"{path}.excluded_bone_count", f"expected {len(excluded_bones)}")
        for bone_index, bone_name in enumerate(excluded_bones):
            self.expect_string(bone_name, f"{path}.excluded_bones[{bone_index}]")

        goals = value.get("goals")
        if not isinstance(goals, list):
            self.error(f"{path}.goals", "expected array")
            goals = []
        if value.get("goal_count") != len(goals):
            self.error(f"{path}.goal_count", f"expected {len(goals)}")
        for goal_index, goal in enumerate(goals):
            goal_path = f"{path}.goals[{goal_index}]"
            if not self.require_keys(
                goal,
                goal_path,
                ["id", "index", "name", "bone_name", "position_alpha", "rotation_alpha", "current_position", "initial_position"],
            ):
                continue
            self.expect_hex64(goal.get("id"), f"{goal_path}.id")
            if goal.get("index") != goal_index:
                self.error(f"{goal_path}.index", f"expected {goal_index}")
            self.expect_string(goal.get("name"), f"{goal_path}.name")
            self.expect_string(goal.get("bone_name"), f"{goal_path}.bone_name")
            for key in ("position_alpha", "rotation_alpha"):
                if not isinstance(goal.get(key), (int, float)):
                    self.error(f"{goal_path}.{key}", "expected number")
            self.validate_vector(goal.get("current_position"), f"{goal_path}.current_position")
            self.validate_vector(goal.get("initial_position"), f"{goal_path}.initial_position")

        solvers = value.get("solvers")
        if not isinstance(solvers, list):
            self.error(f"{path}.solvers", "expected array")
            solvers = []
        if value.get("solver_count") != len(solvers):
            self.error(f"{path}.solver_count", f"expected {len(solvers)}")
        for solver_index, solver in enumerate(solvers):
            solver_path = f"{path}.solvers[{solver_index}]"
            if not self.require_keys(
                solver,
                solver_path,
                [
                    "id",
                    "index",
                    "class",
                    "enabled",
                    "root_bone",
                    "end_bone",
                    "requires_root_bone",
                    "requires_end_bone",
                    "uses_bone_settings",
                    "bone_setting_count",
                ],
            ):
                continue
            self.expect_hex64(solver.get("id"), f"{solver_path}.id")
            if solver.get("index") != solver_index:
                self.error(f"{solver_path}.index", f"expected {solver_index}")
            for key in ("class", "root_bone", "end_bone"):
                self.expect_string(solver.get(key), f"{solver_path}.{key}", allow_empty=key != "class")
            for key in ("enabled", "requires_root_bone", "requires_end_bone", "uses_bone_settings"):
                if not isinstance(solver.get(key), bool):
                    self.error(f"{solver_path}.{key}", "expected boolean")
            if not isinstance(solver.get("bone_setting_count"), int) or solver.get("bone_setting_count") < 0:
                self.error(f"{solver_path}.bone_setting_count", "expected non-negative integer")

        chains = value.get("chains")
        if not isinstance(chains, list):
            self.error(f"{path}.chains", "expected array")
            chains = []
        if value.get("chain_count") != len(chains):
            self.error(f"{path}.chain_count", f"expected {len(chains)}")
        for chain_index, chain in enumerate(chains):
            chain_path = f"{path}.chains[{chain_index}]"
            if not self.require_keys(chain, chain_path, ["id", "index", "name", "start_bone", "end_bone", "ik_goal_name"]):
                continue
            self.expect_hex64(chain.get("id"), f"{chain_path}.id")
            if chain.get("index") != chain_index:
                self.error(f"{chain_path}.index", f"expected {chain_index}")
            for key in ("name", "start_bone", "end_bone", "ik_goal_name"):
                self.expect_string(chain.get(key), f"{chain_path}.{key}", allow_empty=key != "name")

    def validate_ik_retarget_pose(self, value: Any, path: str) -> None:
        if not self.require_keys(value, path, ["name", "root_translation_offset", "bone_rotation_offset_count", "bone_rotation_offsets"]):
            return
        self.expect_string(value.get("name"), f"{path}.name")
        self.validate_vector(value.get("root_translation_offset"), f"{path}.root_translation_offset")
        offsets = value.get("bone_rotation_offsets")
        if not isinstance(offsets, list):
            self.error(f"{path}.bone_rotation_offsets", "expected array")
            offsets = []
        if value.get("bone_rotation_offset_count") != len(offsets):
            self.error(f"{path}.bone_rotation_offset_count", f"expected {len(offsets)}")
        for offset_index, offset in enumerate(offsets):
            offset_path = f"{path}.bone_rotation_offsets[{offset_index}]"
            if not self.require_keys(offset, offset_path, ["bone_name", "rotation_offset"]):
                continue
            self.expect_string(offset.get("bone_name"), f"{offset_path}.bone_name", allow_empty=False)
            self.validate_quat(offset.get("rotation_offset"), f"{offset_path}.rotation_offset")

    def validate_ik_retargeter_snapshot(self, value: Any, path: str) -> None:
        if not self.require_keys(
            value,
            path,
            [
                "schema_version",
                "source_layer",
                "ik_retargeter_path",
                "source_ik_rig_path",
                "target_ik_rig_path",
                "chain_map_count",
                "source_current_pose_name",
                "target_current_pose_name",
                "source_current_pose",
                "target_current_pose",
                "root_settings",
                "global_settings",
                "chain_maps",
            ],
        ):
            return
        if value.get("schema_version") != "uepi.ik_retargeter.v1":
            self.error(f"{path}.schema_version", "expected uepi.ik_retargeter.v1")
        if value.get("source_layer") != "animation_data_model":
            self.error(f"{path}.source_layer", "expected animation_data_model")
        for key in ("ik_retargeter_path", "source_ik_rig_path", "target_ik_rig_path", "source_current_pose_name", "target_current_pose_name"):
            self.expect_string(value.get(key), f"{path}.{key}", allow_empty=key != "ik_retargeter_path")
        if not isinstance(value.get("chain_map_count"), int) or value.get("chain_map_count") < 0:
            self.error(f"{path}.chain_map_count", "expected non-negative integer")

        self.validate_ik_retarget_pose(value.get("source_current_pose"), f"{path}.source_current_pose")
        self.validate_ik_retarget_pose(value.get("target_current_pose"), f"{path}.target_current_pose")

        root_settings = value.get("root_settings")
        if self.require_keys(
            root_settings,
            f"{path}.root_settings",
            [
                "rotation_alpha",
                "translation_alpha",
                "blend_to_source",
                "blend_to_source_weights",
                "scale_horizontal",
                "scale_vertical",
                "translation_offset",
                "affect_ik_horizontal",
                "affect_ik_vertical",
            ],
        ):
            for key in (
                "rotation_alpha",
                "translation_alpha",
                "blend_to_source",
                "scale_horizontal",
                "scale_vertical",
                "affect_ik_horizontal",
                "affect_ik_vertical",
            ):
                if not isinstance(root_settings.get(key), (int, float)):
                    self.error(f"{path}.root_settings.{key}", "expected number")
            self.validate_vector(root_settings.get("blend_to_source_weights"), f"{path}.root_settings.blend_to_source_weights")
            self.validate_vector(root_settings.get("translation_offset"), f"{path}.root_settings.translation_offset")

        global_settings = value.get("global_settings")
        if self.require_keys(
            global_settings,
            f"{path}.global_settings",
            [
                "enable_root",
                "enable_fk",
                "enable_ik",
                "enable_warping",
                "direction_source",
                "forward_direction",
                "direction_chain",
                "warp_forwards",
                "sideways_offset",
                "warp_splay",
            ],
        ):
            for key in ("enable_root", "enable_fk", "enable_ik", "enable_warping"):
                if not isinstance(global_settings.get(key), bool):
                    self.error(f"{path}.global_settings.{key}", "expected boolean")
            for key in ("direction_source", "forward_direction", "direction_chain"):
                self.expect_string(global_settings.get(key), f"{path}.global_settings.{key}")
            for key in ("warp_forwards", "sideways_offset", "warp_splay"):
                if not isinstance(global_settings.get(key), (int, float)):
                    self.error(f"{path}.global_settings.{key}", "expected number")

        chain_maps = value.get("chain_maps")
        if not isinstance(chain_maps, list):
            self.error(f"{path}.chain_maps", "expected array")
            chain_maps = []
        if value.get("chain_map_count") != len(chain_maps):
            self.error(f"{path}.chain_map_count", f"expected {len(chain_maps)}")
        for chain_map_index, chain_map in enumerate(chain_maps):
            chain_map_path = f"{path}.chain_maps[{chain_map_index}]"
            if not self.require_keys(
                chain_map,
                chain_map_path,
                [
                    "id",
                    "index",
                    "source_chain",
                    "target_chain",
                    "fk_enabled",
                    "fk_rotation_mode",
                    "fk_rotation_alpha",
                    "fk_translation_mode",
                    "fk_translation_alpha",
                    "ik_enabled",
                    "ik_blend_to_source",
                    "ik_blend_to_source_weights",
                    "ik_extension",
                    "ik_affected_by_warping",
                    "speed_planting_enabled",
                    "speed_curve_name",
                    "speed_threshold",
                ],
            ):
                continue
            self.expect_hex64(chain_map.get("id"), f"{chain_map_path}.id")
            if chain_map.get("index") != chain_map_index:
                self.error(f"{chain_map_path}.index", f"expected {chain_map_index}")
            for key in ("source_chain", "target_chain", "fk_rotation_mode", "fk_translation_mode", "speed_curve_name"):
                self.expect_string(chain_map.get(key), f"{chain_map_path}.{key}")
            for key in ("fk_enabled", "ik_enabled", "ik_affected_by_warping", "speed_planting_enabled"):
                if not isinstance(chain_map.get(key), bool):
                    self.error(f"{chain_map_path}.{key}", "expected boolean")
            for key in ("fk_rotation_alpha", "fk_translation_alpha", "ik_blend_to_source", "ik_extension", "speed_threshold"):
                if not isinstance(chain_map.get(key), (int, float)):
                    self.error(f"{chain_map_path}.{key}", "expected number")
            self.validate_vector(chain_map.get("ik_blend_to_source_weights"), f"{chain_map_path}.ik_blend_to_source_weights")

    def result(self, scan: Any) -> dict[str, Any]:
        entities = scan.get("entities", []) if isinstance(scan, dict) else []
        relations = scan.get("relations", []) if isinstance(scan, dict) else []
        return {
            "ok": not self.errors,
            "error_count": len(self.errors),
            "warning_count": len(self.warnings),
            "entity_count": len(entities) if isinstance(entities, list) else 0,
            "relation_count": len(relations) if isinstance(relations, list) else 0,
            "errors": self.errors,
            "warnings": self.warnings,
        }


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def count_by(items: list[Any], key: str) -> dict[str, int]:
    counter: Counter[str] = Counter()
    for item in items:
        if isinstance(item, dict):
            value = item.get(key)
            if isinstance(value, str):
                counter[value] += 1
    return dict(sorted(counter.items()))


def scan_summary(scan: dict[str, Any]) -> dict[str, Any]:
    entities = scan.get("entities", [])
    relations = scan.get("relations", [])
    diagnostics = scan.get("diagnostics", [])
    if not isinstance(entities, list):
        entities = []
    if not isinstance(relations, list):
        relations = []
    if not isinstance(diagnostics, list):
        diagnostics = []

    semantic_kinds: Counter[str] = Counter()
    world_snapshot_count = 0
    world_level_count = 0
    world_streaming_level_count = 0
    world_level_script_actor_count = 0
    world_level_instance_actor_count = 0
    world_data_layers_snapshot_count = 0
    data_layer_instance_count = 0
    data_layer_asset_reference_count = 0
    world_partition_snapshot_count = 0
    world_partition_actor_desc_snapshot_count = 0
    world_partition_actor_desc_count = 0
    world_partition_actor_desc_reference_count = 0
    blueprint_graph_count = 0
    cfg_basic_block_count = 0
    dfg_value_count = 0
    widget_blueprint_snapshot_count = 0
    widget_count = 0
    widget_animation_count = 0
    widget_binding_count = 0
    widget_named_slot_count = 0
    input_action_snapshot_count = 0
    input_mapping_context_count = 0
    input_mapping_count = 0
    input_value_types: Counter[str] = Counter()
    common_ui_input_data_snapshot_count = 0
    common_ui_input_data_row_handle_count = 0
    common_ui_input_data_enhanced_action_reference_count = 0
    common_ui_hold_data_snapshot_count = 0
    common_ui_input_action_table_snapshot_count = 0
    common_ui_input_action_row_count = 0
    common_ui_input_action_hold_binding_count = 0
    blackboard_snapshot_count = 0
    blackboard_key_count = 0
    behavior_tree_snapshot_count = 0
    behavior_tree_node_count = 0
    behavior_tree_root_decorator_count = 0
    env_query_snapshot_count = 0
    env_query_option_count = 0
    env_query_test_count = 0
    state_tree_snapshot_count = 0
    state_tree_state_count = 0
    state_tree_node_count = 0
    state_tree_transition_count = 0
    state_tree_external_data_count = 0
    state_tree_context_data_count = 0
    state_tree_property_binding_count = 0
    gameplay_ability_snapshot_count = 0
    gameplay_ability_trigger_count = 0
    gameplay_effect_snapshot_count = 0
    gameplay_effect_modifier_count = 0
    gameplay_effect_execution_count = 0
    gameplay_effect_cue_count = 0
    gameplay_effect_overflow_count = 0
    gameplay_effect_granted_ability_count = 0
    gameplay_cue_notify_snapshot_count = 0
    gameplay_cue_notify_actor_count = 0
    gameplay_cue_notify_static_count = 0
    user_defined_struct_snapshot_count = 0
    user_defined_struct_field_count = 0
    user_defined_enum_snapshot_count = 0
    user_defined_enum_entry_count = 0
    user_defined_enum_hidden_entry_count = 0
    user_defined_enum_max_entry_count = 0
    data_table_snapshot_count = 0
    data_table_column_count = 0
    data_table_row_count = 0
    data_table_field_count = 0
    string_table_snapshot_count = 0
    string_table_entry_count = 0
    string_table_metadata_count = 0
    curve_snapshot_count = 0
    curve_channel_count = 0
    curve_key_count = 0
    curve_linear_color_atlas_snapshot_count = 0
    curve_atlas_entry_count = 0
    curve_atlas_curve_reference_count = 0
    skeleton_snapshot_count = 0
    skeleton_bone_count = 0
    skeletal_mesh_snapshot_count = 0
    skeletal_mesh_lod_count = 0
    animation_sequence_snapshot_count = 0
    animation_sequence_track_count = 0
    animation_sequence_notify_count = 0
    blend_space_snapshot_count = 0
    blend_space_axis_count = 0
    blend_space_sample_count = 0
    blend_space_animation_reference_count = 0
    pose_asset_snapshot_count = 0
    pose_asset_pose_count = 0
    pose_asset_curve_count = 0
    pose_asset_track_count = 0
    pose_asset_source_animation_reference_count = 0
    physics_asset_snapshot_count = 0
    physics_body_count = 0
    physics_shape_count = 0
    physics_constraint_count = 0
    anim_blueprint_snapshot_count = 0
    anim_state_machine_count = 0
    anim_state_count = 0
    anim_transition_count = 0
    anim_asset_player_count = 0
    anim_cached_pose_count = 0
    anim_slot_count = 0
    anim_control_rig_node_count = 0
    control_rig_blueprint_snapshot_count = 0
    control_rig_vm_graph_count = 0
    control_rig_vm_node_count = 0
    control_rig_vm_pin_count = 0
    control_rig_vm_link_count = 0
    control_rig_hierarchy_element_count = 0
    ik_rig_snapshot_count = 0
    ik_rig_chain_count = 0
    ik_rig_goal_count = 0
    ik_rig_solver_count = 0
    ik_retargeter_snapshot_count = 0
    ik_retarget_chain_map_count = 0
    ik_retarget_pose_bone_offset_count = 0
    static_mesh_snapshot_count = 0
    static_mesh_lod_count = 0
    static_mesh_material_count = 0
    texture_snapshot_count = 0
    texture_total_pixel_count = 0
    texture_mip_count = 0
    texture_generic_snapshot_count = 0
    texture_cube_snapshot_count = 0
    texture_cube_face_count = 0
    sound_cue_snapshot_count = 0
    sound_cue_node_count = 0
    sound_cue_wave_player_count = 0
    sound_cue_child_edge_count = 0
    sound_wave_snapshot_count = 0
    sound_wave_cue_point_count = 0
    sound_wave_subtitle_count = 0
    sound_wave_channel_count = 0
    level_sequence_snapshot_count = 0
    level_sequence_spawnable_count = 0
    level_sequence_possessable_count = 0
    level_sequence_binding_count = 0
    level_sequence_track_count = 0
    level_sequence_section_count = 0
    level_sequence_channel_count = 0
    level_sequence_key_count = 0
    level_sequence_binding_tag_count = 0
    level_sequence_marked_frame_count = 0
    level_sequence_camera_cut_section_count = 0
    level_sequence_subsequence_section_count = 0
    level_sequence_audio_section_count = 0
    level_sequence_animation_section_count = 0
    level_sequence_control_rig_section_count = 0
    sound_submix_snapshot_count = 0
    sound_submix_child_count = 0
    sound_submix_effect_count = 0
    sound_submix_with_parent_count = 0
    sound_submix_with_ambisonics_settings_count = 0
    sound_submix_effect_preset_snapshot_count = 0
    sound_submix_effect_setting_property_count = 0
    material_snapshot_count = 0
    material_function_snapshot_count = 0
    material_instance_snapshot_count = 0
    material_parameter_collection_snapshot_count = 0
    material_collection_parameter_count = 0
    material_collection_scalar_parameter_count = 0
    material_collection_vector_parameter_count = 0
    material_expression_snapshot_count = 0
    material_expression_input_count = 0
    material_expression_output_count = 0
    material_function_reference_count = 0
    material_texture_reference_count = 0
    material_parameter_override_count = 0
    niagara_system_snapshot_count = 0
    niagara_emitter_snapshot_count = 0
    niagara_script_snapshot_count = 0
    niagara_script_ref_count = 0
    niagara_parameter_count = 0
    niagara_renderer_count = 0
    niagara_event_handler_count = 0
    niagara_simulation_stage_count = 0
    niagara_parameter_definitions_snapshot_count = 0
    niagara_parameter_definition_count = 0
    niagara_parameter_definition_static_switch_count = 0
    niagara_parameter_definition_advanced_display_count = 0
    vector_field_static_snapshot_count = 0
    vector_field_voxel_count = 0
    vector_field_source_data_size_bytes = 0
    pcg_graph_snapshot_count = 0
    pcg_node_count = 0
    pcg_pin_count = 0
    pcg_edge_count = 0
    pcg_settings_count = 0
    pcg_setting_property_count = 0
    pcg_subgraph_reference_count = 0
    pcg_universal_graph_count = 0
    pcg_universal_node_count = 0
    pcg_universal_port_count = 0
    pcg_universal_link_count = 0
    metasound_snapshot_count = 0
    metasound_graph_count = 0
    metasound_node_count = 0
    metasound_vertex_count = 0
    metasound_edge_count = 0
    metasound_literal_count = 0
    metasound_dependency_count = 0
    metasound_interface_count = 0
    metasound_preset_count = 0
    metasound_universal_graph_count = 0
    metasound_universal_node_count = 0
    metasound_universal_port_count = 0
    metasound_universal_link_count = 0

    def count_niagara_emitter_snapshot(emitter: Any) -> None:
        nonlocal niagara_emitter_snapshot_count
        nonlocal niagara_script_ref_count
        nonlocal niagara_renderer_count
        nonlocal niagara_event_handler_count
        nonlocal niagara_simulation_stage_count
        if not isinstance(emitter, dict):
            return
        niagara_emitter_snapshot_count += 1
        renderers = emitter.get("renderers", [])
        event_handlers = emitter.get("event_handlers", [])
        simulation_stages = emitter.get("simulation_stages", [])
        scripts = emitter.get("scripts", [])
        if isinstance(renderers, list):
            niagara_renderer_count += len(renderers)
        if isinstance(event_handlers, list):
            niagara_event_handler_count += len(event_handlers)
        if isinstance(simulation_stages, list):
            niagara_simulation_stage_count += len(simulation_stages)
        if isinstance(scripts, list):
            niagara_script_ref_count += len(scripts)

    def count_niagara_system_snapshot(system: Any) -> None:
        nonlocal niagara_system_snapshot_count
        nonlocal niagara_script_ref_count
        nonlocal niagara_parameter_count
        if not isinstance(system, dict):
            return
        niagara_system_snapshot_count += 1
        scripts = system.get("scripts", [])
        user_parameters = system.get("user_parameters", [])
        emitters = system.get("emitters", [])
        if isinstance(scripts, list):
            niagara_script_ref_count += len(scripts)
        if isinstance(user_parameters, list):
            niagara_parameter_count += len(user_parameters)
        if isinstance(emitters, list):
            for emitter in emitters:
                count_niagara_emitter_snapshot(emitter)

    def count_niagara_script_snapshot(script: Any) -> None:
        nonlocal niagara_script_snapshot_count
        nonlocal niagara_parameter_count
        if not isinstance(script, dict):
            return
        niagara_script_snapshot_count += 1
        parameters = script.get("rapid_iteration_parameters", [])
        if isinstance(parameters, list):
            niagara_parameter_count += len(parameters)

    def count_niagara_parameter_definitions_snapshot(definitions: Any) -> None:
        nonlocal niagara_parameter_definitions_snapshot_count
        nonlocal niagara_parameter_definition_count
        nonlocal niagara_parameter_definition_static_switch_count
        nonlocal niagara_parameter_definition_advanced_display_count
        if not isinstance(definitions, dict):
            return
        niagara_parameter_definitions_snapshot_count += 1
        parameters = definitions.get("parameters", [])
        if isinstance(parameters, list):
            niagara_parameter_definition_count += len(parameters)
            for parameter in parameters:
                if not isinstance(parameter, dict):
                    continue
                if parameter.get("static_switch"):
                    niagara_parameter_definition_static_switch_count += 1
                if parameter.get("advanced_display"):
                    niagara_parameter_definition_advanced_display_count += 1

    def count_vector_field_static_snapshot(vector_field: Any) -> None:
        nonlocal vector_field_static_snapshot_count
        nonlocal vector_field_voxel_count
        nonlocal vector_field_source_data_size_bytes
        if not isinstance(vector_field, dict):
            return
        vector_field_static_snapshot_count += 1
        vector_field_voxel_count += int(vector_field.get("voxel_count", 0) or 0)
        vector_field_source_data_size_bytes += int(vector_field.get("source_data_size_bytes", 0) or 0)

    def count_pcg_graph_snapshot(pcg_graph: Any) -> None:
        nonlocal pcg_graph_snapshot_count
        nonlocal pcg_node_count
        nonlocal pcg_pin_count
        nonlocal pcg_edge_count
        nonlocal pcg_settings_count
        nonlocal pcg_setting_property_count
        nonlocal pcg_subgraph_reference_count
        nonlocal pcg_universal_graph_count
        nonlocal pcg_universal_node_count
        nonlocal pcg_universal_port_count
        nonlocal pcg_universal_link_count
        if not isinstance(pcg_graph, dict):
            return
        pcg_graph_snapshot_count += 1
        nodes = pcg_graph.get("nodes", [])
        pins = pcg_graph.get("pins", [])
        edges = pcg_graph.get("edges", [])
        settings = pcg_graph.get("settings", [])
        subgraphs = pcg_graph.get("subgraphs", [])
        if isinstance(nodes, list):
            pcg_node_count += len(nodes)
        if isinstance(pins, list):
            pcg_pin_count += len(pins)
        if isinstance(edges, list):
            pcg_edge_count += len(edges)
        if isinstance(settings, list):
            pcg_settings_count += len(settings)
            for setting in settings:
                if isinstance(setting, dict):
                    properties = setting.get("properties", [])
                    if isinstance(properties, list):
                        pcg_setting_property_count += len(properties)
        if isinstance(subgraphs, list):
            pcg_subgraph_reference_count += len(subgraphs)
        universal_graph = pcg_graph.get("universal_graph")
        if isinstance(universal_graph, dict):
            pcg_universal_graph_count += 1
            universal_nodes = universal_graph.get("nodes", [])
            universal_ports = universal_graph.get("ports", [])
            universal_links = universal_graph.get("links", [])
            if isinstance(universal_nodes, list):
                pcg_universal_node_count += len(universal_nodes)
            if isinstance(universal_ports, list):
                pcg_universal_port_count += len(universal_ports)
            if isinstance(universal_links, list):
                pcg_universal_link_count += len(universal_links)

    def count_metasound_snapshot(metasound: Any) -> None:
        nonlocal metasound_snapshot_count
        nonlocal metasound_graph_count
        nonlocal metasound_node_count
        nonlocal metasound_vertex_count
        nonlocal metasound_edge_count
        nonlocal metasound_literal_count
        nonlocal metasound_dependency_count
        nonlocal metasound_interface_count
        nonlocal metasound_preset_count
        nonlocal metasound_universal_graph_count
        nonlocal metasound_universal_node_count
        nonlocal metasound_universal_port_count
        nonlocal metasound_universal_link_count
        if not isinstance(metasound, dict):
            return
        metasound_snapshot_count += 1
        if metasound.get("preset"):
            metasound_preset_count += 1
        for array_key, counter_name in (
            ("graphs", "graph"),
            ("nodes", "node"),
            ("vertices", "vertex"),
            ("edges", "edge"),
            ("literals", "literal"),
            ("dependencies", "dependency"),
            ("interfaces", "interface"),
        ):
            values = metasound.get(array_key, [])
            if not isinstance(values, list):
                continue
            if counter_name == "graph":
                metasound_graph_count += len(values)
            elif counter_name == "node":
                metasound_node_count += len(values)
            elif counter_name == "vertex":
                metasound_vertex_count += len(values)
            elif counter_name == "edge":
                metasound_edge_count += len(values)
            elif counter_name == "literal":
                metasound_literal_count += len(values)
            elif counter_name == "dependency":
                metasound_dependency_count += len(values)
            elif counter_name == "interface":
                metasound_interface_count += len(values)
        universal_graph = metasound.get("universal_graph")
        if isinstance(universal_graph, dict):
            metasound_universal_graph_count += 1
            universal_nodes = universal_graph.get("nodes", [])
            universal_ports = universal_graph.get("ports", [])
            universal_links = universal_graph.get("links", [])
            if isinstance(universal_nodes, list):
                metasound_universal_node_count += len(universal_nodes)
            if isinstance(universal_ports, list):
                metasound_universal_port_count += len(universal_ports)
            if isinstance(universal_links, list):
                metasound_universal_link_count += len(universal_links)

    for entity in entities:
        if not isinstance(entity, dict):
            continue
        attributes = entity.get("attributes", {})
        if isinstance(attributes, dict) and isinstance(attributes.get("semantic_kind"), str):
            semantic_kinds[attributes["semantic_kind"]] += 1
        snapshot = entity.get("snapshot", {})
        if isinstance(snapshot, dict):
            world = snapshot.get("world")
            if isinstance(world, dict):
                world_snapshot_count += 1
                levels = world.get("levels", [])
                streaming_levels = world.get("streaming_levels", [])
                if isinstance(levels, list):
                    world_level_count += len(levels)
                    for level in levels:
                        if not isinstance(level, dict):
                            continue
                        actors = level.get("actors", [])
                        if isinstance(actors, list):
                            for actor in actors:
                                if not isinstance(actor, dict):
                                    continue
                                if actor.get("is_level_script_actor"):
                                    world_level_script_actor_count += 1
                                if actor.get("is_level_instance"):
                                    world_level_instance_actor_count += 1
                if isinstance(streaming_levels, list):
                    world_streaming_level_count += len(streaming_levels)
            world_data_layers = snapshot.get("world_data_layers")
            if isinstance(world_data_layers, dict):
                world_data_layers_snapshot_count += 1
                data_layers = world_data_layers.get("data_layers", [])
                if isinstance(data_layers, list):
                    data_layer_instance_count += len(data_layers)
                    for data_layer in data_layers:
                        if isinstance(data_layer, dict) and data_layer.get("asset_path"):
                            data_layer_asset_reference_count += 1
            world_partition = snapshot.get("world_partition")
            if isinstance(world_partition, dict):
                world_partition_snapshot_count += 1
                actor_descs = world_partition.get("actor_descs", [])
                if isinstance(actor_descs, list):
                    world_partition_actor_desc_count += len(actor_descs)
                    for actor_desc in actor_descs:
                        if isinstance(actor_desc, dict) and isinstance(actor_desc.get("references"), list):
                            world_partition_actor_desc_reference_count += len(actor_desc["references"])
            world_partition_actor_desc = snapshot.get("world_partition_actor_desc")
            if isinstance(world_partition_actor_desc, dict):
                world_partition_actor_desc_snapshot_count += 1
                world_partition_actor_desc_count += 1
                references = world_partition_actor_desc.get("references", [])
                if isinstance(references, list):
                    world_partition_actor_desc_reference_count += len(references)
            blueprint_graphs = snapshot.get("blueprint_graphs", {})
            if isinstance(blueprint_graphs, dict):
                graphs = blueprint_graphs.get("graphs", [])
                if isinstance(graphs, list):
                    blueprint_graph_count += len(graphs)
                    for graph in graphs:
                        if isinstance(graph, dict):
                            cfg_basic_block_count += int(graph.get("cfg_basic_block_count", 0) or 0)
                            dfg_value_count += int(graph.get("dfg_value_count", 0) or 0)
            widget_blueprint = snapshot.get("widget_blueprint")
            if isinstance(widget_blueprint, dict):
                widget_blueprint_snapshot_count += 1
                widgets = widget_blueprint.get("widgets", [])
                animations = widget_blueprint.get("animations", [])
                bindings = widget_blueprint.get("bindings", [])
                named_slots = widget_blueprint.get("named_slots", [])
                if isinstance(widgets, list):
                    widget_count += len(widgets)
                if isinstance(animations, list):
                    widget_animation_count += len(animations)
                if isinstance(bindings, list):
                    widget_binding_count += len(bindings)
                if isinstance(named_slots, list):
                    widget_named_slot_count += len(named_slots)
            input_action = snapshot.get("input_action")
            if isinstance(input_action, dict):
                input_action_snapshot_count += 1
                value_type = input_action.get("value_type")
                if isinstance(value_type, str):
                    input_value_types[value_type] += 1
            input_mapping_context = snapshot.get("input_mapping_context")
            if isinstance(input_mapping_context, dict):
                input_mapping_context_count += 1
                mappings = input_mapping_context.get("mappings", [])
                if isinstance(mappings, list):
                    input_mapping_count += len(mappings)
            common_ui_input_data = snapshot.get("common_ui_input_data")
            if not isinstance(common_ui_input_data, dict) and snapshot.get("schema_version") == "uepi.common_ui_input_data.v1":
                common_ui_input_data = snapshot
            if isinstance(common_ui_input_data, dict):
                common_ui_input_data_snapshot_count += 1
                for row_handle_key in ("default_click_action", "default_back_action"):
                    row_handle = common_ui_input_data.get(row_handle_key)
                    if isinstance(row_handle, dict) and row_handle.get("table_path") and row_handle.get("row_name"):
                        common_ui_input_data_row_handle_count += 1
                for action_key in ("enhanced_input_click_action", "enhanced_input_back_action"):
                    action_reference = common_ui_input_data.get(action_key)
                    if isinstance(action_reference, dict) and action_reference.get("path"):
                        common_ui_input_data_enhanced_action_reference_count += 1
            common_ui_hold_data = snapshot.get("common_ui_hold_data")
            if not isinstance(common_ui_hold_data, dict) and snapshot.get("schema_version") == "uepi.common_ui_hold_data.v1":
                common_ui_hold_data = snapshot
            if isinstance(common_ui_hold_data, dict):
                common_ui_hold_data_snapshot_count += 1
            common_ui_input_action_table = snapshot.get("common_ui_input_action_table")
            if not isinstance(common_ui_input_action_table, dict) and snapshot.get("schema_version") == "uepi.common_ui_input_action_table.v1":
                common_ui_input_action_table = snapshot
            if isinstance(common_ui_input_action_table, dict):
                common_ui_input_action_table_snapshot_count += 1
                rows = common_ui_input_action_table.get("rows", [])
                if isinstance(rows, list):
                    common_ui_input_action_row_count += len(rows)
                    for row in rows:
                        if isinstance(row, dict) and row.get("has_hold_bindings"):
                            common_ui_input_action_hold_binding_count += 1
            blackboard = snapshot.get("blackboard")
            if not isinstance(blackboard, dict) and snapshot.get("schema_version") == "uepi.blackboard.v1":
                blackboard = snapshot
            if isinstance(blackboard, dict):
                blackboard_snapshot_count += 1
                keys = blackboard.get("keys", [])
                if isinstance(keys, list):
                    blackboard_key_count += len(keys)
            behavior_tree = snapshot.get("behavior_tree")
            if not isinstance(behavior_tree, dict) and snapshot.get("schema_version") == "uepi.behavior_tree.v1":
                behavior_tree = snapshot
            if isinstance(behavior_tree, dict):
                behavior_tree_snapshot_count += 1
                behavior_tree_root_decorator_count += int(behavior_tree.get("root_decorator_count", 0) or 0)
                nodes = behavior_tree.get("nodes", [])
                if isinstance(nodes, list):
                    behavior_tree_node_count += len(nodes)
            env_query = snapshot.get("env_query")
            if not isinstance(env_query, dict) and snapshot.get("schema_version") == "uepi.env_query.v1":
                env_query = snapshot
            if isinstance(env_query, dict):
                env_query_snapshot_count += 1
                options = env_query.get("options", [])
                if isinstance(options, list):
                    env_query_option_count += len(options)
                    for option in options:
                        if isinstance(option, dict):
                            tests = option.get("tests", [])
                            if isinstance(tests, list):
                                env_query_test_count += len(tests)
            state_tree = snapshot.get("state_tree")
            if not isinstance(state_tree, dict) and snapshot.get("schema_version") == "uepi.state_tree.v1":
                state_tree = snapshot
            if isinstance(state_tree, dict):
                state_tree_snapshot_count += 1
                states = state_tree.get("states", [])
                nodes = state_tree.get("nodes", [])
                transitions = state_tree.get("transitions", [])
                external_data = state_tree.get("external_data", [])
                context_data = state_tree.get("context_data", [])
                bindings = state_tree.get("property_bindings", [])
                if isinstance(states, list):
                    state_tree_state_count += len(states)
                if isinstance(nodes, list):
                    state_tree_node_count += len(nodes)
                if isinstance(transitions, list):
                    state_tree_transition_count += len(transitions)
                if isinstance(external_data, list):
                    state_tree_external_data_count += len(external_data)
                if isinstance(context_data, list):
                    state_tree_context_data_count += len(context_data)
                if isinstance(bindings, list):
                    state_tree_property_binding_count += len(bindings)
            gameplay_ability = snapshot.get("gameplay_ability")
            if not isinstance(gameplay_ability, dict) and snapshot.get("schema_version") == "uepi.gameplay_ability.v1":
                gameplay_ability = snapshot
            if isinstance(gameplay_ability, dict):
                gameplay_ability_snapshot_count += 1
                triggers = gameplay_ability.get("triggers", [])
                if isinstance(triggers, list):
                    gameplay_ability_trigger_count += len(triggers)
            gameplay_effect = snapshot.get("gameplay_effect")
            if not isinstance(gameplay_effect, dict) and snapshot.get("schema_version") == "uepi.gameplay_effect.v1":
                gameplay_effect = snapshot
            if isinstance(gameplay_effect, dict):
                gameplay_effect_snapshot_count += 1
                modifiers = gameplay_effect.get("modifiers", [])
                executions = gameplay_effect.get("executions", [])
                gameplay_cues = gameplay_effect.get("gameplay_cues", [])
                overflow_effects = gameplay_effect.get("overflow_effects", [])
                granted_abilities = gameplay_effect.get("granted_abilities", [])
                if isinstance(modifiers, list):
                    gameplay_effect_modifier_count += len(modifiers)
                if isinstance(executions, list):
                    gameplay_effect_execution_count += len(executions)
                if isinstance(gameplay_cues, list):
                    gameplay_effect_cue_count += len(gameplay_cues)
                if isinstance(overflow_effects, list):
                    gameplay_effect_overflow_count += len(overflow_effects)
                if isinstance(granted_abilities, list):
                    gameplay_effect_granted_ability_count += len(granted_abilities)
            gameplay_cue_notify = snapshot.get("gameplay_cue_notify")
            if not isinstance(gameplay_cue_notify, dict) and snapshot.get("schema_version") == "uepi.gameplay_cue_notify.v1":
                gameplay_cue_notify = snapshot
            if isinstance(gameplay_cue_notify, dict):
                gameplay_cue_notify_snapshot_count += 1
                if gameplay_cue_notify.get("cue_type") == "actor":
                    gameplay_cue_notify_actor_count += 1
                elif gameplay_cue_notify.get("cue_type") == "static":
                    gameplay_cue_notify_static_count += 1
            user_defined_struct = snapshot.get("user_defined_struct")
            if not isinstance(user_defined_struct, dict) and snapshot.get("schema_version") == "uepi.user_defined_struct.v1":
                user_defined_struct = snapshot
            if isinstance(user_defined_struct, dict):
                user_defined_struct_snapshot_count += 1
                fields = user_defined_struct.get("fields", [])
                if isinstance(fields, list):
                    user_defined_struct_field_count += len(fields)
            user_defined_enum = snapshot.get("user_defined_enum")
            if not isinstance(user_defined_enum, dict) and snapshot.get("schema_version") == "uepi.user_defined_enum.v1":
                user_defined_enum = snapshot
            if isinstance(user_defined_enum, dict):
                user_defined_enum_snapshot_count += 1
                entries = user_defined_enum.get("entries", [])
                if isinstance(entries, list):
                    user_defined_enum_entry_count += len(entries)
                    for entry in entries:
                        if isinstance(entry, dict):
                            if entry.get("is_hidden"):
                                user_defined_enum_hidden_entry_count += 1
                            if entry.get("is_max"):
                                user_defined_enum_max_entry_count += 1
            data_table = snapshot.get("data_table")
            if isinstance(data_table, dict):
                data_table_snapshot_count += 1
                columns = data_table.get("columns", [])
                rows = data_table.get("rows", [])
                if isinstance(columns, list):
                    data_table_column_count += len(columns)
                if isinstance(rows, list):
                    data_table_row_count += len(rows)
                    for row in rows:
                        if isinstance(row, dict):
                            fields = row.get("fields", [])
                            if isinstance(fields, list):
                                data_table_field_count += len(fields)
            string_table = snapshot.get("string_table")
            if not isinstance(string_table, dict) and snapshot.get("schema_version") == "uepi.string_table.v1":
                string_table = snapshot
            if isinstance(string_table, dict):
                string_table_snapshot_count += 1
                entries = string_table.get("entries", [])
                if isinstance(entries, list):
                    string_table_entry_count += len(entries)
                    for entry in entries:
                        if isinstance(entry, dict):
                            metadata = entry.get("metadata", {})
                            if isinstance(metadata, dict):
                                string_table_metadata_count += len(metadata)
            curve = snapshot.get("curve")
            if isinstance(curve, dict):
                curve_snapshot_count += 1
                channels = curve.get("channels", [])
                if isinstance(channels, list):
                    curve_channel_count += len(channels)
                    for channel in channels:
                        if isinstance(channel, dict):
                            keys = channel.get("keys", [])
                            if isinstance(keys, list):
                                curve_key_count += len(keys)
            curve_linear_color_atlas = snapshot.get("curve_linear_color_atlas")
            if not isinstance(curve_linear_color_atlas, dict) and snapshot.get("schema_version") == "uepi.curve_linear_color_atlas.v1":
                curve_linear_color_atlas = snapshot
            if isinstance(curve_linear_color_atlas, dict):
                curve_linear_color_atlas_snapshot_count += 1
                entries = curve_linear_color_atlas.get("gradient_curves", [])
                if isinstance(entries, list):
                    curve_atlas_entry_count += len(entries)
                    for entry in entries:
                        if isinstance(entry, dict) and entry.get("curve_path"):
                            curve_atlas_curve_reference_count += 1
            niagara_system = snapshot.get("niagara_system")
            if not isinstance(niagara_system, dict) and snapshot.get("schema_version") == "uepi.niagara_system.v1":
                niagara_system = snapshot
            count_niagara_system_snapshot(niagara_system)

            niagara_emitter = snapshot.get("niagara_emitter")
            if not isinstance(niagara_emitter, dict) and snapshot.get("schema_version") == "uepi.niagara_emitter.v1":
                niagara_emitter = snapshot
            count_niagara_emitter_snapshot(niagara_emitter)

            niagara_script = snapshot.get("niagara_script")
            if not isinstance(niagara_script, dict) and snapshot.get("schema_version") == "uepi.niagara_script.v1":
                niagara_script = snapshot
            count_niagara_script_snapshot(niagara_script)

            niagara_parameter_definitions = snapshot.get("niagara_parameter_definitions")
            if not isinstance(niagara_parameter_definitions, dict) and snapshot.get("schema_version") == "uepi.niagara_parameter_definitions.v1":
                niagara_parameter_definitions = snapshot
            count_niagara_parameter_definitions_snapshot(niagara_parameter_definitions)

            vector_field_static = snapshot.get("vector_field_static")
            if not isinstance(vector_field_static, dict) and snapshot.get("schema_version") == "uepi.vector_field_static.v1":
                vector_field_static = snapshot
            count_vector_field_static_snapshot(vector_field_static)

            pcg_graph = snapshot.get("pcg_graph")
            if not isinstance(pcg_graph, dict) and snapshot.get("schema_version") == "uepi.pcg_graph.v1":
                pcg_graph = snapshot
            count_pcg_graph_snapshot(pcg_graph)

            metasound = snapshot.get("metasound")
            if not isinstance(metasound, dict) and snapshot.get("schema_version") == "uepi.metasound.v1":
                metasound = snapshot
            count_metasound_snapshot(metasound)
            skeleton = snapshot.get("skeleton")
            if isinstance(skeleton, dict):
                skeleton_snapshot_count += 1
                skeleton_bone_count += int(skeleton.get("bone_count", 0) or 0)
            skeletal_mesh = snapshot.get("skeletal_mesh")
            if isinstance(skeletal_mesh, dict):
                skeletal_mesh_snapshot_count += 1
                skeletal_mesh_lod_count += int(skeletal_mesh.get("lod_count", 0) or 0)
            animation_sequence = snapshot.get("animation_sequence")
            if isinstance(animation_sequence, dict):
                animation_sequence_snapshot_count += 1
                animation_sequence_track_count += int(animation_sequence.get("bone_track_count", 0) or 0)
                animation_sequence_notify_count += int(animation_sequence.get("notify_count", 0) or 0)
            blend_space = snapshot.get("blend_space")
            if isinstance(blend_space, dict):
                blend_space_snapshot_count += 1
                axes = blend_space.get("axes", [])
                samples = blend_space.get("samples", [])
                if isinstance(axes, list):
                    blend_space_axis_count += len(axes)
                if isinstance(samples, list):
                    blend_space_sample_count += len(samples)
                    for sample in samples:
                        if isinstance(sample, dict) and sample.get("animation_path"):
                            blend_space_animation_reference_count += 1
            pose_asset = snapshot.get("pose_asset")
            if isinstance(pose_asset, dict):
                pose_asset_snapshot_count += 1
                poses = pose_asset.get("poses", [])
                curves = pose_asset.get("curves", [])
                tracks = pose_asset.get("tracks", [])
                if isinstance(poses, list):
                    pose_asset_pose_count += len(poses)
                if isinstance(curves, list):
                    pose_asset_curve_count += len(curves)
                if isinstance(tracks, list):
                    pose_asset_track_count += len(tracks)
                if pose_asset.get("source_animation_path"):
                    pose_asset_source_animation_reference_count += 1
            physics_asset = snapshot.get("physics_asset")
            if isinstance(physics_asset, dict):
                physics_asset_snapshot_count += 1
                bodies = physics_asset.get("bodies", [])
                constraints = physics_asset.get("constraints", [])
                if isinstance(bodies, list):
                    physics_body_count += len(bodies)
                    for body in bodies:
                        if isinstance(body, dict):
                            shapes = body.get("shapes", [])
                            if isinstance(shapes, list):
                                physics_shape_count += len(shapes)
                if isinstance(constraints, list):
                    physics_constraint_count += len(constraints)
            anim_blueprint = snapshot.get("anim_blueprint")
            if isinstance(anim_blueprint, dict):
                anim_blueprint_snapshot_count += 1
                state_machines = anim_blueprint.get("state_machines", [])
                states = anim_blueprint.get("states", [])
                transitions = anim_blueprint.get("transitions", [])
                asset_players = anim_blueprint.get("asset_players", [])
                cached_poses = anim_blueprint.get("cached_poses", [])
                slots = anim_blueprint.get("slots", [])
                control_rig_nodes = anim_blueprint.get("control_rig_nodes", [])
                if isinstance(state_machines, list):
                    anim_state_machine_count += len(state_machines)
                if isinstance(states, list):
                    anim_state_count += len(states)
                if isinstance(transitions, list):
                    anim_transition_count += len(transitions)
                if isinstance(asset_players, list):
                    anim_asset_player_count += len(asset_players)
                if isinstance(cached_poses, list):
                    anim_cached_pose_count += len(cached_poses)
                if isinstance(slots, list):
                    anim_slot_count += len(slots)
                if isinstance(control_rig_nodes, list):
                    anim_control_rig_node_count += len(control_rig_nodes)
            control_rig_blueprint = snapshot.get("control_rig_blueprint")
            if isinstance(control_rig_blueprint, dict):
                control_rig_blueprint_snapshot_count += 1
                rigvm_graphs = control_rig_blueprint.get("rigvm_graphs", [])
                rigvm_nodes = control_rig_blueprint.get("rigvm_nodes", [])
                rigvm_pins = control_rig_blueprint.get("rigvm_pins", [])
                rigvm_links = control_rig_blueprint.get("rigvm_links", [])
                hierarchy_elements = control_rig_blueprint.get("hierarchy_elements", [])
                if isinstance(rigvm_graphs, list):
                    control_rig_vm_graph_count += len(rigvm_graphs)
                if isinstance(rigvm_nodes, list):
                    control_rig_vm_node_count += len(rigvm_nodes)
                if isinstance(rigvm_pins, list):
                    control_rig_vm_pin_count += len(rigvm_pins)
                if isinstance(rigvm_links, list):
                    control_rig_vm_link_count += len(rigvm_links)
                if isinstance(hierarchy_elements, list):
                    control_rig_hierarchy_element_count += len(hierarchy_elements)
            ik_rig = snapshot.get("ik_rig")
            if isinstance(ik_rig, dict):
                ik_rig_snapshot_count += 1
                chains = ik_rig.get("chains", [])
                goals = ik_rig.get("goals", [])
                solvers = ik_rig.get("solvers", [])
                if isinstance(chains, list):
                    ik_rig_chain_count += len(chains)
                if isinstance(goals, list):
                    ik_rig_goal_count += len(goals)
                if isinstance(solvers, list):
                    ik_rig_solver_count += len(solvers)
            ik_retargeter = snapshot.get("ik_retargeter")
            if isinstance(ik_retargeter, dict):
                ik_retargeter_snapshot_count += 1
                chain_maps = ik_retargeter.get("chain_maps", [])
                if isinstance(chain_maps, list):
                    ik_retarget_chain_map_count += len(chain_maps)
                for pose_key in ("source_current_pose", "target_current_pose"):
                    pose = ik_retargeter.get(pose_key)
                    if isinstance(pose, dict):
                        offsets = pose.get("bone_rotation_offsets", [])
                        if isinstance(offsets, list):
                            ik_retarget_pose_bone_offset_count += len(offsets)
            static_mesh = snapshot.get("static_mesh")
            if isinstance(static_mesh, dict):
                static_mesh_snapshot_count += 1
                static_mesh_lod_count += int(static_mesh.get("lod_count", 0) or 0)
                static_mesh_material_count += int(static_mesh.get("material_count", 0) or 0)
            texture = snapshot.get("texture")
            if isinstance(texture, dict):
                texture_snapshot_count += 1
                texture_generic_snapshot_count += 1
                size_x = int(texture.get("surface_width", 0) or 0)
                size_y = int(texture.get("surface_height", 0) or 0)
                array_size = int(texture.get("surface_array_size", 1) or 1)
                texture_total_pixel_count += size_x * size_y * array_size
                texture_mip_count += int(texture.get("source_mip_count", 0) or 0)
            texture2d = snapshot.get("texture2d")
            if isinstance(texture2d, dict):
                texture_snapshot_count += 1
                size_x = int(texture2d.get("size_x", 0) or 0)
                size_y = int(texture2d.get("size_y", 0) or 0)
                texture_total_pixel_count += size_x * size_y
                texture_mip_count += int(texture2d.get("mip_count", 0) or 0)
            texture_cube = snapshot.get("texture_cube")
            if isinstance(texture_cube, dict):
                texture_snapshot_count += 1
                texture_cube_snapshot_count += 1
                size_x = int(texture_cube.get("size_x", 0) or 0)
                size_y = int(texture_cube.get("size_y", 0) or 0)
                face_count = int(texture_cube.get("face_count", 0) or 0)
                texture_total_pixel_count += size_x * size_y * face_count
                texture_mip_count += int(texture_cube.get("mip_count", 0) or 0)
                texture_cube_face_count += face_count
            sound_cue = snapshot.get("sound_cue")
            if isinstance(sound_cue, dict):
                sound_cue_snapshot_count += 1
                nodes = sound_cue.get("nodes", [])
                if isinstance(nodes, list):
                    sound_cue_node_count += len(nodes)
                    for node in nodes:
                        if isinstance(node, dict):
                            if node.get("semantic_kind") == "wave_player":
                                sound_cue_wave_player_count += 1
                            sound_cue_child_edge_count += int(node.get("child_count", 0) or 0)
            sound_wave = snapshot.get("sound_wave")
            if isinstance(sound_wave, dict):
                sound_wave_snapshot_count += 1
                sound_wave_cue_point_count += int(sound_wave.get("cue_point_count", 0) or 0)
                sound_wave_subtitle_count += int(sound_wave.get("subtitle_count", 0) or 0)
                sound_wave_channel_count += int(sound_wave.get("num_channels", 0) or 0)
            level_sequence = snapshot.get("level_sequence")
            if not isinstance(level_sequence, dict) and snapshot.get("schema_version") == "uepi.level_sequence.v1":
                level_sequence = snapshot
            if isinstance(level_sequence, dict):
                level_sequence_snapshot_count += 1
                level_sequence_spawnable_count += int(level_sequence.get("spawnable_count", 0) or 0)
                level_sequence_possessable_count += int(level_sequence.get("possessable_count", 0) or 0)
                level_sequence_binding_count += int(level_sequence.get("binding_count", 0) or 0)
                level_sequence_track_count += int(level_sequence.get("track_count", 0) or 0)
                level_sequence_section_count += int(level_sequence.get("section_count", 0) or 0)
                level_sequence_channel_count += int(level_sequence.get("channel_count", 0) or 0)
                level_sequence_key_count += int(level_sequence.get("key_count", 0) or 0)
                level_sequence_binding_tag_count += int(level_sequence.get("binding_tag_count", 0) or 0)
                level_sequence_marked_frame_count += int(level_sequence.get("marked_frame_count", 0) or 0)
                sections = level_sequence.get("sections", [])
                if isinstance(sections, list):
                    for section in sections:
                        if not isinstance(section, dict):
                            continue
                        semantic_kind = section.get("semantic_kind")
                        if semantic_kind == "camera_cut":
                            level_sequence_camera_cut_section_count += 1
                        elif semantic_kind == "subsequence":
                            level_sequence_subsequence_section_count += 1
                        elif semantic_kind == "audio":
                            level_sequence_audio_section_count += 1
                        elif semantic_kind == "animation":
                            level_sequence_animation_section_count += 1
                        elif semantic_kind == "control_rig":
                            level_sequence_control_rig_section_count += 1
            sound_submix = snapshot.get("sound_submix")
            if isinstance(sound_submix, dict):
                sound_submix_snapshot_count += 1
                sound_submix_child_count += int(sound_submix.get("child_submix_count", 0) or 0)
                sound_submix_effect_count += int(sound_submix.get("effect_count", 0) or 0)
                if sound_submix.get("parent_submix_path"):
                    sound_submix_with_parent_count += 1
                if sound_submix.get("ambisonics_plugin_settings_path"):
                    sound_submix_with_ambisonics_settings_count += 1
            sound_submix_effect_preset = snapshot.get("sound_submix_effect_preset")
            if isinstance(sound_submix_effect_preset, dict):
                sound_submix_effect_preset_snapshot_count += 1
                sound_submix_effect_setting_property_count += int(sound_submix_effect_preset.get("setting_property_count", 0) or 0)
            material = snapshot.get("material")
            if isinstance(material, dict):
                material_snapshot_count += 1
                expressions = material.get("expressions", [])
                if isinstance(expressions, list):
                    material_expression_snapshot_count += len(expressions)
                    for expression in expressions:
                        if isinstance(expression, dict):
                            material_expression_input_count += int(expression.get("input_count", 0) or 0)
                            material_expression_output_count += int(expression.get("output_count", 0) or 0)
                            if expression.get("material_function_path"):
                                material_function_reference_count += 1
                            if expression.get("referenced_texture_path"):
                                material_texture_reference_count += 1
            material_parameter_collection = snapshot.get("material_parameter_collection")
            if not isinstance(material_parameter_collection, dict) and snapshot.get("schema_version") == "uepi.material_parameter_collection.v1":
                material_parameter_collection = snapshot
            if isinstance(material_parameter_collection, dict):
                material_parameter_collection_snapshot_count += 1
                parameters = material_parameter_collection.get("parameters", [])
                if isinstance(parameters, list):
                    material_collection_parameter_count += len(parameters)
                    for parameter in parameters:
                        if isinstance(parameter, dict):
                            if parameter.get("type") == "scalar":
                                material_collection_scalar_parameter_count += 1
                            elif parameter.get("type") == "vector":
                                material_collection_vector_parameter_count += 1
            material_function = snapshot.get("material_function")
            if isinstance(material_function, dict):
                material_function_snapshot_count += 1
                expressions = material_function.get("expressions", [])
                if isinstance(expressions, list):
                    material_expression_snapshot_count += len(expressions)
                    for expression in expressions:
                        if isinstance(expression, dict):
                            material_expression_input_count += int(expression.get("input_count", 0) or 0)
                            material_expression_output_count += int(expression.get("output_count", 0) or 0)
                            if expression.get("material_function_path"):
                                material_function_reference_count += 1
                            if expression.get("referenced_texture_path"):
                                material_texture_reference_count += 1
            material_instance = snapshot.get("material_instance")
            if isinstance(material_instance, dict):
                material_instance_snapshot_count += 1
                material_parameter_override_count += int(material_instance.get("parameter_override_count", 0) or 0)

    attributed_relation_count = sum(
        1
        for relation in relations
        if isinstance(relation, dict)
        and isinstance(relation.get("attributes"), dict)
        and len(relation["attributes"]) > 0
    )

    return {
        "schema_version": scan.get("schema_version", ""),
        "project_name": scan.get("project_name", ""),
        "engine_version": scan.get("engine_version", ""),
        "diagnostic_count": len(diagnostics),
        "entity_count": len(entities),
        "relation_count": len(relations),
        "entity_kinds": count_by(entities, "kind"),
        "relation_types": count_by(relations, "type"),
        "semantic_kinds": dict(sorted(semantic_kinds.items())),
        "attributed_relation_count": attributed_relation_count,
        "world_snapshot_count": world_snapshot_count,
        "world_level_count": world_level_count,
        "world_streaming_level_count": world_streaming_level_count,
        "world_level_script_actor_count": world_level_script_actor_count,
        "world_level_instance_actor_count": world_level_instance_actor_count,
        "world_data_layers_snapshot_count": world_data_layers_snapshot_count,
        "data_layer_instance_count": data_layer_instance_count,
        "data_layer_asset_reference_count": data_layer_asset_reference_count,
        "world_partition_snapshot_count": world_partition_snapshot_count,
        "world_partition_actor_desc_snapshot_count": world_partition_actor_desc_snapshot_count,
        "world_partition_actor_desc_count": world_partition_actor_desc_count,
        "world_partition_actor_desc_reference_count": world_partition_actor_desc_reference_count,
        "blueprint_graph_count": blueprint_graph_count,
        "cfg_basic_block_count": cfg_basic_block_count,
        "dfg_value_count": dfg_value_count,
        "widget_blueprint_snapshot_count": widget_blueprint_snapshot_count,
        "widget_count": widget_count,
        "widget_animation_count": widget_animation_count,
        "widget_binding_count": widget_binding_count,
        "widget_named_slot_count": widget_named_slot_count,
        "input_action_snapshot_count": input_action_snapshot_count,
        "input_mapping_context_count": input_mapping_context_count,
        "input_mapping_count": input_mapping_count,
        "input_value_types": dict(sorted(input_value_types.items())),
        "common_ui_input_data_snapshot_count": common_ui_input_data_snapshot_count,
        "common_ui_input_data_row_handle_count": common_ui_input_data_row_handle_count,
        "common_ui_input_data_enhanced_action_reference_count": common_ui_input_data_enhanced_action_reference_count,
        "common_ui_hold_data_snapshot_count": common_ui_hold_data_snapshot_count,
        "common_ui_input_action_table_snapshot_count": common_ui_input_action_table_snapshot_count,
        "common_ui_input_action_row_count": common_ui_input_action_row_count,
        "common_ui_input_action_hold_binding_count": common_ui_input_action_hold_binding_count,
        "blackboard_snapshot_count": blackboard_snapshot_count,
        "blackboard_key_count": blackboard_key_count,
        "behavior_tree_snapshot_count": behavior_tree_snapshot_count,
        "behavior_tree_node_count": behavior_tree_node_count,
        "behavior_tree_root_decorator_count": behavior_tree_root_decorator_count,
        "env_query_snapshot_count": env_query_snapshot_count,
        "env_query_option_count": env_query_option_count,
        "env_query_test_count": env_query_test_count,
        "state_tree_snapshot_count": state_tree_snapshot_count,
        "state_tree_state_count": state_tree_state_count,
        "state_tree_node_count": state_tree_node_count,
        "state_tree_transition_count": state_tree_transition_count,
        "state_tree_external_data_count": state_tree_external_data_count,
        "state_tree_context_data_count": state_tree_context_data_count,
        "state_tree_property_binding_count": state_tree_property_binding_count,
        "gameplay_ability_snapshot_count": gameplay_ability_snapshot_count,
        "gameplay_ability_trigger_count": gameplay_ability_trigger_count,
        "gameplay_effect_snapshot_count": gameplay_effect_snapshot_count,
        "gameplay_effect_modifier_count": gameplay_effect_modifier_count,
        "gameplay_effect_execution_count": gameplay_effect_execution_count,
        "gameplay_effect_cue_count": gameplay_effect_cue_count,
        "gameplay_effect_overflow_count": gameplay_effect_overflow_count,
        "gameplay_effect_granted_ability_count": gameplay_effect_granted_ability_count,
        "gameplay_cue_notify_snapshot_count": gameplay_cue_notify_snapshot_count,
        "gameplay_cue_notify_actor_count": gameplay_cue_notify_actor_count,
        "gameplay_cue_notify_static_count": gameplay_cue_notify_static_count,
        "user_defined_struct_snapshot_count": user_defined_struct_snapshot_count,
        "user_defined_struct_field_count": user_defined_struct_field_count,
        "user_defined_enum_snapshot_count": user_defined_enum_snapshot_count,
        "user_defined_enum_entry_count": user_defined_enum_entry_count,
        "user_defined_enum_hidden_entry_count": user_defined_enum_hidden_entry_count,
        "user_defined_enum_max_entry_count": user_defined_enum_max_entry_count,
        "data_table_snapshot_count": data_table_snapshot_count,
        "data_table_column_count": data_table_column_count,
        "data_table_row_count": data_table_row_count,
        "data_table_field_count": data_table_field_count,
        "string_table_snapshot_count": string_table_snapshot_count,
        "string_table_entry_count": string_table_entry_count,
        "string_table_metadata_count": string_table_metadata_count,
        "curve_snapshot_count": curve_snapshot_count,
        "curve_channel_count": curve_channel_count,
        "curve_key_count": curve_key_count,
        "curve_linear_color_atlas_snapshot_count": curve_linear_color_atlas_snapshot_count,
        "curve_atlas_entry_count": curve_atlas_entry_count,
        "curve_atlas_curve_reference_count": curve_atlas_curve_reference_count,
        "skeleton_snapshot_count": skeleton_snapshot_count,
        "skeleton_bone_count": skeleton_bone_count,
        "skeletal_mesh_snapshot_count": skeletal_mesh_snapshot_count,
        "skeletal_mesh_lod_count": skeletal_mesh_lod_count,
        "animation_sequence_snapshot_count": animation_sequence_snapshot_count,
        "animation_sequence_track_count": animation_sequence_track_count,
        "animation_sequence_notify_count": animation_sequence_notify_count,
        "blend_space_snapshot_count": blend_space_snapshot_count,
        "blend_space_axis_count": blend_space_axis_count,
        "blend_space_sample_count": blend_space_sample_count,
        "blend_space_animation_reference_count": blend_space_animation_reference_count,
        "pose_asset_snapshot_count": pose_asset_snapshot_count,
        "pose_asset_pose_count": pose_asset_pose_count,
        "pose_asset_curve_count": pose_asset_curve_count,
        "pose_asset_track_count": pose_asset_track_count,
        "pose_asset_source_animation_reference_count": pose_asset_source_animation_reference_count,
        "physics_asset_snapshot_count": physics_asset_snapshot_count,
        "physics_body_count": physics_body_count,
        "physics_shape_count": physics_shape_count,
        "physics_constraint_count": physics_constraint_count,
        "anim_blueprint_snapshot_count": anim_blueprint_snapshot_count,
        "anim_state_machine_count": anim_state_machine_count,
        "anim_state_count": anim_state_count,
        "anim_transition_count": anim_transition_count,
        "anim_asset_player_count": anim_asset_player_count,
        "anim_cached_pose_count": anim_cached_pose_count,
        "anim_slot_count": anim_slot_count,
        "anim_control_rig_node_count": anim_control_rig_node_count,
        "control_rig_blueprint_snapshot_count": control_rig_blueprint_snapshot_count,
        "control_rig_vm_graph_count": control_rig_vm_graph_count,
        "control_rig_vm_node_count": control_rig_vm_node_count,
        "control_rig_vm_pin_count": control_rig_vm_pin_count,
        "control_rig_vm_link_count": control_rig_vm_link_count,
        "control_rig_hierarchy_element_count": control_rig_hierarchy_element_count,
        "ik_rig_snapshot_count": ik_rig_snapshot_count,
        "ik_rig_chain_count": ik_rig_chain_count,
        "ik_rig_goal_count": ik_rig_goal_count,
        "ik_rig_solver_count": ik_rig_solver_count,
        "ik_retargeter_snapshot_count": ik_retargeter_snapshot_count,
        "ik_retarget_chain_map_count": ik_retarget_chain_map_count,
        "ik_retarget_pose_bone_offset_count": ik_retarget_pose_bone_offset_count,
        "static_mesh_snapshot_count": static_mesh_snapshot_count,
        "static_mesh_lod_count": static_mesh_lod_count,
        "static_mesh_material_count": static_mesh_material_count,
        "texture_snapshot_count": texture_snapshot_count,
        "texture_total_pixel_count": texture_total_pixel_count,
        "texture_mip_count": texture_mip_count,
        "texture_generic_snapshot_count": texture_generic_snapshot_count,
        "texture_cube_snapshot_count": texture_cube_snapshot_count,
        "texture_cube_face_count": texture_cube_face_count,
        "sound_cue_snapshot_count": sound_cue_snapshot_count,
        "sound_cue_node_count": sound_cue_node_count,
        "sound_cue_wave_player_count": sound_cue_wave_player_count,
        "sound_cue_child_edge_count": sound_cue_child_edge_count,
        "sound_wave_snapshot_count": sound_wave_snapshot_count,
        "sound_wave_cue_point_count": sound_wave_cue_point_count,
        "sound_wave_subtitle_count": sound_wave_subtitle_count,
        "sound_wave_channel_count": sound_wave_channel_count,
        "level_sequence_snapshot_count": level_sequence_snapshot_count,
        "level_sequence_spawnable_count": level_sequence_spawnable_count,
        "level_sequence_possessable_count": level_sequence_possessable_count,
        "level_sequence_binding_count": level_sequence_binding_count,
        "level_sequence_track_count": level_sequence_track_count,
        "level_sequence_section_count": level_sequence_section_count,
        "level_sequence_channel_count": level_sequence_channel_count,
        "level_sequence_key_count": level_sequence_key_count,
        "level_sequence_binding_tag_count": level_sequence_binding_tag_count,
        "level_sequence_marked_frame_count": level_sequence_marked_frame_count,
        "level_sequence_camera_cut_section_count": level_sequence_camera_cut_section_count,
        "level_sequence_subsequence_section_count": level_sequence_subsequence_section_count,
        "level_sequence_audio_section_count": level_sequence_audio_section_count,
        "level_sequence_animation_section_count": level_sequence_animation_section_count,
        "level_sequence_control_rig_section_count": level_sequence_control_rig_section_count,
        "sound_submix_snapshot_count": sound_submix_snapshot_count,
        "sound_submix_child_count": sound_submix_child_count,
        "sound_submix_effect_count": sound_submix_effect_count,
        "sound_submix_with_parent_count": sound_submix_with_parent_count,
        "sound_submix_with_ambisonics_settings_count": sound_submix_with_ambisonics_settings_count,
        "sound_submix_effect_preset_snapshot_count": sound_submix_effect_preset_snapshot_count,
        "sound_submix_effect_setting_property_count": sound_submix_effect_setting_property_count,
        "material_snapshot_count": material_snapshot_count,
        "material_function_snapshot_count": material_function_snapshot_count,
        "material_instance_snapshot_count": material_instance_snapshot_count,
        "material_parameter_collection_snapshot_count": material_parameter_collection_snapshot_count,
        "material_collection_parameter_count": material_collection_parameter_count,
        "material_collection_scalar_parameter_count": material_collection_scalar_parameter_count,
        "material_collection_vector_parameter_count": material_collection_vector_parameter_count,
        "material_expression_snapshot_count": material_expression_snapshot_count,
        "material_expression_input_count": material_expression_input_count,
        "material_expression_output_count": material_expression_output_count,
        "material_function_reference_count": material_function_reference_count,
        "material_texture_reference_count": material_texture_reference_count,
        "material_parameter_override_count": material_parameter_override_count,
        "niagara_system_snapshot_count": niagara_system_snapshot_count,
        "niagara_emitter_snapshot_count": niagara_emitter_snapshot_count,
        "niagara_script_snapshot_count": niagara_script_snapshot_count,
        "niagara_script_ref_count": niagara_script_ref_count,
        "niagara_parameter_count": niagara_parameter_count,
        "niagara_renderer_count": niagara_renderer_count,
        "niagara_event_handler_count": niagara_event_handler_count,
        "niagara_simulation_stage_count": niagara_simulation_stage_count,
        "niagara_parameter_definitions_snapshot_count": niagara_parameter_definitions_snapshot_count,
        "niagara_parameter_definition_count": niagara_parameter_definition_count,
        "niagara_parameter_definition_static_switch_count": niagara_parameter_definition_static_switch_count,
        "niagara_parameter_definition_advanced_display_count": niagara_parameter_definition_advanced_display_count,
        "vector_field_static_snapshot_count": vector_field_static_snapshot_count,
        "vector_field_voxel_count": vector_field_voxel_count,
        "vector_field_source_data_size_bytes": vector_field_source_data_size_bytes,
        "pcg_graph_snapshot_count": pcg_graph_snapshot_count,
        "pcg_node_count": pcg_node_count,
        "pcg_pin_count": pcg_pin_count,
        "pcg_edge_count": pcg_edge_count,
        "pcg_settings_count": pcg_settings_count,
        "pcg_setting_property_count": pcg_setting_property_count,
        "pcg_subgraph_reference_count": pcg_subgraph_reference_count,
        "pcg_universal_graph_count": pcg_universal_graph_count,
        "pcg_universal_node_count": pcg_universal_node_count,
        "pcg_universal_port_count": pcg_universal_port_count,
        "pcg_universal_link_count": pcg_universal_link_count,
        "metasound_snapshot_count": metasound_snapshot_count,
        "metasound_graph_count": metasound_graph_count,
        "metasound_node_count": metasound_node_count,
        "metasound_vertex_count": metasound_vertex_count,
        "metasound_edge_count": metasound_edge_count,
        "metasound_literal_count": metasound_literal_count,
        "metasound_dependency_count": metasound_dependency_count,
        "metasound_interface_count": metasound_interface_count,
        "metasound_preset_count": metasound_preset_count,
        "metasound_universal_graph_count": metasound_universal_graph_count,
        "metasound_universal_node_count": metasound_universal_node_count,
        "metasound_universal_port_count": metasound_universal_port_count,
        "metasound_universal_link_count": metasound_universal_link_count,
    }


def compare_subset(actual: Any, expected: Any, path: str, errors: list[str]) -> None:
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            errors.append(f"{path}: expected object")
            return
        for key, expected_value in expected.items():
            if key not in actual:
                errors.append(f"{path}.{key}: missing key")
                continue
            compare_subset(actual[key], expected_value, f"{path}.{key}", errors)
        return

    if actual != expected:
        errors.append(f"{path}: expected {expected!r}, got {actual!r}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate a UEPI scan JSON artifact.")
    parser.add_argument("--scan", type=Path, required=True, help="Path to a UEPI scan JSON file.")
    parser.add_argument("--golden", type=Path, help="Optional golden summary JSON to compare as a subset.")
    parser.add_argument("--print-summary", action="store_true", help="Print the derived golden summary.")
    parser.add_argument("--max-errors", type=int, default=50)
    args = parser.parse_args(argv)

    scan = load_json(args.scan)
    validator = ScanValidator(max_errors=max(args.max_errors, 1))
    validator.validate_scan(scan)
    result = validator.result(scan)
    if isinstance(scan, dict):
        result["summary"] = scan_summary(scan)

    golden_errors: list[str] = []
    if args.golden:
        expected = load_json(args.golden)
        compare_subset(result.get("summary", {}), expected, "$.summary", golden_errors)
        result["golden_error_count"] = len(golden_errors)
        result["golden_errors"] = golden_errors
        result["ok"] = result["ok"] and not golden_errors

    if not args.print_summary:
        result.pop("summary", None)

    print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())

