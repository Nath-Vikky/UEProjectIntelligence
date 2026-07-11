from __future__ import annotations

import re
import json
from pathlib import Path
from typing import Any


TYPE_MACROS = ("UCLASS", "USTRUCT", "UENUM", "UINTERFACE")
MEMBER_MACROS = ("UPROPERTY", "UFUNCTION", "UDELEGATE")


def _project_root_from_store_root(store_root: Path) -> Path:
    root = store_root.resolve()
    if root.name == "UEProjectIntelligence" and root.parent.name == "Saved":
        return root.parent.parent
    return root


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def project_module_manifest(store_root: Path) -> dict[str, Any]:
    project_root = _project_root_from_store_root(store_root)
    editor_manifest = _load_json(Path(store_root).resolve() / "store" / "project-modules.json")
    if editor_manifest.get("schema_version") == "uepi.project-modules.v1":
        return editor_manifest
    uprojects = sorted(project_root.glob("*.uproject"))
    project_file = uprojects[0] if len(uprojects) == 1 else None
    descriptor = _load_json(project_file) if project_file else {}
    references = {
        str(item.get("Name")): bool(item.get("Enabled", True))
        for item in descriptor.get("Plugins") or []
        if isinstance(item, dict) and item.get("Name")
    }

    plugins: list[dict[str, Any]] = []
    source_roots: list[dict[str, Any]] = []
    asset_roots = ["/Game"]
    project_source = project_root / "Source"
    if project_source.exists():
        source_roots.append({"path": str(project_source.resolve()), "kind": "project", "plugin": None})

    plugin_root = project_root / "Plugins"
    for plugin_file in sorted(plugin_root.rglob("*.uplugin")) if plugin_root.exists() else []:
        plugin_descriptor = _load_json(plugin_file)
        name = plugin_file.stem
        enabled_default = bool(plugin_descriptor.get("EnabledByDefault", True))
        enabled = references.get(name, enabled_default)
        can_contain_content = bool(plugin_descriptor.get("CanContainContent", False))
        source = plugin_file.parent / "Source"
        content = plugin_file.parent / "Content"
        plugin = {
            "name": name,
            "descriptor": str(plugin_file.resolve()),
            "type": "project_plugin",
            "enabled": enabled,
            "can_contain_content": can_contain_content,
            "mounted_asset_root": f"/{name}" if can_contain_content else None,
            "content_directory": str(content.resolve()),
            "source_directory": str(source.resolve()),
        }
        plugins.append(plugin)
        if enabled and source.exists():
            source_roots.append({"path": str(source.resolve()), "kind": "project_plugin", "plugin": name})
        if enabled and can_contain_content:
            asset_roots.append(f"/{name}")

    modules: list[dict[str, Any]] = []
    for root in source_roots:
        path = Path(root["path"])
        for build_file in sorted(path.rglob("*.Build.cs")):
            modules.append({"name": build_file.name.removesuffix(".Build.cs"), "build_file": str(build_file), "source_root": str(path), "plugin": root.get("plugin")})
    return {
        "schema_version": "uepi.project-modules.v1",
        "project_file": str(project_file.resolve()) if project_file else None,
        "modules": modules,
        "plugins": plugins,
        "asset_roots": sorted(set(asset_roots)),
        "source_roots": source_roots,
        "diagnostics": [] if project_file else [{"severity": "warning", "code": "UEPI_PROJECT_DESCRIPTOR_AMBIGUOUS", "message": "Exactly one .uproject was not found at the project root."}],
    }


def _specifier_text(lines: list[str], index: int) -> str:
    text = lines[index].strip()
    while text.count("(") > text.count(")") and index + 1 < len(lines):
        index += 1
        text += " " + lines[index].strip()
    return text


def _next_declaration(lines: list[str], start: int) -> str:
    for line in lines[start + 1 : start + 8]:
        stripped = line.strip()
        if not stripped or stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("*"):
            continue
        return stripped
    return ""


