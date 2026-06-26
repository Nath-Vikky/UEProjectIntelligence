# UE Project Intelligence Development Status

`main` is now the `2.0-dev` Snapshot-first line.

## Implemented

- UE5.3 editor module and `UEPIIndex` commandlet.
- Read-only Asset Registry, UObject Reflection, Blueprint, Animation, World, Data, UI, AI, Audio, Cinematics, Render, and Material readers.
- Optional domain readers for EnhancedInput, CommonUI, GameplayAbilities, StateTree, IKRig, ControlRig, Niagara, PCG, and MetaSound are compile-gated and return safe no-op readers when their UE plugins are not explicitly enabled.
- Snapshot Store v2 layout under `Saved/UEProjectIntelligence/store`.
- Immutable `asset_fragment` objects are written for each indexed asset and referenced from Snapshot manifests.
- Project-level metadata is written as a small `project_fragment`; full `project_scan` fragments remain readable only for older snapshots.
- Editor dashboard `Run Snapshot Scan` action that writes `saved.json`.
- Editor live session heartbeat under `store/sessions/editor-session.json`.
- Debounced editor invalidation queue is now limited to package-saved and rename promotion events.
- Editor live session writes incremental change events without scanning the whole project on open.
- Editor polls `store/requests` for MCP-created targeted refresh requests and scans only requested assets.
- Asset removal and old rename paths are represented as `asset_tombstone` Snapshot fragments.
- Targeted live Snapshot overlay writes to `manifests/live.json`.
- Package-saved targeted scans append to the saved manifest instead of replacing the full saved baseline.
- Commandlet one-shot Snapshot writer.
- Python `Services/uepi` query package.
- Generic `attributes` are mirrored into `typed_attributes` v2 wrappers while the legacy string map remains available.
- Rebuildable SQLite v2.1 cache via `python -m uepi sync`; cache state is reported by `uepi_status`.
- MCP query automatically rebuilds a stale or missing SQLite cache from the current Snapshot view.
- MCP search, context, asset, Blueprint, Blueprint trace, animation, and impact tools route through the synced SQLite cache when available.
- MCP query auto-selects a fresh live overlay and merges it over the saved Snapshot baseline.
- MCP asset-level read tools compare incremental events against Snapshot observation time and return `UEPI_REFRESH_REQUESTED` or `UEPI_SNAPSHOT_STALE` diagnostics when needed.
- Current-view Snapshot merge applies saved/live fragments and tombstones before cache generation.
- Relation identity no longer includes descriptive attributes; IDs are based on project, relation type, from ID, and to ID.
- Blueprint derived projections now report `confidence_basis` and use sub-1.0 confidence for static derived flows.
- Ten-tool stdio MCP server: status, overview, search, context, asset, blueprint, blueprint trace, animation, impact, diff.
- v2 MCP smoke test without daemon, worker, HTTP, Web UI, or SQLite service, including saved+live overlay merge, tombstones, cache sync, initialize instructions, and targeted refresh request creation.
- Reader Gate build validation with optional domain modules disabled by default and Blueprint L2 commandlet smoke coverage.

## Removed From Mainline

- Local daemon.
- HTTP API.
- Web UI.
- Worker registration, heartbeat, queue, lease, and job APIs.
- Commandlet worker mode.
- Extension SDK interfaces.
- Write-operation and runtime-evaluation settings.

## Next

- Broader real-project verification across Blueprint, animation, world, UI, AI, audio, PCG, Niagara, and material-heavy samples.
- Future write-operation work should remain separate from the current read-only MCP contract.
