from __future__ import annotations

from collections import Counter, deque
import json
from pathlib import Path
from typing import Any
from uuid import uuid4

from .store import SnapshotState, SnapshotStore, SnapshotStoreError, _load_json


BLUEPRINT_KINDS = {
    "blueprint_graph",
    "blueprint_node",
    "blueprint_pin",
    "blueprint_event",
    "cfg_basic_block",
    "dfg_value",
}

ANIMATION_KINDS = {
    "skeleton",
    "bone",
    "virtual_bone",
    "skeletal_mesh",
    "animation_sequence",
    "animation_track",
    "anim_notify",
    "animation_montage",
    "animation_composite",
    "blend_space",
    "blend_space_sample",
    "pose_asset",
    "anim_blueprint",
    "anim_state_machine",
    "anim_state",
    "anim_transition",
    "anim_asset_player",
    "anim_slot",
}


def _as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def _as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def _short_entity(entity: dict[str, Any], include_snapshot: bool = False) -> dict[str, Any]:
    result = {
        "id": entity.get("id"),
        "kind": entity.get("kind"),
        "canonical_key": entity.get("canonical_key"),
        "display_name": entity.get("display_name"),
        "source_layer": entity.get("source_layer"),
        "attributes": _as_dict(entity.get("attributes")),
        "completeness": _as_dict(entity.get("completeness")),
        "diagnostics": _as_list(entity.get("diagnostics")),
        "evidence": _as_list(entity.get("evidence")),
    }
    if include_snapshot and isinstance(entity.get("snapshot"), dict):
        result["snapshot"] = entity["snapshot"]
    return result


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
        "evidence": _as_list(relation.get("evidence")),
    }


