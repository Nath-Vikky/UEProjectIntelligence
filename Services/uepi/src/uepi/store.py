from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import json
from pathlib import Path
from typing import Any


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

    def _project_scan_fragments(self, state: SnapshotState) -> list[dict[str, Any]]:
        scans: list[dict[str, Any]] = []
        fragments = state.manifest.get("fragments")
        if not isinstance(fragments, list):
            raise SnapshotStoreError("Snapshot manifest does not include fragments.")
        for fragment in fragments:
            if isinstance(fragment, dict) and fragment.get("kind") == "project_scan":
                scans.append(_load_json(self.resolve_fragment_path(fragment)))
        if not scans:
            raise SnapshotStoreError("Snapshot manifest does not include a project_scan fragment.")
        return scans

    @staticmethod
    def _merge_project_scans(scans: list[dict[str, Any]]) -> dict[str, Any]:
        if not scans:
            raise SnapshotStoreError("No project scans were supplied for merge.")
        merged = dict(scans[0])
        entity_by_id: dict[str, dict[str, Any]] = {}
        relation_by_id: dict[str, dict[str, Any]] = {}
        diagnostics: list[Any] = []

        for scan in scans:
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

        merged["entities"] = sorted(entity_by_id.values(), key=lambda item: str(item.get("id") or ""))
        merged["relations"] = sorted(relation_by_id.values(), key=lambda item: str(item.get("id") or ""))
        merged["diagnostics"] = diagnostics
        return merged

    def load_project_scan(self, state: SnapshotState | None = None) -> dict[str, Any]:
        state = state or self.load_state()
        scans: list[dict[str, Any]] = []
        if state.data_mode == "live":
            try:
                scans.extend(self._project_scan_fragments(self.load_state("saved")))
            except SnapshotStoreError:
                pass
        scans.extend(self._project_scan_fragments(state))
        return self._merge_project_scans(scans)

    def versioned_manifest(self, generation: int, data_mode: str = "saved") -> Path:
        prefix = "live" if data_mode == "live" else "saved"
        return self.manifests_dir / f"{prefix}-{generation}.json"
