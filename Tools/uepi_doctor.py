from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
PYTHON_SOURCE = ROOT / "Services" / "uepi" / "src"
if str(PYTHON_SOURCE) not in sys.path:
    sys.path.insert(0, str(PYTHON_SOURCE))

from uepi import __version__  # noqa: E402
from uepi.bridge_client import bridge_session_path, call_bridge, read_bridge_session  # noqa: E402
from uepi.identity import project_identity, session_matches_identity  # noqa: E402
from uepi.store import SnapshotStore, SnapshotStoreError, _parse_utc  # noqa: E402


def _project_path(value: str) -> Path:
    candidate = Path(value).expanduser().resolve()
    if candidate.is_dir():
        projects = sorted(candidate.glob("*.uproject"))
        if len(projects) != 1:
            raise ValueError(f"Expected exactly one .uproject in {candidate}, found {len(projects)}.")
        candidate = projects[0]
    return candidate


def _check(name: str, ok: bool, message: str, *, required: bool = True, details: dict[str, Any] | None = None) -> dict[str, Any]:
    return {
        "name": name,
        "ok": bool(ok),
        "status": "pass" if ok else ("fail" if required else "warning"),
        "required": required,
        "message": message,
        **({"details": details} if details else {}),
    }


def _skipped(name: str, message: str, *, required: bool = False) -> dict[str, Any]:
    return {"name": name, "ok": not required, "status": "skip", "required": required, "message": message}


def _load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError("JSON root is not an object.")
    return value


def _pid_alive(pid: Any) -> bool:
    try:
        pid_value = int(pid)
        if pid_value <= 0:
            return False
        if os.name == "nt":
            process_query_limited_information = 0x1000
            handle = ctypes.windll.kernel32.OpenProcess(process_query_limited_information, False, pid_value)
            if not handle:
                return False
            ctypes.windll.kernel32.CloseHandle(handle)
            return True
        os.kill(pid_value, 0)
        return True
    except (OSError, TypeError, ValueError):
        return False


def _capability_settings(session: dict[str, Any], catalog_settings: dict[str, Any] | None = None) -> dict[str, Any]:
    advertised = {str(value) for value in session.get("capabilities") or []}
    settings = catalog_settings if isinstance(catalog_settings, dict) else {}
    return {
        "write_enabled": bool(session.get("write_enabled", "edit.apply" in advertised)),
        "allow_save": bool(session.get("allow_save")),
        "allow_pie": bool(session.get("allow_pie")),
        "allow_runtime_invoke": bool(session.get("allow_runtime_invoke")),
        "max_operations_per_transaction": int(settings.get("max_operations_per_transaction") or 0),
        "max_assets_per_transaction": int(settings.get("max_assets_per_transaction") or 0),
    }


def _config_candidates(project_root: Path) -> list[Path]:
    return [
        project_root / ".codex" / "config.toml",
        Path.home() / ".codex" / "config.toml",
    ]