class UEPIQueryEngine:
    def __init__(self, store: SnapshotStore):
        self.store = store
        self.state: SnapshotState = store.load_state()
        self.scan = store.load_project_scan(self.state)
        self.entities = [entity for entity in _as_list(self.scan.get("entities")) if isinstance(entity, dict)]
        self.relations = [relation for relation in _as_list(self.scan.get("relations")) if isinstance(relation, dict)]
        self.counts = {
            "entities": len(self.entities),
            "relations": len(self.relations),
            "diagnostics": len(_as_list(self.scan.get("diagnostics"))),
            "asset_entities": sum(1 for entity in self.entities if entity.get("kind") in {"asset", "asset_redirector"}),
        }
        self.entity_by_id = {entity.get("id"): entity for entity in self.entities if entity.get("id")}
        self.outgoing: dict[str, list[dict[str, Any]]] = {}
        self.incoming: dict[str, list[dict[str, Any]]] = {}
        for relation in self.relations:
            self.outgoing.setdefault(str(relation.get("from_id") or ""), []).append(relation)
            self.incoming.setdefault(str(relation.get("to_id") or ""), []).append(relation)

    def _envelope(
        self,
        result: dict[str, Any],
        diagnostics: list[dict[str, Any]] | None = None,
        omissions: list[str] | None = None,
    ) -> dict[str, Any]:
        return {
            "schema_version": "uepi.mcp-envelope.v1",
            "request_id": str(uuid4()),
            "project": {
                "id": self.state.project.get("id") or self.scan.get("project_id"),
                "name": self.state.project.get("name") or self.scan.get("project_name"),
                "engine_version": self.state.project.get("engine_version") or self.scan.get("engine_version"),
            },
            "state": self.state.envelope_state(),
            "result": result,
            "diagnostics": diagnostics or [],
            "omissions": omissions or [],
            "continuation": {"cursor": None, "has_more": False},
        }

    def _error(self, code: str, message: str, candidates: list[dict[str, Any]] | None = None) -> dict[str, Any]:
        return {
            "schema_version": "uepi.mcp-envelope.v1",
            "request_id": str(uuid4()),
            "project": {
                "id": self.state.project.get("id") or self.scan.get("project_id"),
                "name": self.state.project.get("name") or self.scan.get("project_name"),
                "engine_version": self.state.project.get("engine_version") or self.scan.get("engine_version"),
            },
            "state": self.state.envelope_state(),
            "error": {
                "code": code,
                "message": message,
                "retryable": False,
                "candidates": candidates or [],
            },
            "diagnostics": [],
        }

    def _matches_entity(self, entity: dict[str, Any], query: str, kind: str | None = None) -> bool:
        if kind and entity.get("kind") != kind:
            return False
        if not query:
            return True
        text = query.casefold()
        attributes = _as_dict(entity.get("attributes"))
        haystack = " ".join(
            [
                str(entity.get("id") or ""),
                str(entity.get("kind") or ""),
                str(entity.get("canonical_key") or ""),
                str(entity.get("display_name") or ""),
                json.dumps(attributes, ensure_ascii=False, sort_keys=True),
            ]
        ).casefold()
        return text in haystack

    def _resolve_entity(self, identifier: str) -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
        needle = (identifier or "").strip()
        if not needle:
            return None, []
        folded = needle.casefold()

        exact_candidates: list[dict[str, Any]] = []
        fuzzy_candidates: list[dict[str, Any]] = []
        for entity in self.entities:
            attributes = _as_dict(entity.get("attributes"))
            exact_values = [
                entity.get("id"),
                entity.get("canonical_key"),
                entity.get("display_name"),
                attributes.get("object_path"),
                attributes.get("package_name"),
                attributes.get("asset_name"),
            ]
            if any(isinstance(value, str) and value.casefold() == folded for value in exact_values):
                exact_candidates.append(entity)
                continue
            if self._matches_entity(entity, needle):
                fuzzy_candidates.append(entity)

        candidates = exact_candidates or fuzzy_candidates
        if not candidates:
            return None, []
        asset_first = sorted(candidates, key=lambda item: (0 if item.get("kind") == "asset" else 1, str(item.get("canonical_key") or "")))
        return asset_first[0], [_short_entity(item) for item in asset_first[:10]]

    def _domain_entities_for_asset(self, asset: dict[str, Any], domain_kinds: set[str], limit: int) -> list[dict[str, Any]]:
        asset_id = str(asset.get("id") or "")
        seen = {asset_id}
        results: list[dict[str, Any]] = []
        queue = deque([asset_id])
        while queue and len(results) < limit:
            current = queue.popleft()
            adjacent = self.outgoing.get(current, []) + self.incoming.get(current, [])
            for relation in adjacent:
                next_id = relation.get("to_id") if relation.get("from_id") == current else relation.get("from_id")
                if not isinstance(next_id, str) or next_id in seen:
                    continue
                seen.add(next_id)
                entity = self.entity_by_id.get(next_id)
                if not entity:
                    continue
                if entity.get("kind") in domain_kinds:
                    results.append(_short_entity(entity, include_snapshot=True))
                queue.append(next_id)
                if len(results) >= limit:
                    break
        return results

    def status(self) -> dict[str, Any]:
        fragment_paths = []
        fragment_kinds = Counter()
        for fragment in _as_list(self.state.manifest.get("fragments")):
            if isinstance(fragment, dict):
                fragment_kinds[str(fragment.get("kind") or "unknown")] += 1
                try:
                    if len(fragment_paths) < 20:
                        fragment_paths.append(str(self.store.resolve_fragment_path(fragment)))
                except SnapshotStoreError as exc:
                    if len(fragment_paths) < 20:
                        fragment_paths.append(f"unreadable: {exc}")
        return self._envelope(
            {
                "ok": True,
                "project_state": "snapshot_ready",
                "store_root": str(self.store.root),
                "manifest_path": str(self.state.manifest_path),
                "schema_version": self.state.manifest.get("schema_version"),
                "generation": self.state.generation,
                "counts": self.counts,
                "manifest_counts": self.state.counts,
                "fragment_kinds": dict(fragment_kinds),
                "fragment_count": sum(fragment_kinds.values()),
                "fragment_path_samples": fragment_paths,
                "llm_readiness": {
                    "can_query_snapshot": bool(self.entities),
                    "requires_daemon": False,
                    "requires_editor_for_reads": False,
                    "can_refresh_without_editor": False,
                },
            }
        )

    def overview(self, limit: int = 20) -> dict[str, Any]:
        entity_kinds = Counter(str(entity.get("kind") or "unknown") for entity in self.entities)
        relation_types = Counter(str(relation.get("type") or "unknown") for relation in self.relations)
        top_assets = [
            _short_entity(entity)
            for entity in self.entities
            if entity.get("kind") == "asset"
        ][:limit]
        return self._envelope(
            {
                "counts": self.counts,
                "entity_kinds": dict(entity_kinds.most_common(limit)),
                "relation_types": dict(relation_types.most_common(limit)),
                "top_assets": top_assets,
            }
        )

    def search(self, query: str = "", kind: str | None = None, limit: int = 20) -> dict[str, Any]:
        limit = max(1, min(int(limit or 20), 100))
        matches = [_short_entity(entity) for entity in self.entities if self._matches_entity(entity, query, kind)][:limit]
        return self._envelope({"query": query, "kind": kind, "matches": matches, "match_count": len(matches)})

    def asset(self, asset: str, include_snapshot: bool = True, relation_limit: int = 80) -> dict[str, Any]:
        entity, candidates = self._resolve_entity(asset)
        if not entity:
            return self._error("UEPI_ASSET_NOT_FOUND", "No indexed entity matched the supplied asset identifier.", candidates)
        entity_id = str(entity.get("id") or "")
        relations = (self.outgoing.get(entity_id, []) + self.incoming.get(entity_id, []))[: max(0, relation_limit)]
        related_ids = {relation.get("from_id") for relation in relations} | {relation.get("to_id") for relation in relations}
        related_ids.discard(entity_id)
        related = [_short_entity(self.entity_by_id[item]) for item in related_ids if item in self.entity_by_id][:relation_limit]
        return self._envelope(
            {
                "entity": _short_entity(entity, include_snapshot=include_snapshot),
                "relations": [_relation_summary(relation) for relation in relations],
                "related_entities": related,
                "resolution_candidates": candidates,
            }
        )

    def blueprint(self, asset: str, limit: int = 200) -> dict[str, Any]:
        entity, candidates = self._resolve_entity(asset)
        if not entity:
            return self._error("UEPI_ASSET_NOT_FOUND", "No indexed Blueprint asset matched the supplied identifier.", candidates)
        domain_entities = self._domain_entities_for_asset(entity, BLUEPRINT_KINDS, max(1, min(limit, 1000)))
        entity_id = str(entity.get("id") or "")
        relation_ids = {item.get("id") for item in domain_entities if item.get("id")}
        relation_ids.add(entity_id)
        relations = [
            _relation_summary(relation)
            for relation in self.relations
            if relation.get("from_id") in relation_ids and relation.get("to_id") in relation_ids
        ][:limit]
        omissions: list[str] = []
        if not domain_entities:
            omissions.append("blueprint_graph_entities_not_present_in_snapshot")
        return self._envelope(
            {
                "asset": _short_entity(entity, include_snapshot=True),
                "blueprint_entities": domain_entities,
                "relations": relations,
                "resolution_candidates": candidates,
            },
            omissions=omissions,
        )

    def blueprint_trace(self, asset: str, start: str | None = None, relation_types: list[str] | None = None, max_depth: int = 8, max_paths: int = 20) -> dict[str, Any]:
        entity, candidates = self._resolve_entity(asset)
        if not entity:
            return self._error("UEPI_ASSET_NOT_FOUND", "No indexed Blueprint asset matched the supplied identifier.", candidates)
        allowed = set(relation_types or ["exec_flows_to", "data_flows_to", "delegate_flows_to", "calls_function"])
        blueprint_entities = self._domain_entities_for_asset(entity, BLUEPRINT_KINDS, 1000)
        domain_ids = {str(item.get("id")) for item in blueprint_entities if item.get("id")}
        if not domain_ids:
            return self._envelope(
                {"paths": [], "resolution_candidates": candidates},
                omissions=["blueprint_graph_entities_not_present_in_snapshot"],
            )

        start_ids = [
            item_id
            for item_id in domain_ids
            if not start or start.casefold() in json.dumps(self.entity_by_id.get(item_id, {}), ensure_ascii=False).casefold()
        ]
        start_ids = start_ids[:max_paths] or list(domain_ids)[:1]
        paths: list[list[dict[str, Any]]] = []
        for start_id in start_ids:
            queue = deque([(start_id, [])])
            visited = {start_id}
            while queue and len(paths) < max_paths:
                current, path = queue.popleft()
                if len(path) >= max_depth:
                    paths.append(path)
                    continue
                for relation in self.outgoing.get(current, []):
                    if relation.get("type") not in allowed:
                        continue
                    next_id = relation.get("to_id")
                    if next_id not in domain_ids or next_id in visited:
                        continue
                    visited.add(str(next_id))
                    step = {
                        "relation": _relation_summary(relation),
                        "to_entity": _short_entity(self.entity_by_id[str(next_id)]),
                    }
                    next_path = path + [step]
                    queue.append((str(next_id), next_path))
                    if next_path:
                        paths.append(next_path)
                    if len(paths) >= max_paths:
                        break
        return self._envelope(
            {
                "asset": _short_entity(entity),
                "start": start,
                "relation_types": sorted(allowed),
                "paths": paths[:max_paths],
                "resolution": "static_snapshot",
            }
        )

    def animation(self, asset: str, include: list[str] | None = None, limit: int = 300) -> dict[str, Any]:
        entity, candidates = self._resolve_entity(asset)
        if not entity:
            return self._error("UEPI_ASSET_NOT_FOUND", "No indexed animation entity matched the supplied identifier.", candidates)
        domain_entities = self._domain_entities_for_asset(entity, ANIMATION_KINDS, max(1, min(limit, 1000)))
        snapshot = _as_dict(entity.get("snapshot"))
        requested = include or ["summary", "tracks", "notifies", "curves", "relations"]
        result = {
            "asset": _short_entity(entity, include_snapshot=True),
            "include": requested,
            "animation_entities": domain_entities,
            "motion_summary": snapshot.get("animation_motion_summary") or snapshot.get("motion_summary"),
            "sequence": snapshot.get("animation_sequence") or (snapshot if snapshot.get("schema_version") == "uepi.anim_sequence.v1" else None),
            "resolution_candidates": candidates,
        }
        omissions: list[str] = []
        if not domain_entities and not snapshot:
            omissions.append("animation_details_not_present_in_snapshot")
        if "pose_samples" in requested and not result["sequence"]:
            omissions.append("pose_samples_require_animation_sequence_snapshot")
        return self._envelope(result, omissions=omissions)

    def impact(self, asset: str, relation_limit: int = 200) -> dict[str, Any]:
        entity, candidates = self._resolve_entity(asset)
        if not entity:
            return self._error("UEPI_ASSET_NOT_FOUND", "No indexed entity matched the supplied asset identifier.", candidates)
        entity_id = str(entity.get("id") or "")
        incoming = self.incoming.get(entity_id, [])[:relation_limit]
        outgoing = self.outgoing.get(entity_id, [])[:relation_limit]
        affected_ids = {relation.get("from_id") for relation in incoming} | {relation.get("to_id") for relation in outgoing}
        affected_ids.discard(entity_id)
        return self._envelope(
            {
                "asset": _short_entity(entity),
                "incoming": [_relation_summary(relation) for relation in incoming],
                "outgoing": [_relation_summary(relation) for relation in outgoing],
                "affected_entities": [_short_entity(self.entity_by_id[item]) for item in affected_ids if item in self.entity_by_id][:relation_limit],
            }
        )

    def diff(self, from_generation: int | None = None, to_generation: int | None = None) -> dict[str, Any]:
        to_generation = int(to_generation or self.state.generation)
        from_generation = int(from_generation or max(1, to_generation - 1))
        if from_generation == to_generation:
            return self._envelope({"from_generation": from_generation, "to_generation": to_generation, "changed": False})

        try:
            from_manifest = _load_json(self.store.versioned_manifest(from_generation))
            to_manifest = _load_json(self.store.versioned_manifest(to_generation))
            from_scan = self.store.load_project_scan(SnapshotState(self.store.root, self.store.versioned_manifest(from_generation), from_manifest))
            to_scan = self.store.load_project_scan(SnapshotState(self.store.root, self.store.versioned_manifest(to_generation), to_manifest))
        except SnapshotStoreError as exc:
            return self._error("UEPI_DIFF_INPUT_MISSING", str(exc))

        def ids(scan: dict[str, Any], key: str) -> set[str]:
            return {str(item.get("id")) for item in _as_list(scan.get(key)) if isinstance(item, dict) and item.get("id")}

        from_entities = ids(from_scan, "entities")
        to_entities = ids(to_scan, "entities")
        from_relations = ids(from_scan, "relations")
        to_relations = ids(to_scan, "relations")
        return self._envelope(
            {
                "from_generation": from_generation,
                "to_generation": to_generation,
                "entities": {
                    "added": sorted(to_entities - from_entities)[:100],
                    "removed": sorted(from_entities - to_entities)[:100],
                    "added_count": len(to_entities - from_entities),
                    "removed_count": len(from_entities - to_entities),
                },
                "relations": {
                    "added": sorted(to_relations - from_relations)[:100],
                    "removed": sorted(from_relations - to_relations)[:100],
                    "added_count": len(to_relations - from_relations),
                    "removed_count": len(from_relations - to_relations),
                },
            }
        )

    def context(self, question: str, scope: list[str] | None = None, max_items: int = 40) -> dict[str, Any]:
        scope = scope or []
        terms = [term for term in question.replace("/", " ").replace("_", " ").split() if len(term) >= 2]
        query = " ".join(terms[:4]) if terms else question
        matches = [entity for entity in self.entities if self._matches_entity(entity, query)]
        if not matches and terms:
            matches = [entity for entity in self.entities if any(self._matches_entity(entity, term) for term in terms[:8])]
        matches = matches[: max(1, min(max_items, 100))]
        related: list[dict[str, Any]] = []
        for entity in matches[:10]:
            entity_id = str(entity.get("id") or "")
            for relation in (self.outgoing.get(entity_id, []) + self.incoming.get(entity_id, []))[:10]:
                related.append(_relation_summary(relation))
        return self._envelope(
            {
                "question": question,
                "scope": scope,
                "interpretation": "static snapshot context",
                "matches": [_short_entity(entity, include_snapshot=False) for entity in matches],
                "relations": related[:max_items],
                "uncertainties": [
                    "Runtime branch conditions, latent action timing, and live unsaved editor changes are not observed in saved snapshot mode."
                ],
            }
        )


def make_engine(project: str | Path | None = None, store: str | Path | None = None, db: str | Path | None = None) -> UEPIQueryEngine:
    return UEPIQueryEngine(SnapshotStore.from_paths(project=project, store=store, db=db))
