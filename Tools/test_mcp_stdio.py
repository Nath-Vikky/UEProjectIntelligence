from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


def send_message(process: subprocess.Popen[bytes], message: dict[str, Any], framing: str) -> None:
    data = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    assert process.stdin is not None
    if framing == "json-line":
        process.stdin.write(data + b"\n")
    else:
        process.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
        process.stdin.write(data)
    process.stdin.flush()


def read_message(process: subprocess.Popen[bytes], expected_framing: str | None = None) -> dict[str, Any]:
    assert process.stdout is not None
    line = process.stdout.readline()
    while line in (b"\r\n", b"\n"):
        line = process.stdout.readline()
    if not line:
        raise RuntimeError("MCP server closed stdout.")

    stripped = line.strip()
    if stripped.startswith((b"{", b"[")):
        if expected_framing and expected_framing != "json-line":
            raise RuntimeError("MCP response used JSON-line framing unexpectedly.")
        return json.loads(stripped.decode("utf-8"))
    if expected_framing and expected_framing != "content-length":
        raise RuntimeError("MCP response used Content-Length framing unexpectedly.")

    headers: dict[str, str] = {}
    while line not in (b"\r\n", b"\n", b""):
        decoded = line.decode("ascii", errors="replace")
        if ":" in decoded:
            key, value = decoded.split(":", 1)
            headers[key.lower()] = value.strip()
        line = process.stdout.readline()

    length = int(headers.get("content-length", "0"))
    if length <= 0:
        raise RuntimeError("MCP response did not include Content-Length.")
    content_type = headers.get("content-type", "")
    if not content_type.startswith("application/vscode-jsonrpc"):
        raise RuntimeError(f"MCP response did not include JSON-RPC Content-Type: {content_type!r}")
    return json.loads(process.stdout.read(length).decode("utf-8"))


def request(
    process: subprocess.Popen[bytes],
    request_id: int,
    method: str,
    params: dict[str, Any] | None = None,
    framing: str = "content-length",
) -> dict[str, Any]:
    send_message(
        process,
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
            "params": params or {},
        },
        framing,
    )
    response = read_message(process, expected_framing=framing)
    if "error" in response:
        raise RuntimeError(json.dumps(response["error"], indent=2))
    return response["result"]


def notification(
    process: subprocess.Popen[bytes],
    method: str,
    params: dict[str, Any] | None = None,
    framing: str = "content-length",
) -> None:
    send_message(
        process,
        {
            "jsonrpc": "2.0",
            "method": method,
            "params": params or {},
        },
        framing,
    )


def assert_no_empty_required(value: Any) -> None:
    if isinstance(value, dict):
        if value.get("required") == []:
            raise AssertionError("empty required arrays should be omitted from MCP schemas")
        for child in value.values():
            assert_no_empty_required(child)
    elif isinstance(value, list):
        for child in value:
            assert_no_empty_required(child)


def assert_no_codex_schema_extras(value: Any) -> None:
    disallowed = {
        "additionalProperties",
        "default",
        "examples",
        "exclusiveMaximum",
        "exclusiveMinimum",
        "format",
        "maximum",
        "minimum",
    }
    if isinstance(value, dict):
        present = disallowed.intersection(value)
        if present:
            raise AssertionError(f"Codex profile schema contains extra keywords: {sorted(present)}")
        for child in value.values():
            assert_no_codex_schema_extras(child)
    elif isinstance(value, list):
        for child in value:
            assert_no_codex_schema_extras(child)


def cleanup_db(db_path: Path) -> None:
    for candidate in [db_path, db_path.with_name(db_path.name + "-wal"), db_path.with_name(db_path.name + "-shm")]:
        if candidate.exists():
            candidate.unlink()
    artifact_root = db_path.parent / "mcp_artifacts" / db_path.stem
    if artifact_root.exists():
        for artifact in artifact_root.glob("*.json"):
            artifact.unlink()
        try:
            artifact_root.rmdir()
        except OSError:
            pass
        try:
            artifact_root.parent.rmdir()
        except OSError:
            pass


def server_command(
    args: argparse.Namespace,
    include_output_schema: bool = False,
    tool_profile: str = "full",
    trace_file: Path | None = None,
) -> list[str]:
    command = [sys.executable, "-B", str(args.server), "--db", str(args.db), "--token-budget", "4000"]
    if include_output_schema:
        command.append("--include-output-schema")
    if tool_profile != "full":
        command.extend(["--tool-profile", tool_profile])
    if trace_file:
        command.extend(["--trace-file", str(trace_file)])
    return command


