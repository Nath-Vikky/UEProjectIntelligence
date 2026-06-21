#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


def read_scan_golden_pairs(readme_path: Path) -> list[tuple[str, str]]:
    text = readme_path.read_text(encoding="utf-8")
    pairs: list[tuple[str, str]] = []
    last_scan: str | None = None
    for raw in text.splitlines():
        line = raw.strip().rstrip("`").strip()
        if line.startswith("--scan "):
            last_scan = line.split()[1]
        elif line.startswith("--golden ") and last_scan:
            pairs.append((last_scan, line.split()[1]))
            last_scan = None
    return pairs


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run all README scan/golden summary regressions.")
    parser.add_argument("--readme", type=Path, default=Path("Plugins/UEProjectIntelligence/README.md"))
    parser.add_argument("--validator", type=Path, default=Path("Plugins/UEProjectIntelligence/Tools/validate_scan.py"))
    args = parser.parse_args(argv)

    pairs = read_scan_golden_pairs(args.readme)
    print(f"golden pairs: {len(pairs)}")
    for scan, golden in pairs:
        result = subprocess.run(
            [sys.executable, "-B", str(args.validator), "--scan", scan, "--golden", golden],
            text=True,
            capture_output=True,
        )
        if result.returncode:
            print(f"golden regression failed: scan={scan} golden={golden}")
            print(result.stdout)
            print(result.stderr, file=sys.stderr)
            return result.returncode
    print("all golden pairs ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
