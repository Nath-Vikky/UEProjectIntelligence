# Installation

## Requirements

- Unreal Engine 5.3.2 editor or commandlet.
- Python 3.11+ for the stdio MCP server.

No daemon, HTTP server, Web UI, worker, or database service is required.

## Unreal Plugin

Place `UEProjectIntelligence` under the project's `Plugins` directory:

```text
<PROJECT_ROOT>/Plugins/UEProjectIntelligence
```

Build the project:

```powershell
& "<UE_ROOT>\Engine\Build\BatchFiles\Build.bat" `
  <PROJECT_NAME>Editor Win64 Development `
  "-Project=<PROJECT_ROOT>\<PROJECT_NAME>.uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

## Create A Snapshot

Use the editor dashboard button `Run Snapshot Scan`, or run the `UEPIIndex` commandlet. The source of truth is written under:

```text
<PROJECT_ROOT>/Saved/UEProjectIntelligence/store/
```

## Optional Domain Readers

UEPI does not force-enable optional Unreal plugins. Readers such as EnhancedInput, CommonUI, GameplayAbilities, StateTree, IKRig, ControlRig, Niagara, PCG, and MetaSound compile only when the project explicitly enables the matching plugin, or when `UEPI_OPTIONAL_READERS` is set to `all` or a comma-separated plugin list before building.

When an optional reader is disabled, UEPI still reports basic Asset Registry and reflection metadata for those assets.

## Codex MCP

Configure Codex to launch:

```text
<PYTHON_EXE> -B <PROJECT_ROOT>/Plugins/UEProjectIntelligence/Services/uepi/src/uepi/mcp_server.py --project <PROJECT_ROOT>/<PROJECT_NAME>.uproject --tool-profile codex
```

Run `uepi_status` first after connecting.