def assert_include_output_schema_mode(args: argparse.Namespace) -> None:
    process = subprocess.Popen(
        server_command(args, include_output_schema=True),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        request(
            process,
            1,
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "uepi-mcp-output-schema-check", "version": "1"},
            },
        )
        tools = request(process, 2, "tools/list")["tools"]
        assert "inputSchema" in tools[0] and "outputSchema" in tools[0]
    finally:
        if process.stdin:
            process.stdin.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def assert_codex_tool_profile(args: argparse.Namespace) -> None:
    trace_file = args.db.parent / "uepi_mcp_trace_test.jsonl"
    if trace_file.exists():
        trace_file.unlink()
    process = subprocess.Popen(
        server_command(args, tool_profile="codex", trace_file=trace_file),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        init = request(
            process,
            1,
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "uepi-mcp-codex-profile-check", "version": "1"},
            },
        )
        assert init["serverInfo"]["name"] == "uepi-mcp"
        assert set(init["serverInfo"]) == {"name", "version"}
        assert init["capabilities"] == {"tools": {"listChanged": False}}
        notification(process, "notifications/initialized")
        tools = request(process, 2, "tools/list")["tools"]
        tool_names = {tool["name"] for tool in tools}
        assert tool_names == {
            "uepi_health",
            "uepi_project_status",
            "uepi_project_refresh",
            "uepi_read_asset_context",
            "uepi_read_blueprint",
            "uepi_read_animation",
            "uepi_summary",
            "uepi_search",
            "uepi_graph_query",
            "uepi_security_audit",
        }
        assert all("inputSchema" in tool and "outputSchema" not in tool for tool in tools)
        assert_no_empty_required(tools)
        assert_no_codex_schema_extras(tools)
        trace_text = trace_file.read_text(encoding="utf-8")
        assert '"event":"tools_list"' in trace_text
        assert '"tool_count":10' in trace_text
    finally:
        if process.stdin:
            process.stdin.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        if trace_file.exists():
            trace_file.unlink()


