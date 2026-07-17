from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from typing import Any

from .context_pack import ContextPack
from .context_ranker import route_score_from_keywords


def _as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def _as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def _short_entity(entity: dict[str, Any]) -> dict[str, Any]:
    return {
        "id": entity.get("id"),
        "kind": entity.get("kind"),
        "canonical_key": entity.get("canonical_key"),
        "display_name": entity.get("display_name"),
        "source_layer": entity.get("source_layer"),
        "attributes": _as_dict(entity.get("attributes")),
        "typed_attributes": _as_dict(entity.get("typed_attributes")),
        "completeness": _as_dict(entity.get("completeness")),
        "diagnostics": _as_list(entity.get("diagnostics")),
        "evidence": _as_list(entity.get("evidence")),
    }


def _relation_summary(relation: dict[str, Any]) -> dict[str, Any]:
    return {
        "id": relation.get("id"),
        "type": relation.get("type"),
        "from_id": relation.get("from_id"),
        "to_id": relation.get("to_id"),
        "source_layer": relation.get("source_layer"),
        "derived": bool(relation.get("derived")),
        "confidence": relation.get("confidence"),
        "attributes": _as_dict(relation.get("attributes")),
        "typed_attributes": _as_dict(relation.get("typed_attributes")),
        "evidence": _as_list(relation.get("evidence")),
    }


def _entity_text(entity: dict[str, Any]) -> str:
    return " ".join(
        [
            str(entity.get("id") or ""),
            str(entity.get("kind") or ""),
            str(entity.get("canonical_key") or ""),
            str(entity.get("display_name") or ""),
            str(_as_dict(entity.get("attributes"))),
            str(_as_dict(entity.get("typed_attributes"))),
        ]
    ).casefold()


def _normalized_asset_identity(value: Any) -> str:
    text = str(value or "").strip().replace("\\", "/")
    if not text.startswith("/"):
        return text.casefold()
    suffix = ""
    if "::" in text:
        text, suffix = text.split("::", 1)
    if "." not in text.rsplit("/", 1)[-1]:
        asset_name = text.rsplit("/", 1)[-1]
        text = f"{text}.{asset_name}"
    package, object_name = text.rsplit(".", 1)
    if object_name.endswith("_C"):
        object_name = object_name[:-2]
    normalized = f"{package}.{object_name}"
    return (f"{normalized}::{suffix}" if suffix else normalized).casefold()


def _in_hard_scope(entity: dict[str, Any], hard_scope: list[str]) -> bool:
    if not hard_scope:
        return True
    key = _normalized_asset_identity(entity.get("canonical_key"))
    attributes = _as_dict(entity.get("attributes"))
    owner_values = [
        key,
        _normalized_asset_identity(attributes.get("object_path")),
        _normalized_asset_identity(attributes.get("asset_path")),
        _normalized_asset_identity(attributes.get("package_name")),
    ]
    for scope in hard_scope:
        folded = _normalized_asset_identity(scope)
        if any(value == folded or value.startswith(folded + "::") for value in owner_values if value):
            return True
    return False


def _search(engine: Any, query: str, terms: list[str], limit: int, kinds: set[str] | None = None, hard_scope: list[str] | None = None) -> tuple[list[dict[str, Any]], str]:
    seen: set[str] = set()
    matches: list[dict[str, Any]] = []

    def add(entity: dict[str, Any]) -> None:
        entity_id = str(entity.get("id") or "")
        if not entity_id or entity_id in seen:
            return
        if not _in_hard_scope(entity, hard_scope or []):
            return
        if kinds and str(entity.get("kind") or "") not in kinds:
            text = _entity_text(entity)
            if not any(kind in text for kind in kinds):
                return
        seen.add(entity_id)
        matches.append(entity)

    if hard_scope:
        engine._ensure_loaded()
        for entity in engine.entities:
            if len(matches) >= limit:
                break
            if _in_hard_scope(entity, hard_scope):
                add(entity)
        return matches[:limit], "snapshot_fragments"

    if engine.cache:
        for entity in engine.cache.search_entities(query, limit=limit, include_snapshot=False):
            add(entity)
        for term in terms[:10]:
            if len(matches) >= limit:
                break
            for entity in engine.cache.search_entities(term, limit=limit, include_snapshot=False):
                add(entity)
        return matches[:limit], "sqlite_cache"

    engine._ensure_loaded()
    haystack = engine.entities
    for entity in haystack:
        if len(matches) >= limit:
            break
        text = _entity_text(entity)
        if query.casefold() in text or any(term in text for term in terms[:10]):
            add(entity)
    return matches[:limit], "snapshot_fragments"


