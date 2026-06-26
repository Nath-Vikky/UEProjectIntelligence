from __future__ import annotations

from dataclasses import dataclass
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

    def envelope_state(self) -> dict[str, Any]:
        return {
            "data_mode": self.manifest.get("data_mode") or "snapshot",
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

    @classmethod
    def from_paths(cls, project: str | Path | None = None, store: str | Path | None = None, db: str | Path | None = None) -> "SnapshotStore":
        return cls(resolve_store_root(project=project, store=store, db=db))

    def manifest_path(self, data_mode: str = "saved") -> Path:
        name = "live.json" if data_mode == "live" else "saved.json"
        return self.manifests_dir / name

    def load_state(self, data_mode: str = "saved") -> SnapshotState:
        path = self.manifest_path(data_mode)
        manifest = _load_json(path)
        if manifest.get("schema_version") != "uepi.snapshot-manifest.v2":
            raise SnapshotStoreError(f"Unsupported UEPI manifest schema: {manifest.get('schema_version')!r}")
        return SnapshotState(root=self.root, manifest_path=path, manifest=manifest)

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

    def load_project_scan(self, state: SnapshotState | None = None) -> dict[str, Any]:
        state = state or self.load_state()
        fragments = state.manifest.get("fragments")
        if not isinstance(fragments, list):
            raise SnapshotStoreError("Snapshot manifest does not include fragments.")
        project_scan = next(
            (fragment for fragment in fragments if isinstance(fragment, dict) and fragment.get("kind") == "project_scan"),
            None,
        )
        if project_scan is None:
            raise SnapshotStoreError("Snapshot manifest does not include a project_scan fragment.")
        return _load_json(self.resolve_fragment_path(project_scan))

    def versioned_manifest(self, generation: int, data_mode: str = "saved") -> Path:
        prefix = "live" if data_mode == "live" else "saved"
        return self.manifests_dir / f"{prefix}-{generation}.json"
