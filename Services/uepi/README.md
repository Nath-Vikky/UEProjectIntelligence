# UEPI Snapshot MCP

This package is the v2 read-only path for UE Project Intelligence.

It reads `Saved/UEProjectIntelligence/store/manifests/saved.json` plus immutable `project_fragment` and `asset_fragment` Snapshot objects directly. It does not start a daemon, expose HTTP, register workers, or mutate Unreal assets.

## Codex Example

Command:

```text
C:/Users/renne/AppData/Local/Programs/Python/Python313/python.exe
```

Arguments:

```text
-B
F:/Epic Games/UE5project/GasDemo/Plugins/UEProjectIntelligence/Services/uepi/src/uepi/mcp_server.py
--project
F:/Epic Games/UE5project/GasDemo/GasDemo.uproject
--tool-profile
codex
```

The UE editor is only needed to create or refresh snapshots. Once `saved.json` exists, the MCP server can answer from the latest saved snapshot while the editor is closed.

## Rebuild The Local Cache

```text
python -m uepi sync --project F:/Epic Games/UE5project/GasDemo/GasDemo.uproject
```

The SQLite file is a derived v2.1 query cache under `Saved/UEProjectIntelligence/cache/uepi.sqlite3`. It can be deleted and rebuilt from Snapshot fragments; MCP tools route through it when it is synced with the active Snapshot generation.