def _relations_for(engine: Any, matches: list[dict[str, Any]], limit: int, relation_types: set[str] | None = None, hard_scope: list[str] | None = None, include_external_endpoints: bool = False) -> list[dict[str, Any]]:
    related: list[dict[str, Any]] = []
    seen: set[str] = set()
    for entity in matches:
        entity_id = str(entity.get("id") or "")
        if not entity_id:
            continue
        relations = engine.cache.relations_for_entity(entity_id, limit=50) if engine.cache else (engine.outgoing.get(entity_id, []) + engine.incoming.get(entity_id, []))[:50]
        for relation in relations:
            if relation_types and relation.get("type") not in relation_types:
                continue
            if hard_scope and not include_external_endpoints:
                engine._ensure_loaded()
                from_entity = engine.entity_by_id.get(str(relation.get("from_id") or ""), {})
                to_entity = engine.entity_by_id.get(str(relation.get("to_id") or ""), {})
                if not (_in_hard_scope(from_entity, hard_scope) and _in_hard_scope(to_entity, hard_scope)):
                    continue
            relation_id = str(relation.get("id") or "")
            if relation_id in seen:
                continue
            seen.add(relation_id)
            related.append(_relation_summary(relation))
            if len(related) >= limit:
                return related
    return related


def _first_asset(matches: list[dict[str, Any]]) -> str | None:
    for entity in matches:
        if entity.get("kind") == "asset":
            return str(entity.get("canonical_key") or entity.get("display_name") or entity.get("id") or "")
    for entity in matches:
        value = str(entity.get("canonical_key") or entity.get("display_name") or entity.get("id") or "")
        if value:
            return value
    return None


def _best_domain_asset(matches: list[dict[str, Any]], question: str, preferred_kinds: set[str]) -> str | None:
    question_folded = question.casefold()
    candidates: dict[str, tuple[int, str]] = {}
    for entity in matches:
        canonical = str(entity.get("canonical_key") or "").split("::", 1)[0]
        if not canonical:
            continue
        kind = str(entity.get("kind") or "")
        display = str(entity.get("display_name") or "")
        attributes = _as_dict(entity.get("attributes"))
        score = 0
        if kind in preferred_kinds:
            score += 60
        if kind == "asset":
            score += 10
        if display and display.casefold() in question_folded:
            score += 100
        asset_class = " ".join(str(attributes.get(key) or "") for key in ("asset_class", "class", "class_path")).casefold()
        if any(token in asset_class for token in ("animsequence", "animmontage", "blendspace", "animblueprint")):
            score += 50
        previous = candidates.get(canonical)
        if previous is None or score > previous[0]:
            candidates[canonical] = (score, canonical)
    return max(candidates.values(), default=(0, ""))[1] or None


@dataclass
class KeywordRoute:
    name: str
    priority: int
    keywords: set[str]
    interpretation: str
    kinds: set[str] | None = None
    relation_types: set[str] | None = None

    def match(self, question: str, terms: list[str], project_summary: dict[str, Any]) -> float:
        return route_score_from_keywords(terms, self.keywords, base=0.05)

    def build(self, engine: Any, question: str, args: dict[str, Any]) -> ContextPack:
        max_items = int(args.get("max_items") or 40)
        terms = [str(term) for term in args.get("terms") or []]
        matches, query_source = _search(engine, question, terms + [str(item) for item in args.get("ranking_hints") or []], max_items, self.kinds, [str(item) for item in args.get("hard_scope") or []])
        hard_scope = [str(item) for item in args.get("hard_scope") or []]
        relations = _relations_for(engine, matches[:12], max_items, self.relation_types, hard_scope, bool(args.get("include_external_endpoints", False)))
        next_actions = []
        asset = _first_asset(matches)
        if asset:
            next_actions.append({"reason": "Open the strongest candidate asset for detailed evidence.", "tool": "uepi_asset", "arguments": {"asset": asset}})
        return ContextPack(
            route=self.name,
            confidence=float(args.get("route_confidence") or 0.0),
            question=question,
            interpretation=self.interpretation,
            matches=[_short_entity(entity) for entity in matches],
            relations=relations,
            sections={"relation_type_counts": dict(Counter(str(item.get("type") or "unknown") for item in relations))},
            uncertainties=["This route is built from static Snapshot evidence; live unsaved editor state is only included when a live overlay is current."],
            next_actions=next_actions,
            query_source=query_source,
        )


