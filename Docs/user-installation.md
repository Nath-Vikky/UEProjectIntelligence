# Installation

## Requirements

- Unreal Engine 5.3.2 editor or commandlet.
- Python 3.11+ for the stdio MCP server.

No daemon, HTTP server, Web UI, worker, or database service is required.

## Unreal Plugin

Place `UEProjectIntelligence` under the project's `Plugins` directory:

```text
__PROJECT_ROOT__/Plugins/UEProjectIntelligence
```

Build the project:

```powershell
& "__UE_ROOT__\Engine\Build\BatchFiles\Build.bat" `
  __PROJECT_NAME__Editor Win64 Development `
  "-Project=__PROJECT_ROOT__\__PROJECT_NAME__.uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

## Create A Snapshot

Use the editor dashboard button `Run Snapshot Scan`, or run the `UEPIIndex` commandlet. The source of truth is written under:

```text
__PROJECT_ROOT__/Saved/UEProjectIntelligence/store/
```

## Optional Domain Readers

UEPI does not force-enable optional Unreal plugins. Readers such as EnhancedInput, CommonUI, GameplayAbilities, StateTree, IKRig, ControlRig, Niagara, PCG, and MetaSound compile only when the project explicitly enables the matching plugin, or when `UEPI_OPTIONAL_READERS` is set to `all` or a comma-separated plugin list before building.

When an optional reader is disabled, UEPI still reports basic Asset Registry and reflection metadata for those assets.

## Codex MCP

Configure Codex to launch:

```text
__PYTHON_EXE__ -B __PROJECT_ROOT__/Plugins/UEProjectIntelligence/Services/uepi/src/uepi/mcp_server.py --project __PROJECT_ROOT__/__PROJECT_NAME__.uproject --tool-profile codex
```

Run `uepi_status` first after connecting.
