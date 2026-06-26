# Developer Architecture

UEPI 2.0-dev has one default read path:

```text
UE Editor Plugin / UEPIIndex Commandlet
  -> Snapshot Store
  -> Services/uepi stdio MCP
  -> AI client
```

## Components

- `Source/UEProjectIntelligence`: Unreal-only readers and Snapshot writer.
- `Saved/UEProjectIntelligence/store`: immutable Snapshot objects and manifests.
- `Services/uepi`: Python query package and stdio MCP server.
- `Tools/test_snapshot_mcp_v2.py`: MCP contract smoke test.

## Boundaries

- Unreal code is the only layer that loads assets.
- MCP never writes project assets, config, or source.
- SQLite is a rebuildable query cache for MCP routing, not a fact source.
- Daemon, HTTP, Web UI, worker queue, and extension SDK code are not part of the default product.
