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

Generate the project-local configuration. The first command previews the exact diff; the second writes only the marked UEPI block:

```powershell
python "__PROJECT_ROOT__/Plugins/UEProjectIntelligence/Tools/setup_codex.py" --project "__PROJECT_ROOT__/__PROJECT_NAME__.uproject"
python "__PROJECT_ROOT__/Plugins/UEProjectIntelligence/Tools/setup_codex.py" --project "__PROJECT_ROOT__/__PROJECT_NAME__.uproject" --apply
```

Or use `Resources/codex-config.template.toml` and replace:

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

After replacing or rebuilding UEPI, restart Codex once. The stdio MCP process loads its Python modules and tool contract when Codex starts; reopening only the Unreal Editor reloads the C++ Bridge but does not replace an already running Codex MCP process.

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
Use UEPI first. Call uepi_status, then uepi_overview. When answering project-specific Unreal questions, use uepi_context or uepi_search before guessing asset paths. Treat read results as snapshot evidence unless UEPI says data_mode is live. Use edit tools only for explicit project-change requests. For Blueprint edits, choose a compact maintainable design before choosing operations; prefer variables, timers, loops, custom events, or helper functions over repeated expanded nodes. Always preview before apply.
```

## Recommended Agent Flow

1. Call `uepi_status`.
2. Use `uepi_search` or `uepi_context` to identify candidate assets.
3. Call the narrow read tool needed by the question.
4. If the user asks for a project change, call `uepi_edit_discover`, compare compact and expanded Blueprint designs when relevant, then create one complete `uepi_edit_preview` plan for the intended edit.
5. Ask for explicit approval once. After the user approves the unchanged Preview, the Agent calls `uepi_edit_apply` itself and completes validation, touched-only save, refresh/read, `uepi_diff`, and approved Runtime verification without asking the user to invoke Apply or reconfirm phases.
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
- `uepi_animation`: read animation, skeleton, track, notify, curve, motion-summary, bone-motion profile artifacts, reconstruction profiles, driver track curves, and optional full-pose samples captured in Snapshot.
- `uepi_impact`: inspect incoming/outgoing dependency impact.
- `uepi_diff`: compare saved Snapshot generations.
- `uepi_editor`: live status, selection, incremental output log, and viewport capture.
- `uepi_world`: live Editor or UEPI-owned PIE actors/components.
- `uepi_refresh`: request, inspect, or wait for targeted refresh work.
- `uepi_schema`: authoritative property, operation, Blueprint node, and runtime schemas.
- `uepi_runtime`: transaction-bound controlled PIE verification.
- `uepi_edit_discover`: discover supported guarded edit operations.
- `uepi_edit_preview`: create a dry-run operation plan without modifying assets.
- `uepi_edit_apply`: apply an approved plan through the live editor bridge when write gates allow it.
- `uepi_edit_validate`: validate a transaction.
- `uepi_edit_rollback`: undo the last applied UEPI transaction in the editor session.

Every tool response includes eight-stage `timing` diagnostics. A high `editor_dispatch_ms` means the request waited for the next Editor Bridge tick; a high `editor_execute_ms` means the Unreal operation itself was expensive. Calls over 5000 ms include `UEPI_SLOW_OPERATION` unless `UEPI_SLOW_OPERATION_MS` configures another threshold.

Successful approved plans save only touched packages by default. UEPI never exposes save-all.

Fresh installs allow up to 96 operations and 12 affected assets in one atomic transaction. Project Settings may raise the hard caps to 256 operations and 64 assets. Preview rejects over-budget plans before approval; plans above the normal 64-operation or 12-asset risk thresholds are marked as large atomic transactions and use extended execution timeouts.

## Verify

```powershell
$env:PYTHONPATH="__PROJECT_ROOT__\Plugins\UEProjectIntelligence\Services\uepi\src"
python -m uepi status --project "__PROJECT_ROOT__\__PROJECT_NAME__.uproject"
python "__PROJECT_ROOT__\Plugins\UEProjectIntelligence\Tools\test_snapshot_mcp_v2.py"
python "__PROJECT_ROOT__\Plugins\UEProjectIntelligence\Tools\uepi_doctor.py" --project "__PROJECT_ROOT__\__PROJECT_NAME__.uproject"
python "__PROJECT_ROOT__\Plugins\UEProjectIntelligence\Tools\test_live_read_contract.py" --project "__PROJECT_ROOT__\__PROJECT_NAME__.uproject"
```

The final command requires the matching Editor to be open. It performs read-only Status, World, Schema, and viewport checks, writes only a screenshot under `Saved/UEProjectIntelligence/artifacts`, and never calls Preview or Apply.
