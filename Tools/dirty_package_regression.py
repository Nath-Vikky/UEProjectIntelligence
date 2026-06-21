from __future__ import annotations

import argparse
import json
from pathlib import Path


DIRTY_CODE = "UEPI_DIRTY_PACKAGE_DETECTED"


def scan_files(root: Path) -> list[Path]:
    return sorted(path for path in root.glob("*.json") if path.is_file())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check UEPI scan artifacts for dirty-package diagnostics.")
    parser.add_argument("--root", type=Path, default=Path("Saved/UEProjectIntelligence"))
    args = parser.parse_args(argv)

    checked = 0
    failures = []
    for path in scan_files(args.root):
        try:
            payload = json.loads(path.read_text(encoding="utf-8-sig"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        diagnostics = payload.get("diagnostics")
        if not isinstance(diagnostics, list):
            continue
        checked += 1
        dirty = [diagnostic for diagnostic in diagnostics if diagnostic.get("code") == DIRTY_CODE]
        if dirty:
            failures.append({"path": str(path), "dirty_diagnostic_count": len(dirty), "diagnostics": dirty})

    result = {"ok": not failures, "checked_scan_count": checked, "failures": failures}
    print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
