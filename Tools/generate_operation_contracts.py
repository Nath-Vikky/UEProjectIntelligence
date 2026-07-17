from __future__ import annotations

import json
import hashlib
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "Schemas" / "edit-operation-contracts.json"


def ref_or_path(description: str) -> dict[str, Any]:
    return {
        "description": description,
        "oneOf": [
            {"type": "string", "minLength": 1},
            {
                "type": "object",
                "properties": {"$ref": {"type": "string", "pattern": r"^[A-Za-z0-9_.:-]+(?:#[A-Za-z0-9_.:-]+)?$"}},
                "required": ["$ref"],
                "additionalProperties": False,
            },
        ],
    }


TYPED_VALUE: dict[str, Any] = {
    "description": "A JSON scalar/container or an explicit UEPI typed value object.",
    "oneOf": [
        {"type": ["null", "boolean", "number", "string", "array"]},
        {"type": "object"},
    ],
}
VECTOR2 = {
    "type": "object",
    "properties": {"x": {"type": "number"}, "y": {"type": "number"}},
    "required": ["x", "y"],
    "additionalProperties": False,
}
VECTOR3 = {
    "type": "object",
    "properties": {"x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"}},
    "required": ["x", "y", "z"],
    "additionalProperties": False,
}
ROTATOR = {
    "type": "object",
    "properties": {"pitch": {"type": "number"}, "yaw": {"type": "number"}, "roll": {"type": "number"}},
    "required": ["pitch", "yaw", "roll"],
    "additionalProperties": False,
}
ENDPOINT = {
    "type": "object",
    "properties": {
        "node_guid": {"type": "string"},
        "node_id": {"type": "string"},
        "node_ref": {"type": "string"},
        "ref": {"type": "string"},
        "pin_name": {"type": "string"},
        "pin_id": {"type": "string"},
        "pin": {"type": "string"},
        "direction": {"type": "string", "enum": ["input", "output", "in", "out"]},
    },
    "anyOf": [
        {"required": ["node_guid"]},
        {"required": ["node_id"]},
        {"required": ["node_ref"]},
        {"required": ["ref"]},
    ],
    "oneOf": [
        {"required": ["pin_name"]},
        {"required": ["pin_id"]},
        {"required": ["pin"]},
    ],
    "additionalProperties": False,
}
WRITE = {
    "type": "object",
    "properties": {
        "path": {"type": "string", "minLength": 1},
        "mode": {"type": "string", "enum": ["replace", "append", "insert", "remove", "clear", "add", "set"]},
        "value": TYPED_VALUE,
    },
    "required": ["path", "value"],
    "additionalProperties": False,
}


PATH_FIELDS = {
    "asset", "source", "sequence", "skeleton", "preview_mesh", "material", "texture", "parent",
    "context", "action", "destination_asset", "level", "actor",
}
STRING_FIELDS = {
    "name", "asset_name", "destination_path", "folder", "path", "file", "filename", "asset_class",
    "class_path", "actor_class", "class", "label", "graph", "graph_name", "pin_type", "type_name",
    "component", "component_name", "component_class", "property", "property_path", "function_name",
    "function_path", "function_class", "function", "event_name", "event_kind", "variable", "message",
    "comment", "node_guid", "node_id", "node_ref", "slot_name", "group_name", "section_name", "key",
    "value_type", "widget_class", "kind", "parent_widget", "widget_name", "button", "button_name",
    "delegate", "event", "parameter", "parameter_name",
}
NUMBER_FIELDS = {
    "x", "y", "node_pos_x", "node_pos_y", "material_index", "start_position_ms", "start_position",
    "anim_start_time", "anim_end_time", "play_rate", "loop_count", "start_time", "blend_in_time",
    "blend_out_time", "blend_out_trigger_time", "z_order",
}
BOOL_FIELDS = {"replace_existing", "auto_size", "is_looping", "consume_input", "execute_when_paused"}


