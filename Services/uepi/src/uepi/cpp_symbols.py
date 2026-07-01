from __future__ import annotations

import re
from pathlib import Path
from typing import Any


TYPE_MACROS = ("UCLASS", "USTRUCT", "UENUM", "UINTERFACE")
MEMBER_MACROS = ("UPROPERTY", "UFUNCTION", "UDELEGATE")


def _project_root_from_store_root(store_root: Path) -> Path:
    root = store_root.resolve()
    if root.name == "UEProjectIntelligence" and root.parent.name == "Saved":
        return root.parent.parent
    return root


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
    source_root = project_root / "Source"
    if not source_root.exists():
        return {
            "schema_version": "uepi.cpp-symbol-scan.v1",
            "available": False,
            "project_root": str(project_root),
            "source_root": str(source_root),
            "symbols": [],
            "counts": {},
            "diagnostics": [{"severity": "info", "code": "UEPI_CPP_SOURCE_MISSING", "message": "Project Source directory does not exist."}],
        }

    symbols: list[dict[str, Any]] = []
    counts: dict[str, int] = {}
    files = sorted([*source_root.rglob("*.h"), *source_root.rglob("*.cpp")])
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
        "source_root": str(source_root),
        "scanned_file_count": len(files[:800]),
        "symbol_count": len(symbols),
        "counts": counts,
        "exposed_counts": exposed,
        "symbols": symbols,
        "diagnostics": [],
    }
