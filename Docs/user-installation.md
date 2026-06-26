# Installation

## Requirements

- Unreal Engine 5.3.2 editor or commandlet.
- Python 3.10+ for the stdio MCP server.

No daemon, HTTP server, Web UI, worker, or database service is required.

## Unreal Plugin

Place `UEProjectIntelligence` under the project's `Plugins` directory and enable it in the editor. Build the project:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Build\BatchFiles\Build.bat" GasDemoEditor Win64 Development `
  "-Project=F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" -WaitMutex -NoHotReload
```

## Create A Snapshot

Use the editor dashboard button `Run Snapshot Scan`, or run the `UEPIIndex` commandlet. The source of truth is written under:

```text
Saved/UEProjectIntelligence/store/
```

## Optional Domain Readers

UEPI does not force-enable optional Unreal plugins. Readers such as EnhancedInput, CommonUI, GameplayAbilities, StateTree, IKRig, ControlRig, Niagara, PCG, and MetaSound compile only when the project explicitly enables the matching plugin, or when `UEPI_OPTIONAL_READERS` is set to `all` or a comma-separated plugin list before building.

When an optional reader is disabled, UEPI still reports basic Asset Registry and reflection metadata for those assets.

## Codex MCP

Configure Codex to launch:

```text
python -B Plugins/UEProjectIntelligence/Services/uepi/src/uepi/mcp_server.py --project <YourProject.uproject> --tool-profile codex
```

Run `uepi_status` first after connecting.
