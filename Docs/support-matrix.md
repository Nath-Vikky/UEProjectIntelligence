# Support Matrix

## Supported In 2.0-dev

- UE 5.3.2 editor and commandlet collection.
- Saved Snapshot querying while the editor is closed.
- Blueprint static graph entities when captured in an L2 Snapshot.
- Animation static metadata and motion summaries when captured in an L2 Snapshot.
- Asset search, context, impact, and generation diff over Snapshot data.
- Codex stdio MCP via `Services/uepi/src/uepi/mcp_server.py`.

## Not Supported In 2.0-dev

- Asset writes, saves, deletes, renames, or Blueprint compiles.
- Runtime pose evaluation.
- Daemon, HTTP, Web UI, worker queue, or remote service deployment.
- Third-party extension SDK.
