# UEPI Snapshot MCP

This package is the v2 read-only path for UE Project Intelligence.

It reads `Saved/UEProjectIntelligence/store/manifests/saved.json` plus immutable `project_fragment`, `asset_fragment`, and `asset_tombstone` Snapshot objects. It does not start a daemon, expose HTTP, register workers, or mutate Unreal assets.

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

The UE editor is only needed to create or refresh snapshots. Once `saved.json` exists, the MCP server can answer from the latest saved snapshot while the editor is closed.

## Rebuild The Local Cache

```text
python -m uepi sync --project <PROJECT_ROOT>/<PROJECT_NAME>.uproject
```

The SQLite file is a derived v2.1 query cache under `Saved/UEProjectIntelligence/cache/uepi.sqlite3`. It can be deleted and rebuilt from Snapshot fragments; MCP tools route through it when it is synced with the active Snapshot generation.