class ProjectOverviewRoute(KeywordRoute):
    def __init__(self) -> None:
        super().__init__(
            name="project_overview",
            priority=10,
            keywords={"overview", "project", "structure", "入口", "结构", "项目", "玩家入口"},
            interpretation="Project-level structure, asset kind counts, likely entry assets, and high-level systems.",
        )

    def match(self, question: str, terms: list[str], project_summary: dict[str, Any]) -> float:
        score = super().match(question, terms, project_summary)
        if any(term in terms for term in {"overview", "structure", "入口", "结构", "项目"}):
            score += 0.35
        return min(score, 1.0)

    def build(self, engine: Any, question: str, args: dict[str, Any]) -> ContextPack:
        max_items = int(args.get("max_items") or 40)
        overview = engine.overview(limit=20)
        result = _as_dict(overview.get("result"))
        top_assets = _as_list(result.get("top_assets"))
        return ContextPack(
            route=self.name,
            confidence=float(args.get("route_confidence") or 0.0),
            question=question,
            interpretation=self.interpretation,
            matches=top_assets[:max_items],
            sections={
                "counts": result.get("counts"),
                "entity_kinds": result.get("entity_kinds"),
                "relation_types": result.get("relation_types"),
                "top_assets": top_assets[:max_items],
                "cpp_symbols": result.get("cpp_symbols"),
            },
            next_actions=[
                {
                    "reason": "Search for a concrete entry asset such as GameMode, Character, PlayerController, or AnimBP.",
                    "tool": "uepi_search",
                    "arguments": {"query": "GameMode Character PlayerController AnimBP", "limit": 20},
                }
            ],
            query_source=str(result.get("query_source") or "snapshot_fragments"),
        )


class BlueprintBehaviorRoute(KeywordRoute):
    def __init__(self) -> None:
        super().__init__(
            name="blueprint_behavior",
            priority=90,
            keywords={"blueprint", "bp", "beginplay", "eventgraph", "node", "graph", "蓝图", "节点", "事件", "beginplay"},
            interpretation="Blueprint behavior route: events, functions, variable reads/writes, class interactions, and static flow hints.",
            kinds={"asset", "blueprint_node", "blueprint_event", "blueprint_graph", "blueprint_pin"},
            relation_types={"contains_node", "exec_flows_to", "data_flows_to", "calls_function", "writes_variable", "reads_variable", "spawns_class", "casts_to"},
        )

    def build(self, engine: Any, question: str, args: dict[str, Any]) -> ContextPack:
        pack = super().build(engine, question, args)
        asset = _first_asset(pack.matches)
        if asset:
            blueprint = engine.blueprint(asset=asset, limit=min(int(args.get("max_items") or 40) * 4, 300))
            if blueprint.get("ok"):
                result = _as_dict(blueprint.get("result"))
                pack.sections["blueprint_semantic_summary"] = result.get("semantic_summary")
                pack.sections["blueprint_asset"] = result.get("asset")
                pack.next_actions.insert(0, {"reason": "Trace the specific execution path mentioned by the user.", "tool": "uepi_blueprint_trace", "arguments": {"asset": asset, "start": "<event or node title>"}})
        return pack


class AnimationPlaybackRoute(KeywordRoute):
    def __init__(self) -> None:
        super().__init__(
            name="animation_playback",
            priority=80,
            keywords={"animation", "anim", "montage", "blendspace", "sequence", "run", "walk", "动画", "蒙太奇", "状态机"},
            interpretation="Animation playback route: sequences, montages, blend spaces, AnimBP references, notifies, curves, changing-bone summaries, bone-motion profile artifacts, reconstruction profiles, driver_track_curves, and optional full-pose samples.",
            kinds={"asset", "animation_sequence", "animation_montage", "blend_space", "anim_blueprint", "anim_state_machine", "anim_state", "anim_asset_player"},
            relation_types={"contains_track", "animates_bone", "contains_notify", "plays_animation", "plays_montage", "uses_anim_blueprint", "uses_skeleton", "uses_skeletal_mesh"},
        )

    def build(self, engine: Any, question: str, args: dict[str, Any]) -> ContextPack:
        pack = super().build(engine, question, args)
        asset = _best_domain_asset(
            pack.matches,
            question,
            {"animation_sequence", "animation_montage", "blend_space", "anim_blueprint", "anim_asset_player"},
        )
        if asset:
            animation = engine.animation(asset=asset, limit=min(int(args.get("max_items") or 40) * 4, 300))
            if animation.get("ok"):
                result = _as_dict(animation.get("result"))
                pack.sections["animation_summary"] = {
                    "asset": result.get("asset"),
                    "motion_summary": result.get("motion_summary"),
                    "sequence": result.get("sequence"),
                    "entity_count": len(_as_list(result.get("animation_entities"))),
                }
                pack.next_actions.insert(0, {"reason": "Find Blueprint, AnimBP, or asset references that play this animation.", "tool": "uepi_impact", "arguments": {"asset": asset}})
        return pack