def run_profile(args: argparse.Namespace, framing: str) -> None:
    cleanup_db(args.db)
    process = subprocess.Popen(
        server_command(args),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        init = request(
            process,
            1,
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": f"uepi-mcp-smoke-{framing}", "version": "1"},
            },
            framing,
        )
        assert init["serverInfo"]["name"] == "uepi-mcp"
        assert set(init["serverInfo"]) == {"name", "version"}
        notification(process, "notifications/initialized", framing=framing)

        tools = request(process, 2, "tools/list", framing=framing)["tools"]
        tool_names = {tool["name"] for tool in tools}
        for required in {
            "uepi_ingest",
            "uepi_project_status",
            "uepi_project_refresh",
            "uepi_read_asset_context",
            "uepi_read_blueprint",
            "uepi_read_animation",
            "uepi_summary",
            "uepi_graph_query",
            "uepi_report",
            "uepi_security_audit",
            "uepi_integrity",
            "uepi_agent_protocol",
            "uepi_animation_query",
            "uepi_data_query",
            "uepi_cinematics_key_page",
            "uepi_source_search",
            "uepi_blueprint_cpp_links",
            "uepi_config_values",
            "uepi_worker_register",
            "uepi_queue_submit",
            "uepi_job_start",
        }:
            assert required in tool_names
        assert "inputSchema" in tools[0] and "outputSchema" not in tools[0]
        assert_no_empty_required(tools)

        ingest = request(process, 3, "tools/call", {"name": "uepi_ingest", "arguments": {"scan": str(args.scan)}}, framing)
        assert ingest["structuredContent"]["entity_count"] > 0

        summary = request(process, 4, "tools/call", {"name": "uepi_summary", "arguments": {}}, framing)
        assert summary["structuredContent"]["scan_id"] == ingest["structuredContent"]["scan_id"]

        project_status = request(process, 23, "tools/call", {"name": "uepi_project_status", "arguments": {}}, framing)
        assert project_status["structuredContent"]["llm_readiness"]["can_query_index"]

        blueprint_context = request(
            process,
            24,
            "tools/call",
            {
                "name": "uepi_read_blueprint",
                "arguments": {
                    "asset": "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter",
                    "graph_depth": 1,
                    "graph_limit": 80,
                    "relation_limit": 40,
                    "include_cpp_links": True,
                    "token_budget": 100000,
                },
            },
            framing,
        )
        assert blueprint_context["structuredContent"]["context_found"]
        assert blueprint_context["structuredContent"]["domain"] == "blueprint"
        assert blueprint_context["structuredContent"]["entity"]["kind"] == "asset"
        assert "freshness" in blueprint_context["structuredContent"]

        animation_manifest = request(process, 20, "tools/call", {"name": "uepi_animation_query", "arguments": {"limit": 5}}, framing)
        assert animation_manifest["structuredContent"]["domain"] == "animation"
        data_manifest = request(process, 21, "tools/call", {"name": "uepi_data_query", "arguments": {"limit": 5}}, framing)
        assert data_manifest["structuredContent"]["domain"] == "data"
        config_result = request(process, 22, "tools/call", {"name": "uepi_config_values", "arguments": {"limit": 5}}, framing)
        assert "effective" in config_result["structuredContent"]

        resources = request(process, 5, "resources/list", framing=framing)["resources"]
        assert any(resource["uri"] == "uepi://openapi" for resource in resources)
        assert any(resource["uri"] == "uepi://security-audit" for resource in resources)
        assert any(resource["uri"] == "uepi://blueprint-cpp-links" for resource in resources)
        assert any(resource["uri"] == "uepi://config-values" for resource in resources)
        openapi = request(process, 6, "resources/read", {"uri": "uepi://openapi"}, framing)
        assert "/v1/summary" in openapi["contents"][0]["text"]
        assert "/v1/blueprint-cpp-links" in openapi["contents"][0]["text"]
        assert "/v1/config-values" in openapi["contents"][0]["text"]

        prompts = request(process, 7, "prompts/list", framing=framing)["prompts"]
        assert any(prompt["name"] == "uepi_scan_triage" for prompt in prompts)
        prompt = request(process, 8, "prompts/get", {"name": "uepi_asset_review", "arguments": {"asset": "BP_Hero"}}, framing)
        assert "messages" in prompt

        audit = request(process, 9, "tools/call", {"name": "uepi_security_audit", "arguments": {}}, framing)
        assert audit["structuredContent"]["does_not_save_or_mutate_ue_assets"]
        assert audit["structuredContent"]["no_arbitrary_code_execution"]

        integrity = request(process, 14, "tools/call", {"name": "uepi_integrity", "arguments": {}}, framing)
        assert integrity["structuredContent"]["ok"]

        protocol = request(process, 15, "tools/call", {"name": "uepi_agent_protocol", "arguments": {}}, framing)
        assert protocol["structuredContent"]["schema_version"] == "uepi.agent_protocol.v1"

        worker = request(
            process,
            16,
            "tools/call",
            {"name": "uepi_worker_register", "arguments": {"worker_id": f"mcp-worker-{framing}", "capabilities": {"mode": "test"}}},
            framing,
        )
        session_id = worker["structuredContent"]["session_id"]
        session_token = worker["structuredContent"]["session_token"]

        queued = request(
            process,
            17,
            "tools/call",
            {"name": "uepi_queue_submit", "arguments": {"job_type": "metadata_scan", "request": {"asset": "BP_Hero"}}},
            framing,
        )
        queue_job_id = queued["structuredContent"]["job_id"]
        polled = request(
            process,
            18,
            "tools/call",
            {"name": "uepi_queue_poll", "arguments": {"session_id": session_id, "session_token": session_token}},
            framing,
        )
        assert polled["structuredContent"]["jobs"][0]["job_id"] == queue_job_id
        finished = request(
            process,
            19,
            "tools/call",
            {
                "name": "uepi_queue_update",
                "arguments": {
                    "session_id": session_id,
                    "session_token": session_token,
                    "job_id": queue_job_id,
                    "state": "succeeded",
                    "result": {"ok": True},
                },
            },
            framing,
        )
        assert finished["structuredContent"]["state"] == "succeeded"

        budgeted = request(process, 10, "tools/call", {"name": "uepi_entities", "arguments": {"limit": 20, "token_budget": 1}}, framing)
        artifact_uri = budgeted["structuredContent"]["artifact"]["artifact_uri"]
        artifact = request(process, 11, "resources/read", {"uri": artifact_uri}, framing)
        assert "items" in artifact["contents"][0]["text"]

        job = request(
            process,
            12,
            "tools/call",
            {"name": "uepi_job_start", "arguments": {"operation": "uepi_summary", "arguments": {}}},
            framing,
        )
        job_id = job["structuredContent"]["job_id"]
        job_result = request(process, 13, "tools/call", {"name": "uepi_job_get", "arguments": {"job_id": job_id}}, framing)
        assert job_result["structuredContent"]["status"] == "completed"
    finally:
        if process.stdin:
            process.stdin.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        stderr = process.stderr.read().decode("utf-8", errors="replace") if process.stderr else ""
        if process.returncode not in (0, None):
            print(stderr, file=sys.stderr)
        cleanup_db(args.db)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Smoke test the UEPI MCP stdio server.")
    parser.add_argument("--db", type=Path, default=Path("Saved/UEProjectIntelligence/uepi_mcp_test.sqlite3"))
    parser.add_argument("--scan", type=Path, default=Path("Saved/UEProjectIntelligence/l2_character_scan.json"))
    parser.add_argument(
        "--server",
        type=Path,
        default=Path("Plugins/UEProjectIntelligence/Services/uepi_daemon/uepi_mcp_server.py"),
    )
    args = parser.parse_args(argv)

    assert_include_output_schema_mode(args)
    assert_codex_tool_profile(args)
    run_profile(args, "content-length")
    run_profile(args, "json-line")
    print("mcp stdio compatibility assertions ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
