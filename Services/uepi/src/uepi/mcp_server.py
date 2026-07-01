from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import traceback
from typing import Any, BinaryIO

try:
    from . import __version__
    from . import edit
    from .query import make_engine
    from .result import tool_response
except ImportError:  # Allows direct execution as a script from Codex config.
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from uepi import __version__  # type: ignore
    from uepi import edit  # type: ignore
    from uepi.query import make_engine  # type: ignore
    from uepi.result import tool_response  # type: ignore


def object_schema(properties: dict[str, Any], required: list[str] | None = None) -> dict[str, Any]:
    schema: dict[str, Any] = {"type": "object", "properties": properties}
    if required:
        schema["required"] = required
    return schema


TOOLS: list[dict[str, Any]] = [
    {
        "name": "uepi_status",
        "description": "Return Snapshot-backed UEPI project status, freshness, counts, and LLM readiness.",
        "inputSchema": object_schema({}),
    },
    {
        "name": "uepi_overview",
        "description": "Return a compact project overview with entity-kind and relation-type counts.",
        "inputSchema": object_schema({"limit": {"type": "integer"}}),
    },
    {
        "name": "uepi_search",
        "description": "Search indexed UE project entities by asset path, display name, kind, or attributes.",
        "inputSchema": object_schema(
            {
                "query": {"type": "string"},
                "kind": {"type": "string"},
                "limit": {"type": "integer"},
            }
        ),
    },
    {
        "name": "uepi_context",
        "description": "Build a bounded static context bundle for a natural-language UE project question.",
        "inputSchema": object_schema(
            {
                "question": {"type": "string"},
                "route": {"type": "string"},
                "live": {"type": "boolean"},
                "scope": {"type": "array", "items": {"type": "string"}},
                "max_items": {"type": "integer"},
            },
            ["question"],
        ),
    },
    {
        "name": "uepi_asset",
        "description": "Resolve and read one indexed entity or asset with nearby graph context.",
        "inputSchema": object_schema(
            {
                "asset": {"type": "string"},
                "refresh": {"type": "string"},
                "include_snapshot": {"type": "boolean"},
                "relation_limit": {"type": "integer"},
            },
            ["asset"],
        ),
    },
    {
        "name": "uepi_blueprint",
        "description": "Read Blueprint graph/node/pin entities and relations for an indexed Blueprint asset.",
        "inputSchema": object_schema(
            {
                "asset": {"type": "string"},
                "refresh": {"type": "string"},
                "limit": {"type": "integer"},
            },
            ["asset"],
        ),
    },
    {
        "name": "uepi_blueprint_trace",
        "description": "Trace static Blueprint execution/data/delegate/call relations from an indexed Blueprint snapshot.",
        "inputSchema": object_schema(
            {
                "asset": {"type": "string"},
                "start": {"type": "string"},
                "relation_types": {"type": "array", "items": {"type": "string"}},
                "max_depth": {"type": "integer"},
                "max_paths": {"type": "integer"},
            },
            ["asset"],
        ),
    },
    {
        "name": "uepi_animation",
        "description": "Read indexed animation, skeleton, track, notify, curve, and motion-summary context.",
        "inputSchema": object_schema(
            {
                "asset": {"type": "string"},
                "refresh": {"type": "string"},
                "include": {"type": "array", "items": {"type": "string"}},
                "limit": {"type": "integer"},
            },
            ["asset"],
        ),
    },
    {
        "name": "uepi_impact",
        "description": "Return incoming/outgoing relations and affected entities for one asset or entity.",
        "inputSchema": object_schema(
            {
                "asset": {"type": "string"},
                "relation_limit": {"type": "integer"},
            },
            ["asset"],
        ),
    },
    {
        "name": "uepi_diff",
        "description": "Compare two saved Snapshot generations by stable entity and relation IDs.",
        "inputSchema": object_schema(
            {
                "from_generation": {"type": "integer"},
                "to_generation": {"type": "integer"},
            }
        ),
    },
]

