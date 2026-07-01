from __future__ import annotations

from collections import Counter, deque
import json
from pathlib import Path
from typing import Any

from .blueprint_semantics import summarize_blueprint_semantics
from .cache import SQLiteSnapshotCache, cache_status, sync_cache
from .bridge_client import bridge_status
from .result import envelope as make_envelope
from .snapshot import SnapshotView
from .store import SnapshotState, SnapshotStore, SnapshotStoreError, _load_json, _parse_utc


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
        "typed_attributes": _as_dict(entity.get("typed_attributes")),
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
        "typed_attributes": _as_dict(relation.get("typed_attributes")),
        "evidence": _as_list(relation.get("evidence")),
    }


class UEPIQueryEngine:
    def __init__(self, store: SnapshotStore):
        self.store = store
        self.snapshot = SnapshotView.open(store)
        self.state: SnapshotState = self.snapshot.state
        self._cache_status = cache_status(self.store, self.state)
        self._cache_sync_result: dict[str, Any] | None = None
        self._cache_sync_error: str | None = None
        if not self._cache_status.get("synced"):
            try:
                self._cache_sync_result = sync_cache(self.store)
            except Exception as exc:
                self._cache_sync_error = str(exc)
        self.cache = SQLiteSnapshotCache.open_if_synced(store, self.state)
        self._cache_status = cache_status(self.store, self.state)
        self._scan: dict[str, Any] | None = None
        self.entities: list[dict[str, Any]] = []
        self.relations: list[dict[str, Any]] = []
        self.counts = dict(self.state.counts)
        if self.cache:
            self.counts.update(self.cache.counts())
        self.counts.setdefault("entities", 0)
        self.counts.setdefault("relations", 0)
        self.counts.setdefault("diagnostics", 0)
        self.counts.setdefault("asset_entities", 0)
        self.entity_by_id: dict[str, dict[str, Any]] = {}
        self.outgoing: dict[str, list[dict[str, Any]]] = {}
        self.incoming: dict[str, list[dict[str, Any]]] = {}

    @property
    def scan(self) -> dict[str, Any]:
        self._ensure_loaded()
        return self._scan or {}

    def _project_id(self) -> Any:
        return self.state.project.get("id") or (self._scan or {}).get("project_id")

    def _project_name(self) -> Any:
        return self.state.project.get("name") or (self._scan or {}).get("project_name")

    def _engine_version(self) -> Any:
        return self.state.project.get("engine_version") or (self._scan or {}).get("engine_version")

    def _ensure_loaded(self) -> None:
        if self._scan is not None:
            return
        self._scan = self.snapshot.load_current_view()
        self.entities = [entity for entity in _as_list(self._scan.get("entities")) if isinstance(entity, dict)]
        self.relations = [relation for relation in _as_list(self._scan.get("relations")) if isinstance(relation, dict)]
        self.counts = {
            "entities": len(self.entities),
            "relations": len(self.relations),
            "diagnostics": len(_as_list(self._scan.get("diagnostics"))),
            "asset_entities": sum(1 for entity in self.entities if entity.get("kind") in {"asset", "asset_redirector"}),
        }
        self.entity_by_id = {entity.get("id"): entity for entity in self.entities if entity.get("id")}
        self.outgoing = {}
        self.incoming = {}
        for relation in self.relations:
            self.outgoing.setdefault(str(relation.get("from_id") or ""), []).append(relation)
            self.incoming.setdefault(str(relation.get("to_id") or ""), []).append(relation)

    def _envelope(
        self,
        result: dict[str, Any],
        diagnostics: list[dict[str, Any]] | None = None,
        omissions: list[str] | None = None,
        freshness: str | None = None,
        truncation: dict[str, Any] | None = None,
        tool: str = "uepi_query",
        operation: str = "query",
        evidence: list[dict[str, Any]] | None = None,
        next_actions: list[dict[str, Any]] | None = None,
    ) -> dict[str, Any]:
        state = self.state.envelope_state()
        if freshness:
            state["freshness"] = freshness
        return make_envelope(
            tool=tool,
            operation=operation,
            project={
                "id": self._project_id(),
                "name": self._project_name(),
                "engine_version": self._engine_version(),
                "project_root": str(self.store.root.parent.parent) if self.store.root.name == "UEProjectIntelligence" else str(self.store.root),
            },
            state=state,
            result=result,
            diagnostics=diagnostics,
            omissions=omissions,
            truncation=truncation,
            evidence=evidence,
            next_actions=next_actions,
        )

    def _error(
        self,
        code: str,
        message: str,
        candidates: list[dict[str, Any]] | None = None,
        diagnostics: list[dict[str, Any]] | None = None,
        freshness: str | None = None,
        tool: str = "uepi_query",
        operation: str = "query",
        next_actions: list[dict[str, Any]] | None = None,
    ) -> dict[str, Any]:
        state = self.state.envelope_state()
        if freshness:
            state["freshness"] = freshness
        return make_envelope(
            tool=tool,
            operation=operation,
            project={
                "id": self._project_id(),
                "name": self._project_name(),
                "engine_version": self._engine_version(),
                "project_root": str(self.store.root.parent.parent) if self.store.root.name == "UEProjectIntelligence" else str(self.store.root),
            },
            state=state,
            error={
                "code": code,
                "message": message,
                "retryable": False,
                "candidates": candidates or [],
            },
            diagnostics=diagnostics,
            next_actions=next_actions,
        )

    def _matches_entity(self, entity: dict[str, Any], query: str, kind: str | None = None) -> bool:
        if kind and entity.get("kind") != kind:
            return False
        if not query:
            return True
        text = query.casefold()
        attributes = _as_dict(entity.get("attributes"))
        typed_attributes = _as_dict(entity.get("typed_attributes"))
        haystack = " ".join(
            [
                str(entity.get("id") or ""),
                str(entity.get("kind") or ""),
                str(entity.get("canonical_key") or ""),
                str(entity.get("display_name") or ""),
                json.dumps(attributes, ensure_ascii=False, sort_keys=True),
                json.dumps(typed_attributes, ensure_ascii=False, sort_keys=True),
            ]
        ).casefold()
        return text in haystack

    def _resolve_entity(self, identifier: str) -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
        needle = (identifier or "").strip()
        if not needle:
            return None, []
        if self.cache:
            entity, candidates = self.cache.resolve_entity(needle)
            return entity, [_short_entity(item) for item in candidates]

        self._ensure_loaded()
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

    def _tombstone_candidate(self, identifier: str) -> list[dict[str, Any]]:
        tombstone = self.snapshot.find_tombstone(identifier)
        if not tombstone:
            return []
        return [
            {
                "kind": "asset_tombstone",
                "asset_key": tombstone.get("asset_key"),
                "asset_name": tombstone.get("asset_name"),
                "asset_id": tombstone.get("asset_id"),
                "reason": tombstone.get("reason"),
                "old_object_path": tombstone.get("old_object_path"),
                "new_object_path": tombstone.get("new_object_path"),
                "created_at_utc": tombstone.get("created_at_utc"),
            }
        ]

    def _freshness_for_identifier(self, identifier: str, tool_name: str) -> tuple[str | None, list[dict[str, Any]]]:
        if not identifier:
            return None, []
        event = self.snapshot.latest_incremental_event(identifier)
        if not event:
            return None, []

        event_time = _parse_utc(event.get("timestamp_utc"))
        observed_at = self.snapshot.observed_at(identifier)
        if event_time is not None and observed_at is not None and event_time <= observed_at:
            return None, []

        tombstone = self.snapshot.find_tombstone(identifier)
        if tombstone and event.get("event_type") in {"asset_removed", "asset_renamed"}:
            tombstone_time = _parse_utc(tombstone.get("created_at_utc"))
            if event_time is None or tombstone_time is None or tombstone_time >= event_time:
                return None, []

        session = self.snapshot.active_editor_session()
        if session:
            request_path = self.snapshot.request_refresh(
                [identifier],
                reason="incremental_event_newer_than_snapshot",
                tool_name=tool_name,
                data_mode="live",
            )
            return "refresh_requested", [
                {
                    "severity": "warning",
                    "code": "UEPI_REFRESH_REQUESTED",
                    "message": "An editor change event is newer than the current snapshot for this target. A targeted refresh request was queued; retry the same read after the editor processes it.",
                    "event": event,
                    "observed_at": observed_at.isoformat().replace("+00:00", "Z") if observed_at else None,
                    "request_path": str(request_path),
                    "editor_session_id": session.get("session_id"),
                }
            ]

        return "stale", [
            {
                "severity": "warning",
                "code": "UEPI_SNAPSHOT_STALE",
                "message": "An editor change event is newer than the current snapshot, but no active editor session is available to service a targeted refresh request.",
                "event": event,
                "observed_at": observed_at.isoformat().replace("+00:00", "Z") if observed_at else None,
            }
        ]

    def _asset_refresh_target(self, entity: dict[str, Any], fallback: str = "") -> str:
        attributes = _as_dict(entity.get("attributes"))
        for value in (
            entity.get("canonical_key"),
            attributes.get("object_path"),
            attributes.get("package_name"),
            entity.get("display_name"),
            fallback,
        ):
            text = str(value or "").strip()
            if text:
                return text
        return ""

    def _domain_refresh_for_missing_snapshot(
        self,
        entity: dict[str, Any],
        *,
        fallback: str,
        tool_name: str,
        reason: str,
        domain: str,
        freshness: str | None,
        diagnostics: list[dict[str, Any]],
    ) -> tuple[str | None, list[dict[str, Any]]]:
        if freshness == "refresh_requested" or any(item.get("code") == "UEPI_REFRESH_REQUESTED" for item in diagnostics):
            return freshness, diagnostics

        target = self._asset_refresh_target(entity, fallback)
        if not target:
            return freshness, diagnostics

        request_path = self.snapshot.request_refresh(
            [target],
            reason=reason,
            tool_name=tool_name,
            data_mode="live",
        )
        session = self.snapshot.active_editor_session()
        message = f"{domain} details are not present in the current snapshot. A targeted editor refresh request was queued; retry the same read after the editor processes it."
        if not session:
            message = f"{domain} details are not present in the current snapshot. A targeted editor refresh request was queued, but no active editor session is available yet; open the editor/plugin and retry after it processes the request."
        return "refresh_requested", diagnostics + [
            {
                "severity": "warning",
                "code": "UEPI_REFRESH_REQUESTED",
                "message": message,
                "target_object_path": target,
                "request_path": str(request_path),
                "editor_session_id": session.get("session_id") if session else None,
            }
        ]

    def _domain_entities_for_asset(self, asset: dict[str, Any], domain_kinds: set[str], limit: int) -> list[dict[str, Any]]:
        asset_id = str(asset.get("id") or "")
        if self.cache:
            return [
                _short_entity(entity, include_snapshot=True)
                for entity in self.cache.domain_entities_for_asset(asset_id, domain_kinds, max(1, int(limit)))
            ]

        self._ensure_loaded()
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
        editor_session = self.snapshot.active_editor_session()
        latest_event = self.snapshot.latest_incremental_event()
        cache = cache_status(self.store, self.state)
        if self._cache_sync_result:
            cache["auto_sync"] = self._cache_sync_result
        if self._cache_sync_error:
            cache["auto_sync_error"] = self._cache_sync_error
        bridge = bridge_status(self.store)
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
                "manifest_counts_scope": self.state.manifest.get("counts_scope") or "manifest",
                "fragment_kinds": dict(fragment_kinds),
                "fragment_count": sum(fragment_kinds.values()),
                "fragment_path_samples": fragment_paths,
                "cache": cache,
                "editor_session": editor_session,
                "incremental_events": {
                    "latest": latest_event,
                    "log_path": str(self.store.logs_dir / "incremental_events.jsonl"),
                },
                "bridge": bridge,
                "refresh_requests": {
                    "pending": self.store.pending_refresh_requests(),
                    "directory": str(self.store.requests_dir),
                },
                "llm_readiness": {
                    "can_query_snapshot": int(self.counts.get("entities") or 0) > 0,
                    "requires_daemon": False,
                    "requires_editor_for_reads": False,
                    "bridge_ready": bool(bridge.get("ready")),
                    "can_refresh_without_editor": False,
                    "can_request_editor_refresh": editor_session is not None,
                    "recommended_flow": [
                        "Call uepi_status first.",
                        "Use uepi_search or uepi_context to identify candidate assets.",
                        "Call the domain-specific read tool only for assets needed by the user question.",
                        "If diagnostics include UEPI_REFRESH_REQUESTED, retry the same read after the editor processes the request.",
                    ],
                },
            },
            tool="uepi_status",
            operation="status",
            next_actions=[
                {
                    "reason": "Build a bounded evidence pack before answering project-specific questions.",
                    "tool": "uepi_context",
                    "arguments": {"question": "<user question>", "max_items": 40},
                }
            ],
        )

    def overview(self, limit: int = 20) -> dict[str, Any]:
        if self.cache:
            entity_kinds = self.cache.entity_kind_counts(limit)
            relation_types = self.cache.relation_type_counts(limit)
            top_assets = [_short_entity(entity) for entity in self.cache.top_assets(limit)]
            query_source = "sqlite_cache"
        else:
            self._ensure_loaded()
            entity_kinds = dict(Counter(str(entity.get("kind") or "unknown") for entity in self.entities).most_common(limit))
            relation_types = dict(Counter(str(relation.get("type") or "unknown") for relation in self.relations).most_common(limit))
            top_assets = [
                _short_entity(entity)
                for entity in self.entities
                if entity.get("kind") == "asset"
            ][:limit]
            query_source = "snapshot_fragments"
        return self._envelope(
            {
                "counts": self.counts,
                "query_source": query_source,
                "entity_kinds": entity_kinds,
                "relation_types": relation_types,
                "top_assets": top_assets,
            },
            tool="uepi_overview",
            operation="project_overview",
            next_actions=[
                {
                    "reason": "Ask uepi_context to choose a route for a concrete project question.",
                    "tool": "uepi_context",
                    "arguments": {"question": "<user question>", "max_items": 40},
                }
            ],
        )

    def search(self, query: str = "", kind: str | None = None, limit: int = 20) -> dict[str, Any]:
        limit = max(1, min(int(limit or 20), 100))
        if self.cache:
            matches = [_short_entity(entity) for entity in self.cache.search_entities(query, kind, limit)]
            query_source = "sqlite_cache"
        else:
            self._ensure_loaded()
            matches = [_short_entity(entity) for entity in self.entities if self._matches_entity(entity, query, kind)][:limit]
            query_source = "snapshot_fragments"
        return self._envelope(
            {"query": query, "kind": kind, "matches": matches, "match_count": len(matches), "query_source": query_source},
            tool="uepi_search",
            operation="entity_search",
        )

    def asset(self, asset: str, include_snapshot: bool = True, relation_limit: int = 80) -> dict[str, Any]:
        freshness, diagnostics = self._freshness_for_identifier(asset, "uepi_asset")
        entity, candidates = self._resolve_entity(asset)
        if not entity:
            tombstone = self._tombstone_candidate(asset)
            if tombstone:
                return self._error(
                    "UEPI_ASSET_TOMBSTONED",
                    "The supplied asset identifier matches a deleted or renamed asset tombstone.",
                    tombstone,
                    diagnostics,
                    freshness,
                    tool="uepi_asset",
                    operation="asset_read",
                )
            return self._error(
                "UEPI_ASSET_NOT_FOUND",
                "No indexed entity matched the supplied asset identifier.",
                candidates,
                diagnostics,
                freshness,
                tool="uepi_asset",
                operation="asset_read",
            )
        entity_id = str(entity.get("id") or "")
        if self.cache:
            relations = self.cache.relations_for_entity(entity_id, max(0, relation_limit))
            related_ids = {relation.get("from_id") for relation in relations} | {relation.get("to_id") for relation in relations}
            related_ids.discard(entity_id)
            related = [_short_entity(item) for item in self.cache.entities_by_ids({str(item) for item in related_ids if item}, relation_limit)]
            query_source = "sqlite_cache"
        else:
            self._ensure_loaded()
            relations = (self.outgoing.get(entity_id, []) + self.incoming.get(entity_id, []))[: max(0, relation_limit)]
            related_ids = {relation.get("from_id") for relation in relations} | {relation.get("to_id") for relation in relations}
            related_ids.discard(entity_id)
            related = [_short_entity(self.entity_by_id[item]) for item in related_ids if item in self.entity_by_id][:relation_limit]
            query_source = "snapshot_fragments"
        return self._envelope(
            {
                "entity": _short_entity(entity, include_snapshot=include_snapshot),
                "relations": [_relation_summary(relation) for relation in relations],
                "related_entities": related,
                "resolution_candidates": candidates,
                "query_source": query_source,
            },
            diagnostics=diagnostics,
            freshness=freshness,
            tool="uepi_asset",
            operation="asset_read",
        )

    def blueprint(self, asset: str, limit: int = 200) -> dict[str, Any]:
        freshness, diagnostics = self._freshness_for_identifier(asset, "uepi_blueprint")
        entity, candidates = self._resolve_entity(asset)
        if not entity:
            tombstone = self._tombstone_candidate(asset)
            if tombstone:
                return self._error(
                    "UEPI_ASSET_TOMBSTONED",
                    "The supplied Blueprint identifier matches a deleted or renamed asset tombstone.",
                    tombstone,
                    diagnostics,
                    freshness,
                    tool="uepi_blueprint",
                    operation="graph_summary",
                )
            return self._error(
                "UEPI_ASSET_NOT_FOUND",
                "No indexed Blueprint asset matched the supplied identifier.",
                candidates,
                diagnostics,
                freshness,
                tool="uepi_blueprint",
                operation="graph_summary",
            )
        domain_entities = self._domain_entities_for_asset(entity, BLUEPRINT_KINDS, max(1, min(limit, 1000)))
        entity_id = str(entity.get("id") or "")
        relation_ids = {item.get("id") for item in domain_entities if item.get("id")}
        relation_ids.add(entity_id)
        if self.cache:
            relations = [_relation_summary(relation) for relation in self.cache.relations_between_ids({str(item) for item in relation_ids if item}, limit)]
            query_source = "sqlite_cache"
        else:
            self._ensure_loaded()
            relations = [
                _relation_summary(relation)
                for relation in self.relations
                if relation.get("from_id") in relation_ids and relation.get("to_id") in relation_ids
            ][:limit]
            query_source = "snapshot_fragments"
        omissions: list[str] = []
        if not domain_entities:
            omissions.append("blueprint_graph_entities_not_present_in_snapshot")
            freshness, diagnostics = self._domain_refresh_for_missing_snapshot(
                entity,
                fallback=asset,
                tool_name="uepi_blueprint",
                reason="blueprint_graph_missing_from_snapshot",
                domain="Blueprint graph",
                freshness=freshness,
                diagnostics=diagnostics,
            )
        semantic_summary = summarize_blueprint_semantics(_short_entity(entity), domain_entities, relations, limit=max(20, min(limit, 120)))
        return self._envelope(
            {
                "asset": _short_entity(entity, include_snapshot=True),
                "blueprint_entities": domain_entities,
                "relations": relations,
                "semantic_summary": semantic_summary,
                "call_graph": semantic_summary["call_graph"],
                "data_mutations": semantic_summary["data_mutations"],
                "resolution_candidates": candidates,
                "query_source": query_source,
            },
            diagnostics=diagnostics,
            omissions=omissions,
            freshness=freshness,
            tool="uepi_blueprint",
            operation="graph_summary",
            next_actions=[
                {
                    "reason": "Trace static execution/data flow from a specific entrypoint when the user asks what happens next.",
                    "tool": "uepi_blueprint_trace",
                    "arguments": {"asset": asset, "start": "<entrypoint title or node id>"},
                },
                {
                    "reason": "Check incoming/outgoing project impact if the Blueprint may be changed later.",
                    "tool": "uepi_impact",
                    "arguments": {"asset": asset},
                },
            ],
        )

    def blueprint_trace(self, asset: str, start: str | None = None, relation_types: list[str] | None = None, max_depth: int = 8, max_paths: int = 20) -> dict[str, Any]:
        freshness, diagnostics = self._freshness_for_identifier(asset, "uepi_blueprint_trace")
        entity, candidates = self._resolve_entity(asset)
        if not entity:
            tombstone = self._tombstone_candidate(asset)
            if tombstone:
                return self._error(
                    "UEPI_ASSET_TOMBSTONED",
                    "The supplied Blueprint identifier matches a deleted or renamed asset tombstone.",
                    tombstone,
                    diagnostics,
                    freshness,
                    tool="uepi_blueprint_trace",
                    operation="static_flow_trace",
                )
            return self._error(
                "UEPI_ASSET_NOT_FOUND",
                "No indexed Blueprint asset matched the supplied identifier.",
                candidates,
                diagnostics,
                freshness,
                tool="uepi_blueprint_trace",
                operation="static_flow_trace",
            )
        allowed = set(relation_types or ["exec_flows_to", "data_flows_to", "delegate_flows_to", "calls_function"])
        blueprint_entities = self._domain_entities_for_asset(entity, BLUEPRINT_KINDS, 1000)
        domain_ids = {str(item.get("id")) for item in blueprint_entities if item.get("id")}
        if not domain_ids:
            freshness, diagnostics = self._domain_refresh_for_missing_snapshot(
                entity,
                fallback=asset,
                tool_name="uepi_blueprint_trace",
                reason="blueprint_trace_missing_from_snapshot",
                domain="Blueprint graph",
                freshness=freshness,
                diagnostics=diagnostics,
            )
            return self._envelope(
                {"paths": [], "resolution_candidates": candidates},
                diagnostics=diagnostics,
                omissions=["blueprint_graph_entities_not_present_in_snapshot"],
                freshness=freshness,
                tool="uepi_blueprint_trace",
                operation="static_flow_trace",
            )
        if not self.cache:
            self._ensure_loaded()

        start_ids = [
            item_id
            for item_id in domain_ids
            if not start or start.casefold() in json.dumps(
                (self.cache.entity_by_id(item_id, include_snapshot=True) if self.cache else self.entity_by_id.get(item_id, {})),
                ensure_ascii=False,
            ).casefold()
        ]
        start_ids = start_ids[:max_paths] or list(domain_ids)[:1]
        paths: list[list[dict[str, Any]]] = []
        domain_by_id = {str(item.get("id")): item for item in blueprint_entities if item.get("id")}
        for start_id in start_ids:
            queue = deque([(start_id, [])])
            visited = {start_id}
            while queue and len(paths) < max_paths:
                current, path = queue.popleft()
                if len(path) >= max_depth:
                    paths.append(path)
                    continue
                outgoing = self.cache.outgoing_relations(current, allowed, limit=1000) if self.cache else self.outgoing.get(current, [])
                for relation in outgoing:
                    if relation.get("type") not in allowed:
                        continue
                    next_id = relation.get("to_id")
                    if next_id not in domain_ids or next_id in visited:
                        continue
                    visited.add(str(next_id))
                    to_entity = domain_by_id.get(str(next_id))
                    if to_entity is None and self.cache:
                        cached_entity = self.cache.entity_by_id(str(next_id))
                        to_entity = _short_entity(cached_entity) if cached_entity else {}
                    elif to_entity is None:
                        to_entity = _short_entity(self.entity_by_id[str(next_id)])
                    step = {
                        "relation": _relation_summary(relation),
                        "to_entity": to_entity,
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
                "query_source": "sqlite_cache" if self.cache else "snapshot_fragments",
            },
            diagnostics=diagnostics,
            freshness=freshness,
            tool="uepi_blueprint_trace",
            operation="static_flow_trace",
        )

    def animation(self, asset: str, include: list[str] | None = None, limit: int = 300) -> dict[str, Any]:
        freshness, diagnostics = self._freshness_for_identifier(asset, "uepi_animation")
        entity, candidates = self._resolve_entity(asset)
        if not entity:
            tombstone = self._tombstone_candidate(asset)
            if tombstone:
                return self._error(
                    "UEPI_ASSET_TOMBSTONED",
                    "The supplied animation identifier matches a deleted or renamed asset tombstone.",
                    tombstone,
                    diagnostics,
                    freshness,
                    tool="uepi_animation",
                    operation="animation_summary",
                )
            return self._error(
                "UEPI_ASSET_NOT_FOUND",
                "No indexed animation entity matched the supplied identifier.",
                candidates,
                diagnostics,
                freshness,
                tool="uepi_animation",
                operation="animation_summary",
            )
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
            "query_source": "sqlite_cache" if self.cache else "snapshot_fragments",
        }
        omissions: list[str] = []
        if not domain_entities and not snapshot:
            omissions.append("animation_details_not_present_in_snapshot")
            freshness, diagnostics = self._domain_refresh_for_missing_snapshot(
                entity,
                fallback=asset,
                tool_name="uepi_animation",
                reason="animation_details_missing_from_snapshot",
                domain="Animation",
                freshness=freshness,
                diagnostics=diagnostics,
            )
        if "pose_samples" in requested and not result["sequence"]:
            omissions.append("pose_samples_require_animation_sequence_snapshot")
        return self._envelope(
            result,
            diagnostics=diagnostics,
            omissions=omissions,
            freshness=freshness,
            tool="uepi_animation",
            operation="animation_summary",
            next_actions=[
                {
                    "reason": "Check Blueprint or AnimBP references that may play this animation.",
                    "tool": "uepi_impact",
                    "arguments": {"asset": asset},
                }
            ],
        )

    def impact(self, asset: str, relation_limit: int = 200) -> dict[str, Any]:
        freshness, diagnostics = self._freshness_for_identifier(asset, "uepi_impact")
        entity, candidates = self._resolve_entity(asset)
        if not entity:
            tombstone = self._tombstone_candidate(asset)
            if tombstone:
                return self._error(
                    "UEPI_ASSET_TOMBSTONED",
                    "The supplied asset identifier matches a deleted or renamed asset tombstone.",
                    tombstone,
                    diagnostics,
                    freshness,
                    tool="uepi_impact",
                    operation="dependency_impact",
                )
            return self._error(
                "UEPI_ASSET_NOT_FOUND",
                "No indexed entity matched the supplied asset identifier.",
                candidates,
                diagnostics,
                freshness,
                tool="uepi_impact",
                operation="dependency_impact",
            )
        entity_id = str(entity.get("id") or "")
        if self.cache:
            adjacent = self.cache.relations_for_entity(entity_id, relation_limit * 2)
            incoming = [relation for relation in adjacent if relation.get("to_id") == entity_id][:relation_limit]
            outgoing = [relation for relation in adjacent if relation.get("from_id") == entity_id][:relation_limit]
            affected_ids = {relation.get("from_id") for relation in incoming} | {relation.get("to_id") for relation in outgoing}
            affected_ids.discard(entity_id)
            affected = [_short_entity(item) for item in self.cache.entities_by_ids({str(item) for item in affected_ids if item}, relation_limit)]
            query_source = "sqlite_cache"
        else:
            self._ensure_loaded()
            incoming = self.incoming.get(entity_id, [])[:relation_limit]
            outgoing = self.outgoing.get(entity_id, [])[:relation_limit]
            affected_ids = {relation.get("from_id") for relation in incoming} | {relation.get("to_id") for relation in outgoing}
            affected_ids.discard(entity_id)
            affected = [_short_entity(self.entity_by_id[item]) for item in affected_ids if item in self.entity_by_id][:relation_limit]
            query_source = "snapshot_fragments"
        return self._envelope(
            {
                "asset": _short_entity(entity),
                "incoming": [_relation_summary(relation) for relation in incoming],
                "outgoing": [_relation_summary(relation) for relation in outgoing],
                "affected_entities": affected,
                "query_source": query_source,
            },
            diagnostics=diagnostics,
            freshness=freshness,
            tool="uepi_impact",
            operation="dependency_impact",
        )

    def diff(self, from_generation: int | None = None, to_generation: int | None = None) -> dict[str, Any]:
        to_generation = int(to_generation or self.state.generation)
        from_generation = int(from_generation or max(1, to_generation - 1))
        if from_generation == to_generation:
            return self._envelope(
                {"from_generation": from_generation, "to_generation": to_generation, "changed": False},
                tool="uepi_diff",
                operation="generation_diff",
            )

        try:
            from_manifest = _load_json(self.store.versioned_manifest(from_generation))
            to_manifest = _load_json(self.store.versioned_manifest(to_generation))
            from_scan = self.store.load_project_scan(SnapshotState(self.store.root, self.store.versioned_manifest(from_generation), from_manifest))
            to_scan = self.store.load_project_scan(SnapshotState(self.store.root, self.store.versioned_manifest(to_generation), to_manifest))
        except SnapshotStoreError as exc:
            return self._error("UEPI_DIFF_INPUT_MISSING", str(exc), tool="uepi_diff", operation="generation_diff")

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
            },
            tool="uepi_diff",
            operation="generation_diff",
        )

    def context(self, question: str, scope: list[str] | None = None, max_items: int = 40, route: str = "auto") -> dict[str, Any]:
        from .context_ranker import select_route
        from .context_routes import make_routes

        scope = scope or []
        limit = max(1, min(max_items, 100))
        routes = make_routes()
        project_summary = {"counts": self.counts, "project": self.state.project}
        selected_route, route_confidence, terms = select_route(routes, question, project_summary)

        requested_route = route if route and route != "auto" else ""
        if not requested_route:
            requested_route = next((item for item in scope if any(item == candidate.name for candidate in routes)), "")
        if requested_route:
            selected_route = next((candidate for candidate in routes if candidate.name == requested_route), selected_route)
            route_confidence = max(route_confidence, 0.95)

        pack = selected_route.build(
            self,
            question,
            {
                "scope": scope,
                "max_items": limit,
                "terms": terms,
                "route_confidence": route_confidence,
            },
        )
        result = pack.to_result(scope, limit)
        result["available_routes"] = [candidate.name for candidate in routes]
        result["terms"] = terms[:20]
        return self._envelope(
            result,
            evidence=pack.evidence,
            next_actions=pack.next_actions,
            tool="uepi_context",
            operation=f"context_route:{pack.route}",
        )


def make_engine(project: str | Path | None = None, store: str | Path | None = None, db: str | Path | None = None) -> UEPIQueryEngine:
    return UEPIQueryEngine(SnapshotStore.from_paths(project=project, store=store, db=db))
