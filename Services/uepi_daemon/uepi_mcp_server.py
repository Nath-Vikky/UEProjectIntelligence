from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import time
import traceback
from typing import Any
from urllib.parse import parse_qs, unquote, urlparse
import uuid

from uepi_daemon import (
    ARTIFACT_RANGE_MAX_BYTES,
    SCHEMA_VERSION,
    agent_protocol_document,
    animation_query,
    api_document,
    artifact_range,
    asset_history,
    blueprint_cpp_links,
    cancel_job,
    config_values,
    connect,
    db_integrity,
    db_recover,
    cinematics_key_page,
    data_query,
    data_snapshot_page,
    export_graph,
    find_entity,
    get_job,
    graph_page,
    graph_query,
    ingest,
    index_source,
    json_text,
    latest_scan_id,
    list_entities,
    list_jobs,
    list_relations,
    list_scans,
    list_worker_sessions,
    markdown_report,
    poll_jobs,
    related,
    recover_jobs,
    register_worker,
    resolve_scan_id,
    scan_diff,
    search,
    submit_job,
    staleness,
    source_references,
    source_search,
    source_symbols,
    subgraph,
    summary_dict,
    update_job,
    upload_job_chunk,
    worker_heartbeat,
    entity_row,
)


OFFICIAL_SDK_AVAILABLE = importlib.util.find_spec("mcp") is not None
SERVER_NAME = "uepi-mcp"
SERVER_VERSION = str(SCHEMA_VERSION)
JOBS: dict[str, dict[str, Any]] = {}
MCP_ARTIFACT_DIR_NAME = "mcp_artifacts"


def object_schema(properties: dict[str, Any], required: list[str] | None = None) -> dict[str, Any]:
    schema = {
        "type": "object",
        "properties": properties,
        "additionalProperties": False,
    }
    if required:
        schema["required"] = required
    return schema


TOKEN_BUDGET_PROPERTY = {
    "type": "integer",
    "description": "Maximum approximate response tokens. Large results return a preview object.",
    "minimum": 0,
}


def with_token_budget(schema: dict[str, Any]) -> dict[str, Any]:
    result = json.loads(json.dumps(schema))
    result.setdefault("properties", {})["token_budget"] = TOKEN_BUDGET_PROPERTY
    return result


def array_of_strings(description: str) -> dict[str, Any]:
    return {"type": "array", "items": {"type": "string"}, "description": description}