DOMAIN_FIELDS = {
    "blueprint": [
        "asset", "graph", "graph_name", "name", "pin_type", "type_name", "value", "component",
        "component_name", "component_class", "property", "property_path", "properties", "writes",
        "function_name", "function_path", "function_class", "function", "event_name", "event_kind",
        "variable", "message", "kind", "struct_path", "key", "source", "target", "node_guid",
        "node_id", "node_ref", "pin_name", "pin_id", "position", "x", "y", "node_pos_x",
        "node_pos_y", "defaults", "comment", "slot_name", "ref", "depends_on",
    ],
    "animgraph": [
        "asset", "graph", "graph_name", "slot_name", "source", "target", "node_guid", "node_id",
        "node_ref", "property", "property_path", "value", "position", "x", "y", "ref", "depends_on",
    ],
    "animation": [
        "asset", "skeleton", "sequence", "preview_mesh", "destination_asset", "destination_path", "folder",
        "name", "asset_name", "group_name", "slot_name", "section_name", "start_position_ms",
        "start_position", "anim_start_time", "anim_end_time", "play_rate", "loop_count", "start_time",
        "blend_in_time", "blend_out_time", "blend_out_trigger_time", "ref", "depends_on",
    ],
    "content": [
        "asset", "source", "destination_asset", "destination_path", "folder", "path", "name", "asset_name",
        "asset_class", "class_path", "file", "filename", "replace_existing", "assets", "ref", "depends_on",
    ],
    "asset": ["asset", "properties", "writes", "ref", "depends_on"],
    "actor": [
        "actor", "path", "targets", "actor_class", "class", "asset", "level", "name", "label",
        "transform", "location", "rotation", "scale", "property", "properties", "writes", "value",
        "set", "operation", "ref", "depends_on",
    ],
    "material": [
        "asset", "parent", "destination_asset", "destination_path", "folder", "name", "asset_name", "parameter",
        "parameter_name", "value", "texture", "actor", "targets", "component", "material", "material_index",
        "ref", "depends_on",
    ],
    "umg": [
        "asset", "destination_asset", "destination_path", "folder", "name", "asset_name", "parent",
        "parent_widget", "widget_name", "widget_class", "kind", "text", "label", "anchors", "offsets",
        "auto_size", "z_order", "button", "button_name", "delegate", "event", "ref", "depends_on",
    ],
    "input": [
        "asset", "context", "action", "destination_asset", "destination_path", "folder", "name", "asset_name",
        "value_type", "key", "ref", "depends_on",
    ],
}


def field_schema(name: str) -> dict[str, Any]:
    if name in PATH_FIELDS:
        return ref_or_path(f"Exact Unreal object path or a same-plan reference for {name}.")
    if name == "struct_path":
        return {"type": "string", "pattern": r"^/Script/"}
    if name in STRING_FIELDS or name in {"pin_name", "pin_id", "text"}:
        return {"type": "string"}
    if name in NUMBER_FIELDS:
        return {"type": "number"}
    if name in BOOL_FIELDS:
        return {"type": "boolean"}
    if name == "position":
        return VECTOR2
    if name in {"location", "scale"}:
        return VECTOR3
    if name == "rotation":
        return ROTATOR
    if name in {"source", "target"}:
        return ENDPOINT
    if name == "writes":
        return {"type": "array", "items": WRITE, "minItems": 1}
    if name == "assets":
        return {"type": "array", "items": ref_or_path("Exact Unreal object path or a same-plan reference."), "minItems": 1}
    if name == "depends_on":
        return {"type": "array", "items": {"type": "string"}, "uniqueItems": True}
    if name == "targets":
        return {
            "type": "object",
            "properties": {"paths": {"type": "array", "items": {"type": "string"}, "minItems": 1}},
            "required": ["paths"],
            "additionalProperties": False,
        }
    if name == "transform":
        return {
            "type": "object",
            "properties": {"location": VECTOR3, "rotation": ROTATOR, "scale": VECTOR3},
            "additionalProperties": False,
        }
    if name in {"properties", "defaults", "set", "operation"}:
        return {"type": "object", "additionalProperties": TYPED_VALUE}
    if name == "anchors":
        return {
            "type": "object",
            "properties": {"minimum": VECTOR2, "maximum": VECTOR2, "min": VECTOR2, "max": VECTOR2},
            "additionalProperties": False,
        }
    if name == "offsets":
        return {
            "type": "object",
            "properties": {key: {"type": "number"} for key in ("left", "top", "right", "bottom")},
            "required": ["left", "top", "right", "bottom"],
            "additionalProperties": False,
        }
    if name == "value":
        return TYPED_VALUE
    if name == "ref":
        return {"type": "string", "pattern": r"^[A-Za-z0-9_.:-]+$"}
    return {}


