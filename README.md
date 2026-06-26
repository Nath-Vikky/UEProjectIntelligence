# UE Project Intelligence

UE Project Intelligence is a read-only, project-aware MCP server for helping AI agents understand, navigate, and explain Unreal Engine projects.

This repository is currently on the `2.0-dev` Snapshot-first line described in `DOCX/Improvement.md`. The previous daemon/worker/web based stable point is preserved only as the historical `v1.0.0` tag.

## Product Shape

UEPI is organized around three components:

- **UEPI Editor Plugin**: authoritative Unreal-side read-only collection, change observation, and snapshot writing.
- **UEPI Snapshot Store**: saved snapshots, live overlay state, history manifests, and large read-only artifacts under `Saved/UEProjectIntelligence`.
- **UEPI MCP Process**: stdio MCP entry point, snapshot sync, rebuildable SQLite query cache, and AI context construction.

The default user path should be:

```text
Install and enable the UEPI plugin
  -> open the Unreal project
  -> the plugin collects read-only project snapshots
  -> Codex / Claude Code / Cursor starts UEPI MCP over stdio
  -> the AI queries live state or the latest saved snapshot
```

The default product no longer treats a local daemon, HTTP API, worker registration, job queue, or Web UI as part of the main workflow.

## Current Stability

- `main`: active `2.0-dev` Snapshot-first read-only MCP line.
- `v1.0.0`: historical tag for the previous daemon-compatible implementation.

## Read-Only Contract

UEPI does not provide tools to:

- save, delete, rename, move, or create Unreal assets;
- compile Blueprints;
- edit Blueprint pins, graphs, actors, levels, animations, or config;
- execute shell commands or arbitrary Python/C++;
- launch PIE or evaluate runtime gameplay state.

UEPI writes only its own observations, snapshots, indexes, logs, and artifacts under:

```text
Saved/UEProjectIntelligence
```

## Build

Close Unreal Editor before compiling C++ changes.

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Build\BatchFiles\Build.bat" `
  GasDemoEditor Win64 Development `
  "-Project=F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -WaitMutex -NoHotReload
```

If Unreal reports Live Coding is active, close the editor or press `Ctrl+Alt+F11` in the editor before rebuilding.

## Commandlet Collection

`UEPIIndex` remains as a headless, one-shot Snapshot collection entry point. It is not a worker and does not run as a queue consumer.

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_character_scan.json"
```

The commandlet writes the requested scan JSON and submits the same observations into the Snapshot Store manifest.

## MCP Surface

The public MCP surface is task-oriented:

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

Internal maintenance, ingest, worker, queue, HTTP, and recovery tools are not part of the target public MCP interface.

## Documentation

- Current v2 plan: `DOCX/Improvement.md`
- User guide: `Docs/user-guide.md`
- Development status: `Docs/DevelopmentStatus.md`
- Architecture notes: `Docs/developer-architecture.md`
- Troubleshooting: `Docs/troubleshooting.md`
