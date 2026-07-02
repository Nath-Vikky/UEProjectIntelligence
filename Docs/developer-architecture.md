# Developer Architecture

UEPI 2.0-dev has one default read path and one guarded edit path:

```text
UE Editor Plugin / UEPIIndex Commandlet
  -> Snapshot Store
  -> Rebuildable SQLite query cache
  -> Services/uepi stdio MCP
  -> AI client
```

```text
AI client
  -> uepi_edit_preview
  -> user approval
  -> Services/uepi edit_apply
  -> localhost live editor bridge
  -> UE editor transaction
  -> validation + targeted refresh + diff report
```

## Components

- `Source/UEProjectIntelligence`: Unreal-only readers and Snapshot writer.
- `Saved/UEProjectIntelligence/store`: immutable Snapshot objects and manifests.
- `Saved/UEProjectIntelligence/store/logs`: append-only incremental editor events.
- `Saved/UEProjectIntelligence/store/requests`: file-based targeted refresh requests created by MCP reads and serviced by the editor.
- `Services/uepi`: Python query package and stdio MCP server.
- `Tools/test_snapshot_mcp_v2.py`: MCP contract smoke test.

## Boundaries

- Unreal code is the only layer that loads assets.
- MCP never directly loads or mutates Unreal UObjects.
- MCP may write only UEPI-owned artifacts under `Saved/UEProjectIntelligence`, such as refresh requests, edit plans, backups, and audit logs.
- Approved asset edits are executed only by the Unreal Editor live bridge on the editor side, behind preview plans, explicit user approval, validation, and rollback/diff reporting.
- SQLite is a rebuildable query cache for MCP routing, not a fact source.
- Daemon, HTTP, Web UI, worker queue, and extension SDK code are not part of the default product.

## Current View

The query layer builds a current view from saved fragments, an active live overlay when present, and `asset_tombstone` fragments. Tombstones remove deleted assets and their asset-local domain entities before cache generation.

## Freshness

Asset-level read tools compare `store/logs/incremental_events.jsonl` with the newest Snapshot observation for the requested target. If the editor is active and the event is newer, the tool queues a targeted refresh request and returns `UEPI_REFRESH_REQUESTED`. If the editor is closed, it returns `UEPI_SNAPSHOT_STALE` while still serving the best available saved Snapshot data.
