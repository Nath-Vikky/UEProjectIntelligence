# UE Project Intelligence

UE Project Intelligence is a read-only, project-aware MCP server for helping AI agents understand, navigate, and explain Unreal Engine projects.

This repository is currently on the `2.0-dev` refactor line described in `DOCX/Improvement.md`. The previous daemon/worker/web based stable point is preserved as the `v1.0.0` tag.

## Product Shape

UEPI is being converged to three components:

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

- `v1.0.0`: stable read-only MCP loop using the legacy SQLite/daemon-compatible implementation.
- `main`: active `2.0-dev` convergence work toward direct snapshot + stdio MCP.

Use `v1.0.0` if you need the previously tested live worker flow. Use `main` if you are developing the new snapshot-first architecture.

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

`UEPIIndex` remains as a headless, one-shot collection entry point. It is not a worker and should not run as a queue consumer in the target architecture.

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_character_scan.json"
```

During the refactor this still writes legacy scan JSON. The target state is direct Snapshot Store submission.

## Target MCP Surface

The public MCP surface is being reduced to task-oriented tools:

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
- v1 user guide: `Docs/user-guide.md`
- Development status: `Docs/DevelopmentStatus.md`
- Architecture notes: `Docs/developer-architecture.md`
- Troubleshooting: `Docs/troubleshooting.md`

Docs are being updated in phases. Any document that still describes daemon/worker/web as the default path should be treated as legacy v1 material until rewritten.
