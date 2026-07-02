# Codex Setup

UEPI is project-local and uses a Python stdio MCP server. The Codex profile is a unified Agent profile: it exposes read tools and guarded edit tools together so Codex can choose the right workflow without profile switching.

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

Edit apply still requires the Unreal Editor, the live editor bridge, a preview plan, and explicit user approval. Without those gates, edit tools return structured rejection diagnostics and do not mutate assets.

## Project Selection

When an Unreal Editor with UEPI is open, the plugin writes a local active-session record under the user's app data directory. The MCP server uses that record as the online project context when it matches the configured project, or when it is the only active UEPI editor session.

When no online editor session is available, the MCP server falls back to the explicit `--project` path in the Codex config and serves the latest saved Snapshot for that project. This keeps offline reads intentional: change the configured project path only when the user has clearly selected a different offline project.

## Recommended Prompt Rule

```text
Use UEPI first. Call uepi_status before other UEPI tools. Use uepi_context to build bounded evidence before answering Unreal project questions. Treat Blueprint pin links, GUIDs, refs, and evidence as source of truth. For edits, build one complete preview plan, ask for explicit user approval once, then apply, validate, refresh/diff, and report the result without additional approval prompts unless the plan changes.
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
