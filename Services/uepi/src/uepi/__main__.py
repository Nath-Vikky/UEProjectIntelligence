from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
from typing import Any

from .cache import sync_cache
from .mcp_server import main as mcp_main
from .query import make_engine
from .store import SnapshotStore


def emit(value: dict[str, Any]) -> int:
    print(json.dumps(value, ensure_ascii=False, indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="UEPI v2 Snapshot query CLI.")
    parser.add_argument("command", choices=["mcp", "sync", "compact", "status", "overview", "search", "asset"], nargs="?", default="status")
    parser.add_argument("--project", type=Path)
    parser.add_argument("--store", type=Path)
    parser.add_argument("--db", type=Path)
    parser.add_argument("--query", default="")
    parser.add_argument("--kind")
    parser.add_argument("--asset")
    parser.add_argument("--limit", type=int, default=20)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args, passthrough = parser.parse_known_args(argv)
    if args.command == "mcp":
        mcp_args = passthrough[:]
        if args.project:
            mcp_args.extend(["--project", str(args.project)])
        if args.store:
            mcp_args.extend(["--store", str(args.store)])
        if args.db:
            mcp_args.extend(["--db", str(args.db)])
        return mcp_main(mcp_args)

    if args.command == "sync":
        store = SnapshotStore.from_paths(project=args.project, store=args.store, db=args.db)
        return emit(sync_cache(store))

    if args.command == "compact":
        store = SnapshotStore.from_paths(project=args.project, store=args.store, db=args.db)
        state = store.load_state()
        scan = store.load_project_scan(state)
        artifacts_dir = store.store_dir / "artifacts"
        artifacts_dir.mkdir(parents=True, exist_ok=True)
        output_path = artifacts_dir / "current_view_compact.json"
        payload = {
            "schema_version": "uepi.current-view-compact.v1",
            "generated_at_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            "source_manifest": str(state.manifest_path),
            "data_mode": state.data_mode,
            "generation": state.generation,
            "counts": {
                "entities": len(scan.get("entities") or []),
                "relations": len(scan.get("relations") or []),
                "diagnostics": len(scan.get("diagnostics") or []),
                "tombstones": len(scan.get("tombstones") or []),
            },
            "scan": scan,
        }
        output_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        cache = sync_cache(store)
        return emit({"schema_version": "uepi.compact-result.v1", "output_path": str(output_path), "cache": cache})

    engine = make_engine(project=args.project, store=args.store, db=args.db)
    try:
        if args.command == "status":
            return emit(engine.status())
        if args.command == "overview":
            return emit(engine.overview(limit=args.limit))
        if args.command == "search":
            return emit(engine.search(query=args.query, kind=args.kind, limit=args.limit))
        if args.command == "asset":
            if not args.asset:
                parser.error("--asset is required for the asset command")
            return emit(engine.asset(args.asset, relation_limit=args.limit))
    finally:
        engine.close()
    parser.error(f"Unknown command: {args.command}")
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
