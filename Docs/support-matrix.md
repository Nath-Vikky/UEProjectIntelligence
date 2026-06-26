# Support Matrix

## Supported In 2.0-dev

- UE 5.3.2 editor and commandlet collection.
- Saved Snapshot querying while the editor is closed.
- Fresh editor live overlay querying when `editor-session.json` heartbeat is current.
- Manifests with `project_scan` and `asset_fragment` scan fragments.
- Automatic targeted live overlay scans for asset added, updated, and renamed editor events.
- Automatic saved manifest append on package saved events.
- Blueprint static graph entities when captured in an L2 Snapshot.
- Animation static metadata and motion summaries when captured in an L2 Snapshot.
- Asset search, context, impact, and generation diff over Snapshot data.
- Codex stdio MCP via `Services/uepi/src/uepi/mcp_server.py`.

## Not Supported In 2.0-dev

- Asset writes, saves, deletes, renames, or Blueprint compiles.
- Runtime pose evaluation.
- Daemon, HTTP, Web UI, worker queue, or remote service deployment.
- Third-party extension SDK.