class AssetDependencyImpactRoute(KeywordRoute):
    def __init__(self) -> None:
        super().__init__(
            name="asset_dependency_impact",
            priority=70,
            keywords={"impact", "dependency", "references", "delete", "affect", "影响", "引用", "删除", "依赖", "风险"},
            interpretation="Asset dependency route: incoming/outgoing relations, direct and indirect affected assets, and risk hints.",
            kinds=None,
            relation_types=None,
        )

    def build(self, engine: Any, question: str, args: dict[str, Any]) -> ContextPack:
        pack = super().build(engine, question, args)
        asset = _first_asset(pack.matches)
        if asset:
            impact = engine.impact(asset=asset, relation_limit=int(args.get("max_items") or 40) * 2)
            if impact.get("ok"):
                pack.sections["impact"] = impact.get("result")
                pack.next_actions.insert(0, {"reason": "Inspect the candidate asset before planning edits.", "tool": "uepi_asset", "arguments": {"asset": asset}})
        return pack


def make_routes() -> list[Any]:
    return [
        ProjectOverviewRoute(),
        KeywordRoute(
            name="input_to_gameplay",
            priority=85,
            keywords={"input", "action", "mapping", "attack", "jump", "sprint", "输入", "按键", "攻击", "跳跃", "冲刺"},
            interpretation="Input-to-gameplay route: InputAction/MappingContext candidates, controller/character handlers, Blueprint call chains, and side effects.",
            kinds={"asset", "input_action", "input_mapping_context", "blueprint_node", "blueprint_event"},
            relation_types={"uses_input_action", "uses_input_mapping_context", "calls_function", "exec_flows_to", "data_flows_to", "plays_animation", "plays_montage", "writes_variable"},
        ),
        BlueprintBehaviorRoute(),
        AnimationPlaybackRoute(),
        KeywordRoute(
            name="ui_flow",
            priority=75,
            keywords={"ui", "widget", "umg", "button", "hud", "healthbar", "界面", "按钮", "血条", "控件"},
            interpretation="UI flow route: WidgetBlueprint candidates, creation paths, viewport attachment, events, and data binding hints.",
            kinds={"asset", "widget_blueprint", "widget_tree", "widget", "blueprint_node"},
            relation_types={"creates_widget", "uses_widget", "adds_widget_to_viewport", "binds_delegate", "calls_function", "exec_flows_to"},
        ),
        AssetDependencyImpactRoute(),
        KeywordRoute(
            name="data_driven_behavior",
            priority=60,
            keywords={"dataasset", "datatable", "row", "config", "数据表", "数据资产", "配置"},
            interpretation="Data-driven behavior route: DataAsset/DataTable references, row structs, and Blueprint consumers.",
            kinds={"asset", "data_asset", "data_table", "blueprint_node"},
            relation_types={"uses_data_table", "uses_data_asset", "calls_function", "data_flows_to"},
        ),
        KeywordRoute(
            name="gas_ability_flow",
            priority=55,
            keywords={"gas", "ability", "effect", "tag", "GameplayAbility", "能力", "标签"},
            interpretation="GAS route: ability assets, gameplay tags/effects/cues, Blueprint calls, and animation side effects.",
            kinds={"asset", "gameplay_ability", "gameplay_effect", "gameplay_tag", "blueprint_node"},
            relation_types={"uses_gameplay_tag", "calls_function", "plays_montage", "data_flows_to"},
        ),
        KeywordRoute(
            name="ai_behavior_flow",
            priority=50,
            keywords={"ai", "behavior", "blackboard", "statetree", "bt", "npc", "行为树", "黑板", "状态树"},
            interpretation="AI route: behavior tree, blackboard, StateTree, controller, pawn, and task/service links.",
            kinds={"asset", "behavior_tree", "blackboard", "state_tree", "ai_task"},
            relation_types={"calls_function", "uses_data_asset", "data_flows_to"},
        ),
        KeywordRoute(
            name="network_replication_flow",
            priority=45,
            keywords={"network", "replication", "rpc", "server", "client", "multicast", "网络", "复制"},
            interpretation="Networking route: replicated properties, RPC-like Blueprint functions, and authority/client flow hints.",
            kinds={"asset", "blueprint_node", "blueprint_event"},
            relation_types={"calls_function", "exec_flows_to", "writes_variable", "reads_variable"},
        ),
    ]
