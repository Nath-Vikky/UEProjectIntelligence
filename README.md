# UE Project Intelligence

UE Project Intelligence is a self-contained, Codex-first MCP server and Unreal Editor plugin for helping AI agents understand, navigate, explain, and safely edit Unreal Engine 5.3.2 projects from UEPI snapshots.

UEPI does not depend on Epic's UE5.8 ModelContextProtocol plugin. The official UE5.8 MCP implementation is only used as a design reference.

## What It Is

- An Unreal Editor plugin that reads project assets, Blueprints, animations, worlds, data, UI, AI, audio, render, and material metadata.
- A Snapshot Store under `Saved/UEProjectIntelligence` that keeps saved and live project observations.
- A Python stdio MCP server under `Services/uepi` for Codex.
- A unified Agent profile that exposes read tools and guarded edit tools together so the Agent can choose the right workflow.
- A rebuildable SQLite query cache derived from Snapshot fragments.

## What It Does Not Do

UEPI does not provide arbitrary Python/shell/console execution, plugin enablement, source-control submission, save-all, or broad destructive project edits. It provides only transaction-bound, UEPI-owned PIE verification.

Write operations are exposed through guarded edit tools and remain gated by an immutable preview plan, exact-project live bridge availability, authorization policy, repeat preflight, validation, touched-only saving, backup/rollback, targeted refresh, transaction diff, and a post-action report. `ReviewEachPlan` requires one explicit approval; `TrustedSession` and `TrustedProject` can authorize an unchanged in-policy plan automatically.

The default product path does not use a daemon, HTTP API, worker queue, Web UI, or remote service registration.

## Requirements

- Unreal Engine 5.3.2.
- Python 3.11+ for the stdio MCP server.
- Codex as the primary supported MCP client.
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

Preview and install the project-local Codex block:

```powershell
python "__PROJECT_ROOT__/Plugins/UEProjectIntelligence/Tools/setup_codex.py" `
  --project "__PROJECT_ROOT__/__PROJECT_NAME__.uproject"
python "__PROJECT_ROOT__/Plugins/UEProjectIntelligence/Tools/setup_codex.py" `
  --project "__PROJECT_ROOT__/__PROJECT_NAME__.uproject" --apply
```

Alternatively copy `Resources/codex-config.template.toml` and replace:

```text
__PYTHON_EXE__
__PROJECT_ROOT__
__PROJECT_NAME__
```

The MCP command should point to:

```text
__PROJECT_ROOT__/Plugins/UEProjectIntelligence/Services/uepi/src/uepi/mcp_server.py
```

When the editor is open, UEPI publishes an active local bridge session and the MCP server uses that online project context for live reads and guarded edits. When no editor session is active, `--project` is the explicit offline Snapshot project selector.

## Ask Questions Or Request Safe Edits

Recommended Codex flow:

1. Call `uepi_status`.
2. Use `uepi_overview`, `uepi_search`, or `uepi_context` to identify evidence.
3. Use the narrow domain tool for the question.
4. If the user asks to modify the project, use `uepi_edit_discover`, then create one complete `uepi_edit_preview` plan for the intended edit.
5. Let UEPI evaluate the project authorization policy. Ask once only in `ReviewEachPlan`; continue directly in `TrustedSession` or `TrustedProject` when the immutable plan is in policy.
6. If diagnostics include `UEPI_REFRESH_REQUESTED`, wait for the editor plugin to process the targeted request and retry.
7. If diagnostics include `UEPI_SNAPSHOT_STALE`, open the editor/plugin or run a commandlet scan for realtime freshness.

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
- `uepi_editor`
- `uepi_world`
- `uepi_refresh`
- `uepi_schema`
- `uepi_runtime_preview`
- `uepi_runtime_approve`
- `uepi_runtime`
- `uepi_recovery_inspect`
- `uepi_recovery_finalize`
- `uepi_recovery_rollback`
- `uepi_edit_discover`
- `uepi_edit_preview`
- `uepi_edit_apply`
- `uepi_edit_validate`
- `uepi_edit_rollback`

The single `codex` profile exposes these read and edit tools together. `codex_write_alpha` is still accepted as a legacy alias, but new installs should not require switching profiles.

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

The edit tools support discovery, dry-run planning, apply, validate, and rollback. The current catalog includes generic reflected DataAsset/property writes, Blueprint graph maintenance, AnimGraph Slot/pose links, Actor, Material Instance, scoped `/Game` Content, basic UMG, and Enhanced Input operations. Successful plans save touched packages by default; save-all remains unavailable.

## Snapshot Modes

- `saved`: editor can be closed; tools read the latest saved Snapshot.
- `live`: editor is active; tools merge live overlay observations over the saved baseline.
- `refresh_requested`: a target changed after the latest Snapshot and UEPI queued a targeted editor refresh request.
- `stale`: a target changed after the latest Snapshot, but the editor is not active to service a refresh.

SQLite cache files are derived data and can be deleted. UEPI can rebuild them from Snapshot fragments.

## Limitations

- Release-qualified engine target is UE5.3.2.
- Current version is a Codex-first UE5.3.2 Beta. Guarded writes remain policy-bound and intentionally narrower than the read surface.
- Animation reads provide static summaries, driver curves, reconstruction profiles, and optional full-pose sample artifacts; they do not evaluate the final runtime pose after every AnimGraph, Control Rig, IK, physics, or retargeting layer.
- Editor or commandlet collection is required to refresh Snapshots.
- Editor-closed mode uses the latest saved Snapshot only.
- The live editor bridge and guarded edit apply are enabled by default. Preview, policy evaluation, validation, touched-only save, recovery, diff, and post-action reporting remain mandatory in every authorization mode.

See [`Docs/Known-Limitations.md`](Docs/Known-Limitations.md) for the complete Beta boundary.

## Validation Status

- Real-machine Codex MCP read loop validated on UE5.3.2 project `GasDemo`.
- Snapshot/live/cache/tombstone smoke tests pass through `Tools/test_snapshot_mcp_v2.py`.
- The `codex` profile now exposes read and guarded edit tools together for one-step Agent setup.
- The UE5.3.2 Beta gate passes LLMNPCDemo, Third Person, blank-project, two-project routing, save/restart, rollback, and owned-PIE real-machine scenarios.

## Troubleshooting

Run `python Tools/uepi_doctor.py --project "__PROJECT_ROOT__/__PROJECT_NAME__.uproject"` and see `Docs/troubleshooting.md`.

## License

UE Project Intelligence is licensed under the MIT License. See `LICENSE`.