TOOL_SPECS: list[dict[str, Any]] = [
    {
        "name": "uepi_health",
        "description": "Check SQLite index availability and MCP server status.",
        "inputSchema": with_token_budget(object_schema({})),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_project_status",
        "description": "Return the LLM-facing UEPI project, index, freshness, worker, and queue status.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "include_stale": {"type": "boolean", "default": True},
                    "job_limit": {"type": "integer", "default": 10},
                    "worker_limit": {"type": "integer", "default": 20},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_project_refresh",
        "description": "Submit a read-only metadata_scan job, optionally wait for completion, and ingest the resulting scan artifact.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "asset": {"type": "string", "description": "Optional target object path or canonical asset key."},
                    "assets": array_of_strings("Optional target object paths."),
                    "level": {"type": "string", "default": "L2"},
                    "output_path": {"type": "string"},
                    "wait_seconds": {"type": "integer", "default": 0},
                    "poll_interval_seconds": {"type": "number", "default": 0.5},
                    "priority": {"type": "integer", "default": 0},
                    "timeout_seconds": {"type": "integer", "default": 900},
                    "read_blueprints": {"type": "boolean", "default": True},
                    "read_uobject_reflection": {"type": "boolean", "default": True},
                    "ingest_on_success": {"type": "boolean", "default": True},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_read_asset_context",
        "description": "Return LLM-ready read-only context for one asset, with optional refresh, related entities, subgraph, and candidates.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "asset": {"type": "string"},
                    "refresh": {"type": "boolean", "default": False},
                    "wait_seconds": {"type": "integer", "default": 0},
                    "include_snapshot": {"type": "boolean", "default": True},
                    "relation_limit": {"type": "integer", "default": 80},
                    "graph_depth": {"type": "integer", "default": 1},
                    "graph_limit": {"type": "integer", "default": 200},
                    "candidate_limit": {"type": "integer", "default": 10},
                    "relation_type": array_of_strings("Optional relation type filters for the subgraph."),
                },
                ["asset"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_read_blueprint",
        "description": "Return LLM-ready Blueprint graph, node, pin, semantic, CFG/DFG, and source-link context for one Blueprint asset.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "asset": {"type": "string"},
                    "refresh": {"type": "boolean", "default": False},
                    "wait_seconds": {"type": "integer", "default": 0},
                    "include_snapshot": {"type": "boolean", "default": True},
                    "relation_limit": {"type": "integer", "default": 120},
                    "graph_depth": {"type": "integer", "default": 2},
                    "graph_limit": {"type": "integer", "default": 400},
                    "candidate_limit": {"type": "integer", "default": 10},
                    "include_cpp_links": {"type": "boolean", "default": True},
                },
                ["asset"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_read_animation",
        "description": "Return LLM-ready animation-domain manifest, relation, subgraph, and snapshot context for one animation asset.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "asset": {"type": "string"},
                    "refresh": {"type": "boolean", "default": False},
                    "wait_seconds": {"type": "integer", "default": 0},
                    "include_snapshot": {"type": "boolean", "default": True},
                    "relation_limit": {"type": "integer", "default": 100},
                    "graph_depth": {"type": "integer", "default": 2},
                    "graph_limit": {"type": "integer", "default": 300},
                    "candidate_limit": {"type": "integer", "default": 10},
                },
                ["asset"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_ingest",
        "description": "Ingest an existing UEPI scan JSON artifact into the local SQLite index.",
        "inputSchema": with_token_budget(
            object_schema({"scan": {"type": "string", "description": "Path to a UEPI scan JSON file."}}, ["scan"])
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_summary",
        "description": "Return a scan summary.",
        "inputSchema": with_token_budget(object_schema({"scan": {"type": "string", "description": "Optional scan id or prefix."}})),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_scans",
        "description": "List ingested scans with cursor pagination.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "limit": {"type": "integer", "default": 50},
                    "cursor": {"type": "string"},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_entities",
        "description": "List entities with cursor pagination.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "scan": {"type": "string"},
                    "kind": {"type": "string"},
                    "limit": {"type": "integer", "default": 100},
                    "cursor": {"type": "string"},
                    "include_snapshot": {"type": "boolean", "default": False},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_relations",
        "description": "List relations with cursor pagination.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "scan": {"type": "string"},
                    "relation_type": {"type": "string"},
                    "limit": {"type": "integer", "default": 100},
                    "cursor": {"type": "string"},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_search",
        "description": "Search entities in the latest scan.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "query": {"type": "string"},
                    "limit": {"type": "integer", "default": 20},
                    "include_snapshot": {"type": "boolean", "default": False},
                },
                ["query"],
            )
        ),
        "outputSchema": {"type": "array"},
    },
    {
        "name": "uepi_related",
        "description": "Return direct relations for an entity id or canonical key.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "entity": {"type": "string"},
                    "limit": {"type": "integer", "default": 50},
                    "include_snapshot": {"type": "boolean", "default": False},
                },
                ["entity"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_subgraph",
        "description": "Return a bounded relation subgraph.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "entity": {"type": "string"},
                    "depth": {"type": "integer", "default": 1},
                    "limit": {"type": "integer", "default": 200},
                    "relation_type": array_of_strings("Optional relation type filters."),
                },
                ["entity"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_graph_page",
        "description": "Page nodes or edges from a bounded relation subgraph.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "entity": {"type": "string"},
                    "depth": {"type": "integer", "default": 1},
                    "collection": {"type": "string", "enum": ["nodes", "edges"], "default": "edges"},
                    "limit": {"type": "integer", "default": 100},
                    "cursor": {"type": "string"},
                    "graph_limit": {"type": "integer", "default": 1000},
                    "relation_type": array_of_strings("Optional relation type filters."),
                },
                ["entity"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_graph_query",
        "description": "Run the compact graph DSL: from <entity> depth <n> relation <type[,type]> limit <n>.",
        "inputSchema": with_token_budget(object_schema({"query": {"type": "string"}}, ["query"])),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_export_graph",
        "description": "Export a bounded subgraph as JSON, DOT, GraphML, Cytoscape JSON, or optional Parquet.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "entity": {"type": "string"},
                    "format": {"type": "string", "enum": ["json", "dot", "mermaid", "graphml", "cytoscape", "parquet"], "default": "json"},
                    "depth": {"type": "integer", "default": 1},
                    "limit": {"type": "integer", "default": 200},
                    "relation_type": array_of_strings("Optional relation type filters."),
                },
                ["entity"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_artifact_range",
        "description": "Read a capped byte range from an ingested scan JSON artifact.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "scan": {"type": "string"},
                    "offset": {"type": "integer", "default": 0},
                    "length": {"type": "integer", "default": 4096, "maximum": ARTIFACT_RANGE_MAX_BYTES},
                    "encoding": {"type": "string", "enum": ["text", "base64"], "default": "text"},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_report",
        "description": "Render a Markdown scan report.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "scan": {"type": "string"},
                    "limit": {"type": "integer", "default": 20},
                }
            )
        ),
        "outputSchema": object_schema({"markdown": {"type": "string"}}),
    },
    {
        "name": "uepi_diff",
        "description": "Compare two ingested scans and return structural impact.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "base": {"type": "string"},
                    "compare": {"type": "string"},
                    "limit": {"type": "integer", "default": 100},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_stale",
        "description": "Check whether scan and package artifacts are stale.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "scan": {"type": "string"},
                    "limit": {"type": "integer", "default": 100},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_history",
        "description": "Return asset revision history.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "asset": {"type": "string"},
                    "limit": {"type": "integer", "default": 50},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_animation_query",
        "description": "Query animation-domain manifest entities from an ingested scan.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "scan": {"type": "string"},
                    "asset": {"type": "string"},
                    "limit": {"type": "integer", "default": 100},
                    "include_snapshot": {"type": "boolean", "default": False},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_data_query",
        "description": "Query data-domain manifest entities from an ingested scan.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "scan": {"type": "string"},
                    "asset": {"type": "string"},
                    "limit": {"type": "integer", "default": 100},
                    "include_snapshot": {"type": "boolean", "default": False},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_data_page",
        "description": "Page rows, columns, entries, bundles, or parent_tables inside a data-domain snapshot.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "entity": {"type": "string"},
                    "scan": {"type": "string"},
                    "collection": {
                        "type": "string",
                        "enum": ["rows", "columns", "entries", "bundles", "parent_tables"],
                        "default": "rows",
                    },
                    "limit": {"type": "integer", "default": 100},
                    "cursor": {"type": "string"},
                    "include_artifact": {"type": "boolean", "default": False},
                },
                ["entity"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_cinematics_key_page",
        "description": "Page Sequencer key time rows from a LevelSequence snapshot and optionally materialize a JSON key artifact.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "entity": {"type": "string"},
                    "scan": {"type": "string"},
                    "section": {"type": "string"},
                    "channel": {"type": "string"},
                    "limit": {"type": "integer", "default": 100},
                    "cursor": {"type": "string"},
                    "include_artifact": {"type": "boolean", "default": False},
                },
                ["entity"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_security_audit",
        "description": "Return the MCP adapter security posture and read-only operation audit.",
        "inputSchema": with_token_budget(object_schema({})),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_integrity",
        "description": "Run SQLite integrity and foreign-key checks.",
        "inputSchema": with_token_budget(object_schema({})),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_recover",
        "description": "Checkpoint WAL, optimize SQLite, and run integrity checks.",
        "inputSchema": with_token_budget(object_schema({})),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_agent_protocol",
        "description": "Return the UEPI worker/session/job protocol document.",
        "inputSchema": with_token_budget(object_schema({})),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_source_index",
        "description": "Index C++/Config source symbols and Unreal path references into SQLite.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "project": {"type": "string"},
                    "compile_database": {"type": "string"},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_source_symbols",
        "description": "List indexed Unreal reflection and C++ source symbols.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "project": {"type": "string"},
                    "kind": {"type": "string"},
                    "query": {"type": "string"},
                    "limit": {"type": "integer", "default": 100},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_source_references",
        "description": "List indexed C++/Config Unreal path references.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "project": {"type": "string"},
                    "kind": {"type": "string"},
                    "query": {"type": "string"},
                    "limit": {"type": "integer", "default": 100},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_source_search",
        "description": "Search indexed source symbols and references.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "query": {"type": "string"},
                    "project": {"type": "string"},
                    "limit": {"type": "integer", "default": 50},
                },
                ["query"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_blueprint_cpp_links",
        "description": "Link Blueprint call-function relations from an ingested scan to indexed C++ source symbols.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "scan": {"type": "string"},
                    "project": {"type": "string"},
                    "query": {"type": "string"},
                    "limit": {"type": "integer", "default": 100},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_config_values",
        "description": "List indexed Unreal config rows and effective values.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "project": {"type": "string"},
                    "section": {"type": "string"},
                    "key": {"type": "string"},
                    "query": {"type": "string"},
                    "limit": {"type": "integer", "default": 100},
                    "include_history": {"type": "boolean", "default": False},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_worker_register",
        "description": "Register a UE worker session and issue a session token.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "worker_id": {"type": "string"},
                    "worker_type": {"type": "string", "default": "editor"},
                    "capabilities": {"type": "object", "default": {}},
                    "ttl_seconds": {"type": "integer", "default": 60},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_worker_heartbeat",
        "description": "Refresh a UE worker session lease.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "session_id": {"type": "string"},
                    "session_token": {"type": "string"},
                    "status": {"type": "string", "default": "online"},
                    "capabilities": {"type": "object"},
                    "ttl_seconds": {"type": "integer", "default": 60},
                },
                ["session_id", "session_token"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_workers",
        "description": "List UE worker sessions.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "status": {"type": "string"},
                    "limit": {"type": "integer", "default": 50},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_queue_submit",
        "description": "Submit a UEPI worker job to the SQLite-backed queue.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "job_type": {"type": "string"},
                    "request": {"type": "object", "default": {}},
                    "priority": {"type": "integer", "default": 0},
                    "timeout_seconds": {"type": "integer", "default": 900},
                    "max_retries": {"type": "integer", "default": 1},
                    "trace_id": {"type": "string"},
                },
                ["job_type"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_queue_poll",
        "description": "Long-poll and lease queued jobs for a worker session.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "session_id": {"type": "string"},
                    "session_token": {"type": "string"},
                    "limit": {"type": "integer", "default": 1},
                    "wait_seconds": {"type": "integer", "default": 0},
                },
                ["session_id", "session_token"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_queue_update",
        "description": "Update a leased worker job state, result, error, or artifact manifest.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "session_id": {"type": "string"},
                    "session_token": {"type": "string"},
                    "job_id": {"type": "string"},
                    "state": {"type": "string"},
                    "result": {"type": "object"},
                    "error": {"type": "object"},
                    "artifacts": {"type": "array"},
                },
                ["session_id", "session_token", "job_id", "state"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_queue_chunk",
        "description": "Upload one base64 artifact chunk for a leased worker job.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "session_id": {"type": "string"},
                    "session_token": {"type": "string"},
                    "job_id": {"type": "string"},
                    "artifact_id": {"type": "string", "default": "artifact"},
                    "ordinal": {"type": "integer", "default": 0},
                    "data_base64": {"type": "string"},
                    "sha256": {"type": "string"},
                },
                ["session_id", "session_token", "job_id", "data_base64"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_queue_jobs",
        "description": "List SQLite-backed worker jobs.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "state": {"type": "string"},
                    "limit": {"type": "integer", "default": 50},
                    "include_events": {"type": "boolean", "default": False},
                }
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_queue_get",
        "description": "Return a SQLite-backed worker job with optional event history.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "job_id": {"type": "string"},
                    "include_events": {"type": "boolean", "default": True},
                },
                ["job_id"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_queue_cancel",
        "description": "Cancel a queued or active worker job.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "job_id": {"type": "string"},
                    "reason": {"type": "string", "default": "cancelled by MCP client"},
                },
                ["job_id"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_queue_recover",
        "description": "Recover stale worker sessions and expired job leases.",
        "inputSchema": with_token_budget(object_schema({})),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_job_start",
        "description": "Run a UEPI MCP operation through the job envelope and retain the result by job id.",
        "inputSchema": with_token_budget(
            object_schema(
                {
                    "operation": {"type": "string"},
                    "arguments": {"type": "object", "default": {}},
                },
                ["operation"],
            )
        ),
        "outputSchema": {"type": "object"},
    },
    {
        "name": "uepi_job_get",
        "description": "Return a retained UEPI MCP job result.",
        "inputSchema": with_token_budget(object_schema({"job_id": {"type": "string"}}, ["job_id"])),
        "outputSchema": {"type": "object"},
    },
]

