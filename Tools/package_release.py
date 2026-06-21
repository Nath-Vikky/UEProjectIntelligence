from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import zipfile


EXCLUDED_DIRS = {
    ".git",
    "__pycache__",
    "Binaries",
    "DerivedDataCache",
    "Intermediate",
    "Saved",
}
EXCLUDED_SUFFIXES = {".pyc", ".pdb", ".obj", ".exp", ".lib", ".dll"}


def should_include(path: Path, plugin_root: Path) -> bool:
    rel = path.relative_to(plugin_root)
    if any(part in EXCLUDED_DIRS for part in rel.parts):
        return False
    if path.suffix.lower() in EXCLUDED_SUFFIXES:
        return False
    return path.is_file()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_plugin_version(plugin_root: Path) -> str:
    descriptor = json.loads((plugin_root / "UEProjectIntelligence.uplugin").read_text(encoding="utf-8-sig"))
    return str(descriptor.get("VersionName") or descriptor.get("Version") or "0")


def build_manifest(plugin_root: Path, files: list[Path], archive_path: Path) -> dict[str, object]:
    file_entries = []
    for path in files:
        rel = path.relative_to(plugin_root).as_posix()
        file_entries.append({"path": rel, "size": path.stat().st_size, "sha256": sha256_file(path)})
    return {
        "schema_version": "uepi.release_manifest.v1",
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "plugin": "UEProjectIntelligence",
        "version": load_plugin_version(plugin_root),
        "archive": str(archive_path),
        "archive_sha256": sha256_file(archive_path) if archive_path.exists() else "",
        "file_count": len(file_entries),
        "files": file_entries,
        "install_plan": {
            "target": "Project/Plugins/UEProjectIntelligence",
            "steps": ["backup_existing_plugin", "extract_archive", "rebuild_project_modules"],
        },
        "upgrade_plan": {
            "compatible_with": "UE5.3 editor plugin layout",
            "steps": ["stop_editor", "backup_existing_plugin", "replace_files", "regenerate_project_files", "rebuild"],
        },
        "uninstall_plan": {
            "steps": ["stop_editor", "remove_project_plugin_directory", "delete_saved_uepi_indexes_if_requested", "regenerate_project_files"],
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Package UEProjectIntelligence plugin release artifacts.")
    parser.add_argument("--plugin-root", type=Path, default=Path("Plugins/UEProjectIntelligence"))
    parser.add_argument("--output", type=Path, default=Path("Saved/UEProjectIntelligence/Releases/UEProjectIntelligence.zip"))
    parser.add_argument("--manifest", type=Path, default=Path("Saved/UEProjectIntelligence/Releases/UEProjectIntelligence.manifest.json"))
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    plugin_root = args.plugin_root.resolve()
    files = sorted(path for path in plugin_root.rglob("*") if should_include(path, plugin_root))
    if not args.dry_run:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in files:
                archive.write(path, f"UEProjectIntelligence/{path.relative_to(plugin_root).as_posix()}")
    manifest = build_manifest(plugin_root, files, args.output.resolve())
    if not args.dry_run:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
