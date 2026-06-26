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
- A project-local install at `<PROJECT_ROOT>/Plugins/UEProjectIntelligence`.

Optional Unreal plugins such as EnhancedInput, GameplayAbilities, Niagara, PCG, CommonUI, StateTree, IKRig, ControlRig, and MetaSound are compile-gated and are not required for ordinary projects.

## Install As Project Plugin

Copy or extract the plugin to:

```text
<PROJECT_ROOT>/Plugins/UEProjectIntelligence
```

Regenerate project files if needed, then build the editor target:

```powershell
& "<UE_ROOT>\Engine\Build\BatchFiles\Build.bat" `
  <PROJECT_NAME>Editor Win64 Development `
  "-Project=<PROJECT_ROOT>\<PROJECT_NAME>.uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

Open the editor and enable `UEProjectIntelligence` if Unreal asks.

## Generate A Snapshot

In the editor, open `Tools > UE Project Intelligence` and click `Run Snapshot Scan`.

For a targeted commandlet scan with the editor closed:

```powershell
& "<UE_ROOT>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "<PROJECT_ROOT>\<PROJECT_NAME>.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/Path/To/BP_Player.BP_Player" `
  -UEPIOutput="<PROJECT_ROOT>\Saved\UEProjectIntelligence\l2_scan.json"
```

Snapshot data is written under:

```text
<PROJECT_ROOT>/Saved/UEProjectIntelligence/store
```

## Connect Codex

Copy `Resources/codex-config.template.toml` into the appropriate Codex configuration file and replace the placeholders:

```text
<PYTHON_EXE>
<PROJECT_ROOT>
<PROJECT_NAME>
```

The MCP command should point to:

```text
<PROJECT_ROOT>/Plugins/UEProjectIntelligence/Services/uepi/src/uepi/mcp_server.py
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

## Troubleshooting

See `Docs/troubleshooting.md`.

## License

UE Project Intelligence is licensed under the MIT License. See `LICENSE`.