TOOL_BY_NAME = {tool["name"]: tool for tool in TOOL_SPECS}
CODEX_TOOL_PROFILE = set(TOOL_BY_NAME)
JOB_ALLOWED_OPERATIONS = set(TOOL_BY_NAME) - {"uepi_job_start", "uepi_job_get"}
INDEX_WRITE_TOOLS = [
    "uepi_ingest",
    "uepi_project_refresh",
    "uepi_read_asset_context",
    "uepi_read_blueprint",
    "uepi_read_animation",
    "uepi_recover",
    "uepi_source_index",
    "uepi_worker_register",
    "uepi_worker_heartbeat",
    "uepi_queue_submit",
    "uepi_queue_poll",
    "uepi_queue_update",
    "uepi_queue_chunk",
    "uepi_queue_cancel",
    "uepi_queue_recover",
]
READ_ONLY_TOOLS = sorted(set(TOOL_BY_NAME) - set(INDEX_WRITE_TOOLS) - {"uepi_job_start", "uepi_job_get"})


def simplify_schema_for_codex(value: Any) -> Any:
    if isinstance(value, dict):
        simplified: dict[str, Any] = {}
        for key, child in value.items():
            if key in {
                "additionalProperties",
                "default",
                "examples",
                "exclusiveMaximum",
                "exclusiveMinimum",
                "format",
                "maximum",
                "minimum",
            }:
                continue
            if key == "required" and not child:
                continue
            simplified[key] = simplify_schema_for_codex(child)
        return simplified
    if isinstance(value, list):
        return [simplify_schema_for_codex(item) for item in value]
    return value


def mcp_tool_specs(include_output_schema: bool = False, tool_profile: str = "full") -> list[dict[str, Any]]:
    """Return tool metadata in the broadest MCP-client-compatible shape by default."""
    tools = json.loads(json.dumps(TOOL_SPECS))
    if tool_profile == "codex":
        tools = [tool for tool in tools if tool["name"] in CODEX_TOOL_PROFILE]
        for tool in tools:
            tool["inputSchema"] = simplify_schema_for_codex(tool["inputSchema"])
    elif tool_profile != "full":
        raise ValueError(f"Unknown MCP tool profile: {tool_profile}")
    if not include_output_schema:
        for tool in tools:
            tool.pop("outputSchema", None)
    return tools


def trace_event(trace_file: Path | None, event: str, **fields: Any) -> None:
    if not trace_file:
        return
    try:
        trace_file.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "event": event,
            **fields,
        }
        with trace_file.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(payload, ensure_ascii=False, default=str, separators=(",", ":")) + "\n")
    except Exception:
        pass


def as_int(value: Any, default: int) -> int:
    if value is None or value == "":
        return default
    return int(value)


def as_bool(value: Any, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    return str(value).lower() in {"1", "true", "yes", "on"}


def as_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return [str(item) for item in value if str(item)]
    return [part.strip() for part in str(value).split(",") if part.strip()]


def scan_summary(db_path: Path, scan: str | None) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        scan_id = resolve_scan_id(conn, scan, "Target") if scan else latest_scan_id(conn)
        return summary_dict(conn, scan_id)
    finally:
        conn.close()


def health(db_path: Path) -> dict[str, Any]:
    conn = connect(db_path)
    try:
        scan_count = conn.execute("SELECT COUNT(*) AS count FROM scans").fetchone()["count"]
        latest = None
        if scan_count:
            latest = latest_scan_id(conn)
        return {
            "ok": True,
            "db_path": str(db_path),
            "scan_count": scan_count,
            "latest_scan_id": latest,
            "server": SERVER_NAME,
            "server_version": SERVER_VERSION,
            "official_sdk_available": OFFICIAL_SDK_AVAILABLE,
        }
    finally:
        conn.close()


def security_audit(db_path: Path) -> dict[str, Any]:
    return {
        "read_only_asset_policy": True,
        "does_not_launch_editor": True,
        "does_not_compile_blueprints": True,
        "does_not_save_or_mutate_ue_assets": True,
        "no_shell_execution_tools": True,
        "no_arbitrary_code_execution": True,
        "database_path": str(db_path),
        "artifact_directory": str(mcp_artifact_root(db_path)),
        "write_scopes": [
            "SQLite index selected by --db",
            "MCP response artifacts under the SQLite database directory",
            "Worker-session, job-queue, event, and uploaded-chunk tables under the SQLite database directory",
            "Offline source-symbol and source-reference tables under the SQLite database directory",
        ],
        "read_only_tools": READ_ONLY_TOOLS,
        "index_write_tools": INDEX_WRITE_TOOLS,
        "job_tools": [
            "uepi_job_start",
            "uepi_job_get",
            "uepi_queue_submit",
            "uepi_queue_poll",
            "uepi_queue_update",
            "uepi_queue_chunk",
            "uepi_queue_cancel",
            "uepi_queue_recover",
        ],
        "artifact_policy": {
            "large_structured_results": "written to uepi://mcp-artifact/{artifact_id} when token_budget is exceeded",
            "scan_artifact_ranges": f"capped at {ARTIFACT_RANGE_MAX_BYTES} bytes per call",
        },
        "official_sdk_available": OFFICIAL_SDK_AVAILABLE,
        "official_sdk_note": "Install optional requirements-mcp.txt to run against the official Python MCP SDK when desired.",
    }


JOB_TERMINAL_STATES = {"succeeded", "failed", "cancelled"}

BLUEPRINT_CONTEXT_RELATIONS = [
    "contains_graph",
    "contains_node",
    "has_pin",
    "connects_to",
    "exec_flows_to",
    "data_flows_to",
    "contains_cfg_block",
    "cfg_flows_to",
    "contains_dfg_value",
    "dfg_defines",
    "dfg_uses",
    "calls_function",
    "reads_variable",
    "writes_variable",
    "casts_to",
    "spawns_class",
    "loads_asset",
    "declares_function",
    "declares_variable",
    "class_references",
    "asset_reference",
]

ANIMATION_CONTEXT_RELATIONS = [
    "uses_skeleton",
    "uses_skeletal_mesh",
    "contains_bone",
    "contains_virtual_bone",
    "contains_socket",
    "contains_track",
    "animates_bone",
    "contains_montage",
    "contains_composite",
    "contains_blend_space",
    "contains_blend_sample",
    "samples_animation",
    "contains_pose_asset",
    "contains_pose",
    "contains_curve",
    "uses_animation",
    "contains_anim_blueprint",
    "contains_anim_state_machine",
    "contains_anim_state",
    "contains_anim_transition",
    "contains_anim_asset_player",
    "contains_anim_cached_pose",
    "contains_anim_slot",
    "state_transitions_to",
    "uses_blendspace",
    "uses_cached_pose",
    "uses_source_ik_rig",
    "uses_target_ik_rig",
]


def caught_error(exc: BaseException) -> dict[str, str]:
    return {"type": type(exc).__name__, "message": str(exc)}


def call_or_error(fn: Any, default: Any = None) -> tuple[Any, dict[str, str] | None]:
    try:
        return fn(), None
    except BaseException as exc:  # Keep high-level LLM tools from terminating the stdio server on lookup misses.
        if isinstance(exc, KeyboardInterrupt):
            raise
        return default, caught_error(exc)


def safe_slug(value: str, default: str = "project") -> str:
    text = value.strip().replace("\\", "/")
    if "." in text:
        text = text.rsplit(".", 1)[-1]
    if "/" in text:
        text = text.rsplit("/", 1)[-1]
    safe = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in text)
    return safe[:80] or default


