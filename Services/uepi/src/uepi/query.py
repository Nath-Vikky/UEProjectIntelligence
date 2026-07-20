from __future__ import annotations

from collections import Counter, deque
import json
from pathlib import Path
import time
from typing import Any

from .blueprint_semantics import summarize_blueprint_semantics
from .cache import SQLiteSnapshotCache, cache_status, sync_cache
from .cpp_symbols import project_module_manifest, scan_cpp_symbols
from .bridge_client import bridge_status, call_bridge, live_context
from .identity import project_guard_diagnostics, project_identity
from .refresh import request_payload
from .status import resolve_status
from .result import envelope as make_envelope
from .snapshot import SnapshotView
from .store import SnapshotState, SnapshotStore, SnapshotStoreError, _load_json, _parse_utc


BLUEPRINT_KINDS = {
    "blueprint_graph",
    "blueprint_node",
    "blueprint_pin",
    "blueprint_event",
    "anim_slot",
    "anim_state_machine",
    "anim_state",
    "anim_transition",
    "anim_asset_player",
    "cfg_basic_block",
    "dfg_value",
}

BLUEPRINT_SCOPE_RELATIONS = {
    "contains_graph",
    "contains_node",
    "has_pin",
    "contains_basic_block",
    "defines_value",
    "uses_value",
    "overrides_event",
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


def typed_attribute_value(entity: dict[str, Any], key: str, default: Any = None) -> Any:
    typed = _as_dict(entity.get("typed_attributes"))
    if key in typed:
        wrapped = typed[key]
        if isinstance(wrapped, dict) and "value" in wrapped:
            return wrapped["value"]
        if not isinstance(wrapped, dict):
            return wrapped
    attributes = _as_dict(entity.get("attributes"))
    return attributes.get(key, default)


def _first_attribute_value(entity: dict[str, Any], *keys: str) -> Any:
    for key in keys:
        value = typed_attribute_value(entity, key)
        if value not in (None, ""):
            return value
    return None


def _graph_name_matches(value: Any, requested: str) -> bool:
    candidate = str(value or "").strip().replace("\\", "/").casefold()
    expected = requested.strip().casefold()
    if candidate == expected:
        return True
    graph_suffix = candidate.rsplit(":", 1)[-1]
    return graph_suffix == expected or graph_suffix.split(".", 1)[0] == expected


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


def _bounded_context_value(value: Any, *, depth: int = 0, list_limit: int = 12) -> Any:
    if depth >= 4:
        return None if isinstance(value, (dict, list)) else value
    if isinstance(value, list):
        return [_bounded_context_value(item, depth=depth + 1, list_limit=list_limit) for item in value[:list_limit]]
    if isinstance(value, dict):
        result: dict[str, Any] = {}
        for key, child in value.items():
            compact = _bounded_context_value(child, depth=depth + 1, list_limit=list_limit)
            if compact not in (None, [], {}):
                result[key] = compact
            if isinstance(child, list) and len(child) > list_limit:
                result[f"{key}_total_count"] = len(child)
                result[f"{key}_truncated"] = True
        return result
    if isinstance(value, str) and len(value) > 1024:
        return value[:1024]
    return value


def _animation_context_sequence(sequence: dict[str, Any] | None) -> dict[str, Any] | None:
    if not sequence:
        return None
    scalar_fields = (
        "schema_version",
        "source_layer",
        "sequence_path",
        "skeleton_path",
        "play_length_seconds",
        "sample_key_count",
        "sampling_frame_rate",
        "frame_count",
        "data_model_key_count",
        "rate_scale",
        "loop",
        "additive_anim_type",
        "additive_base_pose_type",
        "additive_ref_frame_index",
        "additive_ref_pose_sequence_path",
        "valid_additive",
        "root_motion_enabled",
        "root_motion_root_lock",
        "force_root_lock",
        "use_normalized_root_motion_scale",
        "bone_track_count",
        "float_curve_count",
        "transform_curve_count",
        "attribute_count",
        "notify_count",
        "sampled_pose_count",
    )
    result = {key: sequence[key] for key in scalar_fields if key in sequence}
    for key, limit in (("tracks", 24), ("notifies", 12)):
        values = sequence.get(key)
        if isinstance(values, list):
            result[key] = [_bounded_context_value(item, list_limit=8) for item in values[:limit]]
            result[f"{key}_returned_count"] = min(len(values), limit)
            result[f"{key}_total_count"] = len(values)
            result[f"{key}_truncated"] = len(values) > limit
    for key in ("bone_motion_profile", "reconstruction_profile"):
        if isinstance(sequence.get(key), dict):
            result[key] = _bounded_context_value(sequence[key], list_limit=4)
    return result


_FOCUSED_BLUEPRINT_ATTRIBUTE_KEYS = {
    "object_path",
    "asset_path",
    "package_name",
    "graph_name",
    "graph_path",
    "graph_role",
    "node_guid",
    "node_title",
    "node_class",
    "node_class_path",
    "semantic_kind",
    "semantic_event",
    "semantic_function",
    "semantic_variable",
    "semantic_input_key",
    "input_key",
    "event_name",
    "function_name",
    "pin_id",
    "pin_name",
    "direction",
    "pin_category",
    "pin_subcategory",
    "pin_subcategory_object",
    "pin_container_type",
    "default_value",
    "default_object",
    "default_text",
    "autogenerated_default_value",
    "is_reference",
    "is_const",
    "is_weak_pointer",
    "is_uobject_wrapper",
    "is_self_pin",
    "slot_name",
    "state_name",
    "animation_asset",
}


def _focused_blueprint_entity(entity: dict[str, Any]) -> dict[str, Any]:
    attributes = _as_dict(entity.get("attributes"))
    return {
        "id": entity.get("id"),
        "kind": entity.get("kind"),
        "canonical_key": entity.get("canonical_key"),
        "display_name": entity.get("display_name"),
        "source_layer": entity.get("source_layer"),
        "attributes": {
            key: _bounded_context_value(value, list_limit=8)
            for key, value in attributes.items()
            if key in _FOCUSED_BLUEPRINT_ATTRIBUTE_KEYS and value not in (None, "", [], {})
        },
    }


def _focused_relation(relation: dict[str, Any]) -> dict[str, Any]:
    result = {
        key: relation.get(key)
        for key in ("id", "type", "from_id", "to_id", "source_layer", "derived", "confidence")
        if relation.get(key) is not None
    }
    attributes = _as_dict(relation.get("attributes"))
    if attributes:
        result["attributes"] = _bounded_context_value(attributes, list_limit=8)
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


def _path_is_relative_to(child: Path, parent: Path) -> bool:
    try:
        child.relative_to(parent)
        return True
    except ValueError:
        return False


class UEPIQueryEngine:
    def __init__(self, store: SnapshotStore, configured_project: str | Path | None = None):
        self.store = store
        self.snapshot = SnapshotView.open(store)
        self.state: SnapshotState = self.snapshot.state
        self.identity = project_identity(configured_project, self.state.project, self.store.root)
        self.identity_diagnostics: list[dict[str, Any]] = []
        snapshot_file = self.state.project.get("project_file")
        if configured_project and snapshot_file:
            configured_identity = project_identity(configured_project, {}, self.store.root)
            snapshot_identity = project_identity(snapshot_file, self.state.project, self.store.root)
            if configured_identity.get("project_binding_id") != snapshot_identity.get("project_binding_id"):
                self.identity_diagnostics.append(
                    {
                        "severity": "error",
                        "blocking": True,
                        "code": "UEPI_PROJECT_BINDING_MISMATCH",
                        "message": "The configured MCP project does not match the Snapshot project identity.",
                        "phase": "request_guard",
                        "retryable": False,
                        "recoverable": True,
                    }
                )
        self._cache_status = cache_status(self.store, self.state)
        self._cache_sync_result: dict[str, Any] | None = None
        self._cache_sync_error: str | None = None
        if not self._cache_status.get("synced"):
            try:
                self._cache_sync_result = sync_cache(self.store, self.state)
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

    def close(self) -> None:
        cache = self.cache
        self.cache = None
        if cache is not None:
            cache.close()

    def __enter__(self) -> "UEPIQueryEngine":
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self.close()

    def _cache_diagnostics(self) -> list[dict[str, Any]]:
        if not self._cache_sync_error:
            return []
        return [
            {
                "severity": "warning",
                "blocking": False,
                "code": "UEPI_CACHE_SYNC_FAILED",
                "message": f"SQLite cache synchronization failed; this request fell back to Snapshot fragments: {self._cache_sync_error}",
                "phase": "cache_sync",
                "retryable": True,
                "recoverable": True,
            }
        ]

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

    def _reload_current_state(self) -> None:
        self.close()
        self.snapshot = SnapshotView.open(self.store)
        self.state = self.snapshot.state
        self._cache_sync_result = None
        self._cache_sync_error = None
        self._cache_status = cache_status(self.store, self.state)
        if not self._cache_status.get("synced"):
            try:
                self._cache_sync_result = sync_cache(self.store, self.state)
            except Exception as exc:
                self._cache_sync_error = str(exc)
        self.cache = SQLiteSnapshotCache.open_if_synced(self.store, self.state)
        self._cache_status = cache_status(self.store, self.state)
        self._scan = None
        self.entities = []
        self.relations = []
        self.entity_by_id = {}
        self.outgoing = {}
        self.incoming = {}
        self.counts = dict(self.state.counts)
        if self.cache:
            self.counts.update(self.cache.counts())
        self.counts.setdefault("entities", 0)
        self.counts.setdefault("relations", 0)
        self.counts.setdefault("diagnostics", 0)
        self.counts.setdefault("asset_entities", 0)

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
        status = resolve_status(self.store, self.state, self.identity, probe_bridge=False)
        return make_envelope(
            tool=tool,
            operation=operation,
            project=self.identity,
            editor=status["editor"],
            state=state,
            result=result,
            diagnostics=self.identity_diagnostics + self._cache_diagnostics() + status["diagnostics"] + (diagnostics or []),
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
        result: dict[str, Any] | None = None,
        diagnostics: list[dict[str, Any]] | None = None,
        freshness: str | None = None,
        tool: str = "uepi_query",
        operation: str = "query",
        next_actions: list[dict[str, Any]] | None = None,
    ) -> dict[str, Any]:
        state = self.state.envelope_state()
        if freshness:
            state["freshness"] = freshness
        status = resolve_status(self.store, self.state, self.identity, probe_bridge=False)
        return make_envelope(
            tool=tool,
            operation=operation,
            project=self.identity,
            editor=status["editor"],
            state=state,
            error={
                "code": code,
                "message": message,
                "retryable": False,
                "candidates": candidates or [],
            },
            result=result,
            diagnostics=self.identity_diagnostics + self._cache_diagnostics() + status["diagnostics"] + (diagnostics or []),
            next_actions=next_actions,
        )

    def _load_artifact_payload(self, manifest: dict[str, Any], *, expected_schema: str, diagnostic_code: str) -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
        path_text = str(manifest.get("path") or "").strip()
        if not path_text:
            return None, [
                {
                    "severity": "warning",
                    "code": diagnostic_code,
                    "message": "Artifact manifest did not include a readable path.",
                    "recoverable": False,
                }
            ]

        path = Path(path_text).expanduser()
        if not path.is_absolute():
            path = self.store.root / path
        try:
            resolved = path.resolve()
            allowed_roots = [
                (self.store.store_dir / "artifacts").resolve(),
                (self.store.root / "Artifacts").resolve(),
                (self.store.root / "artifacts").resolve(),
            ]
        except OSError as exc:
            return None, [
                {
                    "severity": "warning",
                    "code": diagnostic_code,
                    "message": f"Artifact path could not be resolved: {exc}",
                    "path": path_text,
                    "recoverable": False,
                }
            ]

        if not any(_path_is_relative_to(resolved, root) for root in allowed_roots):
            return None, [
                {
                    "severity": "warning",
                    "code": diagnostic_code,
                    "message": "Artifact path is outside the UEPI artifact directories and was not read.",
                    "path": str(resolved),
                    "recoverable": False,
                }
            ]

        try:
            payload = _load_json(resolved)
        except SnapshotStoreError as exc:
            return None, [
                {
                    "severity": "warning",
                    "code": diagnostic_code,
                    "message": str(exc),
                    "path": str(resolved),
                    "recoverable": True,
                    "recommended_agent_action": {"tool": "uepi_animation"},
                }
            ]

        if expected_schema and payload.get("schema_version") != expected_schema:
            return None, [
                {
                    "severity": "warning",
                    "code": diagnostic_code,
                    "message": f"Artifact schema mismatch: expected {expected_schema}, got {payload.get('schema_version')!r}.",
                    "path": str(resolved),
                    "recoverable": False,
                }
            ]

        return payload, []

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

    def _resolve_entity_with_exact(self, identifier: str, exact: bool) -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
        entity, candidates = self._resolve_entity(identifier)
        if not entity or not exact:
            return entity, candidates
        folded = str(identifier or "").strip().casefold()
        attributes = _as_dict(entity.get("attributes"))
        exact_values = {
            str(entity.get("id") or "").casefold(),
            str(entity.get("canonical_key") or "").casefold(),
            str(attributes.get("object_path") or "").casefold(),
            str(attributes.get("package_name") or "").casefold(),
        }
        if folded in exact_values:
            return entity, candidates
        return None, candidates

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
        observed_at = _parse_utc(self.cache.observed_at_for_identifier(identifier)) if self.cache else None
        if observed_at is None:
            observed_at = self.snapshot.observed_at(identifier)
        if event_time is not None and observed_at is not None and event_time <= observed_at:
            return None, []

        tombstone = self.snapshot.find_tombstone(identifier) if event.get("event_type") in {"asset_removed", "asset_renamed"} else None
        if tombstone:
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
            refresh = request_payload(request_path)
            return "refresh_requested", [
                {
                    "severity": "warning",
                    "code": "UEPI_REFRESH_REQUESTED",
                    "message": "An editor change event is newer than the current snapshot for this target. A targeted refresh request was queued; retry the same read after the editor processes it.",
                    "event": event,
                    "observed_at": observed_at.isoformat().replace("+00:00", "Z") if observed_at else None,
                    "request_path": str(request_path),
                    "request_id": refresh["request_id"],
                    "request": refresh["request"],
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
        refresh = request_payload(request_path)
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
                "request_id": refresh["request_id"],
                "request": refresh["request"],
                "editor_session_id": session.get("session_id") if session else None,
            }
        ]

    def _force_bridge_refresh(
        self,
        target: str,
        *,
        diagnostics: list[dict[str, Any]],
        freshness: str | None,
    ) -> tuple[str | None, list[dict[str, Any]]]:
        next_freshness, next_diagnostics, _ = self._force_bridge_refresh_many(
            [target], diagnostics=diagnostics, freshness=freshness
        )
        return next_freshness, next_diagnostics

    def _force_bridge_refresh_many(
        self,
        targets: list[str],
        *,
        diagnostics: list[dict[str, Any]],
        freshness: str | None,
    ) -> tuple[str | None, list[dict[str, Any]], dict[str, Any]]:
        clean_targets = list(dict.fromkeys(str(item or "").strip() for item in targets if str(item or "").strip()))
        previous_generation = self.state.generation
        summary: dict[str, Any] = {
            "requested_targets": clean_targets,
            "requested_assets": clean_targets,
            "refreshed_targets": [],
            "refreshed_assets": [],
            "failed_targets": [],
            "failed_assets": [],
            "previous_generation": previous_generation,
            "new_generation": previous_generation,
            "atomic": True,
        }
        if not clean_targets:
            return freshness, diagnostics, summary
        response = call_bridge(self.store, "asset.refresh_now", {"target_object_paths": clean_targets, "data_mode": "live"}, timeout=2.0)
        if response.get("ok"):
            result = _as_dict(response.get("result"))
            request_id = str(result.get("request_id") or "")
            deadline = time.monotonic() + 30.0
            completed: dict[str, Any] | None = None
            while request_id and time.monotonic() < deadline:
                for candidate in self.store.requests_dir.glob("*.json"):
                    try:
                        value = json.loads(candidate.read_text(encoding="utf-8-sig"))
                    except (OSError, json.JSONDecodeError):
                        continue
                    if str(value.get("request_id") or "") != request_id:
                        continue
                    status = str(value.get("status") or "").casefold()
                    if status in {"succeeded", "completed", "failed", "cancelled", "expired", "aborted"}:
                        completed = {**value, "request_path": str(candidate)}
                        break
                if completed:
                    break
                time.sleep(0.1)
            if completed and str(completed.get("status") or "").casefold() in {"succeeded", "completed"}:
                self._reload_current_state()
                summary.update(
                    {
                        "request_id": request_id,
                        "request_path": completed.get("request_path"),
                        "refreshed_targets": clean_targets,
                        "refreshed_assets": clean_targets,
                        "new_generation": self.state.generation,
                        "status": str(completed.get("status") or "succeeded"),
                    }
                )
                return "current", diagnostics + [
                    {
                        "severity": "info",
                        "blocking": False,
                        "code": "UEPI_REFRESH_COMPLETED",
                        "message": "The forced Editor refresh completed and this query was retried against the published generation.",
                        "target_object_paths": clean_targets,
                        "request_id": request_id,
                        "request_path": completed.get("request_path"),
                        "previous_generation": previous_generation,
                        "view_generation": self.state.generation,
                        "recoverable": True,
                    }
                ], summary
            summary.update(
                {
                    "request_id": request_id,
                    "request_path": (completed or {}).get("request_path") or result.get("request_path"),
                    "failed_targets": clean_targets,
                    "failed_assets": clean_targets,
                    "status": str((completed or {}).get("status") or "wait_timeout"),
                    "error": (completed or {}).get("error"),
                }
            )
            return "refresh_requested", diagnostics + [
                {
                    "severity": "info",
                    "code": "UEPI_REFRESH_REQUESTED",
                    "message": "A live editor bridge refresh request was queued but did not complete before the atomic refresh timeout.",
                    "target_object_paths": clean_targets,
                    "request_path": result.get("request_path"),
                    "request_id": request_id,
                    "request": result.get("request") or {},
                    "recoverable": True,
                    "recommended_user_action": "Keep the editor open and retry after UEPI processes the refresh request.",
                    "recommended_agent_action": {
                        "tool": "uepi_refresh" if result.get("request_id") else "uepi_status",
                        "arguments": {"action": "wait", "request_id": result.get("request_id")} if result.get("request_id") else {},
                        "after_seconds": 0 if result.get("request_id") else 2,
                    },
                }
            ], summary
        error = _as_dict(response.get("error"))
        summary.update({"failed_targets": clean_targets, "failed_assets": clean_targets, "status": "bridge_unavailable", "error": error.get("message")})
        return freshness, diagnostics + [
            {
                "severity": "warning",
                "code": error.get("code") or "UEPI_BRIDGE_REFRESH_UNAVAILABLE",
                "message": error.get("message") or "The optional live editor bridge could not queue a forced refresh.",
                "recoverable": True,
                "recommended_user_action": "Open or restart the editor so the UEPI live bridge can start, or use Run Snapshot Scan.",
                "recommended_agent_action": {"tool": "uepi_status"},
            }
        ], summary

    def _domain_entities_for_asset(
        self,
        asset: dict[str, Any],
        domain_kinds: set[str],
        limit: int,
        relation_types: set[str] | None = None,
    ) -> list[dict[str, Any]]:
        asset_id = str(asset.get("id") or "")
        if self.cache:
            return [
                _short_entity(entity, include_snapshot=True)
                for entity in self.cache.domain_entities_for_asset(
                    asset_id,
                    domain_kinds,
                    max(1, int(limit)),
                    relation_types=relation_types,
                )
            ]

        self._ensure_loaded()
        seen = {asset_id}
        results: list[dict[str, Any]] = []
        queue = deque([asset_id])
        while queue and len(results) < limit:
            current = queue.popleft()
            adjacent = self.outgoing.get(current, []) + self.incoming.get(current, [])
            for relation in adjacent:
                if relation_types and str(relation.get("type") or "") not in relation_types:
                    continue
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

    def status(self, expected_project_file: str | Path | None = None, expected_editor_session_id: str | None = None) -> dict[str, Any]:
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
        latest_event = self.snapshot.latest_incremental_event()
        cache = cache_status(self.store, self.state)
        if self._cache_sync_result:
            cache["auto_sync"] = self._cache_sync_result
        if self._cache_sync_error:
            cache["auto_sync_error"] = self._cache_sync_error
        resolved = resolve_status(
            self.store,
            self.state,
            self.identity,
            probe_bridge=True,
            expected_editor_session_id=expected_editor_session_id,
        )
        guard_diagnostics = project_guard_diagnostics(self.identity, expected_project_file=expected_project_file)
        status_state = {**self.state.envelope_state(), "freshness": resolved["snapshot"]["freshness"]}
        recovery_next_actions = []
        if resolved["editor"].get("recovery_required"):
            recovery_next_actions.append(
                {
                    "reason": "Inspect unresolved transaction markers and disk/backup fingerprints before any new mutation.",
                    "tool": "uepi_recovery_inspect",
                    "arguments": {},
                }
            )
        return make_envelope(
            tool="uepi_status",
            operation="status",
            project=self.identity,
            editor=resolved["editor"],
            state=status_state,
            diagnostics=self.identity_diagnostics + self._cache_diagnostics() + guard_diagnostics + resolved["diagnostics"],
            result={
                "ok": True,
                "project_state": "snapshot_ready",
                "store_root": str(self.store.root),
                "manifest_path": str(self.state.manifest_path),
                "schema_version": self.state.manifest.get("schema_version"),
                "generation": self.state.generation,
                "plugin_version": resolved["doctor"].get("plugin_version"),
                "plugin_build_id": resolved["doctor"].get("plugin_build_id"),
                "catalog_hash": resolved["doctor"].get("catalog_hash"),
                "service_version": resolved["doctor"].get("service_version"),
                "service_build_id": resolved["doctor"].get("service_build_id"),
                "service_source_hash": resolved["doctor"].get("service_source_hash"),
                "service_disk_source_hash": resolved["doctor"].get("service_disk_source_hash"),
                "service_restart_required": resolved["doctor"].get("service_restart_required"),
                "service_process_start_time": resolved["doctor"].get("service_process_start_time"),
                "service_process_id": resolved["doctor"].get("service_process_id"),
                "service_loaded_module_path": resolved["doctor"].get("service_loaded_module_path"),
                "counts": self.counts,
                "manifest_counts": self.state.counts,
                "manifest_counts_scope": self.state.manifest.get("counts_scope") or "manifest",
                "fragment_kinds": dict(fragment_kinds),
                "fragment_count": sum(fragment_kinds.values()),
                "fragment_path_samples": fragment_paths,
                "cache": cache,
                "incremental_events": {
                    "latest": latest_event,
                    "log_path": str(self.store.logs_dir / "incremental_events.jsonl"),
                },
                "refresh_requests": {
                    "pending": self.store.pending_refresh_requests(),
                    "directory": str(self.store.requests_dir),
                },
                "llm_readiness": {
                    "can_query_snapshot": int(self.counts.get("entities") or 0) > 0,
                    "requires_daemon": False,
                    "requires_editor_for_reads": False,
                    "bridge_ready": bool(resolved["editor"].get("connected")),
                    "can_refresh_without_editor": False,
                    "can_request_editor_refresh": bool(resolved["editor"].get("connected")),
                    "recommended_flow": [
                        "Call uepi_status first.",
                        "Use uepi_search or uepi_context to identify candidate assets.",
                        "Call the domain-specific read tool only for assets needed by the user question.",
                        "If diagnostics include UEPI_REFRESH_REQUESTED, retry the same read after the editor processes the request.",
                    ],
                },
                "capabilities": resolved["capabilities"],
                "doctor": resolved["doctor"],
                "editor": resolved["editor"],
                "snapshot": resolved["snapshot"],
            },
            next_actions=[
                *recovery_next_actions,
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
                "cpp_symbols": scan_cpp_symbols(self.store.root, limit=80),
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

    def search(self, query: str = "", kind: str | None = None, limit: int = 20, scope: str = "project") -> dict[str, Any]:
        limit = max(1, min(int(limit or 20), 100))
        if self.cache:
            search_limit = 500 if scope == "project_plugins" else limit
            matches = [_short_entity(entity) for entity in self.cache.search_entities(query, kind, search_limit)]
            query_source = "sqlite_cache"
        else:
            self._ensure_loaded()
            matches = [_short_entity(entity) for entity in self.entities if self._matches_entity(entity, query, kind)][:limit]
            query_source = "snapshot_fragments"
        if scope == "project_plugins":
            roots = [str(item).casefold() for item in project_module_manifest(self.store.root).get("asset_roots") or [] if str(item) != "/Game"]
            matches = [item for item in matches if any(str(item.get("canonical_key") or "").casefold().startswith(root + "/") for root in roots)][:limit]
        return self._envelope(
            {"query": query, "kind": kind, "scope": scope, "matches": matches, "match_count": len(matches), "query_source": query_source},
            tool="uepi_search",
            operation="entity_search",
        )

    def asset(self, asset: str, include_snapshot: bool = True, relation_limit: int = 80, refresh: str = "auto", exact: bool = True) -> dict[str, Any]:
        freshness, diagnostics = self._freshness_for_identifier(asset, "uepi_asset")
        if refresh == "force":
            freshness, diagnostics = self._force_bridge_refresh(asset, diagnostics=diagnostics, freshness=freshness)
        entity, candidates = self._resolve_entity_with_exact(asset, exact)
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

    def blueprint(
        self,
        asset: str,
        limit: int = 200,
        refresh: str = "auto",
        exact: bool = True,
        graph: str = "",
        graph_role: str = "",
        node_guid: str = "",
        node_classes: list[str] | None = None,
        semantic_kinds: list[str] | None = None,
    ) -> dict[str, Any]:
        freshness, diagnostics = self._freshness_for_identifier(asset, "uepi_blueprint")
        if refresh == "force":
            freshness, diagnostics = self._force_bridge_refresh(asset, diagnostics=diagnostics, freshness=freshness)
        entity, candidates = self._resolve_entity_with_exact(asset, exact)
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
        node_class_set = {str(item).casefold() for item in (node_classes or [])}
        semantic_set = {str(item).casefold() for item in (semantic_kinds or [])}
        filters_active = bool(graph or graph_role or node_guid or node_class_set or semantic_set)
        domain_entities = self._domain_entities_for_asset(
            entity,
            BLUEPRINT_KINDS,
            max(1, min(limit, 1000)),
            relation_types=BLUEPRINT_SCOPE_RELATIONS,
        )
        if filters_active:
            asset_key = str(entity.get("canonical_key") or "")
            if self.cache:
                scoped_entities = self.cache.scoped_entities_for_asset(asset_key, BLUEPRINT_KINDS)
            else:
                self._ensure_loaded()
                folded_asset = asset_key.casefold().rstrip(":")
                scoped_entities = [
                    item
                    for item in self.entities
                    if item.get("kind") in BLUEPRINT_KINDS
                    and (
                        str(item.get("canonical_key") or "").casefold() == folded_asset
                        or str(item.get("canonical_key") or "").casefold().startswith(folded_asset + ":")
                        or folded_asset in json.dumps(_as_dict(item.get("attributes")), ensure_ascii=False).casefold()
                    )
                ]
            merged = {str(item.get("id") or ""): item for item in domain_entities if item.get("id")}
            for item in scoped_entities:
                item_id = str(item.get("id") or "")
                if item_id:
                    merged[item_id] = item
            domain_entities = list(merged.values())
        unfiltered_domain_entities = list(domain_entities)
        if not unfiltered_domain_entities and refresh == "auto" and self.snapshot.active_editor_session():
            target = self._asset_refresh_target(entity, asset)
            retry_freshness, retry_diagnostics, refresh_summary = self._force_bridge_refresh_many(
                [target],
                diagnostics=diagnostics,
                freshness=freshness,
            )
            if refresh_summary.get("refreshed_assets"):
                retried = self.blueprint(
                    asset,
                    limit=limit,
                    refresh="never",
                    exact=exact,
                    graph=graph,
                    graph_role=graph_role,
                    node_guid=node_guid,
                    node_classes=node_classes,
                    semantic_kinds=semantic_kinds,
                )
                retried_result = retried.get("result") if isinstance(retried.get("result"), dict) else {}
                retried_result["refresh"] = refresh_summary
                retried["result"] = retried_result
                retried["diagnostics"] = retry_diagnostics + [
                    item for item in retried.get("diagnostics") or [] if item not in retry_diagnostics
                ]
                if isinstance(retried.get("state"), dict):
                    retried["state"]["freshness"] = retry_freshness or retried["state"].get("freshness")
                return retried

        def blueprint_filter(item: dict[str, Any]) -> bool:
            if graph and not _graph_name_matches(_first_attribute_value(item, "graph_name", "graph", "graph_path"), graph):
                return False
            if graph_role and str(_first_attribute_value(item, "graph_role") or "").casefold() != graph_role.casefold():
                return False
            if node_guid and str(_first_attribute_value(item, "node_guid", "guid") or "").casefold() != node_guid.casefold():
                return False
            node_class = str(_first_attribute_value(item, "node_class", "class_path", "node_class_path") or "").casefold()
            if node_class_set and not any(candidate == node_class or candidate in node_class for candidate in node_class_set):
                return False
            semantic = str(_first_attribute_value(item, "semantic_kind", "semantic") or item.get("kind") or "").casefold()
            if semantic_set and semantic not in semantic_set:
                return False
            return True

        if node_guid and self.cache:
            focused_nodes = [
                item
                for item in unfiltered_domain_entities
                if item.get("kind") == "blueprint_node"
                and str(_first_attribute_value(item, "node_guid", "guid") or "").casefold() == node_guid.casefold()
                and blueprint_filter(item)
            ]
            if focused_nodes:
                focused_node = focused_nodes[0]
                focused_node_id = str(focused_node.get("id") or "")
                direct_relations = self.cache.relations_for_entity(focused_node_id, limit=1000)
                indexed_pin_relations = self.cache.outgoing_relations(focused_node_id, {"has_pin"}, limit=2000)
                direct_relation_by_id = {
                    str(relation.get("id") or f"{relation.get('type')}:{relation.get('from_id')}:{relation.get('to_id')}"): relation
                    for relation in [*direct_relations, *indexed_pin_relations]
                }
                direct_relations = list(direct_relation_by_id.values())
                owned_pin_ids = {
                    str(relation.get("to_id") or "")
                    for relation in direct_relations
                    if relation.get("type") == "has_pin" and relation.get("from_id") == focused_node_id
                }
                pin_relations = self.cache.relations_for_entities(owned_pin_ids, limit=2000)
                linked_pin_ids = {
                    str(relation.get(key) or "")
                    for relation in pin_relations
                    if relation.get("type") == "connects_to"
                    for key in ("from_id", "to_id")
                    if relation.get(key)
                }
                linked_relations = self.cache.relations_for_entities(linked_pin_ids - owned_pin_ids, limit=2000)
                relation_by_id = {
                    str(relation.get("id") or f"{relation.get('type')}:{relation.get('from_id')}:{relation.get('to_id')}"): relation
                    for relation in [*direct_relations, *pin_relations, *linked_relations]
                }
                focus_relations = list(relation_by_id.values())
                focus_entity_ids = {
                    str(relation.get(key) or "")
                    for relation in focus_relations
                    for key in ("from_id", "to_id")
                    if relation.get(key)
                }
                focus_entity_ids.add(focused_node_id)
                focus_entities = self.cache.entities_by_ids(focus_entity_ids, limit=max(1, len(focus_entity_ids)), include_snapshot=False)

                def focus_rank(item: dict[str, Any]) -> tuple[int, str]:
                    item_id = str(item.get("id") or "")
                    if item_id == focused_node_id:
                        priority = 0
                    elif item_id in owned_pin_ids:
                        priority = 1
                    elif item.get("kind") == "blueprint_pin":
                        priority = 2
                    elif item.get("kind") == "blueprint_node":
                        priority = 3
                    else:
                        priority = 4
                    return priority, str(item.get("canonical_key") or item_id).casefold()

                focus_entities = sorted(focus_entities, key=focus_rank)[: max(1, min(limit, 1000))]
                summarized_relations = [_relation_summary(relation) for relation in focus_relations[: max(1, min(limit * 4, 1000))]]
                semantic_summary = summarize_blueprint_semantics(_short_entity(entity), focus_entities, summarized_relations, limit=max(20, min(limit, 120)))
                compact_entities = [_focused_blueprint_entity(item) for item in focus_entities]
                compact_by_id = {str(item.get("id") or ""): item for item in compact_entities if item.get("id")}
                compact_relations = [_focused_relation(relation) for relation in summarized_relations]
                return self._envelope(
                    {
                        "asset": _focused_blueprint_entity(entity),
                        "focus": {
                            "node_guid": node_guid,
                            "node": compact_by_id.get(focused_node_id) or _focused_blueprint_entity(focused_node),
                            "pins": [item for item in compact_entities if str(item.get("id") or "") in owned_pin_ids],
                            "direct_links": [relation for relation in compact_relations if relation.get("type") == "connects_to"],
                        },
                        "blueprint_entities": compact_entities,
                        "relations": compact_relations,
                        "semantic_summary": semantic_summary,
                        "call_graph": semantic_summary["call_graph"],
                        "data_mutations": semantic_summary["data_mutations"],
                        "resolution_candidates": [_focused_blueprint_entity(item) for item in candidates[:5]],
                        "query_source": "sqlite_cache",
                        "unfiltered_entity_count": len(unfiltered_domain_entities),
                        "filters": {"graph": graph or None, "graph_role": graph_role or None, "node_guid": node_guid, "node_classes": node_classes or [], "semantic_kinds": semantic_kinds or []},
                    },
                    diagnostics=diagnostics,
                    freshness=freshness,
                    tool="uepi_blueprint",
                    operation="focused_node_read",
                    next_actions=[
                        {
                            "reason": "Trace outward from this exact node when the user asks what happens next.",
                            "tool": "uepi_blueprint_trace",
                            "arguments": {"asset": asset, "start": node_guid},
                        }
                    ],
                )

        if filters_active:
            domain_entities = [item for item in domain_entities if blueprint_filter(item)]

            def filtered_rank(item: dict[str, Any]) -> tuple[int, str]:
                kind = str(item.get("kind") or "")
                specific_domain = kind not in {"blueprint_node", "blueprint_pin", "cfg_basic_block", "dfg_value"}
                has_domain_metadata = any(
                    _first_attribute_value(item, key) not in (None, "")
                    for key in ("slot_name", "state_name", "transition_name", "animation_asset")
                )
                return (
                    0 if has_domain_metadata else (1 if specific_domain else 2),
                    str(item.get("canonical_key") or item.get("id") or "").casefold(),
                )

            domain_entities = sorted(domain_entities, key=filtered_rank)[: max(1, min(limit, 1000))]
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
        if not unfiltered_domain_entities:
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
        elif filters_active and not domain_entities:
            diagnostics.append(
                {
                    "severity": "info",
                    "blocking": False,
                    "code": "UEPI_BLUEPRINT_FILTER_NO_MATCH",
                    "message": "Blueprint graph data is present, but no graph entity matched the requested filters.",
                    "phase": "query",
                    "retryable": False,
                    "recoverable": True,
                }
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
                "unfiltered_entity_count": len(unfiltered_domain_entities),
                "filters": {"graph": graph or None, "graph_role": graph_role or None, "node_guid": node_guid or None, "node_classes": node_classes or [], "semantic_kinds": semantic_kinds or []},
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
        blueprint_entities = self._domain_entities_for_asset(
            entity,
            BLUEPRINT_KINDS,
            1000,
            relation_types=BLUEPRINT_SCOPE_RELATIONS,
        )
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

    def animation(self, asset: str, include: list[str] | None = None, limit: int = 300, refresh: str = "auto", exact: bool = True, mode: str = "exact_asset", summary_only: bool = False) -> dict[str, Any]:
        freshness, diagnostics = self._freshness_for_identifier(asset, "uepi_animation")
        if refresh == "force":
            freshness, diagnostics = self._force_bridge_refresh(asset, diagnostics=diagnostics, freshness=freshness)
        entity, candidates = self._resolve_entity_with_exact(asset, exact)
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
        if summary_only:
            snapshot = _as_dict(entity.get("snapshot"))
            sequence_snapshot = snapshot.get("animation_sequence") or (snapshot if snapshot.get("schema_version") == "uepi.anim_sequence.v1" else None)
            sequence_snapshot = sequence_snapshot if isinstance(sequence_snapshot, dict) else None
            motion_summary = snapshot.get("animation_motion_summary") or snapshot.get("motion_summary") or (sequence_snapshot or {}).get("motion_summary")
            compact_sequence = _animation_context_sequence(sequence_snapshot)
            animation_entity_count = 0
            if compact_sequence:
                animation_entity_count = sum(
                    int(compact_sequence.get(key) or 0)
                    for key in ("bone_track_count", "notify_count", "float_curve_count", "transform_curve_count")
                )
            return self._envelope(
                {
                    "schema_version": "uepi.animation-context-summary.v1",
                    "asset": _short_entity(entity),
                    "motion_summary": _bounded_context_value(motion_summary, list_limit=16) if isinstance(motion_summary, dict) else motion_summary,
                    "sequence": compact_sequence,
                    "animation_entity_count": animation_entity_count,
                    "resolution_candidates": [_short_entity(item) for item in candidates[:5]],
                    "query_source": "sqlite_cache" if self.cache else "snapshot_fragments",
                },
                diagnostics=diagnostics,
                freshness=freshness,
                tool="uepi_animation",
                operation="animation_context_summary",
            )
        domain_entities = self._domain_entities_for_asset(entity, ANIMATION_KINDS, max(1, min(limit, 1000)))
        if mode == "exact_asset":
            asset_key = str(entity.get("canonical_key") or "").casefold()
            asset_id = str(entity.get("id") or "")
            domain_entities = [
                item for item in domain_entities
                if str(item.get("canonical_key") or "").casefold().startswith(asset_key + "::")
                or str(_as_dict(item.get("attributes")).get("owner_asset_id") or "") == asset_id
                or str(_as_dict(item.get("attributes")).get("asset_path") or "").casefold() == asset_key
            ]
        snapshot = _as_dict(entity.get("snapshot"))
        requested = include or [
            "summary",
            "tracks",
            "notifies",
            "curves",
            "relations",
            "bone_motion_profile_manifest",
            "reconstruction_profile_manifest",
        ]
        sequence_snapshot = snapshot.get("animation_sequence") or (snapshot if snapshot.get("schema_version") == "uepi.anim_sequence.v1" else None)
        sequence_snapshot = sequence_snapshot if isinstance(sequence_snapshot, dict) else None
        bone_motion_profile_manifest = _as_dict(sequence_snapshot.get("bone_motion_profile")) if sequence_snapshot else {}
        reconstruction_profile_manifest = _as_dict(sequence_snapshot.get("reconstruction_profile")) if sequence_snapshot else {}
        result = {
            "asset": _short_entity(entity, include_snapshot=True),
            "mode": mode,
            "include": requested,
            "animation_entities": domain_entities,
            "motion_summary": snapshot.get("animation_motion_summary") or snapshot.get("motion_summary") or (sequence_snapshot or {}).get("motion_summary"),
            "sequence": sequence_snapshot,
            "bone_motion_profile_manifest": bone_motion_profile_manifest or None,
            "reconstruction_profile_manifest": reconstruction_profile_manifest or None,
            "resolution_candidates": candidates,
            "query_source": "sqlite_cache" if self.cache else "snapshot_fragments",
        }
        omissions: list[str] = []
        artifact_diagnostics: list[dict[str, Any]] = []
        if "bone_motion_profile" in requested:
            if bone_motion_profile_manifest:
                payload, artifact_diagnostics = self._load_artifact_payload(
                    bone_motion_profile_manifest,
                    expected_schema="uepi.animation_bone_motion_profile.v1",
                    diagnostic_code="UEPI_ANIMATION_BONE_MOTION_PROFILE_UNAVAILABLE",
                )
                if payload:
                    result["bone_motion_profile"] = payload
                else:
                    omissions.append("bone_motion_profile_artifact_unreadable")
            else:
                omissions.append("bone_motion_profile_artifact_not_present_in_snapshot")
        reconstruction_profile: dict[str, Any] | None = None
        if any(name in requested for name in ("reconstruction_profile", "driver_track_curves", "full_pose_artifact", "full_pose_samples")):
            if reconstruction_profile_manifest:
                payload, reconstruction_diagnostics = self._load_artifact_payload(
                    reconstruction_profile_manifest,
                    expected_schema="uepi.animation_reconstruction_profile.v1",
                    diagnostic_code="UEPI_ANIMATION_RECONSTRUCTION_PROFILE_UNAVAILABLE",
                )
                artifact_diagnostics.extend(reconstruction_diagnostics)
                if payload:
                    reconstruction_profile = payload
                    if "reconstruction_profile" in requested:
                        result["reconstruction_profile"] = payload
                    if "driver_track_curves" in requested:
                        result["driver_track_curves"] = payload.get("driver_track_curves") or []
                        result["reconstruction_guidelines"] = payload.get("reconstruction_guidelines") or []
                        result["phase_estimates"] = payload.get("phase_estimates") or []
                else:
                    omissions.append("reconstruction_profile_artifact_unreadable")
            else:
                omissions.append("reconstruction_profile_artifact_not_present_in_snapshot")
        if any(name in requested for name in ("full_pose_artifact", "full_pose_samples")):
            full_pose_manifest = _as_dict((reconstruction_profile or {}).get("full_pose_sample_artifact"))
            if full_pose_manifest:
                payload, full_pose_diagnostics = self._load_artifact_payload(
                    full_pose_manifest,
                    expected_schema="uepi.animation_full_pose_samples.v1",
                    diagnostic_code="UEPI_ANIMATION_FULL_POSE_SAMPLES_UNAVAILABLE",
                )
                artifact_diagnostics.extend(full_pose_diagnostics)
                if payload:
                    result["full_pose_artifact"] = payload
                else:
                    omissions.append("full_pose_artifact_unreadable")
            elif reconstruction_profile is not None:
                omissions.append("full_pose_artifact_not_present_in_reconstruction_profile")
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
        diagnostics = diagnostics + artifact_diagnostics
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
            from_state = self._load_generation_state(from_generation)
            to_state = self._load_generation_state(to_generation)
            from_scan = self.store.load_project_scan(from_state)
            to_scan = self.store.load_project_scan(to_state)
        except SnapshotStoreError as exc:
            return self._error(
                "UEPI_DIFF_INPUT_MISSING",
                str(exc),
                self._available_generation_candidates(),
                tool="uepi_diff",
                operation="generation_diff",
            )

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
                "from_data_mode": from_state.data_mode,
                "to_data_mode": to_state.data_mode,
                "from_manifest_path": str(from_state.manifest_path),
                "to_manifest_path": str(to_state.manifest_path),
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

    def _load_generation_state(self, generation: int) -> SnapshotState:
        candidates: list[tuple[Path, str]] = []
        preferred_modes = ["live", "saved"] if self.state.data_mode == "live" else ["saved", "live"]
        if self.state.generation == generation:
            candidates.append((self.state.manifest_path, self.state.data_mode))
        for mode in preferred_modes:
            candidates.append((self.store.versioned_manifest(generation, mode), mode))
        for mode in preferred_modes:
            candidates.append((self.store.manifest_path(mode), mode))

        seen: set[str] = set()
        checked: list[str] = []
        for path, data_mode in candidates:
            key = str(path)
            if key in seen:
                continue
            seen.add(key)
            checked.append(key)
            try:
                manifest = _load_json(path)
            except SnapshotStoreError:
                continue
            if int(manifest.get("generation") or 0) != generation:
                continue
            if manifest.get("schema_version") != "uepi.snapshot-manifest.v2":
                continue
            return SnapshotState(self.store.root, path, manifest)

        raise SnapshotStoreError(f"Snapshot generation {generation} was not found. Checked: {checked}")

    def _available_generation_candidates(self) -> list[dict[str, Any]]:
        candidates: list[dict[str, Any]] = []
        if not self.store.manifests_dir.exists():
            return candidates
        for path in sorted(self.store.manifests_dir.glob("*.json")):
            try:
                manifest = _load_json(path)
            except SnapshotStoreError:
                continue
            generation = manifest.get("generation")
            if generation is None:
                continue
            candidates.append(
                {
                    "generation": generation,
                    "data_mode": manifest.get("data_mode") or ("live" if path.name.startswith("live") else "saved"),
                    "path": str(path),
                }
            )
        candidates.sort(key=lambda item: (int(item.get("generation") or 0), str(item.get("data_mode") or ""), str(item.get("path") or "")), reverse=True)
        return candidates[:20]

    def context(
        self,
        question: str,
        scope: list[str] | None = None,
        hard_scope: list[str] | None = None,
        ranking_hints: list[str] | None = None,
        input_key: str | None = None,
        excluded_input_keys: list[str] | None = None,
        include_external_endpoints: bool = False,
        max_items: int = 40,
        route: str = "auto",
        live: bool = False,
        refresh: str = "auto",
    ) -> dict[str, Any]:
        from .context_ranker import select_route
        from .context_routes import make_routes

        scope = scope or []
        legacy_hard_scope = [item for item in scope if isinstance(item, str) and item.startswith("/") and "." in item]
        hard_scope = list(dict.fromkeys((hard_scope or []) + legacy_hard_scope))
        ranking_hints = list(dict.fromkeys((ranking_hints or []) + [item for item in scope if item not in legacy_hard_scope]))
        refresh_summary: dict[str, Any] | None = None
        refresh_diagnostics: list[dict[str, Any]] = []
        if refresh == "force":
            if not hard_scope:
                return self._error(
                    "UEPI_MULTI_ASSET_REFRESH_UNSUPPORTED",
                    "uepi_context refresh=force requires exact hard_scope asset paths so the refresh can be published atomically.",
                    result={"refresh": {"requested_targets": [], "requested_assets": [], "refreshed_targets": [], "refreshed_assets": [], "failed_targets": [], "failed_assets": [], "atomic": True}},
                    tool="uepi_context",
                    operation="context_refresh",
                )
            _, refresh_diagnostics, refresh_summary = self._force_bridge_refresh_many(
                hard_scope,
                diagnostics=[],
                freshness=None,
            )
            if refresh_summary.get("failed_targets"):
                return self._error(
                    "UEPI_MULTI_ASSET_REFRESH_UNSUPPORTED",
                    "The Editor did not publish every hard-scope asset in one atomic live generation.",
                    result={"refresh": refresh_summary},
                    diagnostics=refresh_diagnostics,
                    freshness="refresh_requested",
                    tool="uepi_context",
                    operation="context_refresh",
                )
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
                "hard_scope": hard_scope,
                "ranking_hints": ranking_hints,
                "input_key": input_key,
                "excluded_input_keys": excluded_input_keys or [],
                "include_external_endpoints": include_external_endpoints,
                "max_items": limit,
                "terms": terms,
                "route_confidence": route_confidence,
            },
        )
        result = pack.to_result(scope, limit)
        result["hard_scope"] = hard_scope
        result["ranking_hints"] = ranking_hints
        if input_key:
            result["input_key"] = input_key
        if excluded_input_keys:
            result["excluded_input_keys"] = excluded_input_keys
        if refresh_summary is not None:
            result["refresh"] = refresh_summary
        fail_closed_input = any(
            item.get("code") in {"UEPI_INPUT_KEY_UNMATCHED", "UEPI_INPUT_KEY_AMBIGUOUS"}
            for item in pack.diagnostics
        )
        if hard_scope and not result.get("matches") and not fail_closed_input:
            return self._error(
                "UEPI_SCOPE_NO_MATCH",
                "No indexed entity matched inside the requested hard scope.",
                tool="uepi_context",
                operation=f"context_route:{pack.route}",
            )
        result["available_routes"] = [candidate.name for candidate in routes]
        result["terms"] = terms[:20]
        diagnostics: list[dict[str, Any]] = [*refresh_diagnostics, *pack.diagnostics]
        if live:
            result.setdefault("sections", {})["live_editor"] = live_context(self.store)
            if not result["sections"]["live_editor"].get("status", {}).get("ok"):
                diagnostics.append(
                    {
                        "severity": "warning",
                        "code": "UEPI_BRIDGE_LIVE_CONTEXT_UNAVAILABLE",
                        "message": "uepi_context requested live editor context, but the optional bridge is not connected.",
                        "recoverable": True,
                        "recommended_user_action": "Keep the editor open, or restart it so the UEPI live bridge can start.",
                        "recommended_agent_action": {"tool": "uepi_status"},
                    }
                )
        return self._envelope(
            result,
            diagnostics=diagnostics,
            evidence=pack.evidence,
            next_actions=pack.next_actions,
            tool="uepi_context",
            operation=f"context_route:{pack.route}",
        )


def make_engine(project: str | Path | None = None, store: str | Path | None = None, db: str | Path | None = None) -> UEPIQueryEngine:
    return UEPIQueryEngine(SnapshotStore.from_paths(project=project, store=store, db=db), configured_project=project)