WRITE_ALPHA_TOOLS: list[dict[str, Any]] = [
    {
        "name": "uepi_edit_discover",
        "description": "Discover the experimental codex_write_alpha operation catalog without modifying Unreal assets.",
        "inputSchema": object_schema({}),
    },
    {
        "name": "uepi_edit_preview",
        "description": "Create a dry-run UEPI edit operation plan. This foundation build does not apply edits.",
        "inputSchema": object_schema(
            {
                "intent": {"type": "string"},
                "operations": {"type": "array", "items": {"type": "object"}},
                "evidence": {"type": "array", "items": {"type": "object"}},
            }
        ),
    },
    {
        "name": "uepi_edit_apply",
        "description": "Reject edit apply by default until safe write execution is enabled in a later build.",
        "inputSchema": object_schema({"transaction_id": {"type": "string"}, "approved": {"type": "boolean"}}),
    },
    {
        "name": "uepi_edit_validate",
        "description": "Validate an applied UEPI edit transaction. Currently reports that no apply path is enabled.",
        "inputSchema": object_schema({"transaction_id": {"type": "string"}}),
    },
    {
        "name": "uepi_edit_rollback",
        "description": "Rollback an applied UEPI edit transaction. Currently reports that no apply path is enabled.",
        "inputSchema": object_schema({"transaction_id": {"type": "string"}}),
    },
]

SERVER_INSTRUCTIONS = """UEPI is a project-local Unreal Engine 5.3.2 MCP. Always call uepi_status before other tools. Prefer uepi_context to build bounded evidence before answering project questions. Treat Blueprint pin links and evidence as the source of truth. Distinguish live, saved, stale, and refresh_requested data. In read-only profile, never claim you can edit assets. In write profile, never apply edits without edit_preview, user approval, validate, and post-edit diff. Never guess Blueprint pin names; use returned pins and GUIDs.

Use uepi_search or uepi_context to identify the minimum set of assets needed for the user's question.
uepi_context supports routes such as auto, project_overview, input_to_gameplay, blueprint_behavior, animation_playback, ui_flow, asset_dependency_impact, data_driven_behavior, gas_ability_flow, ai_behavior_flow, and network_replication_flow.
Then call the narrow domain tool: uepi_asset for local context, uepi_blueprint for Blueprint graph/node/pin semantic summaries, uepi_blueprint_trace for static flow paths, uepi_animation for animation/skeleton/track summaries, and uepi_impact for dependency impact.
Reads never require the Unreal Editor when the snapshot/cache is current. If a read response includes diagnostic code UEPI_REFRESH_REQUESTED, a targeted editor refresh request has been queued under the Snapshot Store; retry the same read after the editor processes it. If UEPI_SNAPSHOT_STALE appears, ask the user to open the editor/plugin before expecting realtime data.
Treat uepi_diff as a generation-level saved snapshot comparison, not a live editor diff."""


def read_message(stdin: BinaryIO) -> tuple[dict[str, Any], str] | None:
    line = stdin.readline()
    while line in (b"\r\n", b"\n"):
        line = stdin.readline()
    if not line:
        return None

    stripped = line.strip()
    if stripped.startswith((b"{", b"[")):
        return json.loads(stripped.decode("utf-8")), "json-line"

    headers: dict[str, str] = {}
    while line not in (b"\r\n", b"\n", b""):
        decoded = line.decode("ascii", errors="replace")
        if ":" in decoded:
            key, value = decoded.split(":", 1)
            headers[key.lower()] = value.strip()
        line = stdin.readline()

    length = int(headers.get("content-length", "0"))
    if length <= 0:
        raise ValueError("MCP message did not include Content-Length.")
    return json.loads(stdin.read(length).decode("utf-8")), "content-length"


def write_message(stdout: BinaryIO, payload: dict[str, Any], framing: str = "content-length") -> None:
    data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if framing == "json-line":
        stdout.write(data + b"\n")
    else:
        header = (
            f"Content-Length: {len(data)}\r\n"
            "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n"
            "\r\n"
        )
        stdout.write(header.encode("ascii"))
        stdout.write(data)
    stdout.flush()


