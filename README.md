# UE Project Intelligence

UE Project Intelligence is a self-contained, read-only, Codex-first MCP server and Unreal Editor plugin for helping AI agents understand, navigate, and explain Unreal Engine 5.3.2 projects from UEPI snapshots.

UEPI does not depend on Epic's UE5.8 ModelContextProtocol plugin. The official UE5.8 MCP implementation is only used as a design reference.

## What It Is

- An Unreal Editor plugin that reads project assets, Blueprints, animations, worlds, data, UI, AI, audio, render, and material metadata.
- A Snapshot Store under `Saved/UEProjectIntelligence` that keeps saved and live project observations.
- A Python stdio MCP server under `Services/uepi` for Codex.
- A rebuildable SQLite query cache derived from Snapshot fragments.

## What It Does Not Do

UEPI is read-only. It does not provide tools to save, delete, rename, move, create, compile, run PIE, execute arbitrary code, or mutate Unreal assets.

The default product path does not use a daemon, HTTP API, worker queue, Web UI, or remote service registration.

## Requirements

- Unreal Engine 5.3.2.
- Python 3.11+ for the stdio MCP server.
- Codex as the primary supported MCP client for the current read-only line.
- A project-local install at `__PROJECT_ROOT__/Plugins/UEProjectIntelligence`.

Optional Unreal plugins such as EnhancedInput, GameplayAbilities, Niagara, PCG, CommonUI, StateTree, IKRig, ControlRig, and MetaSound are compile-gated and are not required for ordinary projects.

## Install As Project Plugin

Copy or extract the plugin to:

```text
__PROJECT_ROOT__/Plugins/UEProjectIntelligence
```

Regenerate project files if needed, then build the editor target:

```powershell
& "__UE_ROOT__\Engine\Build\BatchFiles\Build.bat" `
  __PROJECT_NAME__Editor Win64 Development `
  "-Project=__PROJECT_ROOT__\__PROJECT_NAME__.uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

Open the editor and enable `UEProjectIntelligence` if Unreal asks.

## Generate A Snapshot

In the editor, open `Tools > UE Project Intelligence` and click `Run Snapshot Scan`.

For a targeted commandlet scan with the editor closed:

```powershell
& "__UE_ROOT__\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "__PROJECT_ROOT__\__PROJECT_NAME__.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/Path/To/BP_Player.BP_Player" `
  -UEPIOutput="__PROJECT_ROOT__\Saved\UEProjectIntelligence\l2_scan.json"
```

Snapshot data is written under:

```text
__PROJECT_ROOT__/Saved/UEProjectIntelligence/store
```

## Connect Codex

Copy `Resources/codex-config.template.toml` into the appropriate Codex configuration file and replace the placeholders:

```text
__PYTHON_EXE__
__PROJECT_ROOT__
__PROJECT_NAME__
```

The MCP command should point to:

```text
__PROJECT_ROOT__/Plugins/UEProjectIntelligence/Services/uepi/src/uepi/mcp_server.py
```

## Ask Questions

Recommended Codex flow:

1. Call `uepi_status`.
2. Use `uepi_overview`, `uepi_search`, or `uepi_context` to identify evidence.
3. Use the narrow domain tool for the question.
4. If diagnostics include `UEPI_REFRESH_REQUESTED`, wait for the editor plugin to process the targeted request and retry.
5. If diagnostics include `UEPI_SNAPSHOT_STALE`, open the editor/plugin or run a commandlet scan for realtime freshness.

## MCP Tools

- `uepi_status`
- `uepi_overview`
- `uepi_search`
- `uepi_context`
- `uepi_asset`
- `uepi_blueprint`
- `uepi_blueprint_trace`
- `uepi_animation`
- `uepi_impact`
- `uepi_diff`

Codex profile exposes only these ten read-only tools.

`uepi_context` can route questions through:

- `project_overview`
- `input_to_gameplay`
- `blueprint_behavior`
- `animation_playback`
- `ui_flow`
- `asset_dependency_impact`
- `data_driven_behavior`
- `gas_ability_flow`
- `ai_behavior_flow`
- `network_replication_flow`

The experimental `codex_write_alpha` profile exposes five additional edit tools for discovery, dry-run planning, apply, validate, and rollback. Apply remains disabled by default through UEPI project settings; when the live bridge and write flags are explicitly enabled, the alpha executor allows scoped Blueprint variables, components, custom events, function graphs, common graph nodes, pin links, Actor spawn/transform/property edits, Material Instance create/parameter/apply edits, scoped `/Game` Content operations, basic UMG Widget Blueprint edits, and Enhanced Input asset/key-mapping edits when that plugin is enabled. It never saves packages by default.

## Snapshot Modes

- `saved`: editor can be closed; tools read the latest saved Snapshot.
- `live`: editor is active; tools merge live overlay observations over the saved baseline.
- `refresh_requested`: a target changed after the latest Snapshot and UEPI queued a targeted editor refresh request.
- `stale`: a target changed after the latest Snapshot, but the editor is not active to service a refresh.

SQLite cache files are derived data and can be deleted. UEPI can rebuild them from Snapshot fragments.

## Limitations

- Current primary target is UE5.3.2.
- Current version is read-only and Codex-first.
- Animation data is static summary and sampled context, not a full per-frame dump.
- Animation Blueprint final runtime pose is not available.
- Editor or commandlet collection is required to refresh Snapshots.
- Editor-closed mode uses the latest saved Snapshot only.
- Optional live editor bridge and write tools are disabled by default.

## Validation Status

- Real-machine Codex MCP read loop validated on UE5.3.2 project `GasDemo`.
- Snapshot/live/cache/tombstone smoke tests pass through `Tools/test_snapshot_mcp_v2.py`.
- The stable `codex` profile remains exactly ten read-only tools.

## Troubleshooting

See `Docs/troubleshooting.md`.

## License

UE Project Intelligence is licensed under the MIT License. See `LICENSE`.