def run_doctor(project: Path, *, require_editor: bool = False) -> dict[str, Any]:
    checks: list[dict[str, Any]] = []
    project_root = project.parent
    plugin = project_root / "Plugins" / "UEProjectIntelligence"
    server = plugin / "Services" / "uepi" / "src" / "uepi" / "mcp_server.py"
    store = SnapshotStore.from_paths(project=project)
    try:
        descriptor = _load_json(plugin / "UEProjectIntelligence.uplugin") if (plugin / "UEProjectIntelligence.uplugin").is_file() else {}
    except (OSError, ValueError, json.JSONDecodeError):
        descriptor = {}
    plugin_version = str(descriptor.get("VersionName") or "")

    project_data: dict[str, Any] = {}
    try:
        project_data = _load_json(project)
        checks.append(_check("uproject", True, f"Parsed {project}."))
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        checks.append(_check("uproject", False, f"Cannot parse project: {exc}"))

    plugin_ok = plugin.resolve() == ROOT.resolve() and (plugin / "UEProjectIntelligence.uplugin").is_file()
    checks.append(_check("plugin_path", plugin_ok, f"Project plugin path is {plugin}." if plugin_ok else f"UEPI Doctor is not running from the project's UEProjectIntelligence plugin: {plugin}."))

    association = str(project_data.get("EngineAssociation") or "")
    engine_ok = association.startswith("5.3")
    checks.append(_check("engine", engine_ok, f"EngineAssociation is {association or 'missing'}; UEPI targets UE 5.3.2."))

    checks.append(_check("python", sys.version_info >= (3, 11), f"Python {sys.version.split()[0]} at {sys.executable}."))
    if server.is_file():
        try:
            probe = subprocess.run(
                [sys.executable, "-B", str(server), "--help"],
                cwd=project_root,
                capture_output=True,
                text=True,
                timeout=8,
                check=False,
            )
            checks.append(_check("mcp_server", probe.returncode == 0, f"MCP server CLI probe exited with {probe.returncode}."))
        except (OSError, subprocess.TimeoutExpired) as exc:
            checks.append(_check("mcp_server", False, f"MCP server CLI probe failed: {exc}"))
    else:
        checks.append(_check("mcp_server", False, f"MCP server is missing: {server}"))

    project_text = project.as_posix().casefold()
    server_text = server.as_posix().casefold()
    matching_configs: list[str] = []
    inspected_configs: list[str] = []
    for config in _config_candidates(project_root):
        if not config.is_file():
            continue
        inspected_configs.append(config.as_posix())
        try:
            text = config.read_text(encoding="utf-8-sig").replace("\\", "/").casefold()
        except OSError:
            continue
        if "[mcp_servers.uepi]" in text and project_text in text and server_text in text:
            matching_configs.append(config.as_posix())
    checks.append(
        _check(
            "codex_config",
            bool(matching_configs),
            "Codex UEPI config targets this project." if matching_configs else "No inspected Codex UEPI config targets this exact project; run Tools/setup_codex.py.",
            details={"matching": matching_configs, "inspected": inspected_configs},
        )
    )

    try:
        store.root.mkdir(parents=True, exist_ok=True)
        test_path = store.root / f".doctor-write-{os.getpid()}.tmp"
        test_path.write_text("uepi", encoding="ascii")
        test_path.unlink()
        checks.append(_check("snapshot_store_access", True, f"Snapshot root is readable and writable: {store.root}."))
    except OSError as exc:
        checks.append(_check("snapshot_store_access", False, f"Snapshot root is not writable: {exc}"))

    state = None
    identity = project_identity(project, {}, store.root)
    try:
        state = store.load_state("saved")
        identity = project_identity(project, state.project, store.root)
        checks.append(_check("saved_manifest", True, f"Saved manifest generation {state.generation} parsed successfully.", details={"path": state.manifest_path.as_posix(), "generation": state.generation}))
        snapshot_engine = str(identity.get("engine_version") or "")
        checks.append(_check("snapshot_engine", snapshot_engine.startswith("5.3.2"), f"Snapshot engine version is {snapshot_engine or 'missing'}."))
    except SnapshotStoreError as exc:
        checks.append(_check("saved_manifest", False, f"Saved Snapshot is unavailable: {exc}"))
        checks.append(_skipped("snapshot_engine", "Snapshot engine cannot be checked without a saved manifest.", required=True))

    checks.append(_check("project_binding", bool(identity.get("project_binding_id")), f"Project binding is {identity.get('project_binding_id') or 'missing'}."))

    session = read_bridge_session(store)
    catalog_result: dict[str, Any] = {}
    if session:
        matched = session_matches_identity(session, identity)
        heartbeat = _parse_utc(session.get("heartbeat_at") or session.get("last_seen_at_utc") or session.get("started_at"))
        age = (datetime.now(timezone.utc) - heartbeat).total_seconds() if heartbeat else None
        fresh = age is not None and age <= 30.0
        checks.append(_check("editor_session_binding", matched, "Editor session matches this project." if matched else "Editor session belongs to another project.", required=require_editor))
        checks.append(_check("editor_pid_heartbeat", fresh and _pid_alive(session.get("pid")), f"Editor PID={session.get('pid')}, heartbeat_age_seconds={round(age, 3) if age is not None else None}.", required=require_editor))
        token_path = session.get("token_path")
        token_candidate = Path(str(token_path)).expanduser() if token_path else None
        if token_candidate and not token_candidate.is_absolute():
            token_candidate = bridge_session_path(store).parent / token_candidate
        token_ok = bool(token_candidate and token_candidate.is_file())
        checks.append(_check("bridge_token", token_ok, "Bridge token file is present." if token_ok else "Bridge token file is missing.", required=require_editor))
        probe = call_bridge(store, "editor.get_status", timeout=1.5, expected_identity=identity)
        checks.append(_check("bridge_probe", bool(probe.get("ok")), "Exact-project bridge probe succeeded." if probe.get("ok") else str((probe.get("error") or {}).get("message") or "Bridge probe failed."), required=require_editor))
        catalog_response = call_bridge(store, "edit.discover", timeout=2.0, expected_identity=identity) if probe.get("ok") else {}
        catalog_result = catalog_response.get("result") if isinstance(catalog_response.get("result"), dict) else {}
        catalog_ok = bool(catalog_response.get("ok") and catalog_result.get("catalog_hash") and catalog_result.get("operations"))
        checks.append(_check("operation_catalog", catalog_ok, f"Operation catalog contains {len(catalog_result.get('operations') or [])} operations." if catalog_ok else "Operation catalog is unavailable.", required=require_editor))
        catalog_settings = catalog_result.get("settings") if isinstance(catalog_result.get("settings"), dict) else {}
        capabilities = _capability_settings(session, catalog_settings)
        capabilities_ok = capabilities["write_enabled"] and capabilities["allow_save"] and capabilities["max_operations_per_transaction"] > 0 and capabilities["max_assets_per_transaction"] > 0
        checks.append(_check("capability_settings", capabilities_ok, f"Guarded write and touched-only save are enabled with limits of {capabilities['max_operations_per_transaction']} operations and {capabilities['max_assets_per_transaction']} assets." if capabilities_ok else "Guarded write, touched-only save, or transaction budgets are unavailable.", required=require_editor, details=capabilities))
    else:
        for name, message in (
            ("editor_session_binding", "No live Editor Bridge session is published."),
            ("editor_pid_heartbeat", "No Editor PID/heartbeat is available."),
            ("bridge_token", "No live bridge token is expected while the Editor is offline."),
            ("bridge_probe", "Bridge probe skipped while the Editor is offline."),
            ("operation_catalog", "Live operation catalog is unavailable while the Editor is offline."),
            ("capability_settings", "Live capability settings are unavailable while the Editor is offline."),
        ):
            checks.append(_skipped(name, message, required=require_editor))

    plugin_roots = sorted(
        item.parent.as_posix()
        for item in (project_root / "Plugins").glob("**/*.uplugin")
        if "Intermediate" not in item.parts
    ) if (project_root / "Plugins").is_dir() else []
    checks.append(_check("project_plugin_roots", bool(plugin_roots), f"Discovered {len(plugin_roots)} project plugin roots.", required=False, details={"roots": plugin_roots}))

    ok = all(item["ok"] for item in checks if item.get("required"))
    return {
        "schema_version": "uepi.doctor-report.v1",
        "ok": ok,
        "project": identity,
        "versions": {
            "plugin_version": str((session or {}).get("plugin_version") or plugin_version or "") or None,
            "plugin_build_id": str((session or {}).get("plugin_build_id") or (f"uepi-{plugin_version}" if plugin_version else "")) or None,
            "catalog_hash": str(catalog_result.get("catalog_hash") or (session or {}).get("catalog_hash") or "") or None,
            "service_version": __version__,
        },
        "editor_required": require_editor,
        "checks": checks,
        "summary": {
            "passed": sum(item["status"] == "pass" for item in checks),
            "warnings": sum(item["status"] == "warning" for item in checks),
            "failed": sum(item["status"] == "fail" for item in checks),
            "skipped": sum(item["status"] == "skip" for item in checks),
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Diagnose a project-local UEPI and Codex MCP installation.")
    parser.add_argument("--project", required=True, help="Path to a .uproject file or its directory.")
    parser.add_argument("--require-editor", action="store_true", help="Fail unless the exact Editor bridge and write catalog are online.")
    parser.add_argument("--output", type=Path, help="Also write the JSON report to this path.")
    args = parser.parse_args(argv)
    try:
        report = run_doctor(_project_path(args.project), require_editor=args.require_editor)
    except (OSError, ValueError) as exc:
        report = {"schema_version": "uepi.doctor-report.v1", "ok": False, "checks": [_check("project", False, str(exc))]}
    rendered = json.dumps(report, ensure_ascii=False, indent=2)
    print(rendered)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    return 0 if report.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
