# UEPI 2.0-dev User Guide (Codex Example)

UE Project Intelligence is a Codex-first MCP bridge for Unreal Engine projects. The default flow is:

```text
Generate or update a UEPI Snapshot
  -> Saved/UEProjectIntelligence/store
  -> Codex starts the UEPI stdio MCP server
  -> The agent chooses only the tools needed for the user's question
  -> Tool results are cached for offline analysis
  -> If the user asks for edits, the agent previews, asks approval, applies through the live editor bridge, validates, and reports diff evidence
```

No daemon, HTTP server, Web UI, worker queue, or global proxy exception is required.

## When The Editor Is Needed

Existing saved Snapshot queries do not require Unreal Editor to be open.

Open the editor and enable the UEPI panel only when you want realtime targeted refreshes. When assets are saved, renamed, compiled, or removed, UEPI records incremental events. If Codex later reads an asset whose event is newer than the Snapshot, the MCP tool queues a targeted refresh request under:

```text
__PROJECT_ROOT__/Saved/UEProjectIntelligence/store/requests
```

The editor plugin polls that directory and scans only the requested asset. The agent can then retry the same read.

## Generate A Baseline Snapshot

In Unreal Editor, open `Tools > UE Project Intelligence` and click `Run Snapshot Scan`.

For a targeted commandlet scan with the editor closed:

```powershell
& "__UE_ROOT__\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "__PROJECT_ROOT__\__PROJECT_NAME__.uproject" `
  -run=UEPIIndex `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/Path/To/BP_Player.BP_Player" `
  -unattended -nop4 -nosplash -NullRHI
```

After collection, these paths should exist:

```text
__PROJECT_ROOT__/Saved/UEProjectIntelligence/store/manifests/saved.json
__PROJECT_ROOT__/Saved/UEProjectIntelligence/store/objects/**
```

## Codex MCP Setup

Use `Resources/codex-config.template.toml` and replace:

```text
__PYTHON_EXE__
__PROJECT_ROOT__
__PROJECT_NAME__
```

Command:

```text
__PYTHON_EXE__
```

Arguments, one item per row:

```text
-B
__PROJECT_ROOT__/Plugins/UEProjectIntelligence/Services/uepi/src/uepi/mcp_server.py
--project
__PROJECT_ROOT__/__PROJECT_NAME__.uproject
--tool-profile
codex
```

This single profile exposes read tools and guarded edit tools together. You do not need a separate write profile. Edit apply is available when the editor live bridge is online, and it still requires a preview plan plus explicit user approval.

Working directory:

```text
__PROJECT_ROOT__
```

## Project Selection

Online editor context wins. If a UE project with UEPI is open, the editor publishes an active bridge session and Codex uses that project for live reads and guarded edits.

Offline Snapshot reads use the explicit `--project` path in the MCP config. Treat that path as the selected offline project label; change it only when the user clearly asks to inspect another installed UEPI project while its editor is closed.

## Recommended Codex Prompt

After connecting UEPI MCP, start with:

```text
Use UEPI first. Call uepi_status, then uepi_overview. When answering project-specific Unreal questions, use uepi_context or uepi_search before guessing asset paths. Treat read results as snapshot evidence unless UEPI says data_mode is live. Use edit tools only for explicit project-change requests, and always preview before apply.
```

## Recommended Agent Flow

1. Call `uepi_status`.
2. Use `uepi_search` or `uepi_context` to identify candidate assets.
3. Call the narrow read tool needed by the question.
4. If the user asks for a project change, call `uepi_edit_discover`, then `uepi_edit_preview`, and wait for explicit approval before `uepi_edit_apply`.
5. After apply, call `uepi_edit_validate`, refresh/read the changed asset, and use `uepi_diff` where applicable.
6. If diagnostics include `UEPI_REFRESH_REQUESTED`, wait briefly and retry the same tool.
7. If diagnostics include `UEPI_SNAPSHOT_STALE`, open the editor/plugin for realtime refresh or run a commandlet scan.

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
- `uepi_edit_discover`: discover supported guarded edit operations.
- `uepi_edit_preview`: create a dry-run operation plan without modifying assets.
- `uepi_edit_apply`: apply an approved plan through the live editor bridge when write gates allow it.
- `uepi_edit_validate`: validate a transaction.
- `uepi_edit_rollback`: undo the last applied UEPI transaction in the editor session.

## Verify

```powershell
$env:PYTHONPATH="__PROJECT_ROOT__\Plugins\UEProjectIntelligence\Services\uepi\src"
python -m uepi status --project "__PROJECT_ROOT__\__PROJECT_NAME__.uproject"
python "__PROJECT_ROOT__\Plugins\UEProjectIntelligence\Tools\test_snapshot_mcp_v2.py"
```