class UEPIMCPServer:
    def __init__(self, args: argparse.Namespace):
        self.args = args

    def _engine(self):
        return make_engine(project=self.args.project, store=self.args.store, db=self.args.db)

    def initialize(self) -> dict[str, Any]:
        capabilities: dict[str, Any] = {"tools": {"listChanged": False}}
        if self.args.tool_profile == "full":
            capabilities["resources"] = {"listChanged": False}
        return {
            "protocolVersion": "2024-11-05",
            "serverInfo": {"name": "uepi-mcp", "version": __version__},
            "capabilities": capabilities,
            "instructions": SERVER_INSTRUCTIONS,
        }

    def tools(self) -> list[dict[str, Any]]:
        exposed_tools = TOOLS + (WRITE_ALPHA_TOOLS if self.args.tool_profile == "codex_write_alpha" else [])
        if not self.args.include_output_schema:
            return exposed_tools
        tools: list[dict[str, Any]] = []
        for tool in exposed_tools:
            enriched = dict(tool)
            enriched["outputSchema"] = object_schema({"schema_version": {"type": "string"}})
            tools.append(enriched)
        return tools

    def call_tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        try:
            engine = self._engine()
            if name == "uepi_status":
                return tool_response(engine.status())
            if name == "uepi_overview":
                return tool_response(engine.overview(limit=int(arguments.get("limit") or 20)))
            if name == "uepi_search":
                return tool_response(
                    engine.search(
                        query=str(arguments.get("query") or ""),
                        kind=arguments.get("kind") if isinstance(arguments.get("kind"), str) else None,
                        limit=int(arguments.get("limit") or 20),
                    )
                )
            if name == "uepi_context":
                scope = arguments.get("scope")
                return tool_response(
                    engine.context(
                        question=str(arguments.get("question") or ""),
                        route=str(arguments.get("route") or "auto"),
                        live=bool(arguments.get("live", False)),
                        scope=scope if isinstance(scope, list) else None,
                        max_items=int(arguments.get("max_items") or 40),
                    )
                )
            if name == "uepi_asset":
                return tool_response(
                    engine.asset(
                        asset=str(arguments.get("asset") or ""),
                        refresh=str(arguments.get("refresh") or "auto"),
                        include_snapshot=bool(arguments.get("include_snapshot", True)),
                        relation_limit=int(arguments.get("relation_limit") or 80),
                    )
                )
            if name == "uepi_blueprint":
                return tool_response(engine.blueprint(asset=str(arguments.get("asset") or ""), refresh=str(arguments.get("refresh") or "auto"), limit=int(arguments.get("limit") or 200)))
            if name == "uepi_blueprint_trace":
                start = arguments.get("start")
                if isinstance(start, dict):
                    start = json.dumps(start, ensure_ascii=False)
                relation_types = arguments.get("relation_types")
                return tool_response(
                    engine.blueprint_trace(
                        asset=str(arguments.get("asset") or ""),
                        start=str(start) if start else None,
                        relation_types=relation_types if isinstance(relation_types, list) else None,
                        max_depth=int(arguments.get("max_depth") or 8),
                        max_paths=int(arguments.get("max_paths") or 20),
                    )
                )
            if name == "uepi_animation":
                include = arguments.get("include")
                return tool_response(
                    engine.animation(
                        asset=str(arguments.get("asset") or ""),
                        refresh=str(arguments.get("refresh") or "auto"),
                        include=include if isinstance(include, list) else None,
                        limit=int(arguments.get("limit") or 300),
                    )
                )
            if name == "uepi_impact":
                return tool_response(
                    engine.impact(
                        asset=str(arguments.get("asset") or ""),
                        relation_limit=int(arguments.get("relation_limit") or 200),
                    )
                )
            if name == "uepi_diff":
                return tool_response(
                    engine.diff(
                        from_generation=arguments.get("from_generation") if isinstance(arguments.get("from_generation"), int) else None,
                        to_generation=arguments.get("to_generation") if isinstance(arguments.get("to_generation"), int) else None,
                    )
                )
            if self.args.tool_profile == "codex_write_alpha":
                if name == "uepi_edit_discover":
                    return tool_response(edit.discover(engine.store))
                if name == "uepi_edit_preview":
                    operations = arguments.get("operations")
                    evidence = arguments.get("evidence")
                    return tool_response(
                        edit.preview(
                            engine.store,
                            intent=str(arguments.get("intent") or ""),
                            operations=operations if isinstance(operations, list) else [],
                            evidence=evidence if isinstance(evidence, list) else [],
                        )
                    )
                if name == "uepi_edit_apply":
                    return tool_response(
                        edit.apply(
                            engine.store,
                            transaction_id=str(arguments.get("transaction_id") or ""),
                            approved=bool(arguments.get("approved", False)),
                        )
                    )
                if name == "uepi_edit_validate":
                    return tool_response(edit.validate(engine.store, transaction_id=str(arguments.get("transaction_id") or "")))
                if name == "uepi_edit_rollback":
                    return tool_response(edit.rollback(engine.store, transaction_id=str(arguments.get("transaction_id") or "")))
            return tool_response(
                {
                    "schema_version": "uepi.mcp-envelope.v1",
                    "error": {
                        "code": "UEPI_UNKNOWN_TOOL",
                        "message": f"Unknown UEPI tool: {name}",
                        "retryable": False,
                        "candidates": [tool["name"] for tool in TOOLS],
                    },
                    "diagnostics": [],
                }
            )
        except Exception as exc:  # Keep tool failures structured for LLM clients.
            diagnostic = traceback.format_exc(limit=5)
            return tool_response(
                {
                    "schema_version": "uepi.mcp-envelope.v1",
                    "error": {
                        "code": "UEPI_TOOL_FAILED",
                        "message": str(exc),
                        "retryable": False,
                        "candidates": [],
                    },
                    "diagnostics": [{"severity": "error", "message": diagnostic}],
                }
            )

    def resources(self) -> list[dict[str, Any]]:
        return [
            {
                "uri": "uepi://snapshot/manifest",
                "name": "UEPI Saved Snapshot Manifest",
                "mimeType": "application/json",
                "description": "Current saved Snapshot manifest.",
            },
            {
                "uri": "uepi://snapshot/current-view",
                "name": "UEPI Current Snapshot View",
                "mimeType": "application/json",
                "description": "Merged current view after saved fragments, live overlay, and tombstones.",
            },
            {
                "uri": "uepi://snapshot/project-scan",
                "name": "UEPI Current Snapshot View (Legacy URI)",
                "mimeType": "application/json",
                "description": "Legacy URI retained for clients that previously read the merged project scan.",
            },
        ]

    def read_resource(self, uri: str) -> dict[str, Any]:
        engine = self._engine()
        if uri == "uepi://snapshot/manifest":
            text = json.dumps(engine.state.manifest, ensure_ascii=False, indent=2)
        elif uri in {"uepi://snapshot/current-view", "uepi://snapshot/project-scan"}:
            text = json.dumps(engine.scan, ensure_ascii=False, indent=2)
        else:
            raise ValueError(f"Unknown UEPI resource: {uri}")
        return {"contents": [{"uri": uri, "mimeType": "application/json", "text": text}]}

    def handle(self, message: dict[str, Any]) -> dict[str, Any] | None:
        method = message.get("method")
        request_id = message.get("id")
        params = message.get("params") if isinstance(message.get("params"), dict) else {}

        if request_id is None and isinstance(method, str) and method.startswith("notifications/"):
            return None

        try:
            if method == "initialize":
                result = self.initialize()
            elif method == "tools/list":
                result = {"tools": self.tools()}
            elif method == "tools/call":
                name = str(params.get("name") or "")
                arguments = params.get("arguments") if isinstance(params.get("arguments"), dict) else {}
                result = self.call_tool(name, arguments)
            elif method == "resources/list":
                result = {"resources": self.resources()}
            elif method == "resources/read":
                result = self.read_resource(str(params.get("uri") or ""))
            else:
                return {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "error": {"code": -32601, "message": f"Method not found: {method}"},
                }
            return {"jsonrpc": "2.0", "id": request_id, "result": result}
        except Exception as exc:
            return {"jsonrpc": "2.0", "id": request_id, "error": {"code": -32000, "message": str(exc)}}

    def serve(self) -> int:
        stdin = sys.stdin.buffer
        stdout = sys.stdout.buffer
        while True:
            incoming = read_message(stdin)
            if incoming is None:
                return 0
            message, framing = incoming
            response = self.handle(message)
            if response is not None:
                write_message(stdout, response, framing)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="UEPI v2 Snapshot-backed MCP stdio server.")
    parser.add_argument("--project", type=Path, help="Path to a .uproject or project directory.")
    parser.add_argument("--store", type=Path, help="Path to Saved/UEProjectIntelligence or its store directory.")
    parser.add_argument("--db", type=Path, help="Legacy compatibility: derive Saved/UEProjectIntelligence from this DB path's parent.")
    parser.add_argument("--token-budget", type=int, default=4000, help="Accepted for compatibility; v2 tools are bounded by tool arguments.")
    parser.add_argument("--tool-profile", choices=["full", "codex", "codex_write_alpha"], default="full")
    parser.add_argument("--include-output-schema", action="store_true")
    parser.add_argument("--trace-file", type=Path, help="Accepted for compatibility; currently unused.")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    return UEPIMCPServer(args).serve()


if __name__ == "__main__":
    raise SystemExit(main())