def refresh_output_path(db_path: Path, asset: str) -> str:
    root = db_path.parent / "mcp_live_scans"
    root.mkdir(parents=True, exist_ok=True)
    stamp = int(time.time() * 1000)
    return str((root / f"{safe_slug(asset)}_{stamp}.json").resolve())


def wait_for_queue_job(db_path: Path, job_id: str, wait_seconds: int, poll_interval_seconds: float) -> tuple[dict[str, Any], bool]:
    deadline = time.monotonic() + max(0, wait_seconds)
    interval = max(0.1, min(float(poll_interval_seconds or 0.5), 5.0))
    while True:
        job = get_job(db_path, job_id, True)
        if str(job.get("state")) in JOB_TERMINAL_STATES:
            return job, False
        if time.monotonic() >= deadline:
            return job, True
        time.sleep(interval)


def project_status(db_path: Path, include_stale: bool = True, job_limit: int = 10, worker_limit: int = 20) -> dict[str, Any]:
    recover_result, recover_error = call_or_error(lambda: recover_jobs(db_path), {})
    health_result, health_error = call_or_error(lambda: health(db_path), {"ok": False, "scan_count": 0})
    workers_result, workers_error = call_or_error(lambda: list_worker_sessions(db_path, None, worker_limit), {"workers": []})
    jobs_result, jobs_error = call_or_error(lambda: list_jobs(db_path, None, job_limit, False), {"jobs": []})

    latest_summary = None
    latest_error = None
    freshness = None
    freshness_error = None
    if int(health_result.get("scan_count") or 0) > 0:
        latest_summary, latest_error = call_or_error(lambda: scan_summary(db_path, None))
        if include_stale:
            freshness, freshness_error = call_or_error(lambda: staleness(db_path, None, 25))

    workers = workers_result.get("workers", []) if isinstance(workers_result, dict) else []
    online_workers = [
        worker for worker in workers
        if str(worker.get("status")).lower() in {"online", "registered", "idle", "busy"}
    ]
    live_editor_workers = [worker for worker in online_workers if str(worker.get("worker_type")).lower() == "editor"]
    commandlet_workers = [worker for worker in online_workers if str(worker.get("worker_type")).lower() == "commandlet"]
    jobs = jobs_result.get("jobs", []) if isinstance(jobs_result, dict) else []
    queue_counts: dict[str, int] = {}
    for job in jobs:
        state = str(job.get("state") or "unknown")
        queue_counts[state] = queue_counts.get(state, 0) + 1

    errors = [
        {"scope": scope, **error}
        for scope, error in [
            ("recover", recover_error),
            ("health", health_error),
            ("workers", workers_error),
            ("jobs", jobs_error),
            ("summary", latest_error),
            ("freshness", freshness_error),
        ]
        if error
    ]
    return {
        "schema_version": "uepi.project_status.v1",
        "ok": bool(health_result.get("ok")) and not health_error,
        "db_path": str(db_path),
        "project_state": "indexed" if int(health_result.get("scan_count") or 0) > 0 else "empty",
        "latest_scan_id": health_result.get("latest_scan_id"),
        "latest_summary": latest_summary,
        "freshness": freshness,
        "workers": workers,
        "online_worker_count": len(online_workers),
        "live_editor_worker_count": len(live_editor_workers),
        "commandlet_worker_count": len(commandlet_workers),
        "jobs": jobs,
        "queue_counts": queue_counts,
        "recover": recover_result,
        "llm_readiness": {
            "can_query_index": int(health_result.get("scan_count") or 0) > 0,
            "has_live_editor_worker": bool(live_editor_workers),
            "has_commandlet_worker": bool(commandlet_workers),
            "can_refresh_with_worker": bool(live_editor_workers or commandlet_workers),
        },
        "errors": errors,
    }


def project_refresh(db_path: Path, arguments: dict[str, Any]) -> dict[str, Any]:
    asset = str(arguments.get("asset") or "").strip()
    assets = as_list(arguments.get("assets"))
    if asset and asset not in assets:
        assets.insert(0, asset)
    output_path = str(arguments.get("output_path") or "").strip() or refresh_output_path(db_path, asset or "project")
    request: dict[str, Any] = {
        "level": str(arguments.get("level") or "L2"),
        "output_path": output_path,
        "read_blueprints": as_bool(arguments.get("read_blueprints"), True),
        "read_blueprint_graphs": as_bool(arguments.get("read_blueprints"), True),
        "read_uobject_reflection": as_bool(arguments.get("read_uobject_reflection"), True),
    }
    if len(assets) == 1:
        request["asset"] = assets[0]
    elif len(assets) > 1:
        request["assets"] = assets

    submitted = submit_job(
        db_path,
        "metadata_scan",
        request,
        as_int(arguments.get("priority"), 0),
        as_int(arguments.get("timeout_seconds"), 900),
        1,
        arguments.get("trace_id"),
    )
    wait_seconds = as_int(arguments.get("wait_seconds"), 0)
    timed_out = False
    job = submitted
    if wait_seconds > 0:
        job, timed_out = wait_for_queue_job(
            db_path,
            str(submitted["job_id"]),
            wait_seconds,
            float(arguments.get("poll_interval_seconds") or 0.5),
        )

    ingest_result = None
    ingest_error = None
    scan_path = ""
    if str(job.get("state")) == "succeeded" and as_bool(arguments.get("ingest_on_success"), True):
        result = job.get("result") if isinstance(job.get("result"), dict) else {}
        scan_path = str(result.get("scan_path") or request.get("output_path") or "")
        if scan_path:
            ingest_result, ingest_error = call_or_error(lambda: ingest(Path(scan_path), db_path))

    return {
        "schema_version": "uepi.project_refresh.v1",
        "submitted": submitted,
        "job": job,
        "timed_out": timed_out,
        "scan_path": scan_path or output_path,
        "ingest": ingest_result,
        "ingest_error": ingest_error,
        "status": "ready" if ingest_result else ("pending" if timed_out or str(job.get("state")) not in JOB_TERMINAL_STATES else str(job.get("state"))),
    }


def exact_entity_context(db_path: Path, asset: str, include_snapshot: bool) -> tuple[dict[str, Any] | None, str | None, dict[str, str] | None]:
    try:
        conn = connect(db_path)
        try:
            scan_id = latest_scan_id(conn)
            row = find_entity(conn, scan_id, asset)
            return (entity_row(row, include_snapshot) if row else None), scan_id, None
        finally:
            conn.close()
    except BaseException as exc:
        if isinstance(exc, KeyboardInterrupt):
            raise
        return None, None, caught_error(exc)


