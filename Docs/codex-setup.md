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

Edit apply still requires the Unreal Editor, the live editor bridge, explicit UEPI write settings, and user approval. Without those gates, edit tools return structured rejection diagnostics and do not mutate assets.

## Recommended Prompt Rule

```text
Use UEPI first. Call uepi_status before other UEPI tools. Use uepi_context to build bounded evidence before answering Unreal project questions. Treat Blueprint pin links, GUIDs, and evidence as source of truth. For edits, always preview first, wait for explicit user approval, then apply, validate, refresh/diff, and report the result.
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

`uepi_edit_apply` is disabled by default by UEPI project settings. Enable the live editor bridge and the explicit write flags only in a test project or sandbox directory, then use preview -> user approval -> apply -> validate -> refresh/diff.
