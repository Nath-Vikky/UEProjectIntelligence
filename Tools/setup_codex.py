from __future__ import annotations

import argparse
import difflib
from pathlib import Path
import sys


START = "# BEGIN UEPI MANAGED MCP"
END = "# END UEPI MANAGED MCP"


def _toml_string(value: str) -> str:
    return '"' + value.replace("\\", "/").replace('"', '\\"') + '"'


def _find_project(value: str) -> Path:
    path = Path(value).expanduser().resolve()
    if path.is_dir():
        matches = sorted(path.glob("*.uproject"))
        if len(matches) != 1:
            raise ValueError(f"Expected exactly one .uproject in {path}, found {len(matches)}.")
        path = matches[0]
    if path.suffix.lower() != ".uproject" or not path.is_file():
        raise ValueError(f"Project file does not exist: {path}")
    return path


def _managed_block(project: Path, python_exe: Path) -> str:
    plugin = project.parent / "Plugins" / "UEProjectIntelligence"
    server = plugin / "Services" / "uepi" / "src" / "uepi" / "mcp_server.py"
    lines = [
        START,
        "[mcp_servers.uepi]",
        f"command = {_toml_string(str(python_exe))}",
        "args = [",
        '  "-B",',
        f"  {_toml_string(str(server))},",
        '  "--project",',
        f"  {_toml_string(str(project))},",
        '  "--tool-profile",',
        '  "codex",',
        "]",
        f"cwd = {_toml_string(str(project.parent))}",
        "startup_timeout_sec = 20",
        "tool_timeout_sec = 60",
        "required = false",
        "enabled = true",
        END,
    ]
    return "\n".join(lines) + "\n"


def _replace_managed_block(current: str, block: str) -> str:
    start = current.find(START)
    end = current.find(END)
    if (start >= 0) != (end >= 0) or (start >= 0 and end < start):
        raise ValueError("Codex config contains an incomplete UEPI managed block.")
    if start >= 0:
        end += len(END)
        if end < len(current) and current[end] == "\n":
            end += 1
        return current[:start] + block + current[end:]
    prefix = current
    if prefix and not prefix.endswith("\n"):
        prefix += "\n"
    if prefix and not prefix.endswith("\n\n"):
        prefix += "\n"
    return prefix + block


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate the project-local Codex UEPI MCP block.")
    parser.add_argument("--project", required=True, help="Path to a .uproject file or its directory.")
    parser.add_argument("--python", default=sys.executable, help="Python executable used by Codex.")
    parser.add_argument("--config", type=Path, help="Override the default <project>/.codex/config.toml path.")
    parser.add_argument("--apply", action="store_true", help="Write after printing the diff. Default is preview only.")
    args = parser.parse_args(argv)

    try:
        project = _find_project(args.project)
        python_exe = Path(args.python).expanduser().resolve()
        if not python_exe.is_file():
            raise ValueError(f"Python executable does not exist: {python_exe}")
        plugin = project.parent / "Plugins" / "UEProjectIntelligence"
        if not (plugin / "UEProjectIntelligence.uplugin").is_file():
            raise ValueError(f"UEPI is not installed as a project plugin: {plugin}")
        config = (args.config or (project.parent / ".codex" / "config.toml")).resolve()
        current = config.read_text(encoding="utf-8") if config.is_file() else ""
        updated = _replace_managed_block(current, _managed_block(project, python_exe))
    except (OSError, ValueError) as exc:
        print(f"UEPI setup failed: {exc}", file=sys.stderr)
        return 2

    diff = difflib.unified_diff(
        current.splitlines(),
        updated.splitlines(),
        fromfile=str(config),
        tofile=str(config),
        lineterm="",
    )
    print("\n".join(diff) or "UEPI Codex configuration is already current.")
    if args.apply and updated != current:
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_text(updated, encoding="utf-8")
        print(f"Updated {config}")
    elif updated != current:
        print("Preview only. Re-run with --apply to write this change.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