def op(domain: str, required: list[str], example: dict[str, Any]) -> dict[str, Any]:
    properties = {name: field_schema(name) for name in DOMAIN_FIELDS[domain]}
    if domain in {"blueprint", "animgraph"}:
        for endpoint in ("source", "target"):
            if endpoint in properties:
                properties[endpoint] = ENDPOINT
    schema: dict[str, Any] = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "type": "object",
        "properties": properties,
        "required": required,
        "additionalProperties": False,
    }
    return {"input_schema": schema, "examples": [example]}


BP = "/Game/Blueprints/BP_UEPIProbe.BP_UEPIProbe"
ANIM_BP = "/Game/Characters/ABP_UEPIProbe.ABP_UEPIProbe"
MONTAGE = "/Game/Animations/AM_UEPIProbe.AM_UEPIProbe"
SEQUENCE = "/Game/Animations/A_UEPIProbe.A_UEPIProbe"
SKELETON = "/Game/Characters/SK_UEPIProbe_Skeleton.SK_UEPIProbe_Skeleton"
ACTOR = "ThirdPersonMap:PersistentLevel.UEPIProbe"


CONTRACTS: dict[str, dict[str, Any]] = {
    "blueprint.add_variable": op("blueprint", ["asset", "name"], {"asset": BP, "name": "UEPICounter", "pin_type": "int"}),
    "blueprint.set_variable_default": op("blueprint", ["asset", "name", "value"], {"asset": BP, "name": "UEPICounter", "value": 10}),
    "blueprint.add_component": op("blueprint", ["asset", "name", "component_class"], {"asset": BP, "name": "UEPIProbeComponent", "component_class": "/Script/Engine.SceneComponent"}),
    "blueprint.set_component_property": op("blueprint", ["asset", "component", "property", "value"], {"asset": BP, "component": "UEPIProbeComponent", "property": "Mobility", "value": "Movable"}),
    "blueprint.set_component_properties": op("blueprint", ["asset", "component", "writes"], {"asset": BP, "component": "UEPIProbeComponent", "writes": [{"path": "ComponentTags", "mode": "append", "value": "UEPI"}]}),
    "blueprint.create_function": op("blueprint", ["asset", "name"], {"asset": BP, "name": "UEPIProbeFunction"}),
    "blueprint.add_event_node": op("blueprint", ["asset", "graph", "name"], {"asset": BP, "graph": "EventGraph", "name": "UEPIProbeEvent", "ref": "probe.event"}),
    "blueprint.add_function_call_node": op("blueprint", ["asset", "graph", "function_path"], {"asset": BP, "graph": "EventGraph", "function_path": "/Script/Engine.KismetSystemLibrary:PrintString", "ref": "probe.call"}),
    "blueprint.add_variable_get_node": op("blueprint", ["asset", "graph", "variable"], {"asset": BP, "graph": "EventGraph", "variable": "UEPICounter", "ref": "probe.get"}),
    "blueprint.add_variable_set_node": op("blueprint", ["asset", "graph", "variable"], {"asset": BP, "graph": "EventGraph", "variable": "UEPICounter", "ref": "probe.set"}),
    "blueprint.add_branch_node": op("blueprint", ["asset", "graph"], {"asset": BP, "graph": "EventGraph", "ref": "probe.branch"}),
    "blueprint.add_print_string_node": op("blueprint", ["asset", "graph", "message"], {"asset": BP, "graph": "EventGraph", "message": "UEPI probe", "ref": "probe.print"}),
    "blueprint.connect_pins": op("blueprint", ["asset", "graph", "source", "target"], {"asset": BP, "graph": "EventGraph", "source": {"node_ref": "probe.event", "pin_name": "then"}, "target": {"node_ref": "probe.print", "pin_name": "execute"}, "depends_on": ["probe.event", "probe.print"]}),
    "blueprint.compile": op("blueprint", ["asset"], {"asset": BP}),
    "blueprint.add_node": op("blueprint", ["asset", "graph", "kind"], {"asset": BP, "graph": "EventGraph", "kind": "input_key", "key": "One", "ref": "probe.input"}),
    "blueprint.set_pin_default": op("blueprint", ["asset", "graph", "target", "value"], {"asset": BP, "graph": "EventGraph", "target": {"node_ref": "probe.print", "pin_name": "Duration"}, "value": 2.0}),
    "blueprint.disconnect_pins": op("blueprint", ["asset", "graph", "target"], {"asset": BP, "graph": "EventGraph", "target": {"node_ref": "probe.print", "pin_name": "execute"}}),
    "blueprint.break_all_links": op("blueprint", ["asset", "graph", "node_guid"], {"asset": BP, "graph": "EventGraph", "node_guid": "00000000-0000-0000-0000-000000000000"}),
    "blueprint.remove_node": op("blueprint", ["asset", "graph", "node_guid"], {"asset": BP, "graph": "EventGraph", "node_guid": "00000000-0000-0000-0000-000000000000"}),
    "blueprint.move_node": op("blueprint", ["asset", "graph", "node_guid", "position"], {"asset": BP, "graph": "EventGraph", "node_guid": "00000000-0000-0000-0000-000000000000", "position": {"x": 600, "y": 100}}),
    "blueprint.set_node_comment": op("blueprint", ["asset", "graph", "node_guid", "comment"], {"asset": BP, "graph": "EventGraph", "node_guid": "00000000-0000-0000-0000-000000000000", "comment": "UEPI probe"}),
    "animgraph.add_slot_node": op("animgraph", ["asset", "graph", "slot_name"], {"asset": ANIM_BP, "graph": "AnimGraph", "slot_name": "DefaultGroup.DefaultSlot", "ref": "probe.slot"}),
    "animgraph.add_slot": op("animgraph", ["asset", "graph", "slot_name"], {"asset": ANIM_BP, "graph": "AnimGraph", "slot_name": "DefaultGroup.DefaultSlot", "ref": "probe.slot"}),
    "animgraph.set_node_property": op("animgraph", ["asset", "graph", "node_ref", "property_path", "value"], {"asset": ANIM_BP, "graph": "AnimGraph", "node_ref": "probe.slot", "property_path": "Node.SlotName", "value": "DefaultGroup.DefaultSlot"}),
    "animgraph.connect_pose": op("animgraph", ["asset", "graph", "source", "target"], {"asset": ANIM_BP, "graph": "AnimGraph", "source": {"node_ref": "probe.slot", "pin_name": "Pose"}, "target": {"node_guid": "00000000-0000-0000-0000-000000000000", "pin_name": "Result"}}),
    "animgraph.connect_pose_pins": op("animgraph", ["asset", "graph", "source", "target"], {"asset": ANIM_BP, "graph": "AnimGraph", "source": {"node_ref": "probe.slot", "pin_name": "Pose"}, "target": {"node_guid": "00000000-0000-0000-0000-000000000000", "pin_name": "Result"}}),
    "animgraph.disconnect_pose_pins": op("animgraph", ["asset", "graph", "target"], {"asset": ANIM_BP, "graph": "AnimGraph", "target": {"node_ref": "probe.slot", "pin_name": "Source"}}),
    "animgraph.remove_node": op("animgraph", ["asset", "graph", "node_guid"], {"asset": ANIM_BP, "graph": "AnimGraph", "node_guid": "00000000-0000-0000-0000-000000000000"}),
    "animgraph.compile": op("animgraph", ["asset"], {"asset": ANIM_BP}),
    "animation.register_slot": op("animation", ["skeleton", "group_name", "slot_name"], {"skeleton": SKELETON, "group_name": "DefaultGroup", "slot_name": "UEPIProbeSlot"}),
    "animation.create_slot_group": op("animation", ["skeleton", "group_name", "slot_name"], {"skeleton": SKELETON, "group_name": "DefaultGroup", "slot_name": "UEPIProbeSlot"}),
    "animation.create_montage_from_sequence": op("animation", ["sequence", "destination_asset", "slot_name"], {"sequence": SEQUENCE, "destination_asset": MONTAGE, "slot_name": "DefaultSlot", "ref": "probe.montage"}),
    "animation.add_montage_slot_track": op("animation", ["asset", "slot_name"], {"asset": MONTAGE, "slot_name": "DefaultSlot"}),
    "animation.add_montage_segment": op("animation", ["asset", "sequence", "slot_name"], {"asset": MONTAGE, "sequence": SEQUENCE, "slot_name": "DefaultSlot", "play_rate": 1.0, "loop_count": 1}),
    "animation.add_montage_section": op("animation", ["asset", "section_name"], {"asset": MONTAGE, "section_name": "UEPIProbe", "start_time": 0.0}),
    "animation.set_montage_blend": op("animation", ["asset"], {"asset": MONTAGE, "blend_in_time": 0.2, "blend_out_time": 0.2}),
    "animation.set_preview_mesh": op("animation", ["asset", "preview_mesh"], {"asset": MONTAGE, "preview_mesh": "/Game/Characters/SKM_UEPIProbe.SKM_UEPIProbe"}),
    "content.save_assets": op("content", ["assets"], {"assets": [BP]}),
    "content.create_folder": op("content", ["path"], {"path": "/Game/UEPIProbe"}),
    "content.import": op("content", ["file", "destination_path"], {"file": "C:/UEPIProbe/probe.png", "destination_path": "/Game/UEPIProbe", "name": "T_UEPIProbe"}),
    "content.duplicate_asset": op("content", ["source", "destination_asset"], {"source": BP, "destination_asset": "/Game/UEPIProbe/BP_UEPIProbe_Copy.BP_UEPIProbe_Copy"}),
    "content.rename_asset": op("content", ["asset", "destination_asset"], {"asset": BP, "destination_asset": "/Game/UEPIProbe/BP_UEPIProbe_Renamed.BP_UEPIProbe_Renamed"}),
    "content.create_asset": op("content", ["destination_asset", "asset_class"], {"destination_asset": "/Game/UEPIProbe/DA_UEPIProbe.DA_UEPIProbe", "asset_class": "/Script/Engine.PrimaryDataAsset", "ref": "probe.asset"}),
    "asset.set_properties": op("asset", ["asset", "writes"], {"asset": {"$ref": "probe.asset#asset"}, "writes": [{"path": "AssetBundleData", "mode": "replace", "value": {}}], "depends_on": ["probe.asset"]}),
    "actor.spawn": op("actor", ["actor_class"], {"actor_class": "/Script/Engine.StaticMeshActor", "label": "UEPIProbe", "location": {"x": 0, "y": 0, "z": 120}}),
    "actor.set_transform": op("actor", ["actor", "transform"], {"actor": ACTOR, "transform": {"location": {"x": 0, "y": 0, "z": 200}, "rotation": {"pitch": 0, "yaw": 0, "roll": 0}, "scale": {"x": 1, "y": 1, "z": 1}}}),
    "actor.set_property": op("actor", ["actor", "property", "value"], {"actor": ACTOR, "property": "Tags", "value": ["UEPI"]}),
    "actor.set_properties": op("actor", ["actor", "writes"], {"actor": ACTOR, "writes": [{"path": "Tags", "mode": "append", "value": "UEPI"}]}),
    "material.create_instance": op("material", ["parent", "destination_asset"], {"parent": "/Game/Materials/M_Base.M_Base", "destination_asset": "/Game/UEPIProbe/MI_UEPIProbe.MI_UEPIProbe", "ref": "probe.material"}),
    "material.set_scalar_parameter": op("material", ["asset", "parameter", "value"], {"asset": {"$ref": "probe.material#asset"}, "parameter": "Roughness", "value": 0.5}),
    "material.set_vector_parameter": op("material", ["asset", "parameter", "value"], {"asset": {"$ref": "probe.material#asset"}, "parameter": "Color", "value": {"r": 1, "g": 0, "b": 0, "a": 1}}),
    "material.set_texture_parameter": op("material", ["asset", "parameter", "texture"], {"asset": {"$ref": "probe.material#asset"}, "parameter": "BaseColor", "texture": "/Game/Textures/T_UEPIProbe.T_UEPIProbe"}),
    "material.apply_to_actor": op("material", ["material", "actor", "component"], {"material": "/Game/UEPIProbe/MI_UEPIProbe.MI_UEPIProbe", "actor": ACTOR, "component": "StaticMeshComponent", "material_index": 0}),
    "material.apply_to_blueprint_component": op("material", ["material", "asset", "component"], {"material": "/Game/UEPIProbe/MI_UEPIProbe.MI_UEPIProbe", "asset": BP, "component": "StaticMesh", "material_index": 0}),
    "widget.create": op("umg", ["destination_asset"], {"destination_asset": "/Game/UEPIProbe/WBP_UEPIProbe.WBP_UEPIProbe", "ref": "probe.widget"}),
    "widget.add_text": op("umg", ["asset", "name", "text"], {"asset": {"$ref": "probe.widget#asset"}, "name": "ProbeText", "text": "UEPI", "depends_on": ["probe.widget"]}),
    "widget.add_button": op("umg", ["asset", "name"], {"asset": {"$ref": "probe.widget#asset"}, "name": "ProbeButton", "text": "Run"}),
    "widget.add_widget": op("umg", ["asset", "name", "widget_class"], {"asset": {"$ref": "probe.widget#asset"}, "name": "ProbeText", "widget_class": "TextBlock", "text": "UEPI"}),
    "widget.set_slot": op("umg", ["asset", "widget_name"], {"asset": {"$ref": "probe.widget#asset"}, "widget_name": "ProbeButton", "offsets": {"left": 20, "top": 20, "right": 180, "bottom": 48}}),
    "widget.bind_button_to_custom_event": op("umg", ["asset", "button"], {"asset": {"$ref": "probe.widget#asset"}, "button": "ProbeButton", "delegate": "OnClicked"}),
    "input.create_action": op("input", ["destination_asset", "value_type"], {"destination_asset": "/Game/UEPIProbe/IA_UEPIProbe.IA_UEPIProbe", "value_type": "Axis1D", "ref": "probe.action"}),
    "input.create_mapping_context": op("input", ["destination_asset"], {"destination_asset": "/Game/UEPIProbe/IMC_UEPIProbe.IMC_UEPIProbe", "ref": "probe.context"}),
    "input.add_key_mapping": op("input", ["context", "action", "key"], {"context": {"$ref": "probe.context#asset"}, "action": {"$ref": "probe.action#asset"}, "key": "One"}),
    "input.remove_key_mapping": op("input", ["context", "action", "key"], {"context": "/Game/UEPIProbe/IMC_UEPIProbe.IMC_UEPIProbe", "action": "/Game/UEPIProbe/IA_UEPIProbe.IA_UEPIProbe", "key": "One"}),
}


