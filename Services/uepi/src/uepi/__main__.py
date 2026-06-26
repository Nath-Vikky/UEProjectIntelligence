from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any

from .mcp_server import main as mcp_main
from .query import make_engine


def emit(value: dict[str, Any]) -> int:
    print(json.dumps(value, ensure_ascii=False, indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="UEPI v2 Snapshot query CLI.")
    parser.add_argument("command", choices=["mcp", "status", "overview", "search", "asset"], nargs="?", default="status")
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

    engine = make_engine(project=args.project, store=args.store, db=args.db)
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
    parser.error(f"Unknown command: {args.command}")
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
