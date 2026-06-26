# UE Project Intelligence Development Status

`main` is now the `2.0-dev` Snapshot-first line.

## Implemented

- UE5.3 editor module and `UEPIIndex` commandlet.
- Read-only Asset Registry, UObject Reflection, Blueprint, Animation, World, Data, UI, AI, GAS, Niagara, PCG, MetaSound, Audio, Cinematics, Render, and Material readers from the v1 line.
- Optional domain plugins are no longer force-enabled by the UEPI descriptor; remaining module dependencies are tracked for deeper Reader Gate cleanup.
- Snapshot Store v2 layout under `Saved/UEProjectIntelligence/store`.
- Immutable `asset_fragment` objects are written for each indexed asset and referenced from Snapshot manifests.
- Full `project_scan` fragments are still written as a compatibility bridge until the query cache no longer needs them.
- Editor dashboard `Run Snapshot Scan` action that writes `saved.json`.
- Editor live session heartbeat under `store/sessions/editor-session.json`.
- Debounced editor invalidation queue for asset added, updated, renamed, and package saved events.
- Targeted live Snapshot overlay writes to `manifests/live.json`.
- Package-saved targeted scans append to the saved manifest instead of replacing the full saved baseline.
- Commandlet one-shot Snapshot writer.
- Python `Services/uepi` query package.
- Rebuildable SQLite v2 cache via `python -m uepi sync`; cache state is reported by `uepi_status`.
- MCP query auto-selects a fresh live overlay and merges it over the saved Snapshot baseline.
- Relation identity no longer includes descriptive attributes; IDs are based on project, relation type, from ID, and to ID.
- Blueprint derived projections now report `confidence_basis` and use sub-1.0 confidence for static derived flows.
- Ten-tool stdio MCP server: status, overview, search, context, asset, blueprint, blueprint trace, animation, impact, diff.
- v2 MCP smoke test without daemon, worker, HTTP, Web UI, or SQLite service, including saved+live overlay merge.

## Removed From Mainline

- Local daemon.
- HTTP API.
- Web UI.
- Worker registration, heartbeat, queue, lease, and job APIs.
- Commandlet worker mode.
- Extension SDK interfaces.
- Write-operation and runtime-evaluation settings.

## Next

- Remove the compatibility full-scan `project_scan` fragment after the SQLite v2 cache and fragment-only tests are complete.
- Migrate generic string attributes to typed v2 attribute values.
- Route large-project search and graph traversal through the SQLite v2 cache when it is synced.
- Split optional domain readers behind true optional compile/runtime gates to remove UBT dependency warnings.
- Broader v2 fixtures around the new Snapshot store.