def search_candidates(db_path: Path, asset: str, limit: int) -> list[dict[str, Any]]:
    candidates, _ = call_or_error(lambda: search(db_path, asset, limit, False), [])
    if candidates:
        return candidates
    fallback = safe_slug(asset, "")
    if fallback and fallback != asset:
        fallback_candidates, _ = call_or_error(lambda: search(db_path, fallback, limit, False), [])
        return fallback_candidates or []
    return []


def read_asset_context(
    db_path: Path,
    arguments: dict[str, Any],
    *,
    domain: str = "asset",
    relation_types: list[str] | None = None,
) -> dict[str, Any]:
    asset = str(arguments["asset"]).strip()
    refresh_result = None
    if as_bool(arguments.get("refresh"), False):
        refresh_args = dict(arguments)
        refresh_args.setdefault("asset", asset)
        refresh_result = project_refresh(db_path, refresh_args)

    include_snapshot = as_bool(arguments.get("include_snapshot"), True)
    entity, scan_id, entity_error = exact_entity_context(db_path, asset, include_snapshot)
    relation_limit = as_int(arguments.get("relation_limit"), 80)
    graph_depth = as_int(arguments.get("graph_depth"), 1)
    graph_limit = as_int(arguments.get("graph_limit"), 200)
    requested_relation_types = as_list(arguments.get("relation_type")) or list(relation_types or [])

    related_result = None
    related_error = None
    subgraph_result = None
    subgraph_error = None
    if entity:
        related_result, related_error = call_or_error(
            lambda: related(db_path, asset, relation_limit, include_snapshot),
            None,
        )
        subgraph_result, subgraph_error = call_or_error(
            lambda: subgraph(db_path, asset, graph_depth, graph_limit, requested_relation_types),
            None,
        )

    freshness = None
    freshness_error = None
    if scan_id:
        freshness, freshness_error = call_or_error(lambda: staleness(db_path, scan_id, 25), None)

    candidates = [] if entity else search_candidates(db_path, asset, as_int(arguments.get("candidate_limit"), 10))
    return {
        "schema_version": "uepi.read_context.v1",
        "domain": domain,
        "asset": asset,
        "scan_id": scan_id,
        "context_found": entity is not None,
        "entity": entity,
        "related": related_result,
        "subgraph": subgraph_result,
        "candidates": candidates,
        "freshness": freshness,
        "refresh": refresh_result,
        "completeness": {
            "state": "complete" if entity else "missing",
            "covered": ["indexed_entity", "relations", "subgraph"] if entity else ["candidate_search"],
            "omitted": [] if entity else ["exact_entity_context"],
            "warnings": ["exact asset not found in latest scan"] if not entity else [],
        },
        "errors": [
            {"scope": scope, **error}
            for scope, error in [
                ("entity", entity_error),
                ("related", related_error),
                ("subgraph", subgraph_error),
                ("freshness", freshness_error),
            ]
            if error
        ],
    }


def read_blueprint_context(db_path: Path, arguments: dict[str, Any]) -> dict[str, Any]:
    args = dict(arguments)
    args.setdefault("read_blueprints", True)
    args.setdefault("read_uobject_reflection", True)
    args.setdefault("level", "L2")
    result = read_asset_context(db_path, args, domain="blueprint", relation_types=BLUEPRINT_CONTEXT_RELATIONS)
    if as_bool(arguments.get("include_cpp_links"), True):
        links, error = call_or_error(
            lambda: blueprint_cpp_links(
                db_path,
                result.get("scan_id"),
                arguments.get("project"),
                str(arguments["asset"]),
                50,
            ),
            None,
        )
        result["blueprint_cpp_links"] = links
        if error:
            result.setdefault("errors", []).append({"scope": "blueprint_cpp_links", **error})
    return result


def read_animation_context(db_path: Path, arguments: dict[str, Any]) -> dict[str, Any]:
    args = dict(arguments)
    args.setdefault("level", "L2")
    result = read_asset_context(db_path, args, domain="animation", relation_types=ANIMATION_CONTEXT_RELATIONS)
    manifest, error = call_or_error(
        lambda: animation_query(
            db_path,
            result.get("scan_id"),
            str(arguments["asset"]),
            as_int(arguments.get("relation_limit"), 100),
            as_bool(arguments.get("include_snapshot"), True),
        ),
        None,
    )
    result["animation_manifest"] = manifest
    if error:
        result.setdefault("errors", []).append({"scope": "animation_manifest", **error})
    return result


