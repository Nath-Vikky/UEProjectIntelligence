from __future__ import annotations

from collections import Counter
from typing import Any


CALL_RELATIONS = {
    "calls_function",
    "calls_macro",
    "calls_composite",
    "starts_async_action",
    "starts_latent_operation",
    "sends_interface_message",
    "broadcasts_delegate",
    "binds_delegate",
}

DATA_RELATIONS = {"reads_variable", "writes_variable"}
CLASS_RELATIONS = {"casts_to", "spawns_class", "loads_asset", "class_references"}
FLOW_RELATIONS = {"exec_flows_to", "delegate_flows_to", "data_flows_to"}


def _as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def _title(entity: dict[str, Any]) -> str:
    attributes = _as_dict(entity.get("attributes"))
    return str(
        attributes.get("node_title")
        or attributes.get("function_name")
        or attributes.get("variable_name")
        or entity.get("display_name")
        or entity.get("canonical_key")
        or entity.get("id")
        or ""
    )


def _node_semantic_kind(entity: dict[str, Any]) -> str:
    attributes = _as_dict(entity.get("attributes"))
    snapshot = _as_dict(entity.get("snapshot"))
    semantic = _as_dict(snapshot.get("node_semantics"))
    return str(attributes.get("semantic_kind") or semantic.get("semantic_kind") or "")


def _edge(relation: dict[str, Any], by_id: dict[str, dict[str, Any]]) -> dict[str, Any]:
    source = by_id.get(str(relation.get("from_id") or ""), {})
    target = by_id.get(str(relation.get("to_id") or ""), {})
    return {
        "relation_id": relation.get("id"),
        "type": relation.get("type"),
        "from_id": relation.get("from_id"),
        "from": _title(source),
        "to_id": relation.get("to_id"),
        "to": _title(target),
        "confidence": relation.get("confidence"),
        "attributes": _as_dict(relation.get("attributes")),
        "evidence": relation.get("evidence") or [],
    }


def summarize_blueprint_semantics(
    asset: dict[str, Any],
    blueprint_entities: list[dict[str, Any]],
    relations: list[dict[str, Any]],
    *,
    limit: int = 50,
) -> dict[str, Any]:
    by_id = {str(entity.get("id") or ""): entity for entity in blueprint_entities if entity.get("id")}
    node_entities = [entity for entity in blueprint_entities if entity.get("kind") in {"blueprint_node", "blueprint_event"}]
    kind_counts = Counter(_node_semantic_kind(entity) or str(entity.get("kind") or "unknown") for entity in node_entities)
    relation_counts = Counter(str(relation.get("type") or "unknown") for relation in relations)

    entrypoints: list[dict[str, Any]] = []
    for entity in node_entities:
        title = _title(entity)
        semantic_kind = _node_semantic_kind(entity)
        lowered = f"{title} {semantic_kind}".casefold()
        if "event" in lowered or "beginplay" in lowered or "construction" in lowered or "tick" in lowered:
            entrypoints.append(
                {
                    "id": entity.get("id"),
                    "title": title,
                    "semantic_kind": semantic_kind or "event",
                    "canonical_key": entity.get("canonical_key"),
                }
            )
        if len(entrypoints) >= 20:
            break

    call_graph = [_edge(relation, by_id) for relation in relations if relation.get("type") in CALL_RELATIONS][:limit]
    flow_edges = [_edge(relation, by_id) for relation in relations if relation.get("type") in FLOW_RELATIONS][:limit]
    reads = [_edge(relation, by_id) for relation in relations if relation.get("type") == "reads_variable"][:limit]
    writes = [_edge(relation, by_id) for relation in relations if relation.get("type") == "writes_variable"][:limit]
    class_interactions = [_edge(relation, by_id) for relation in relations if relation.get("type") in CLASS_RELATIONS][:limit]
    call_nodes = [entity for entity in node_entities if _node_semantic_kind(entity) in {"call_function", "macro", "composite", "async_action"}]
    class_reference_nodes = [
        entity
        for entity in node_entities
        if _node_semantic_kind(entity) in {"dynamic_cast", "spawn_actor", "load_asset"}
        or bool(_as_dict(entity.get("attributes")).get("semantic_owner_class"))
    ]
    call_graph_explanation = (
        "indexed_call_relations_present"
        if call_graph
        else ("call_nodes_present_but_target_relations_missing_from_this_snapshot_scope" if call_nodes else "no_supported_call_nodes_in_this_snapshot_scope")
    )
    class_interactions_explanation = (
        "indexed_class_relations_present"
        if class_interactions
        else ("class_reference_nodes_present_but_target_relations_missing_from_this_snapshot_scope" if class_reference_nodes else "no_supported_class_reference_nodes_in_this_snapshot_scope")
    )

    side_effects = []
    for edge in call_graph + class_interactions + writes:
        relation_type = str(edge.get("type") or "")
        if relation_type in {"spawns_class", "loads_asset", "starts_async_action", "starts_latent_operation", "writes_variable"}:
            side_effects.append(edge)

    summary_lines = [
        f"Blueprint {asset.get('display_name') or asset.get('canonical_key')} has {len(node_entities)} indexed nodes and {len(relations)} in-graph relations.",
    ]
    if entrypoints:
        summary_lines.append("Primary entrypoints: " + ", ".join(item["title"] for item in entrypoints[:6] if item.get("title")))
    if call_graph:
        summary_lines.append("Calls/functions: " + ", ".join(edge["to"] for edge in call_graph[:8] if edge.get("to")))
    if writes:
        summary_lines.append("Writes variables: " + ", ".join(edge["to"] for edge in writes[:8] if edge.get("to")))
    if class_interactions:
        summary_lines.append("External class/assets: " + ", ".join(edge["to"] for edge in class_interactions[:8] if edge.get("to")))
    if flow_edges:
        summary_lines.append(f"Static flow graph includes {relation_counts.get('exec_flows_to', 0)} exec edges and {relation_counts.get('data_flows_to', 0)} data edges.")

    return {
        "schema_version": "uepi.blueprint-semantic-summary.v1",
        "summary_lines": summary_lines[:30],
        "node_semantic_counts": dict(kind_counts),
        "relation_counts": dict(relation_counts),
        "entrypoints": entrypoints,
        "call_graph": call_graph,
        "call_graph_explanation": call_graph_explanation,
        "data_mutations": {"reads": reads, "writes": writes},
        "class_interactions": class_interactions,
        "class_interactions_explanation": class_interactions_explanation,
        "side_effects": side_effects[:limit],
        "flow_edges": flow_edges,
        "confidence_basis": "snapshot_exact_relations_with_python_semantic_grouping",
        "uncertainties": [
            "Static Snapshot relations do not prove runtime branch conditions, latent timing, or object validity.",
            "Use uepi_blueprint_trace for a focused path from a specific entrypoint or node.",
        ],
    }
