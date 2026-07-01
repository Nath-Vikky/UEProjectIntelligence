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
- Blueprint compile events bind to loaded `UBlueprint::OnCompiled()` delegates and trigger targeted live refresh for the compiled Blueprint.
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
- Unified MCP envelope fields: `ok`, `tool`, `operation`, `data_mode`, `project`, `snapshot`, `result`, `evidence`, `diagnostics`, and `next_actions`, while retaining legacy-compatible `state`, `omissions`, `truncation`, and `continuation`.
- `uepi_status` reports optional live bridge readiness fields without requiring a bridge to exist.
- `uepi_context` routes natural-language questions through project overview, input-to-gameplay, Blueprint behavior, animation playback, UI, dependency impact, data-driven, GAS, AI, and networking routes.
- `uepi_blueprint` includes a semantic summary, call graph grouping, data mutation summary, side effects, and static flow hints derived from Snapshot relations.
- Lightweight C++ UHT-style symbol scan is available in `uepi_overview` and `project_overview` context sections.
- Optional live editor bridge uses localhost length-prefixed TCP JSON when enabled, with token validation and read commands for status, selection, output log tail, and refresh request creation.
- `uepi_context(live=true)` can include live editor bridge status, selection, and output log sections when the bridge is connected.
- `uepi_asset`, `uepi_blueprint`, and `uepi_animation` accept `refresh="force"` and use the live bridge to queue immediate targeted refresh when available.
- Experimental `codex_write_alpha` profile exposes edit discover/preview/apply/validate/rollback tools. Apply is disabled by default in settings; when explicitly enabled, the bridge supports a small Blueprint alpha subset without package saving.
- Disabled-by-default UE settings now reserve live bridge and write safety gates.
- C++ write foundation now includes `IUEPIEditOperation`, `FUEPIEditOperationRegistry`, and a dry-run-aware transaction scope skeleton without registering any asset mutation operations.
- v2 MCP smoke test without daemon, worker, HTTP, Web UI, or SQLite service, including saved+live overlay merge, tombstones, cache sync, initialize instructions, and targeted refresh request creation.
- MCP smoke test now also covers context routes, Blueprint semantic summary, bridge readiness fields, and write-alpha rejection behavior.
- Synthetic Snapshot MCP fixture now covers deleted assets and renamed old-path tombstones with a valid new-path fragment.
- Release packaging script supports `--version` and `--out`, validates required files, scans docs for local paths, writes commit SHA, and packages under a `UEProjectIntelligence/` zip root.
- Public docs and Codex configuration template use `__UE_ROOT__`, `__PROJECT_ROOT__`, `__PROJECT_NAME__`, and `__PYTHON_EXE__` placeholders instead of local machine paths.
- Added Codex setup, read workflow, context routes, optional live bridge, write-alpha design, write safety, operation schema, real-machine test log, and AGENTS template docs.
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

- Broader real-project verification across blank, template, and feature sample projects.
- Future write-operation work should remain separate from the current read-only MCP contract.
- Viewport screenshot artifact creation remains future live-bridge work.
- Write apply/validate/rollback execution remains future work behind `codex_write_alpha`; current implementation is dry-run planning, C++ registry scaffolding, and safe rejection.
