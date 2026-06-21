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


def read_message(process: subprocess.Popen[bytes]) -> dict[str, Any]:
    assert process.stdout is not None
    line = process.stdout.readline()
    while line in (b"\r\n", b"\n"):
        line = process.stdout.readline()
    if not line:
        raise RuntimeError("MCP server closed stdout.")

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
    response = read_message(process)
    if "error" in response:
        raise RuntimeError(json.dumps(response["error"], indent=2))
    return response["result"]


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


def run_profile(args: argparse.Namespace, framing: str) -> None:
    cleanup_db(args.db)
    process = subprocess.Popen(
        [sys.executable, "-B", str(args.server), "--db", str(args.db), "--token-budget", "4000"],
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

        tools = request(process, 2, "tools/list", framing=framing)["tools"]
        tool_names = {tool["name"] for tool in tools}
        for required in {
            "uepi_ingest",
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
        assert "inputSchema" in tools[0] and "outputSchema" in tools[0]

        ingest = request(process, 3, "tools/call", {"name": "uepi_ingest", "arguments": {"scan": str(args.scan)}}, framing)
        assert ingest["structuredContent"]["entity_count"] > 0

        summary = request(process, 4, "tools/call", {"name": "uepi_summary", "arguments": {}}, framing)
        assert summary["structuredContent"]["scan_id"] == ingest["structuredContent"]["scan_id"]

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

    run_profile(args, "content-length")
    run_profile(args, "json-line")
    print("mcp stdio compatibility assertions ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
