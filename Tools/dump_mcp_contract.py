from __future__ import annotations

import argparse
from datetime import datetime, timezone
import importlib
import json
from pathlib import Path
import re
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
PYTHON_SOURCE = ROOT / "Services" / "uepi" / "src"
DEFAULT_OUTPUT = ROOT / "Tests" / "Contracts" / "mcp-contract-v2.json"
DEFAULT_BRIDGE_SOURCE = (
    ROOT
    / "Source"
    / "UEProjectIntelligence"
    / "Private"
    / "Bridge"
    / "UEPIBridgeProtocol.cpp"
)


def _load_mcp_module():
    sys.path.insert(0, str(PYTHON_SOURCE))
    return importlib.import_module("uepi.mcp_server")


def _extract_capability_function(source: str, function_name: str) -> list[str]:
    marker = f"FUEPIBridgeProtocol::{function_name}()"
    start = source.find(marker)
    if start < 0:
        raise RuntimeError(f"Bridge capability function not found: {function_name}")
    next_function = source.find("FUEPIBridgeProtocol::", start + len(marker))
    block = source[start : next_function if next_function >= 0 else len(source)]
    return re.findall(r'TEXT\("([^\"]+)"\)', block)


def build_contract(bridge_source: Path) -> dict[str, Any]:
    mcp_server = _load_mcp_module()
    source = bridge_source.read_text(encoding="utf-8-sig")
    read_tools = list(mcp_server.TOOLS)
    edit_tools = list(mcp_server.WRITE_ALPHA_TOOLS)
    return {
        "schema_version": "uepi.mcp-contract-snapshot.v2",
        "captured_at_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "uepi_version": getattr(mcp_server, "__version__", "unknown"),
        "profiles": {
            "readonly": [tool["name"] for tool in read_tools],
            "codex": [tool["name"] for tool in read_tools + edit_tools],
        },
        "tools": read_tools + edit_tools,
        "server_instructions": mcp_server.SERVER_INSTRUCTIONS,
        "bridge": {
            "protocol_source": str(bridge_source.relative_to(ROOT)).replace("\\", "/"),
            "read_capabilities": _extract_capability_function(source, "ReadCapabilities"),
            "write_capabilities": _extract_capability_function(source, "WriteCapabilities"),
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Export the UEPI MCP and Editor Bridge contract.")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--bridge-source", type=Path, default=DEFAULT_BRIDGE_SOURCE)
    args = parser.parse_args(argv)

    contract = build_contract(args.bridge_source.resolve())
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(contract, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
