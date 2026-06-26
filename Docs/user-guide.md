# UEPI 2.0-dev User Guide (Codex Example)

UE Project Intelligence is a read-only MCP bridge for Unreal Engine projects. The default flow is:

```text
Generate or update a UEPI Snapshot
  -> Saved/UEProjectIntelligence/store
  -> Codex starts the UEPI stdio MCP server
  -> The agent chooses only the tools needed for the user's question
  -> Tool results are cached for offline analysis
```

No daemon, HTTP server, Web UI, worker queue, or global proxy exception is required.

## When The Editor Is Needed

Existing saved Snapshot queries do not require Unreal Editor to be open.

Open the editor and enable the UEPI panel only when you want realtime targeted refreshes. When assets are saved, renamed, or removed, UEPI records incremental events. If Codex later reads an asset whose event is newer than the Snapshot, the MCP tool queues a targeted refresh request under:

```text
Saved/UEProjectIntelligence/store/requests
```

The editor plugin polls that directory and scans only the requested asset. The agent can then retry the same read.

## Generate A Baseline Snapshot

In Unreal Editor, open `Tools > UE Project Intelligence` and click `Run Snapshot Scan`.

For a targeted commandlet scan with the editor closed:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter" `
  -unattended -nop4 -nosplash -NullRHI
```

After collection, these paths should exist:

```text
Saved/UEProjectIntelligence/store/manifests/saved.json
Saved/UEProjectIntelligence/store/objects/**
```

## Codex MCP Setup

Command:

```text
C:/Users/renne/AppData/Local/Programs/Python/Python313/python.exe
```

Arguments, one item per row:

```text
-B
F:/Epic Games/UE5project/GasDemo/Plugins/UEProjectIntelligence/Services/uepi/src/uepi/mcp_server.py
--project
F:/Epic Games/UE5project/GasDemo/GasDemo.uproject
--tool-profile
codex
```

Working directory:

```text
F:/Epic Games/UE5project/GasDemo/Plugins/UEProjectIntelligence
```

A TOML-style example is provided at `Resources/codex-config.template.toml`.

## Recommended Agent Flow

1. Call `uepi_status`.
2. Use `uepi_search` or `uepi_context` to identify candidate assets.
3. Call the narrow read tool needed by the question.
4. If diagnostics include `UEPI_REFRESH_REQUESTED`, wait briefly and retry the same tool.
5. If diagnostics include `UEPI_SNAPSHOT_STALE`, open the editor/plugin for realtime refresh or run a commandlet scan.

## Tools

- `uepi_status`: project state, cache state, freshness, editor session, refresh requests.
- `uepi_overview`: compact counts and top-level project summary.
- `uepi_search`: find entities/assets by name, path, kind, or attributes.
- `uepi_context`: build a bounded context bundle for a natural-language question.
- `uepi_asset`: read one asset/entity and nearby relations.
- `uepi_blueprint`: read Blueprint graph, node, pin, event, CFG, and DFG entities captured in Snapshot.
- `uepi_blueprint_trace`: trace static Blueprint flow relations.
- `uepi_animation`: read animation, skeleton, track, notify, curve, and motion-summary data captured in Snapshot.
- `uepi_impact`: inspect incoming/outgoing dependency impact.
- `uepi_diff`: compare saved Snapshot generations.

## Verify

```powershell
$env:PYTHONPATH="F:\Epic Games\UE5project\GasDemo\Plugins\UEProjectIntelligence\Services\uepi\src"
python -m uepi status --project "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject"
python Plugins\UEProjectIntelligence\Tools\test_snapshot_mcp_v2.py
```
