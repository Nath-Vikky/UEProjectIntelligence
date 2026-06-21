from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request


def cleanup_db(db_path: Path) -> None:
    for candidate in [db_path, db_path.with_name(db_path.name + "-wal"), db_path.with_name(db_path.name + "-shm")]:
        if candidate.exists():
            candidate.unlink()


def run(args: list[str], expect_success: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(args, text=True, capture_output=True)
    if expect_success and result.returncode:
        raise RuntimeError(result.stderr or result.stdout)
    if not expect_success and result.returncode == 0:
        raise RuntimeError(f"Command unexpectedly succeeded: {' '.join(args)}")
    return result


def http_get(url: str, token: str | None = None) -> tuple[int, str]:
    request = urllib.request.Request(url)
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status, response.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", errors="replace")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="UEPI daemon security and fuzz audit.")
    parser.add_argument("--db", type=Path, default=Path("Saved/UEProjectIntelligence/uepi_security_audit.sqlite3"))
    parser.add_argument("--scan", type=Path, default=Path("Saved/UEProjectIntelligence/l2_character_scan.json"))
    parser.add_argument(
        "--daemon",
        type=Path,
        default=Path("Plugins/UEProjectIntelligence/Services/uepi_daemon/uepi_daemon.py"),
    )
    parser.add_argument(
        "--mcp-server",
        type=Path,
        default=Path("Plugins/UEProjectIntelligence/Services/uepi_daemon/uepi_mcp_server.py"),
    )
    parser.add_argument("--web-ui", type=Path, default=Path("Plugins/UEProjectIntelligence/Web/index.html"))
    args = parser.parse_args(argv)

    web_ui = args.web_ui.read_text(encoding="utf-8")
    mcp_server = args.mcp_server.read_text(encoding="utf-8")
    assert "replace(/[&<>\"']/g" in web_ui
    assert "no_arbitrary_code_execution" in mcp_server
    assert "no_shell_execution_tools" in mcp_server

    cleanup_db(args.db)
    base = [sys.executable, "-B", str(args.daemon), "--db", str(args.db)]
    run(base + ["ingest", "--scan", str(args.scan)])

    integrity = json.loads(run(base + ["integrity"]).stdout)
    assert integrity["ok"], integrity

    run(base + ["entities", "--cursor", "not-a-valid-cursor"], expect_success=False)
    run(base + ["artifact-range", "--offset", "-1"], expect_success=False)
    run(base + ["graph-query", "from asset unsupported_key value"], expect_success=False)

    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as handle:
        handle.write("{}")
        outside_scan = Path(handle.name)
    try:
        rejected = run(base + ["ingest", "--scan", str(outside_scan)], expect_success=False)
        assert "sandbox" in (rejected.stderr + rejected.stdout).lower()
    finally:
        outside_scan.unlink(missing_ok=True)

    with tempfile.TemporaryDirectory(prefix="uepi_outside_project_") as outside_project:
        rejected = run(base + ["source-index", "--project", outside_project], expect_success=False)
        assert "sandbox" in (rejected.stderr + rejected.stdout).lower()

    token = "uepi-test-token"
    proc = subprocess.Popen(
        base + ["serve", "--host", "127.0.0.1", "--port", "0", "--token", token],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        assert proc.stdout is not None
        line = proc.stdout.readline().strip()
        port = line.split(":")[-1].split()[0]
        root = f"http://127.0.0.1:{port}/v1"
        assert http_get(root + "/summary")[0] == 401
        assert http_get(root + "/summary", token)[0] == 200
        assert http_get(root + "/ui")[0] == 200
    finally:
        proc.terminate()
        proc.wait(timeout=10)

    cleanup_db(args.db)
    print("security fuzz audit ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