def run_tool(name: str, arguments: dict[str, Any], db_path: Path) -> Any:
    if name == "uepi_health":
        return health(db_path)
    if name == "uepi_project_status":
        return project_status(
            db_path,
            as_bool(arguments.get("include_stale"), True),
            as_int(arguments.get("job_limit"), 10),
            as_int(arguments.get("worker_limit"), 20),
        )
    if name == "uepi_project_refresh":
        return project_refresh(db_path, arguments)
    if name == "uepi_read_asset_context":
        return read_asset_context(db_path, arguments)
    if name == "uepi_read_blueprint":
        return read_blueprint_context(db_path, arguments)
    if name == "uepi_read_animation":
        return read_animation_context(db_path, arguments)
    if name == "uepi_ingest":
        return ingest(Path(str(arguments["scan"])), db_path)
    if name == "uepi_summary":
        return scan_summary(db_path, arguments.get("scan"))
    if name == "uepi_scans":
        return list_scans(db_path, as_int(arguments.get("limit"), 50), arguments.get("cursor"))
    if name == "uepi_entities":
        return list_entities(
            db_path,
            arguments.get("scan"),
            arguments.get("kind"),
            as_int(arguments.get("limit"), 100),
            arguments.get("cursor"),
            as_bool(arguments.get("include_snapshot")),
        )
    if name == "uepi_relations":
        return list_relations(
            db_path,
            arguments.get("scan"),
            arguments.get("relation_type"),
            as_int(arguments.get("limit"), 100),
            arguments.get("cursor"),
        )
    if name == "uepi_search":
        return search(
            db_path,
            str(arguments["query"]),
            as_int(arguments.get("limit"), 20),
            as_bool(arguments.get("include_snapshot")),
        )
    if name == "uepi_related":
        return related(
            db_path,
            str(arguments["entity"]),
            as_int(arguments.get("limit"), 50),
            as_bool(arguments.get("include_snapshot")),
        )
    if name == "uepi_subgraph":
        return subgraph(
            db_path,
            str(arguments["entity"]),
            as_int(arguments.get("depth"), 1),
            as_int(arguments.get("limit"), 200),
            as_list(arguments.get("relation_type")),
        )
    if name == "uepi_graph_page":
        return graph_page(
            db_path,
            str(arguments["entity"]),
            as_int(arguments.get("depth"), 1),
            str(arguments.get("collection") or "edges"),
            as_int(arguments.get("limit"), 100),
            arguments.get("cursor"),
            as_list(arguments.get("relation_type")),
            as_int(arguments.get("graph_limit"), 1000),
        )
    if name == "uepi_graph_query":
        return graph_query(db_path, str(arguments["query"]))
    if name == "uepi_export_graph":
        payload, content_type, is_binary = export_graph(
            db_path,
            str(arguments["entity"]),
            as_int(arguments.get("depth"), 1),
            as_int(arguments.get("limit"), 200),
            as_list(arguments.get("relation_type")),
            str(arguments.get("format") or "json"),
        )
        if is_binary:
            import base64

            payload = base64.b64encode(payload).decode("ascii")
        return {
            "content_type": content_type,
            "is_binary": is_binary,
            "format": str(arguments.get("format") or "json"),
            "payload": payload,
        }
    if name == "uepi_artifact_range":
        return artifact_range(
            db_path,
            arguments.get("scan"),
            as_int(arguments.get("offset"), 0),
            as_int(arguments.get("length"), 4096),
            str(arguments.get("encoding") or "text"),
        )
    if name == "uepi_report":
        return {"markdown": markdown_report(db_path, arguments.get("scan"), as_int(arguments.get("limit"), 20))}
    if name == "uepi_diff":
        return scan_diff(
            db_path,
            arguments.get("base"),
            arguments.get("compare"),
            as_int(arguments.get("limit"), 100),
        )
    if name == "uepi_stale":
        return staleness(db_path, arguments.get("scan"), as_int(arguments.get("limit"), 100))
    if name == "uepi_history":
        return asset_history(db_path, arguments.get("asset"), as_int(arguments.get("limit"), 50))
    if name == "uepi_animation_query":
        return animation_query(
            db_path,
            arguments.get("scan"),
            arguments.get("asset"),
            as_int(arguments.get("limit"), 100),
            as_bool(arguments.get("include_snapshot")),
        )
    if name == "uepi_data_query":
        return data_query(
            db_path,
            arguments.get("scan"),
            arguments.get("asset"),
            as_int(arguments.get("limit"), 100),
            as_bool(arguments.get("include_snapshot")),
        )
    if name == "uepi_data_page":
        return data_snapshot_page(
            db_path,
            str(arguments.get("entity") or ""),
            str(arguments.get("collection") or "rows"),
            arguments.get("scan"),
            as_int(arguments.get("limit"), 100),
            arguments.get("cursor"),
            as_bool(arguments.get("include_artifact")),
        )
    if name == "uepi_cinematics_key_page":
        return cinematics_key_page(
            db_path,
            str(arguments.get("entity") or ""),
            arguments.get("scan"),
            as_int(arguments.get("limit"), 100),
            arguments.get("cursor"),
            arguments.get("section"),
            arguments.get("channel"),
            as_bool(arguments.get("include_artifact")),
        )
    if name == "uepi_security_audit":
        return security_audit(db_path)
    if name == "uepi_integrity":
        return db_integrity(db_path)
    if name == "uepi_recover":
        return db_recover(db_path)
    if name == "uepi_agent_protocol":
        return agent_protocol_document()
    if name == "uepi_source_index":
        return index_source(
            db_path,
            arguments.get("project"),
            arguments.get("compile_database"),
        )
    if name == "uepi_source_symbols":
        return source_symbols(
            db_path,
            arguments.get("project"),
            arguments.get("kind"),
            arguments.get("query"),
            as_int(arguments.get("limit"), 100),
        )
    if name == "uepi_source_references":
        return source_references(
            db_path,
            arguments.get("project"),
            arguments.get("kind"),
            arguments.get("query"),
            as_int(arguments.get("limit"), 100),
        )
    if name == "uepi_source_search":
        return source_search(
            db_path,
            str(arguments["query"]),
            arguments.get("project"),
            as_int(arguments.get("limit"), 50),
        )
    if name == "uepi_blueprint_cpp_links":
        return blueprint_cpp_links(
            db_path,
            arguments.get("scan"),
            arguments.get("project"),
            arguments.get("query"),
            as_int(arguments.get("limit"), 100),
        )
    if name == "uepi_config_values":
        return config_values(
            db_path,
            arguments.get("project"),
            arguments.get("section"),
            arguments.get("key"),
            arguments.get("query"),
            as_int(arguments.get("limit"), 100),
            as_bool(arguments.get("include_history")),
        )
    if name == "uepi_worker_register":
        return register_worker(
            db_path,
            str(arguments.get("worker_id") or ""),
            str(arguments.get("worker_type") or "editor"),
            arguments.get("capabilities") if isinstance(arguments.get("capabilities"), dict) else {},
            as_int(arguments.get("ttl_seconds"), 60),
        )
    if name == "uepi_worker_heartbeat":
        return worker_heartbeat(
            db_path,
            str(arguments["session_id"]),
            str(arguments["session_token"]),
            str(arguments.get("status") or "online"),
            arguments.get("capabilities") if isinstance(arguments.get("capabilities"), dict) else None,
            as_int(arguments.get("ttl_seconds"), 60),
        )
    if name == "uepi_workers":
        return list_worker_sessions(
            db_path,
            arguments.get("status"),
            as_int(arguments.get("limit"), 50),
        )
    if name == "uepi_queue_submit":
        return submit_job(
            db_path,
            str(arguments["job_type"]),
            arguments.get("request") if isinstance(arguments.get("request"), dict) else {},
            as_int(arguments.get("priority"), 0),
            as_int(arguments.get("timeout_seconds"), 900),
            as_int(arguments.get("max_retries"), 1),
            arguments.get("trace_id"),
        )
    if name == "uepi_queue_poll":
        return poll_jobs(
            db_path,
            str(arguments["session_id"]),
            str(arguments["session_token"]),
            as_int(arguments.get("limit"), 1),
            as_int(arguments.get("wait_seconds"), 0),
        )
    if name == "uepi_queue_update":
        return update_job(
            db_path,
            str(arguments["session_id"]),
            str(arguments["session_token"]),
            str(arguments["job_id"]),
            str(arguments["state"]),
            arguments.get("result") if isinstance(arguments.get("result"), dict) else None,
            arguments.get("error") if isinstance(arguments.get("error"), dict) else None,
            arguments.get("artifacts") if isinstance(arguments.get("artifacts"), list) else None,
        )
    if name == "uepi_queue_chunk":
        return upload_job_chunk(
            db_path,
            str(arguments["session_id"]),
            str(arguments["session_token"]),
            str(arguments["job_id"]),
            str(arguments.get("artifact_id") or "artifact"),
            as_int(arguments.get("ordinal"), 0),
            str(arguments["data_base64"]),
            arguments.get("sha256"),
        )
    if name == "uepi_queue_jobs":
        return list_jobs(
            db_path,
            arguments.get("state"),
            as_int(arguments.get("limit"), 50),
            as_bool(arguments.get("include_events")),
        )
    if name == "uepi_queue_get":
        return get_job(
            db_path,
            str(arguments["job_id"]),
            as_bool(arguments.get("include_events"), True),
        )
    if name == "uepi_queue_cancel":
        return cancel_job(
            db_path,
            str(arguments["job_id"]),
            str(arguments.get("reason") or "cancelled by MCP client"),
        )
    if name == "uepi_queue_recover":
        return recover_jobs(db_path)
    if name == "uepi_job_start":
        operation = str(arguments["operation"])
        if operation not in JOB_ALLOWED_OPERATIONS:
            raise ValueError(f"Unsupported job operation: {operation}")
        job_id = uuid.uuid4().hex
        job = {
            "job_id": job_id,
            "operation": operation,
            "status": "running",
            "result": None,
            "error": None,
        }
        JOBS[job_id] = job
        try:
            job["result"] = run_tool(operation, dict(arguments.get("arguments") or {}), db_path)
            job["status"] = "completed"
        except Exception as exc:  # pragma: no cover - defensive job envelope.
            job["status"] = "failed"
            job["error"] = {"type": type(exc).__name__, "message": str(exc)}
        return job
    if name == "uepi_job_get":
        job_id = str(arguments["job_id"])
        if job_id not in JOBS:
            raise ValueError(f"Job not found: {job_id}")
        return JOBS[job_id]
    raise ValueError(f"Unknown tool: {name}")


def mcp_artifact_root(db_path: Path) -> Path:
    return db_path.parent / MCP_ARTIFACT_DIR_NAME / db_path.stem


