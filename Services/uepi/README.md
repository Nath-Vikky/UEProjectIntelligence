# UEPI Snapshot MCP

This package is the v2 Codex MCP path for UE Project Intelligence.

It reads immutable Snapshot objects for offline evidence and connects to the exact-project local Editor Bridge for live reads, guarded plans, writes, refresh, and controlled PIE verification. It does not start a daemon, expose HTTP, or register workers.

## Codex Example

Command:

```text
<PYTHON_EXE>
```

Arguments:

```text
-B
<PROJECT_ROOT>/Plugins/UEProjectIntelligence/Services/uepi/src/uepi/mcp_server.py
--project
<PROJECT_ROOT>/<PROJECT_NAME>.uproject
--tool-profile
codex
```

Once `saved.json` exists, the MCP server can answer from the latest saved snapshot while the editor is closed. Live state, Apply, refresh execution, and runtime verification require the editor.

## Rebuild The Local Cache

```text
python -m uepi sync --project <PROJECT_ROOT>/<PROJECT_NAME>.uproject
```

The SQLite file is a derived v2.1 query cache under `Saved/UEProjectIntelligence/cache/uepi.sqlite3`. It can be deleted and rebuilt from Snapshot fragments; MCP tools route through it when it is synced with the active Snapshot generation.