def _name_from_declaration(declaration: str) -> str:
    patterns = [
        r"\bclass\s+(?:\w+_API\s+)?(\w+)",
        r"\bstruct\s+(?:\w+_API\s+)?(\w+)",
        r"\benum\s+class\s+(\w+)",
        r"\benum\s+(\w+)",
        r"\b(\w+)\s*\(",
        r"\b(\w+)\s*;",
    ]
    for pattern in patterns:
        match = re.search(pattern, declaration)
        if match:
            return match.group(1)
    return declaration[:120]


def _symbol_record(path: Path, project_root: Path, line: int, macro: str, specifier: str, declaration: str) -> dict[str, Any]:
    name = _name_from_declaration(declaration)
    spec_lower = specifier.casefold()
    return {
        "kind": "cpp_symbol",
        "macro": macro,
        "name": name,
        "file": str(path),
        "project_relative_file": str(path.relative_to(project_root)) if path.is_relative_to(project_root) else str(path),
        "line": line + 1,
        "specifier": specifier,
        "declaration": declaration,
        "blueprint_callable": "blueprintcallable" in spec_lower,
        "blueprint_pure": "blueprintpure" in spec_lower,
        "blueprint_implementable_event": "blueprintimplementableevent" in spec_lower,
        "blueprint_native_event": "blueprintnativeevent" in spec_lower,
        "blueprint_type": "blueprinttype" in spec_lower,
        "blueprintable": "blueprintable" in spec_lower,
        "replicated": "replicated" in spec_lower or "replicatedusing" in spec_lower,
        "config": "config" in spec_lower,
    }


def scan_cpp_symbols(store_root: Path, limit: int = 200) -> dict[str, Any]:
    project_root = _project_root_from_store_root(store_root)
    manifest = project_module_manifest(store_root)
    source_roots = [Path(str(item.get("path"))) for item in manifest.get("source_roots") or [] if isinstance(item, dict) and item.get("path")]
    if not source_roots:
        return {
            "schema_version": "uepi.cpp-symbol-scan.v1",
            "available": False,
            "project_root": str(project_root),
            "source_roots": [],
            "symbols": [],
            "counts": {},
            "diagnostics": [{"severity": "info", "code": "UEPI_CPP_SOURCE_MISSING", "message": "Project Source directory does not exist."}],
        }

    symbols: list[dict[str, Any]] = []
    counts: dict[str, int] = {}
    files = sorted({path for source_root in source_roots for path in [*source_root.rglob("*.h"), *source_root.rglob("*.cpp")]})
    for path in files[:800]:
        try:
            lines = path.read_text(encoding="utf-8-sig", errors="ignore").splitlines()
        except OSError:
            continue
        for index, line in enumerate(lines):
            stripped = line.strip()
            macro = next((item for item in TYPE_MACROS + MEMBER_MACROS if stripped.startswith(item)), "")
            if not macro:
                continue
            record = _symbol_record(path, project_root, index, macro, _specifier_text(lines, index), _next_declaration(lines, index))
            source_record = next((item for item in manifest.get("source_roots") or [] if Path(str(item.get("path"))) in path.parents), {})
            record["source_root_kind"] = source_record.get("kind")
            record["plugin"] = source_record.get("plugin")
            record["module"] = next((part.name for part in path.parents if (part / f"{part.name}.Build.cs").exists()), None)
            symbols.append(record)
            counts[macro] = counts.get(macro, 0) + 1
            if len(symbols) >= max(1, limit):
                break
        if len(symbols) >= max(1, limit):
            break

    exposed = {
        "blueprint_callable": sum(1 for item in symbols if item.get("blueprint_callable")),
        "blueprint_events": sum(1 for item in symbols if item.get("blueprint_implementable_event") or item.get("blueprint_native_event")),
        "replicated": sum(1 for item in symbols if item.get("replicated")),
        "config": sum(1 for item in symbols if item.get("config")),
    }
    return {
        "schema_version": "uepi.cpp-symbol-scan.v1",
        "available": True,
        "project_root": str(project_root),
        "source_roots": [str(item) for item in source_roots],
        "project_module_manifest": manifest,
        "scanned_file_count": len(files[:800]),
        "symbol_count": len(symbols),
        "counts": counts,
        "exposed_counts": exposed,
        "symbols": symbols,
        "diagnostics": [],
    }
