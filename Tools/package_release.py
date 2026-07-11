from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile


PLUGIN_NAME = "UEProjectIntelligence"

EXCLUDED_DIRS = {
    ".git",
    ".github",
    ".idea",
    ".vscode",
    "__pycache__",
    "Binaries",
    "DerivedDataCache",
    "Dist",
    "Intermediate",
    "Saved",
}

EXCLUDED_SUFFIXES = {
    ".db",
    ".dll",
    ".exp",
    ".lib",
    ".obj",
    ".opensdf",
    ".pdb",
    ".pyc",
    ".sln",
    ".sqlite3",
    ".suo",
}

REQUIRED_FILES = [
    "UEProjectIntelligence.uplugin",
    "README.md",
    "LICENSE",
    "Services/uepi/src/uepi/mcp_server.py",
]

LOCAL_PATH_PATTERN = re.compile(
    r"F:\\|C:/Users/renne|UE5project|Epic Games|\\Users\\renne|/Users/renne",
    re.IGNORECASE,
)


def default_plugin_root() -> Path:
    return Path(__file__).resolve().parents[1]


def should_include(path: Path, plugin_root: Path, *, include_binaries: bool = False) -> bool:
    rel = path.relative_to(plugin_root)
    excluded_dirs = EXCLUDED_DIRS - ({"Binaries"} if include_binaries else set())
    if any(part in excluded_dirs for part in rel.parts):
        return False
    excluded_suffixes = EXCLUDED_SUFFIXES - ({".dll", ".pdb"} if include_binaries else set())
    if path.suffix.lower() in excluded_suffixes:
        return False
    return path.is_file()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_commit_sha(plugin_root: Path) -> str:
    try:
        result = subprocess.run(
            [
                "git",
                "-c",
                f"safe.directory={plugin_root.as_posix()}",
                "rev-parse",
                "HEAD",
            ],
            cwd=plugin_root,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return ""
    return result.stdout.strip()


def load_plugin_descriptor(plugin_root: Path) -> dict[str, object]:
    return json.loads((plugin_root / "UEProjectIntelligence.uplugin").read_text(encoding="utf-8-sig"))


def assert_required_files(plugin_root: Path) -> None:
    missing = [item for item in REQUIRED_FILES if not (plugin_root / item).is_file()]
    if missing:
        raise RuntimeError(f"Required release files are missing: {', '.join(missing)}")


def assert_no_local_paths(plugin_root: Path) -> None:
    scanned = [
        plugin_root / "README.md",
        plugin_root / "Docs" / "user-guide.md",
        plugin_root / "Services" / "uepi" / "README.md",
        plugin_root / "Resources" / "codex-config.template.toml",
        plugin_root / "CONTRIBUTING.md",
    ]
    hits: list[str] = []
    for path in scanned:
        if not path.exists():
            continue
        for line_no, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), start=1):
            if LOCAL_PATH_PATTERN.search(line):
                hits.append(f"{path.relative_to(plugin_root).as_posix()}:{line_no}: {line.strip()}")
    if hits:
        raise RuntimeError("Local absolute paths remain in release docs:\n" + "\n".join(hits))


def validate_release_inputs(plugin_root: Path) -> None:
    if not plugin_root.is_dir():
        raise RuntimeError(f"Plugin root does not exist: {plugin_root}")
    assert_required_files(plugin_root)
    assert_no_local_paths(plugin_root)


def build_manifest(plugin_root: Path, files: list[Path], archive_path: Path, version: str) -> dict[str, object]:
    descriptor = load_plugin_descriptor(plugin_root)
    file_entries = []
    total_size = 0
    for path in files:
        rel = path.relative_to(plugin_root).as_posix()
        size = path.stat().st_size
        total_size += size
        file_entries.append({"path": rel, "size": size, "sha256": sha256_file(path)})
    return {
        "schema_version": "uepi.release_manifest.v1",
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "plugin": PLUGIN_NAME,
        "version": version,
        "uplugin_version_name": str(descriptor.get("VersionName") or ""),
        "commit_sha": git_commit_sha(plugin_root),
        "archive": archive_path.name,
        "archive_sha256": sha256_file(archive_path) if archive_path.exists() else "",
        "file_count": len(file_entries),
        "total_size": total_size,
        "files": file_entries,
        "install_plan": {
            "target": f"Project/Plugins/{PLUGIN_NAME}",
            "steps": ["stop_editor", "extract_archive", "regenerate_project_files", "rebuild_project_modules"],
        },
        "exclusions": {
            "directories": sorted(EXCLUDED_DIRS),
            "suffixes": sorted(EXCLUDED_SUFFIXES),
        },
    }


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Package UEProjectIntelligence plugin release artifacts.")
    parser.add_argument("--plugin-root", type=Path, default=default_plugin_root())
    parser.add_argument("--version", required=True, help="Release version, for example 2.0.0-alpha.1.")
    parser.add_argument("--out", type=Path, default=Path("Dist"), help="Output directory for zip and manifest.")
    parser.add_argument("--kind", choices=("source", "prebuilt"), default="source")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    plugin_root = args.plugin_root.resolve()
    validate_release_inputs(plugin_root)

    include_binaries = args.kind == "prebuilt"
    if include_binaries and not (plugin_root / "Binaries" / "Win64").is_dir():
        raise RuntimeError("A prebuilt release requires Binaries/Win64 built with UE 5.3.2.")
    files = sorted(path for path in plugin_root.rglob("*") if should_include(path, plugin_root, include_binaries=include_binaries))
    out_dir = args.out.resolve()
    label = "Source" if args.kind == "source" else "UE5.3.2-Win64"
    archive_path = out_dir / f"{PLUGIN_NAME}-{label}-v{args.version}.zip"
    manifest_path = out_dir / "release-manifest.json"
    checksums_path = out_dir / "SHA256SUMS.txt"

    if not args.dry_run:
        out_dir.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in files:
                archive.write(path, f"{PLUGIN_NAME}/{path.relative_to(plugin_root).as_posix()}")

    manifest = build_manifest(plugin_root, files, archive_path, args.version)
    if not args.dry_run:
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8")
        checksums_path.write_text(f"{sha256_file(archive_path)}  {archive_path.name}\n", encoding="ascii")

    print(json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
