from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import re
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


def _context_entity_identity(entity: dict[str, Any]) -> dict[str, Any]:
    attributes = _as_dict(entity.get("attributes"))
    keep_attributes = {
        key: attributes[key]
        for key in (
            "object_path",
            "asset_path",
            "package_name",
            "asset_name",
            "asset_class",
            "class_path",
            "sequence_path",
            "skeleton_path",
            "animation_asset",
            "state_name",
            "slot_name",
        )
        if attributes.get(key) not in (None, "")
    }
    completeness = _as_dict(entity.get("completeness"))
    return {
        "id": entity.get("id"),
        "kind": entity.get("kind"),
        "canonical_key": entity.get("canonical_key"),
        "display_name": entity.get("display_name"),
        "source_layer": entity.get("source_layer"),
        "attributes": keep_attributes,
        "completeness": {
            key: completeness[key]
            for key in ("state", "covered", "omitted", "warnings")
            if completeness.get(key) not in (None, [], {}, "")
        },
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


def _gameplay_entity_summary(entity: dict[str, Any]) -> dict[str, Any]:
    attributes = _as_dict(entity.get("attributes"))
    keep_attributes = {
        key: attributes[key]
        for key in (
            "blueprint_path",
            "event_name",
            "graph_path",
            "input_key",
            "node_class",
            "node_guid",
            "node_title",
            "semantic_event",
            "semantic_function",
            "semantic_input_key",
            "semantic_kind",
            "semantic_owner_class",
        )
        if attributes.get(key) not in (None, "")
    }
    return {
        "id": entity.get("id"),
        "kind": entity.get("kind"),
        "canonical_key": entity.get("canonical_key"),
        "display_name": entity.get("display_name"),
        "source_layer": entity.get("source_layer"),
        "attributes": keep_attributes,
    }


def _gameplay_relation_summary(relation: dict[str, Any]) -> dict[str, Any]:
    attributes = _as_dict(relation.get("attributes"))
    keep_attributes = {
        key: attributes[key]
        for key in (
            "branch_label",
            "edge_kind",
            "source_node_class",
            "source_pin_name",
            "target_node_class",
            "target_pin_name",
        )
        if attributes.get(key) not in (None, "")
    }
    return {
        "id": relation.get("id"),
        "type": relation.get("type"),
        "from_id": relation.get("from_id"),
        "to_id": relation.get("to_id"),
        "source_layer": relation.get("source_layer"),
        "derived": bool(relation.get("derived")),
        "confidence": relation.get("confidence"),
        "attributes": keep_attributes,
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
    if "::" in text:
        text = text.split("::", 1)[0]
    colon = text.find(":", text.rfind("/") + 1)
    if colon >= 0:
        text = text[:colon]
    if "." not in text.rsplit("/", 1)[-1]:
        asset_name = text.rsplit("/", 1)[-1]
        text = f"{text}.{asset_name}"
    package, object_name = text.rsplit(".", 1)
    if object_name.endswith("_C"):
        object_name = object_name[:-2]
    return f"{package}.{object_name}".casefold()


def _root_asset_path(entity: dict[str, Any]) -> str:
    attributes = _as_dict(entity.get("attributes"))
    candidates = [
        entity.get("canonical_key"),
        attributes.get("object_path"),
        attributes.get("asset_path"),
        attributes.get("package_name"),
    ]
    for candidate in candidates:
        text = str(candidate or "").strip().replace("\\", "/")
        if not text.startswith("/"):
            continue
        if "::" in text:
            text = text.split("::", 1)[0]
        colon = text.find(":", text.rfind("/") + 1)
        if colon >= 0:
            text = text[:colon]
        if "." not in text.rsplit("/", 1)[-1]:
            name = text.rsplit("/", 1)[-1]
            text = f"{text}.{name}"
        package, object_name = text.rsplit(".", 1)
        if object_name.endswith("_C"):
            object_name = object_name[:-2]
        return f"{package}.{object_name}"
    return ""


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
        scoped: list[dict[str, Any]] = []
        if engine.cache:
            scoped_by_id: dict[str, dict[str, Any]] = {}
            for scope in hard_scope:
                root, candidates = engine.cache.resolve_entity(scope)
                scope_identity = _normalized_asset_identity(scope)
                root_candidates = [item for item in [root, *candidates] if isinstance(item, dict)]
                root = next(
                    (
                        item
                        for item in root_candidates
                        if item.get("kind") == "asset"
                        and _normalized_asset_identity(_root_asset_path(item)) == scope_identity
                    ),
                    root,
                )
                if not root:
                    continue
                root_id = str(root.get("id") or "")
                if root_id:
                    scoped_by_id[root_id] = root
                root_path = _root_asset_path(root) or str(root.get("canonical_key") or scope)
                for item in engine.cache.scoped_entities_for_asset(root_path, kinds, limit=max(100, limit * 20)):
                    item_id = str(item.get("id") or "")
                    if item_id:
                        scoped_by_id[item_id] = item
                if kinds and root_id:
                    for item in engine.cache.domain_entities_for_asset(root_id, kinds, max(100, limit * 20)):
                        item_id = str(item.get("id") or "")
                        if item_id:
                            scoped_by_id[item_id] = item
            scoped = [item for item in scoped_by_id.values() if _in_hard_scope(item, hard_scope)]
            query_source = "sqlite_cache"
        else:
            engine._ensure_loaded()
            scoped = [entity for entity in engine.entities if _in_hard_scope(entity, hard_scope)]
            query_source = "snapshot_fragments"
        scope_identities = {_normalized_asset_identity(scope) for scope in hard_scope}
        roots: dict[str, dict[str, Any]] = {}
        for entity in scoped:
            root_path = _root_asset_path(entity)
            root_identity = _normalized_asset_identity(root_path)
            if not root_identity or root_identity not in scope_identities:
                continue
            previous = roots.get(root_identity)
            previous_score = 1 if previous and previous.get("kind") == "asset" else 0
            score = 1 if entity.get("kind") == "asset" else 0
            if previous is None or score > previous_score:
                roots[root_identity] = entity

        folded_query = query.casefold()
        folded_terms = [str(term).casefold() for term in terms if str(term).strip()]

        def rank(entity: dict[str, Any]) -> tuple[int, str]:
            text = _entity_text(entity)
            root_path = _root_asset_path(entity)
            asset_name = root_path.rsplit(".", 1)[-1].casefold() if root_path else ""
            score = 0
            if entity.get("kind") == "asset":
                score += 80
            if asset_name and asset_name in folded_query:
                score += 300
            if folded_query and folded_query in text:
                score += 120
            score += sum(25 for term in folded_terms if term and term in text)
            return score, str(entity.get("canonical_key") or entity.get("id") or "").casefold()

        root_entities = sorted(roots.values(), key=lambda item: (-rank(item)[0], rank(item)[1]))
        for entity in root_entities:
            add(entity)
        for entity in sorted(scoped, key=lambda item: (-rank(item)[0], rank(item)[1])):
            if len(matches) >= max(limit, len(root_entities)):
                break
            add(entity)
        return matches[: max(limit, len(root_entities))], query_source

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
    candidate_relations: list[dict[str, Any]] = []
    seen: set[str] = set()
    for entity in matches:
        entity_id = str(entity.get("id") or "")
        if not entity_id:
            continue
        relations = engine.cache.relations_for_entity(entity_id, limit=50) if engine.cache else (engine.outgoing.get(entity_id, []) + engine.incoming.get(entity_id, []))[:50]
        for relation in relations:
            if relation_types and relation.get("type") not in relation_types:
                continue
            relation_id = str(relation.get("id") or "")
            if relation_id in seen:
                continue
            seen.add(relation_id)
            candidate_relations.append(relation)

    if hard_scope and not include_external_endpoints and candidate_relations:
        endpoint_ids = {
            str(relation.get(key) or "")
            for relation in candidate_relations
            for key in ("from_id", "to_id")
            if relation.get(key)
        }
        if engine.cache:
            endpoint_by_id = {
                str(item.get("id") or ""): item
                for item in engine.cache.entities_by_ids(endpoint_ids, limit=max(1, len(endpoint_ids)))
                if item.get("id")
            }
        else:
            engine._ensure_loaded()
            endpoint_by_id = {item_id: engine.entity_by_id.get(item_id, {}) for item_id in endpoint_ids}
        candidate_relations = [
            relation
            for relation in candidate_relations
            if _in_hard_scope(endpoint_by_id.get(str(relation.get("from_id") or ""), {}), hard_scope)
            and _in_hard_scope(endpoint_by_id.get(str(relation.get("to_id") or ""), {}), hard_scope)
        ]

    return [_relation_summary(relation) for relation in candidate_relations[:limit]]


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
        root_asset = _root_asset_path(entity)
        if not root_asset:
            continue
        kind = str(entity.get("kind") or "")
        display = str(entity.get("display_name") or "")
        attributes = _as_dict(entity.get("attributes"))
        asset_name = root_asset.rsplit(".", 1)[-1]
        score = 0
        if kind in preferred_kinds:
            score += 80
        if kind == "asset":
            score += 40
        if asset_name and asset_name.casefold() in question_folded:
            score += 500
        elif display and display.casefold() in question_folded:
            score += 300
        asset_class = " ".join(str(attributes.get(key) or "") for key in ("asset_class", "class", "class_path")).casefold()
        if any(token in asset_class for token in ("animsequence", "animmontage")) or kind in {"animation_sequence", "animation_montage"}:
            score += 220
        elif "blendspace" in asset_class or kind == "blend_space":
            score += 160
        elif "animblueprint" in asset_class or kind == "anim_blueprint":
            score += 80
        if kind in {"blueprint_graph", "anim_state", "anim_state_machine", "anim_asset_player"}:
            score -= 40
        previous = candidates.get(root_asset.casefold())
        if previous is None or score > previous[0]:
            candidates[root_asset.casefold()] = (score, root_asset)
    return sorted(candidates.values(), key=lambda item: (-item[0], item[1].casefold()))[0][1] if candidates else None


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


def _attribute(entity: dict[str, Any], *keys: str) -> Any:
    attributes = _as_dict(entity.get("attributes"))
    typed = _as_dict(entity.get("typed_attributes"))
    for key in keys:
        wrapped = typed.get(key)
        if isinstance(wrapped, dict) and wrapped.get("value") not in (None, ""):
            return wrapped.get("value")
        if wrapped not in (None, "") and not isinstance(wrapped, dict):
            return wrapped
        if attributes.get(key) not in (None, ""):
            return attributes.get(key)
    return None


def _node_title(entity: dict[str, Any]) -> str:
    return str(_attribute(entity, "node_title", "event_name", "function_name") or entity.get("display_name") or "")


def _semantic_call_target(entity: dict[str, Any]) -> tuple[str, str] | None:
    function_path = str(_attribute(entity, "semantic_function", "function_path") or "").strip()
    if not function_path or not function_path.startswith("/Game/"):
        return None
    owner, separator, function_name = function_path.rpartition(":")
    if not separator or not function_name:
        return None
    package, dot, generated_name = owner.partition(".")
    if not dot:
        return None
    asset_name = re.sub(r"^(?:SKEL_|REINST_)", "", generated_name)
    asset_name = re.sub(r"_C(?:_\d+)?$", "", asset_name)
    if not asset_name:
        asset_name = package.rsplit("/", 1)[-1]
    return f"{package}.{asset_name}", function_name


def _semantic_dispatch_name(entity: dict[str, Any]) -> str:
    function_path = str(_attribute(entity, "semantic_function", "function_path") or "").strip()
    if not function_path:
        return ""
    return function_path.rpartition(":")[2] or function_path.rsplit(".", 1)[-1]


def _pin_defaults_for_node(node_id: str, entities: dict[str, dict[str, Any]], relations: list[dict[str, Any]]) -> list[dict[str, Any]]:
    pin_ids = {
        str(relation.get("to_id") or "")
        for relation in relations
        if relation.get("type") == "has_pin" and relation.get("from_id") == node_id
    }
    values: list[dict[str, Any]] = []
    for pin_id in sorted(pin_ids):
        pin = entities.get(pin_id)
        if not pin:
            continue
        default = _attribute(pin, "default_value", "default_object")
        if default in (None, ""):
            continue
        values.append(
            {
                "pin_id": _attribute(pin, "pin_id") or pin.get("id"),
                "name": _attribute(pin, "pin_name") or pin.get("display_name"),
                "value": default,
                "container_type": _attribute(pin, "pin_container_type", "container_type") or "none",
            }
        )
    return values


_INPUT_KEY_ALIASES = {
    "zero": {"0"},
    "one": {"1"},
    "two": {"2"},
    "three": {"3"},
    "four": {"4"},
    "five": {"5"},
    "six": {"6"},
    "seven": {"7"},
    "eight": {"8"},
    "nine": {"9"},
    "leftalt": {"left alt", "左alt", "左 alt"},
    "rightalt": {"right alt", "右alt", "右 alt"},
}

_INPUT_STABLE_NAMES = {
    "zero": "Zero",
    "one": "One",
    "two": "Two",
    "three": "Three",
    "four": "Four",
    "five": "Five",
    "six": "Six",
    "seven": "Seven",
    "eight": "Eight",
    "nine": "Nine",
    "leftalt": "LeftAlt",
    "rightalt": "RightAlt",
}


def _canonical_input_key(value: Any) -> str:
    text = str(value or "").strip()
    compact = re.sub(r"[\s_-]+", "", text.casefold())
    for stable, aliases in _INPUT_KEY_ALIASES.items():
        normalized_aliases = {re.sub(r"[\s_-]+", "", str(alias).casefold()) for alias in aliases}
        if compact == stable or compact in normalized_aliases:
            return _INPUT_STABLE_NAMES[stable]
    return text


def _input_key(entity: dict[str, Any]) -> str:
    title = _node_title(entity).strip()
    explicit = str(_attribute(entity, "semantic_input_key", "input_key", "key") or "").strip()
    if explicit:
        return _canonical_input_key(explicit)
    node_class = str(_attribute(entity, "node_class") or "").casefold()
    semantic = str(_attribute(entity, "semantic_kind") or "").casefold()
    if "inputkey" in node_class or semantic in {"input_key", "input_action"}:
        return _canonical_input_key(title.removeprefix("InputKey ").strip())
    return ""


def _input_aliases(key: str) -> set[str]:
    folded = _canonical_input_key(key).casefold().strip()
    compact = re.sub(r"[\s_-]+", "", folded)
    return {folded, compact, *_INPUT_KEY_ALIASES.get(compact, set())}


def _input_mention_pattern(alias: str) -> re.Pattern[str]:
    escaped = re.escape(alias).replace(r"\ ", r"[\s_-]+")
    if alias and all(character.isalnum() or character in " _-" for character in alias):
        return re.compile(rf"(?<![a-z0-9]){escaped}(?![a-z0-9])", re.IGNORECASE)
    return re.compile(escaped, re.IGNORECASE)


def _input_mention_is_negated(question: str, start: int) -> bool:
    prefix = question[max(0, start - 32):start].casefold()
    return re.search(r"(?:\b(?:exclude|not|except|without)\b|\bdo\s+not(?:\s+use)?\b|不要|排除|不是)\s*(?:\bkey\b|按键)?\s*$", prefix) is not None


def _hint_input_key(ranking_hints: list[str]) -> str | None:
    for hint in ranking_hints:
        match = re.fullmatch(r"\s*(?:input_key|key)\s*=\s*([^\s,]+)\s*", str(hint), re.IGNORECASE)
        if match:
            return match.group(1)
    return None


def _input_resolution(
    question: str,
    nodes: list[dict[str, Any]],
    *,
    input_key: str | None = None,
    excluded_input_keys: list[str] | None = None,
    ranking_hints: list[str] | None = None,
) -> dict[str, Any]:
    folded = question.casefold()
    keyed_nodes = [(node, _canonical_input_key(_input_key(node))) for node in nodes if _input_key(node)]
    available_by_folded = {key.casefold(): key for _, key in keyed_nodes}
    available_inputs = sorted(set(available_by_folded.values()), key=str.casefold)
    excluded: set[str] = {_canonical_input_key(item) for item in excluded_input_keys or [] if str(item).strip()}
    positive_mentions: list[tuple[int, str, str, str]] = []

    for key in available_inputs:
        for alias in sorted(_input_aliases(key), key=len, reverse=True):
            for match in _input_mention_pattern(alias).finditer(folded):
                if _input_mention_is_negated(question, match.start()):
                    excluded.add(key)
                else:
                    positive_mentions.append((match.start(), alias, key, "stable_key_exact" if alias.casefold() == key.casefold() else "alias_exact"))

    structured = str(input_key or "").strip()
    hint = _hint_input_key(ranking_hints or []) if not structured else None
    requested = structured or hint
    request_mode = "structured_exact" if structured else ("ranking_hint_exact" if hint else "")

    if not requested:
        if re.search(r"\bwithout\s+(?:a\s+)?key\b", folded):
            return {
                "requested_input": None,
                "resolved_input": None,
                "excluded_input_keys": sorted(excluded, key=str.casefold),
                "available_inputs": available_inputs,
                "match_mode": "not_requested",
                "matched": False,
            }
        phrase_patterns = [
            re.compile(r"\b(?:press|key|input)\b\s*[:：]?\s*([A-Za-z0-9_+\-]+)", re.IGNORECASE),
            re.compile(r"(?:按键|按下|按)\s*[:：]?\s*([A-Za-z0-9_+\-]+)", re.IGNORECASE),
        ]
        explicit_mentions: list[tuple[int, str]] = []
        for pattern in phrase_patterns:
            explicit_mentions.extend((match.start(1), match.group(1)) for match in pattern.finditer(question) if not _input_mention_is_negated(question, match.start()))
        explicit_mentions.sort(key=lambda item: item[0])
        explicit_keys: list[str] = []
        for _, value in explicit_mentions:
            canonical = _canonical_input_key(value)
            if canonical not in excluded and canonical.casefold() not in {item.casefold() for item in explicit_keys}:
                explicit_keys.append(canonical)
        if len(explicit_keys) > 1:
            return {"requested_input": [value for _, value in explicit_mentions], "resolved_input": None, "excluded_input_keys": sorted(excluded, key=str.casefold), "available_inputs": available_inputs, "ambiguous_inputs": explicit_keys, "match_mode": "ambiguous", "matched": False}
        if explicit_mentions:
            requested = explicit_mentions[0][1]
            request_mode = "explicit_phrase"

            requested_folded = _canonical_input_key(requested).casefold()
            other_positive = {
                key.casefold(): key
                for _, _, key, _ in positive_mentions
                if key not in excluded and key.casefold() != requested_folded
            }
            if other_positive:
                ambiguous = [_canonical_input_key(requested), *other_positive.values()]
                return {
                    "requested_input": ambiguous,
                    "resolved_input": None,
                    "excluded_input_keys": sorted(excluded, key=str.casefold),
                    "available_inputs": available_inputs,
                    "ambiguous_inputs": ambiguous,
                    "match_mode": "ambiguous",
                    "matched": False,
                }

    if requested:
        canonical = _canonical_input_key(requested)
        resolved = available_by_folded.get(canonical.casefold())
        if resolved and resolved not in excluded:
            return {"requested_input": requested, "resolved_input": resolved, "excluded_input_keys": sorted(excluded, key=str.casefold), "available_inputs": available_inputs, "match_mode": request_mode or "alias_exact", "matched": True}
        return {"requested_input": requested, "resolved_input": None, "excluded_input_keys": sorted(excluded, key=str.casefold), "available_inputs": available_inputs, "match_mode": "unmatched", "matched": False}

    distinct_positive: dict[str, tuple[int, str, str]] = {}
    for position, alias, key, mode in positive_mentions:
        if key in excluded:
            continue
        previous = distinct_positive.get(key.casefold())
        if previous is None or position < previous[0]:
            distinct_positive[key.casefold()] = (position, alias, mode)
    if len(distinct_positive) == 1:
        folded_key, (_, alias, mode) = next(iter(distinct_positive.items()))
        return {"requested_input": alias, "resolved_input": available_by_folded[folded_key], "excluded_input_keys": sorted(excluded, key=str.casefold), "available_inputs": available_inputs, "match_mode": mode, "matched": True}
    if len(distinct_positive) > 1:
        ordered = sorted(distinct_positive.items(), key=lambda item: item[1][0])
        return {"requested_input": [value[1] for _, value in ordered], "resolved_input": None, "excluded_input_keys": sorted(excluded, key=str.casefold), "available_inputs": available_inputs, "ambiguous_inputs": [available_by_folded[key] for key, _ in ordered], "match_mode": "ambiguous", "matched": False}
    return {
        "requested_input": None,
        "resolved_input": None,
        "excluded_input_keys": sorted(excluded, key=str.casefold),
        "available_inputs": available_inputs,
        "match_mode": "not_requested",
        "matched": False,
    }


def _owner_score(asset: str, nodes: list[dict[str, Any]], live_gameplay: dict[str, Any]) -> dict[str, Any]:
    folded = asset.casefold()
    text = " ".join(_entity_text(node) for node in nodes)
    reasons: list[str] = []
    counter_evidence: list[str] = []
    score = 0.45
    possessed = str(live_gameplay.get("possessed_pawn_class") or "").casefold()
    default_pawn = str(live_gameplay.get("default_pawn_class") or "").casefold()
    asset_name = asset.rsplit(".", 1)[-1].casefold()
    if possessed and asset_name in possessed:
        score += 0.45
        reasons.append("The live PlayerController possesses this Pawn class.")
    elif default_pawn and asset_name in default_pawn:
        score += 0.35
        reasons.append("This Blueprint matches the active GameMode Default Pawn class.")
    if any(token in folded for token in ("character", "playerpawn", "thirdperson")):
        score += 0.2
        reasons.append("The asset identity is consistent with a player Character/Pawn.")
    if any(token in folded for token in ("npc", "enemy", "manny")):
        score -= 0.15
        counter_evidence.append("The asset identity is consistent with an NPC rather than the possessed player Pawn.")
    if "enableinput" in text or "enable input" in text:
        score -= 0.18
        counter_evidence.append("Input is enabled locally through EnableInput, which is weaker than possession ownership.")
    if "autoreceiveinput" in text or "auto receive input" in text:
        score -= 0.12
        counter_evidence.append("AutoReceiveInput creates a local Actor input path that may compete with the player Pawn.")
    if not reasons:
        reasons.append("Ownership is inferred from static Blueprint identity and input entry placement.")
    return {
        "asset": asset,
        "owner_confidence": round(max(0.05, min(score, 0.99)), 3),
        "reasons": reasons,
        "counter_evidence": counter_evidence,
        "live_evidence": live_gameplay,
    }


class GameplayInputToEffectRoute(KeywordRoute):
    _FLOW_TYPES = {"exec_flows_to", "delegate_flows_to", "data_flows_to"}

    def __init__(self) -> None:
        super().__init__(
            name="gameplay_input_to_effect",
            priority=98,
            keywords={"input", "key", "action", "effect", "animation", "three", "输入", "按键", "效果", "动画", "调用链"},
            interpretation="Resolve gameplay input ownership and follow static Blueprint execution across assets to terminal gameplay effects.",
            kinds={"asset", "blueprint_node", "blueprint_event", "blueprint_pin", "u_function", "animation_sequence", "animation_montage"},
            relation_types=self._FLOW_TYPES | {"has_pin", "calls_function", "plays_animation", "plays_montage", "loads_asset"},
        )

    def match(self, question: str, terms: list[str], project_summary: dict[str, Any]) -> float:
        score = super().match(question, terms, project_summary)
        folded = question.casefold()
        if any(token in folded for token in ("->", "调用链", "按键", "input", "effect")):
            score += 0.35
        return min(score, 1.0)

    def _asset_slice(self, engine: Any, asset: str, limit: int = 1200) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
        if engine.cache:
            asset_identity = _normalized_asset_identity(asset)
            entities = [
                item
                for item in engine.cache.scoped_entities_for_asset(asset, None, limit=limit)
                if _normalized_asset_identity(_root_asset_path(item)) == asset_identity
            ]
            ids = {str(item.get("id") or "") for item in entities if item.get("id")}
            relations = engine.cache.relations_for_entities(ids, limit=limit * 4)
            return entities, relations
        engine._ensure_loaded()
        entities = [item for item in engine.entities if _normalized_asset_identity(_root_asset_path(item)) == _normalized_asset_identity(asset)]
        ids = {str(item.get("id") or "") for item in entities if item.get("id")}
        relations = [item for item in engine.relations if item.get("from_id") in ids or item.get("to_id") in ids]
        return entities[:limit], relations[: limit * 4]

    def build(self, engine: Any, question: str, args: dict[str, Any]) -> ContextPack:
        max_items = max(10, int(args.get("max_items") or 40))
        terms = [str(item) for item in args.get("terms") or [] if str(item).strip()]
        hard_scope = [str(item) for item in args.get("hard_scope") or [] if str(item).strip()]
        candidates, query_source = _search(engine, question, terms, max_items * 4, self.kinds, hard_scope)

        for scoped_asset in hard_scope:
            scoped_entities, _ = self._asset_slice(engine, scoped_asset)
            candidates.extend(scoped_entities)

        input_nodes = [item for item in candidates if item.get("kind") == "blueprint_node" and _input_key(item)]
        if not input_nodes and engine.cache:
            input_nodes = [
                item
                for term in [*terms, *re.findall(r"[A-Za-z0-9_]+", question)[:10]]
                for item in engine.cache.search_entities(term, kind="blueprint_node", limit=100)
                if _input_key(item)
            ]
        deduped_inputs = {str(item.get("id") or ""): item for item in input_nodes if item.get("id")}
        input_nodes = list(deduped_inputs.values())
        input_resolution = _input_resolution(
            question,
            input_nodes,
            input_key=str(args.get("input_key") or "") or None,
            excluded_input_keys=[str(item) for item in args.get("excluded_input_keys") or [] if str(item).strip()],
            ranking_hints=[str(item) for item in args.get("ranking_hints") or [] if str(item).strip()],
        )
        resolved_input = str(input_resolution.get("resolved_input") or "")
        excluded_inputs = {str(item).casefold() for item in input_resolution.get("excluded_input_keys") or []}
        fail_closed = input_resolution.get("match_mode") in {"unmatched", "ambiguous", "not_requested"}
        if resolved_input:
            input_nodes = [item for item in input_nodes if _input_key(item).casefold() == resolved_input.casefold()]
        elif fail_closed:
            input_nodes = []
        elif excluded_inputs:
            input_nodes = [item for item in input_nodes if _input_key(item).casefold() not in excluded_inputs]

        live_gameplay: dict[str, Any] = {}
        try:
            from .bridge_client import call_bridge

            status = call_bridge(engine.store, "editor.get_status", timeout=0.4)
            if status.get("ok"):
                live_gameplay = _as_dict(_as_dict(status.get("result")).get("gameplay_context"))
        except Exception:
            live_gameplay = {}

        by_asset: dict[str, list[dict[str, Any]]] = {}
        for node in input_nodes:
            asset = _root_asset_path(node)
            if asset:
                by_asset.setdefault(asset, []).append(node)
        owner_candidates = []
        for asset, nodes in by_asset.items():
            asset_entities, _ = self._asset_slice(engine, asset, limit=800)
            owner_nodes = [item for item in asset_entities if item.get("kind") in {"blueprint_node", "blueprint_event"}] or nodes
            owner_candidates.append(_owner_score(asset, owner_nodes, live_gameplay))
        owner_candidates.sort(key=lambda item: (-float(item["owner_confidence"]), str(item["asset"]).casefold()))
        selected_owner = owner_candidates[0] if owner_candidates else {
            "asset": None,
            "owner_confidence": 0.0,
            "reasons": ["No indexed gameplay input entry matched the request."],
            "counter_evidence": [],
            "live_evidence": live_gameplay,
        }

        selected_inputs = by_asset.get(str(selected_owner.get("asset") or ""), input_nodes)
        all_entities: dict[str, dict[str, Any]] = {}
        all_relations: dict[str, dict[str, Any]] = {}
        loaded_assets: set[str] = set()

        def load_asset(asset: str) -> None:
            identity = _normalized_asset_identity(asset)
            if not identity or identity in loaded_assets:
                return
            loaded_assets.add(identity)
            entities, relations = self._asset_slice(engine, asset)
            all_entities.update({str(item.get("id") or ""): item for item in entities if item.get("id")})
            all_relations.update({str(item.get("id") or ""): item for item in relations if item.get("id")})

        for asset in by_asset:
            load_asset(asset)
        for asset in hard_scope:
            load_asset(asset)

        paths: list[dict[str, Any]] = []
        terminal_effects: list[dict[str, Any]] = []
        unresolved: list[dict[str, Any]] = []
        path_signatures: dict[str, list[dict[str, Any]]] = {}
        traversed_entity_ids: set[str] = set()
        traversed_relation_ids: set[str] = set()

        for input_node in selected_inputs + [item for item in input_nodes if item not in selected_inputs]:
            start_id = str(input_node.get("id") or "")
            if not start_id:
                continue
            queue: list[tuple[str, list[dict[str, Any]], int]] = [(start_id, [], 0)]
            visited: set[str] = set()
            while queue:
                current_id, steps, depth = queue.pop(0)
                if current_id in visited or depth > 24:
                    continue
                visited.add(current_id)
                current = all_entities.get(current_id)
                if not current:
                    continue
                traversed_entity_ids.add(current_id)
                node_step = {
                    "asset": _root_asset_path(current),
                    "id": current_id,
                    "node_guid": _attribute(current, "node_guid"),
                    "title": _node_title(current),
                    "semantic_kind": _attribute(current, "semantic_kind"),
                    "semantic_function": _attribute(current, "semantic_function"),
                }
                next_steps = [*steps, node_step]
                call_target = _semantic_call_target(current)
                if call_target:
                    target_asset, function_name = call_target
                    load_asset(target_asset)
                    targets = [
                        item
                        for item in all_entities.values()
                        if _normalized_asset_identity(_root_asset_path(item)) == _normalized_asset_identity(target_asset)
                        and str(_attribute(item, "semantic_event", "semantic_function", "event_name", "function_name") or _node_title(item)).casefold() == function_name.casefold()
                    ]
                    if targets:
                        for target in targets[:4]:
                            transition = {
                                "kind": "cross_asset_call",
                                "from_asset": _root_asset_path(current),
                                "to_asset": target_asset,
                                "function_or_event": function_name,
                                "confidence": 0.95,
                            }
                            queue.append((str(target.get("id") or ""), [*next_steps, transition], depth + 1))
                    else:
                        unresolved.append({
                            "caller": node_step,
                            "target_asset": target_asset,
                            "function_or_event": function_name,
                            "reason": "The target Blueprint event/function was not present in the current indexed asset slice.",
                            "confidence": 0.45,
                        })
                elif str(_attribute(current, "semantic_is_interface_call") or "").casefold() == "true":
                    function_name = _semantic_dispatch_name(current)
                    targets = [
                        item
                        for item in all_entities.values()
                        if function_name
                        and str(_attribute(item, "semantic_event", "semantic_function", "event_name", "function_name") or _node_title(item)).casefold() == function_name.casefold()
                        and _normalized_asset_identity(_root_asset_path(item)) != _normalized_asset_identity(_root_asset_path(current))
                    ]
                    for target in targets[:8]:
                        queue.append(
                            (
                                str(target.get("id") or ""),
                                [
                                    *next_steps,
                                    {
                                        "kind": "cross_asset_interface_candidate",
                                        "from_asset": _root_asset_path(current),
                                        "to_asset": _root_asset_path(target),
                                        "function_or_event": function_name,
                                        "confidence": 0.65 if len(targets) == 1 else 0.4,
                                        "dispatch": "dynamic_interface",
                                    },
                                ],
                                depth + 1,
                            )
                        )
                    unresolved.append(
                        {
                            "caller": node_step,
                            "function_or_event": function_name,
                            "candidate_targets": [
                                {"asset": _root_asset_path(item), "id": item.get("id"), "node_guid": _attribute(item, "node_guid")}
                                for item in targets[:8]
                            ],
                            "reason": "Interface dispatch is runtime-selected; indexed implementations are candidates, not a statically unique target.",
                            "confidence": 0.65 if len(targets) == 1 else 0.4,
                        }
                    )

                folded = f"{_node_title(current)} {_attribute(current, 'semantic_function') or ''}".casefold()
                defaults = _pin_defaults_for_node(current_id, all_entities, list(all_relations.values()))
                is_terminal = any(token in folded for token in ("submitpublishedtemplate", "dynamic montage", "play montage", "play animation"))
                if is_terminal:
                    terminal = {"node": node_step, "pin_defaults": defaults, "path_length": len(next_steps)}
                    terminal_effects.append(terminal)
                    signature = "|".join(str(item.get("value") or "") for item in defaults) or _node_title(current).casefold()
                    path = {"input_key": _input_key(input_node), "input_asset": _root_asset_path(input_node), "steps": next_steps, "terminal": terminal}
                    paths.append(path)
                    path_signatures.setdefault(signature, []).append(path)

                for relation in all_relations.values():
                    if relation.get("type") not in self._FLOW_TYPES or relation.get("from_id") != current_id:
                        continue
                    target_id = str(relation.get("to_id") or "")
                    if target_id:
                        if relation.get("id"):
                            traversed_relation_ids.add(str(relation["id"]))
                        queue.append((target_id, next_steps, depth + 1))

        duplicate_paths: list[dict[str, Any]] = []
        for signature, duplicates in path_signatures.items():
            assets = {str(item.get("input_asset") or "") for item in duplicates}
            if len(duplicates) > 1 and len(assets) > 1:
                duplicate_paths.append({
                    "diagnostic_code": "UEPI_DUPLICATE_GAMEPLAY_INPUT_PATH",
                    "signature": signature,
                    "input_assets": sorted(assets),
                    "input_keys": sorted({str(item.get("input_key") or "") for item in duplicates}),
                    "message": "Multiple Blueprint input entries can submit the same terminal gameplay effect.",
                })

        relevant_entity_ids = {
            str(item.get("id") or "") for item in input_nodes if item.get("id")
        } | traversed_entity_ids
        matches = [
            _gameplay_entity_summary(item)
            for item in all_entities.values()
            if str(item.get("id") or "") in relevant_entity_ids
            and item.get("kind") in {"asset", "blueprint_node", "blueprint_event", "animation_sequence", "animation_montage"}
        ]
        deduped_matches = {str(item.get("id") or ""): item for item in matches if item.get("id")}
        route_diagnostics: list[dict[str, Any]] = []
        if input_resolution.get("match_mode") in {"unmatched", "ambiguous"}:
            ambiguous = input_resolution.get("match_mode") == "ambiguous"
            route_diagnostics.append(
                {
                    "severity": "error",
                    "blocking": True,
                    "code": "UEPI_INPUT_KEY_AMBIGUOUS" if ambiguous else "UEPI_INPUT_KEY_UNMATCHED",
                    "message": (
                        "Multiple non-excluded input keys were requested; select exactly one stable FKey."
                        if ambiguous
                        else "The requested input key did not exactly match any stable FKey exported by the scoped Blueprint assets."
                    ),
                    "requested_input": input_resolution.get("requested_input"),
                    "ambiguous_inputs": input_resolution.get("ambiguous_inputs") or [],
                    "available_inputs": input_resolution.get("available_inputs") or [],
                    "recoverable": True,
                }
            )
        relevant_relations = [
            item
            for item in all_relations.values()
            if str(item.get("id") or "") in traversed_relation_ids
        ]
        next_actions = []
        if resolved_input and selected_inputs:
            next_actions.append(
                {
                    "reason": "Verify the selected input path in the active PIE session.",
                    "tool": "uepi_runtime",
                    "arguments": {
                        "action": "input",
                        "delivery": "possessed_pawn_input_stack",
                        "key": resolved_input,
                        "event": "pressed",
                    },
                }
            )
        return ContextPack(
            route=self.name,
            confidence=float(args.get("route_confidence") or 0.0),
            question=question,
            interpretation=self.interpretation,
            matches=list(deduped_matches.values())[:max_items],
            relations=[_gameplay_relation_summary(item) for item in relevant_relations if item.get("type") in self._FLOW_TYPES][:max_items],
            sections={
                "input_resolution": input_resolution,
                "requested_input": input_resolution.get("requested_input"),
                "resolved_input": input_resolution.get("resolved_input"),
                "excluded_input_keys": input_resolution.get("excluded_input_keys") or [],
                "available_inputs": input_resolution.get("available_inputs") or [],
                "match_mode": input_resolution.get("match_mode"),
                "input_owner": selected_owner,
                "input_owner_candidates": owner_candidates,
                "selected_inputs": [_gameplay_entity_summary(item) for item in input_nodes[:20]],
                "cross_asset_paths": paths[:10],
                "duplicate_paths": duplicate_paths,
                "terminal_effects": terminal_effects[:20],
                "unresolved_dispatches": unresolved[:20],
                "call_graph_support": "Static direct Blueprint calls are resolved from semantic_function; interface, delegate, and runtime-selected targets remain explicit unresolved dispatches when no exact target is indexed.",
            },
            diagnostics=route_diagnostics,
            uncertainties=["Static ownership evidence does not prove runtime input consumption; use the runtime input delivery trace for final verification."],
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
        pack.matches = [_context_entity_identity(item) for item in pack.matches]
        pack.relations = [
            {
                key: item.get(key)
                for key in ("id", "type", "from_id", "to_id", "source_layer", "derived", "confidence")
                if item.get(key) is not None
            }
            for item in pack.relations
        ]
        if asset:
            animation = engine.animation(asset=asset, limit=min(int(args.get("max_items") or 40) * 2, 80), summary_only=True)
            if animation.get("ok"):
                result = _as_dict(animation.get("result"))
                pack.sections["animation_summary"] = {
                    "asset": _context_entity_identity(_as_dict(result.get("asset"))),
                    "motion_summary": result.get("motion_summary"),
                    "sequence": result.get("sequence"),
                    "entity_count": int(result.get("animation_entity_count") or 0),
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
        GameplayInputToEffectRoute(),
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
