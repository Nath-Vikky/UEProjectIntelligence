# UE Project Intelligence Development Status

`main` is now the `2.0-dev` Snapshot-first line.

## Implemented

- UE5.3 editor module and `UEPIIndex` commandlet.
- Read-only Asset Registry, UObject Reflection, Blueprint, Animation, World, Data, UI, AI, GAS, Niagara, PCG, MetaSound, Audio, Cinematics, Render, and Material readers from the v1 line.
- Snapshot Store v2 layout under `Saved/UEProjectIntelligence/store`.
- Editor dashboard `Run Snapshot Scan` action that writes `saved.json`.
- Commandlet one-shot Snapshot writer.
- Python `Services/uepi` query package.
- Ten-tool stdio MCP server: status, overview, search, context, asset, blueprint, blueprint trace, animation, impact, diff.
- v2 MCP smoke test without daemon, worker, HTTP, Web UI, or SQLite service.

## Removed From Mainline

- Local daemon.
- HTTP API.
- Web UI.
- Worker registration, heartbeat, queue, lease, and job APIs.
- Commandlet worker mode.
- Extension SDK interfaces.
- Write-operation and runtime-evaluation settings.

## Next

- Per-asset immutable fragments instead of full-scan project fragments.
- Live overlay and automatic invalidation queue.
- Rebuildable SQLite v2 cache for faster large-project queries.
- Broader v2 fixtures around the new Snapshot store.