def write_mcp_artifact(db_path: Path, value: Any) -> dict[str, Any]:
    artifact_root = mcp_artifact_root(db_path)
    artifact_root.mkdir(parents=True, exist_ok=True)
    data = json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True).encode("utf-8")
    digest = hashlib.sha256(data).hexdigest()
    artifact_id = digest[:24]
    artifact_path = artifact_root / f"{artifact_id}.json"
    if not artifact_path.exists():
        artifact_path.write_bytes(data)
    return {
        "artifact_id": artifact_id,
        "artifact_uri": f"uepi://mcp-artifact/{artifact_id}",
        "path": str(artifact_path),
        "size": len(data),
        "sha256": digest,
    }


def read_mcp_artifact(db_path: Path, artifact_id: str) -> str:
    normalized = "".join(ch for ch in artifact_id if ch.isalnum())
    if normalized != artifact_id or not normalized:
        raise ValueError(f"Invalid MCP artifact id: {artifact_id}")
    path = mcp_artifact_root(db_path) / f"{artifact_id}.json"
    if not path.exists():
        raise ValueError(f"MCP artifact not found: {artifact_id}")
    return path.read_text(encoding="utf-8")


def apply_token_budget(value: Any, token_budget: int, db_path: Path) -> Any:
    if token_budget <= 0:
        return value
    text = json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True)
    max_chars = token_budget * 4
    if len(text) <= max_chars:
        return value
    artifact = write_mcp_artifact(db_path, value)
    return {
        "truncated": True,
        "token_budget": token_budget,
        "estimated_tokens": max(1, (len(text) + 3) // 4),
        "artifact": artifact,
        "preview_json": text[:max_chars],
    }


def tool_result(value: Any, token_budget: int, db_path: Path) -> dict[str, Any]:
    structured = apply_token_budget(value, token_budget, db_path)
    text = json.dumps(structured, ensure_ascii=False, indent=2, sort_keys=True)
    return {
        "content": [{"type": "text", "text": text}],
        "structuredContent": structured,
        "isError": False,
    }


def tool_error(exc: Exception) -> dict[str, Any]:
    structured = {"error": type(exc).__name__, "message": str(exc)}
    return {
        "content": [{"type": "text", "text": json_text(structured)}],
        "structuredContent": structured,
        "isError": True,
    }


def read_message(stdin: Any) -> tuple[dict[str, Any], str] | None:
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
        return None
    body = stdin.read(length)
    return json.loads(body.decode("utf-8")), "content-length"


def write_message(stdout: Any, payload: Any, framing: str = "content-length") -> None:
    data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if framing == "json-line":
        stdout.write(data + b"\n")
        stdout.flush()
        return
    stdout.write(
        (
            f"Content-Length: {len(data)}\r\n"
            "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n"
            "\r\n"
        ).encode("ascii")
    )
    stdout.write(data)
    stdout.flush()


class UEPIMCPServer:
    def __init__(
        self,
        db_path: Path,
        token_budget: int,
        include_output_schema: bool = False,
        tool_profile: str = "full",
        trace_file: Path | None = None,
    ) -> None:
        self.db_path = db_path
        self.token_budget = token_budget
        self.include_output_schema = include_output_schema
        self.tool_profile = tool_profile
        self.trace_file = trace_file
        trace_event(
            self.trace_file,
            "server_init",
            db_path=str(db_path),
            include_output_schema=include_output_schema,
            tool_profile=tool_profile,
        )

    def initialize(self, params: dict[str, Any]) -> dict[str, Any]:
        trace_event(
            self.trace_file,
            "initialize",
            protocol_version=params.get("protocolVersion"),
            client_info=params.get("clientInfo"),
        )
        capabilities = {"tools": {"listChanged": False}} if self.tool_profile == "codex" else {
            "tools": {"listChanged": False},
            "resources": {"subscribe": False, "listChanged": False},
            "prompts": {"listChanged": False},
        }
        return {
            "protocolVersion": params.get("protocolVersion", "2024-11-05"),
            "capabilities": capabilities,
            "serverInfo": {
                "name": SERVER_NAME,
                "version": SERVER_VERSION,
            },
        }

    def list_resources(self) -> dict[str, Any]:
        return {
            "resources": [
                {
                    "uri": "uepi://summary",
                    "name": "Latest Scan Summary",
                    "description": "Latest indexed UEPI scan summary.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://scans",
                    "name": "Scan List",
                    "description": "First page of ingested scans.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://report",
                    "name": "Markdown Scan Report",
                    "description": "Markdown report for the latest indexed scan.",
                    "mimeType": "text/markdown",
                },
                {
                    "uri": "uepi://openapi",
                    "name": "UEPI HTTP API Document",
                    "description": "Machine-readable local HTTP API description.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://security-audit",
                    "name": "MCP Security Audit",
                    "description": "Read-only posture, write scopes, and artifact policy for the MCP adapter.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://integrity",
                    "name": "SQLite Integrity",
                    "description": "SQLite integrity and foreign-key check result.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://agent-protocol",
                    "name": "UEPI Agent Protocol",
                    "description": "Worker registration, heartbeat, polling, job, and chunk-upload protocol.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://workers",
                    "name": "Worker Sessions",
                    "description": "Current UEPI worker sessions.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://jobs",
                    "name": "Worker Job Queue",
                    "description": "Recent SQLite-backed worker jobs.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://source-symbols",
                    "name": "Source Symbols",
                    "description": "First page of indexed Unreal reflection and C++ symbols.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://source-references",
                    "name": "Source References",
                    "description": "First page of indexed C++/Config Unreal path references.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://blueprint-cpp-links",
                    "name": "Blueprint C++ Links",
                    "description": "First page of Blueprint call-function links to indexed C++ symbols.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://config-values",
                    "name": "Config Effective Values",
                    "description": "First page of indexed Unreal config rows and effective values.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://animation-query",
                    "name": "Animation Manifest",
                    "description": "First page of animation-domain manifest entities.",
                    "mimeType": "application/json",
                },
                {
                    "uri": "uepi://data-query",
                    "name": "Data Manifest",
                    "description": "First page of data-domain manifest entities.",
                    "mimeType": "application/json",
                },
            ]
        }

    def list_resource_templates(self) -> dict[str, Any]:
        return {
            "resourceTemplates": [
                {
                    "uriTemplate": "uepi://history/{asset}",
                    "name": "Asset History",
                    "description": "Revision history for an asset canonical key, entity id, or revision prefix.",
                    "mimeType": "application/json",
                },
                {
                    "uriTemplate": "uepi://artifact/{scan}?offset={offset}&length={length}",
                    "name": "Scan Artifact Range",
                    "description": "Byte range from an ingested scan JSON artifact.",
                    "mimeType": "application/json",
                },
                {
                    "uriTemplate": "uepi://mcp-artifact/{artifact_id}",
                    "name": "MCP Result Artifact",
                    "description": "Full JSON payload for a token-budgeted tool result.",
                    "mimeType": "application/json",
                },
            ]
        }

    def read_resource(self, uri: str) -> dict[str, Any]:
        parsed = urlparse(uri)
        mime_type = "application/json"
        value: Any
        if uri == "uepi://summary":
            value = scan_summary(self.db_path, None)
        elif uri == "uepi://scans":
            value = list_scans(self.db_path, 50, None)
        elif uri == "uepi://openapi":
            value = api_document()
        elif uri == "uepi://security-audit":
            value = security_audit(self.db_path)
        elif uri == "uepi://integrity":
            value = db_integrity(self.db_path)
        elif uri == "uepi://agent-protocol":
            value = agent_protocol_document()
        elif uri == "uepi://workers":
            value = list_worker_sessions(self.db_path, None, 50)
        elif uri == "uepi://jobs":
            value = list_jobs(self.db_path, None, 50, False)
        elif uri == "uepi://source-symbols":
            value = source_symbols(self.db_path, None, None, None, 50)
        elif uri == "uepi://source-references":
            value = source_references(self.db_path, None, None, None, 50)
        elif uri == "uepi://blueprint-cpp-links":
            value = blueprint_cpp_links(self.db_path, None, None, None, 50)
        elif uri == "uepi://config-values":
            value = config_values(self.db_path, None, None, None, None, 50, False)
        elif uri == "uepi://animation-query":
            value = animation_query(self.db_path, None, None, 50, False)
        elif uri == "uepi://data-query":
            value = data_query(self.db_path, None, None, 50, False)
        elif uri == "uepi://report":
            mime_type = "text/markdown"
            value = markdown_report(self.db_path, None, 20)
        elif parsed.netloc == "history":
            asset = unquote(parsed.path.lstrip("/"))
            value = asset_history(self.db_path, asset or None, 50)
        elif parsed.netloc == "artifact":
            query = parse_qs(parsed.query)
            value = artifact_range(
                self.db_path,
                unquote(parsed.path.lstrip("/")) or None,
                as_int(query.get("offset", [0])[0], 0),
                as_int(query.get("length", [4096])[0], 4096),
                query.get("encoding", ["text"])[0],
            )
        elif parsed.netloc == "mcp-artifact":
            value = read_mcp_artifact(self.db_path, unquote(parsed.path.lstrip("/")))
        else:
            raise ValueError(f"Unknown resource URI: {uri}")

        text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True)
        return {"contents": [{"uri": uri, "mimeType": mime_type, "text": text}]}

    def list_prompts(self) -> dict[str, Any]:
        return {
            "prompts": [
                {
                    "name": "uepi_scan_triage",
                    "description": "Guide an agent through scan health, stale checks, and high-signal graph inspection.",
                    "arguments": [{"name": "focus", "description": "Optional area to prioritize.", "required": False}],
                },
                {
                    "name": "uepi_asset_review",
                    "description": "Guide an agent through revision history and local graph context for one asset.",
                    "arguments": [{"name": "asset", "description": "Asset canonical key or entity id.", "required": True}],
                },
            ]
        }

    def get_prompt(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        if name == "uepi_scan_triage":
            focus = arguments.get("focus") or "the latest scan"
            text = (
                f"Triage {focus} using UEPI MCP tools. Start with uepi_summary and uepi_stale, "
                "then inspect relevant entities, relations, graph-query results, and report only "
                "actionable risks with scan ids and canonical keys."
            )
        elif name == "uepi_asset_review":
            asset = arguments.get("asset")
            if not asset:
                raise ValueError("uepi_asset_review requires asset.")
            text = (
                f"Review UEPI asset context for {asset}. Use uepi_history, uepi_related, and "
                "uepi_subgraph. Summarize revision changes, direct dependencies, and any stale "
                "artifact signals."
            )
        else:
            raise ValueError(f"Unknown prompt: {name}")
        return {
            "description": name,
            "messages": [{"role": "user", "content": {"type": "text", "text": text}}],
        }

    def handle(self, request: Any) -> Any:
        if isinstance(request, list):
            trace_event(self.trace_file, "batch_request", count=len(request))
            responses = [self.handle(item) for item in request]
            return [response for response in responses if response is not None] or None
        if not isinstance(request, dict):
            trace_event(self.trace_file, "invalid_request", request_type=type(request).__name__)
            return {
                "jsonrpc": "2.0",
                "id": None,
                "error": {"code": -32600, "message": "Invalid Request"},
            }
        method = request.get("method")
        params = request.get("params") or {}
        request_id = request.get("id")
        request_trace: dict[str, Any] = {"method": method, "has_id": request_id is not None}
        if method is None:
            request_trace["keys"] = list(request.keys())
            request_trace["payload"] = request
        trace_event(self.trace_file, "request", **request_trace)
        if request_id is None:
            return None

        try:
            if method == "initialize":
                result = self.initialize(params)
            elif method == "ping":
                result = {}
            elif method == "tools/list":
                tools = mcp_tool_specs(self.include_output_schema, self.tool_profile)
                trace_event(
                    self.trace_file,
                    "tools_list",
                    tool_profile=self.tool_profile,
                    tool_count=len(tools),
                    tool_names=[tool["name"] for tool in tools],
                )
                result = {"tools": tools}
            elif method == "tools/call":
                name = params.get("name")
                arguments = dict(params.get("arguments") or {})
                if name not in TOOL_BY_NAME:
                    raise ValueError(f"Unknown tool: {name}")
                token_budget = as_int(arguments.pop("token_budget", None), self.token_budget)
                try:
                    result = tool_result(run_tool(name, arguments, self.db_path), token_budget, self.db_path)
                except Exception as exc:
                    result = tool_error(exc)
            elif method == "resources/list":
                result = self.list_resources()
            elif method == "resources/templates/list":
                result = self.list_resource_templates()
            elif method == "resources/read":
                result = self.read_resource(str(params.get("uri") or ""))
            elif method == "prompts/list":
                result = self.list_prompts()
            elif method == "prompts/get":
                result = self.get_prompt(str(params.get("name") or ""), dict(params.get("arguments") or {}))
            else:
                return {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "error": {"code": -32601, "message": f"Method not found: {method}"},
                }
            return {"jsonrpc": "2.0", "id": request_id, "result": result}
        except Exception as exc:
            return {
                "jsonrpc": "2.0",
                "id": request_id,
                "error": {
                    "code": -32603,
                    "message": str(exc),
                    "data": traceback.format_exc(limit=3),
                },
            }


def run_stdio(
    db_path: Path,
    token_budget: int,
    include_output_schema: bool = False,
    tool_profile: str = "full",
    trace_file: Path | None = None,
) -> int:
    server = UEPIMCPServer(db_path, token_budget, include_output_schema, tool_profile, trace_file)
    stdin = sys.stdin.buffer
    stdout = sys.stdout.buffer
    while True:
        try:
            request_message = read_message(stdin)
        except Exception as exc:
            trace_event(trace_file, "read_error", error=type(exc).__name__, message=str(exc))
            raise
        if request_message is None:
            trace_event(trace_file, "stdin_closed")
            return 0
        request, framing = request_message
        response = server.handle(request)
        if response is not None:
            method = request.get("method") if isinstance(request, dict) else None
            response_trace: dict[str, Any] = {
                "method": method,
                "framing": framing,
                "has_error": isinstance(response, dict) and "error" in response,
                "response_id": response.get("id") if isinstance(response, dict) else None,
            }
            if method == "initialize":
                response_trace["payload"] = response
            trace_event(trace_file, "response", **response_trace)
            write_message(stdout, response, framing)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="UEPI MCP stdio server.")
    parser.add_argument("--db", type=Path, default=Path(".uepi/index.sqlite3"), help="SQLite database path.")
    parser.add_argument("--token-budget", type=int, default=6000, help="Default approximate response token budget.")
    parser.add_argument(
        "--include-output-schema",
        action="store_true",
        help="Include non-essential outputSchema fields in tools/list for clients that support them.",
    )
    parser.add_argument(
        "--tool-profile",
        choices=["full", "codex"],
        default="full",
        help="Limit advertised tools for clients that struggle with large MCP tool catalogs.",
    )
    parser.add_argument(
        "--trace-file",
        type=Path,
        default=None,
        help="Optional JSONL trace file for MCP startup and tools/list diagnostics.",
    )
    parser.add_argument("--sdk-status", action="store_true", help="Print official MCP SDK availability and exit.")
    args = parser.parse_args(argv)

    if args.sdk_status:
        print(json.dumps({"official_sdk_available": OFFICIAL_SDK_AVAILABLE}, indent=2))
        return 0

    env_trace = os.environ.get("UEPI_MCP_TRACE")
    trace_file = args.trace_file or (Path(env_trace) if env_trace else None)
    return run_stdio(args.db, args.token_budget, args.include_output_schema, args.tool_profile, trace_file)


if __name__ == "__main__":
    raise SystemExit(main())