def main() -> int:
    add_node = CONTRACTS["blueprint.add_node"]["input_schema"]
    add_node["properties"]["kind"]["enum"] = [
        "custom_event", "function_call", "variable_get", "variable_set", "branch", "print_string", "make_struct", "input_key",
    ]
    add_node["allOf"] = [
        {"if": {"properties": {"kind": {"const": "make_struct"}}}, "then": {"required": ["struct_path"]}},
        {"if": {"properties": {"kind": {"const": "input_key"}}}, "then": {"required": ["key"]}},
        {"if": {"properties": {"kind": {"const": "custom_event"}}}, "then": {"required": ["name"]}},
        {"if": {"properties": {"kind": {"const": "function_call"}}}, "then": {"required": ["function_path"]}},
        {"if": {"properties": {"kind": {"enum": ["variable_get", "variable_set"]}}}, "then": {"required": ["variable"]}},
    ]
    for contract in CONTRACTS.values():
        canonical = json.dumps(
            {"input_schema": contract["input_schema"], "examples": contract["examples"]},
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        contract["contract_hash"] = "sha256:" + hashlib.sha256(canonical).hexdigest()
    payload = {"schema_version": "uepi.edit-operation-contracts.v1", "operations": CONTRACTS}
    OUTPUT.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(CONTRACTS)} operation contracts to {OUTPUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
