# Project Agent Notes

Use UE Project Intelligence before answering project-specific Unreal Engine questions.

## UEPI Rules

- Call `uepi_status` before other UEPI tools.
- Prefer `uepi_context` with `route: "auto"` to build bounded evidence.
- Use narrow tools after context:
  - `uepi_asset`
  - `uepi_blueprint`
  - `uepi_blueprint_trace`
  - `uepi_animation`
  - `uepi_impact`
  - `uepi_diff`
- Treat Blueprint pin links, GUIDs, relation IDs, and evidence as source of truth.
- Distinguish `saved`, `live`, `stale`, and `refresh_requested`.
- Read tools and guarded edit tools are exposed together. Use read/context tools first, then choose edit tools only when the user asks for a project change.
- Never apply edits without preview, explicit user approval, validation, and post-edit diff.
- Never guess Blueprint pin names; use returned pins and GUIDs.

## Typical Flow

```text
uepi_status
uepi_context
domain-specific read tool
answer with evidence and uncertainty
```

For requested edits:

```text
uepi_status
uepi_context / narrow read tool
uepi_edit_discover
uepi_edit_preview
ask for approval
uepi_edit_apply
uepi_edit_validate
refresh/read changed assets
uepi_diff where applicable
```
