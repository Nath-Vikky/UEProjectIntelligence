from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import json
from pathlib import Path
from typing import Any
from uuid import uuid4


class SnapshotStoreError(RuntimeError):
    """Raised when a UEPI Snapshot Store cannot be read safely."""


def _load_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8-sig") as handle:
            value = json.load(handle)
    except FileNotFoundError as exc:
        raise SnapshotStoreError(f"Snapshot file does not exist: {path}") from exc
    except json.JSONDecodeError as exc:
        raise SnapshotStoreError(f"Snapshot file is not valid JSON: {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise SnapshotStoreError(f"Snapshot file is not a JSON object: {path}")
    return value


def _is_relative_to(child: Path, parent: Path) -> bool:
    try:
        child.relative_to(parent)
        return True
    except ValueError:
        return False


def _parse_utc(value: Any) -> datetime | None:
    if not isinstance(value, str) or not value:
        return None
    normalized = value.replace("Z", "+00:00")
    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def _fold(value: Any) -> str:
    return str(value or "").strip().casefold()


def _dict_values(*objects: dict[str, Any]) -> list[str]:
    values: list[str] = []
    for obj in objects:
        for value in obj.values():
            if isinstance(value, str) and value:
                values.append(value)
    return values


def _identifier_matches_values(identifier: str, values: list[str]) -> bool:
    needle = _fold(identifier)
    if not needle:
        return False
    for value in values:
        hay = _fold(value)
        if hay and (needle == hay or needle in hay or hay in needle):
            return True
    return False


def _entity_match_values(entity: dict[str, Any]) -> list[str]:
    attributes = entity.get("attributes") if isinstance(entity.get("attributes"), dict) else {}
    typed_attributes = entity.get("typed_attributes") if isinstance(entity.get("typed_attributes"), dict) else {}
    values = [
        str(entity.get("id") or ""),
        str(entity.get("canonical_key") or ""),
        str(entity.get("display_name") or ""),
    ]
    values.extend(_dict_values(attributes, typed_attributes))
    return [value for value in values if value]


def _tombstone_match_values(tombstone: dict[str, Any]) -> list[str]:
    keys = [
        "asset_id",
        "asset_key",
        "asset_name",
        "package_name",
        "old_object_path",
        "new_object_path",
    ]
    return [str(tombstone.get(key) or "") for key in keys if tombstone.get(key)]


def _is_asset_entity(entity: dict[str, Any]) -> bool:
    return entity.get("kind") in {"asset", "asset_redirector"}


def resolve_store_root(project: str | Path | None = None, store: str | Path | None = None, db: str | Path | None = None) -> Path:
    if store:
        candidate = Path(store).expanduser()
        if candidate.name == "store":
            return candidate.parent.resolve()
        if candidate.name == "saved.json":
            return candidate.parent.parent.parent.resolve()
        return candidate.resolve()

    if db:
        return Path(db).expanduser().resolve().parent

    if project:
        project_path = Path(project).expanduser().resolve()
        project_dir = project_path.parent if project_path.suffix.lower() == ".uproject" else project_path
        return (project_dir / "Saved" / "UEProjectIntelligence").resolve()

    cwd = Path.cwd().resolve()
    if (cwd / "Saved" / "UEProjectIntelligence").exists():
        return (cwd / "Saved" / "UEProjectIntelligence").resolve()
    return cwd


@dataclass(frozen=True)
class SnapshotState:
    root: Path
    manifest_path: Path
    manifest: dict[str, Any]

    @property
    def generation(self) -> int:
        return int(self.manifest.get("generation") or 0)

    @property
    def project(self) -> dict[str, Any]:
        value = self.manifest.get("project")
        return value if isinstance(value, dict) else {}

    @property
    def counts(self) -> dict[str, Any]:
        value = self.manifest.get("counts")
        return value if isinstance(value, dict) else {}

    @property
    def data_mode(self) -> str:
        return str(self.manifest.get("data_mode") or "saved")

    def envelope_state(self) -> dict[str, Any]:
        if self.data_mode == "live":
            return {
                "data_mode": "live",
                "editor_connected": True,
                "saved_generation": self.manifest.get("base_saved_generation") or self.manifest.get("saved_generation"),
                "live_generation": self.generation,
                "snapshot_observed_at": self.manifest.get("created_at_utc"),
                "freshness": "current",
                "manifest_path": str(self.manifest_path),
            }
        return {
            "data_mode": self.data_mode,
            "editor_connected": False,
            "saved_generation": self.generation,
            "live_generation": None,
            "snapshot_observed_at": self.manifest.get("created_at_utc"),
            "freshness": "current",
            "manifest_path": str(self.manifest_path),
        }


class SnapshotStore:
    def __init__(self, root: str | Path):
        self.root = Path(root).expanduser().resolve()
        self.store_dir = self.root / "store"
        self.manifests_dir = self.store_dir / "manifests"
        self.objects_dir = self.store_dir / "objects"
        self.sessions_dir = self.store_dir / "sessions"
        self.requests_dir = self.store_dir / "requests"
        self.logs_dir = self.store_dir / "logs"

    @classmethod
    def from_paths(cls, project: str | Path | None = None, store: str | Path | None = None, db: str | Path | None = None) -> "SnapshotStore":
        return cls(resolve_store_root(project=project, store=store, db=db))

    def manifest_path(self, data_mode: str = "saved") -> Path:
        name = "live.json" if data_mode == "live" else "saved.json"
        return self.manifests_dir / name

    def load_state(self, data_mode: str = "auto") -> SnapshotState:
        if data_mode == "auto":
            try:
                live_state = self.load_state("live")
                if self._is_live_state_active(live_state):
                    return live_state
            except SnapshotStoreError:
                pass
            data_mode = "saved"

        path = self.manifest_path(data_mode)
        manifest = _load_json(path)
        if manifest.get("schema_version") != "uepi.snapshot-manifest.v2":
            raise SnapshotStoreError(f"Unsupported UEPI manifest schema: {manifest.get('schema_version')!r}")
        return SnapshotState(root=self.root, manifest_path=path, manifest=manifest)

    def _is_live_state_active(self, state: SnapshotState) -> bool:
        session_id = state.manifest.get("session_id")
        session_path = self.sessions_dir / "editor-session.json"
        try:
            session = _load_json(session_path)
        except SnapshotStoreError:
            return False
        if session.get("schema_version") != "uepi.live-session.v2":
            return False
        if session.get("state") != "active":
            return False
        if session_id and session.get("session_id") != session_id:
            return False
        last_seen = _parse_utc(session.get("last_seen_at_utc"))
        if last_seen is None:
            return False
        return (datetime.now(timezone.utc) - last_seen).total_seconds() <= 30.0

    def resolve_fragment_path(self, fragment: dict[str, Any]) -> Path:
        raw_path = fragment.get("path")
        if not isinstance(raw_path, str) or not raw_path:
            raise SnapshotStoreError("Snapshot fragment does not include a path.")
        path = Path(raw_path)
        if not path.is_absolute():
            path = self.root / path
        path = path.resolve()
        if not _is_relative_to(path, self.store_dir.resolve()):
            raise SnapshotStoreError(f"Snapshot fragment is outside the UEPI store: {path}")
        return path

    def _scan_fragments(self, state: SnapshotState) -> list[dict[str, Any]]:
        scans: list[dict[str, Any]] = []
        fragments = state.manifest.get("fragments")
        if not isinstance(fragments, list):
            raise SnapshotStoreError("Snapshot manifest does not include fragments.")
        for fragment in fragments:
            if isinstance(fragment, dict) and fragment.get("kind") in {"project_scan", "asset_fragment", "project_fragment", "asset_tombstone"}:
                scans.append(_load_json(self.resolve_fragment_path(fragment)))
        if not scans:
            raise SnapshotStoreError("Snapshot manifest does not include readable scan fragments.")
        return scans

    @staticmethod
    def _merge_project_scans(scans: list[dict[str, Any]]) -> dict[str, Any]:
        if not scans:
            raise SnapshotStoreError("No project scans were supplied for merge.")
        merged = dict(scans[0])
        entity_by_id: dict[str, dict[str, Any]] = {}
        relation_by_id: dict[str, dict[str, Any]] = {}
        diagnostics: list[Any] = []
        tombstones: list[dict[str, Any]] = []

        for scan in scans:
            if scan.get("schema_version") == "uepi.asset-tombstone.v2":
                tombstones.append(scan)
                tombstone_values = _tombstone_match_values(scan)
                removed_ids = {
                    entity_id
                    for entity_id, entity in entity_by_id.items()
                    if _identifier_matches_values(str(scan.get("asset_id") or ""), [entity_id])
                    or any(_identifier_matches_values(value, _entity_match_values(entity)) for value in tombstone_values)
                }
                if isinstance(scan.get("asset_id"), str) and scan.get("asset_id"):
                    removed_ids.add(str(scan["asset_id"]))
                frontier = list(removed_ids)
                while frontier:
                    current = frontier.pop(0)
                    for relation in relation_by_id.values():
                        from_id = str(relation.get("from_id") or "")
                        to_id = str(relation.get("to_id") or "")
                        next_id = ""
                        if from_id == current:
                            next_id = to_id
                        elif to_id == current:
                            next_id = from_id
                        if not next_id or next_id in removed_ids:
                            continue
                        next_entity = entity_by_id.get(next_id)
                        if not isinstance(next_entity, dict) or _is_asset_entity(next_entity):
                            continue
                        removed_ids.add(next_id)
                        frontier.append(next_id)
                for entity_id in removed_ids:
                    entity_by_id.pop(entity_id, None)
                continue

            for entity in scan.get("entities") or []:
                if isinstance(entity, dict) and entity.get("id"):
                    entity_by_id[str(entity["id"])] = entity
            for relation in scan.get("relations") or []:
                if isinstance(relation, dict) and relation.get("id"):
                    relation_by_id[str(relation["id"])] = relation
            if isinstance(scan.get("diagnostics"), list):
                diagnostics.extend(scan["diagnostics"])
            for key in ("schema_version", "project_id", "project_name", "project_file", "engine_version", "started_at_utc", "finished_at_utc", "completeness"):
                if key in scan:
                    merged[key] = scan[key]

        live_entity_ids = set(entity_by_id)
        relation_by_id = {
            relation_id: relation
            for relation_id, relation in relation_by_id.items()
            if str(relation.get("from_id") or "") in live_entity_ids and str(relation.get("to_id") or "") in live_entity_ids
        }
        merged["entities"] = sorted(entity_by_id.values(), key=lambda item: str(item.get("id") or ""))
        merged["relations"] = sorted(relation_by_id.values(), key=lambda item: str(item.get("id") or ""))
        merged["diagnostics"] = diagnostics
        merged["tombstones"] = tombstones
        return merged

    def load_project_scan(self, state: SnapshotState | None = None) -> dict[str, Any]:
        state = state or self.load_state()
        scans: list[dict[str, Any]] = []
        if state.data_mode == "live":
            try:
                scans.extend(self._scan_fragments(self.load_state("saved")))
            except SnapshotStoreError:
                pass
        scans.extend(self._scan_fragments(state))
        return self._merge_project_scans(scans)

    def active_editor_session(self, max_age_seconds: float = 30.0) -> dict[str, Any] | None:
        session_path = self.sessions_dir / "editor-session.json"
        try:
            session = _load_json(session_path)
        except SnapshotStoreError:
            return None
        if session.get("schema_version") != "uepi.live-session.v2" or session.get("state") != "active":
            return None
        last_seen = _parse_utc(session.get("last_seen_at_utc"))
        if last_seen is None:
            return None
        if (datetime.now(timezone.utc) - last_seen).total_seconds() > max_age_seconds:
            return None
        return session

    def iter_tombstones(self, state: SnapshotState | None = None) -> list[dict[str, Any]]:
        state = state or self.load_state()
        fragments: list[dict[str, Any]] = []
        if state.data_mode == "live":
            try:
                fragments.extend(self._scan_fragments(self.load_state("saved")))
            except SnapshotStoreError:
                pass
        fragments.extend(self._scan_fragments(state))
        return [fragment for fragment in fragments if fragment.get("schema_version") == "uepi.asset-tombstone.v2"]

    def find_tombstone(self, identifier: str, state: SnapshotState | None = None) -> dict[str, Any] | None:
        for tombstone in reversed(self.iter_tombstones(state)):
            if _identifier_matches_values(identifier, _tombstone_match_values(tombstone)):
                return tombstone
        return None

    def read_incremental_events(self, limit: int = 2048) -> list[dict[str, Any]]:
        path = self.logs_dir / "incremental_events.jsonl"
        if not path.exists():
            return []
        try:
            lines = path.read_text(encoding="utf-8-sig").splitlines()
        except OSError:
            return []
        events: list[dict[str, Any]] = []
        for line in lines[-max(1, int(limit)):]:
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                events.append(value)
        return events

    def latest_incremental_event(self, identifier: str | None = None) -> dict[str, Any] | None:
        events = self.read_incremental_events()
        if not identifier:
            return events[-1] if events else None
        for event in reversed(events):
            values = [
                str(event.get("asset_path") or ""),
                str(event.get("package_name") or ""),
                str(event.get("old_object_path") or ""),
                str(event.get("package_file_name") or ""),
            ]
            if _identifier_matches_values(identifier, values):
                return event
        return None

    def asset_fragment_observed_at(self, identifier: str, state: SnapshotState | None = None) -> datetime | None:
        state = state or self.load_state()
        fragments: list[dict[str, Any]] = []
        if state.data_mode == "live":
            try:
                fragments.extend(self._scan_fragments(self.load_state("saved")))
            except SnapshotStoreError:
                pass
        fragments.extend(self._scan_fragments(state))
        for fragment in reversed(fragments):
            if fragment.get("schema_version") == "uepi.asset-tombstone.v2":
                if _identifier_matches_values(identifier, _tombstone_match_values(fragment)):
                    return _parse_utc(fragment.get("created_at_utc"))
                continue

            asset = fragment.get("asset") if isinstance(fragment.get("asset"), dict) else {}
            fragment_values = [
                str(fragment.get("asset_id") or asset.get("id") or ""),
                str(fragment.get("asset_key") or asset.get("canonical_key") or ""),
                str(fragment.get("asset_name") or asset.get("display_name") or ""),
            ]
            if _identifier_matches_values(identifier, fragment_values):
                return _parse_utc(fragment.get("source_scan_finished_at_utc") or fragment.get("finished_at_utc"))
            for entity in fragment.get("entities") or []:
                if isinstance(entity, dict) and _identifier_matches_values(identifier, _entity_match_values(entity)):
                    return _parse_utc(fragment.get("source_scan_finished_at_utc") or fragment.get("finished_at_utc"))
        return None

    def request_refresh(
        self,
        targets: list[str],
        *,
        reason: str,
        tool_name: str,
        data_mode: str = "live",
    ) -> Path:
        clean_targets = sorted({str(target).strip() for target in targets if str(target).strip()})
        request_id = f"{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}-{uuid4().hex[:12]}"
        request = {
            "schema_version": "uepi.refresh-request.v1",
            "request_id": request_id,
            "status": "pending",
            "created_at_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            "data_mode": data_mode,
            "target_object_paths": clean_targets,
            "reason": reason,
            "tool_name": tool_name,
        }
        self.requests_dir.mkdir(parents=True, exist_ok=True)
        for existing_path in self.requests_dir.glob("*.json"):
            try:
                existing = _load_json(existing_path)
            except SnapshotStoreError:
                continue
            status = str(existing.get("status") or "pending").casefold()
            existing_targets = {str(item) for item in existing.get("target_object_paths") or [] if isinstance(item, str)}
            if status in {"pending", "running"} and existing_targets.intersection(clean_targets):
                return existing_path
        path = self.requests_dir / f"refresh-{request_id}.json"
        temp_path = path.with_suffix(".json.tmp")
        temp_path.write_text(json.dumps(request, ensure_ascii=False, indent=2), encoding="utf-8")
        temp_path.replace(path)
        return path

    def pending_refresh_requests(self) -> int:
        if not self.requests_dir.exists():
            return 0
        count = 0
        for path in self.requests_dir.glob("*.json"):
            try:
                request = _load_json(path)
            except SnapshotStoreError:
                continue
            status = str(request.get("status") or "pending").casefold()
            if status in {"pending", "running"}:
                count += 1
        return count

    def versioned_manifest(self, generation: int, data_mode: str = "saved") -> Path:
        prefix = "live" if data_mode == "live" else "saved"
        return self.manifests_dir / f"{prefix}-{generation}.json"
