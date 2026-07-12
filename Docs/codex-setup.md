# Codex Setup

UEPI is project-local and uses a Python stdio MCP server. The Codex profile is a unified Agent profile: it exposes read tools and guarded edit tools together so Codex can choose the right workflow without profile switching.

## Setup Script

Preview, then apply, the project-local managed block:

```powershell
python "__PROJECT_ROOT__/Plugins/UEProjectIntelligence/Tools/setup_codex.py" --project "__PROJECT_ROOT__/__PROJECT_NAME__.uproject"
python "__PROJECT_ROOT__/Plugins/UEProjectIntelligence/Tools/setup_codex.py" --project "__PROJECT_ROOT__/__PROJECT_NAME__.uproject" --apply
```

The script preserves every other Codex setting and MCP block. It changes only the text between `BEGIN UEPI MANAGED MCP` and `END UEPI MANAGED MCP` after printing a diff.

## Template

Copy `Resources/codex-config.template.toml` into the trusted project's `.codex/config.toml` and replace:

```text
__PYTHON_EXE__
__PROJECT_ROOT__
__PROJECT_NAME__
```

The default block uses:

```text
--tool-profile codex
```

The `codex` profile does not require the Unreal Editor to be open when `Saved/UEProjectIntelligence/store/manifests/saved.json` exists.

Edit apply still requires the exact-project Editor Bridge, Plan v2, and explicit user approval. It repeats preflight, backs up, validates, saves touched packages, refreshes, and records diff evidence.

## Project Selection

When an Unreal Editor with UEPI is open, the plugin writes a local active-session record under the user's app data directory. The MCP server uses that record as the online project context when it matches the configured project, or when it is the only active UEPI editor session.

When no online editor session is available, the MCP server falls back to the explicit `--project` path in the Codex config and serves the latest saved Snapshot for that project. This keeps offline reads intentional: change the configured project path only when the user has clearly selected a different offline project.

## Recommended Prompt Rule

```text
Use UEPI first. Call uepi_status before other UEPI tools. Use uepi_context to build bounded evidence before answering Unreal project questions. Treat Blueprint pin links, GUIDs, refs, and evidence as source of truth. For edits, choose a compact, idiomatic Blueprint design before choosing operations; prefer variables, loops, timers, custom events, and helper functions over expanded repeated nodes. Build one complete preview plan and ask for explicit user approval once. After approval of the unchanged plan, call uepi_edit_apply yourself and continue validate, touched-only save, refresh/diff, and approved runtime verification without asking the user to invoke Apply or reconfirm phases.
```

## Quick Check

Ask Codex to run:

```text
Call uepi_status, then tell me the project name, snapshot data mode, generation, and whether bridge.ready is true.
```

Expected result:

```text
tool = uepi_status
ok = true
llm_readiness.can_query_snapshot = true
llm_readiness.requires_daemon = false
```

## Edit Tools

```text
uepi_edit_discover
uepi_edit_preview
uepi_edit_apply
uepi_edit_validate
uepi_edit_rollback
```

These tools are part of the default `codex` profile. `codex_write_alpha` remains accepted as a legacy alias, but new installs should use only the single `codex` MCP server.

`uepi_edit_apply` is Agent-ready by default when the editor bridge is online, but the safe workflow is still preview -> one user approval -> apply -> validate -> refresh/diff. For complex Blueprint graph edits, use operation `ref` / endpoint `node_ref` aliases so one preview can create nodes, connect pins, and compile in a single approved transaction. Use the project settings only when you want to opt out of a write domain or disable package saving/bridge behavior.

The default atomic budget is 96 operations and 12 affected assets. Project Settings can raise the hard limits to 256 operations and 64 assets. UEPI checks the active Editor limits during Preview, before approval, and scales Apply/Refresh timeouts for larger accepted plans.

Run `python Tools/uepi_doctor.py --project <path-to-uproject> --require-editor` before live write/runtime acceptance.
