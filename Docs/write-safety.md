# Write Safety

UEPI write support must never bypass these rules.

## Non-Negotiable Rules

- Read before mutate.
- Describe before mutate.
- Preview before apply.
- User approval before apply.
- Use GUIDs and pin names returned by UEPI, never guessed display text.
- Wrap editor changes in a scoped transaction.
- Write backup artifacts before touching assets.
- Compile or validate touched assets.
- Run targeted Snapshot refresh after apply.
- Return `uepi_diff` evidence after apply.
- Keep rollback available for every applied transaction.

## Always-On Safety Gates

Project settings are Agent-ready by default, while the non-negotiable gates remain on:

```text
bEnableLiveEditorBridge = true
bEnableWriteTools = true
bRequirePreviewBeforeApply = true
bRequireSnapshotDiffAfterApply = true
bAllowSavingPackages = false
```

## Rejected Commands

These capabilities must not be exposed directly as MCP tools:

```text
create_blueprint
add_node
connect_pin
save_asset
run_python
run_console_command
delete_asset
```

They may exist internally only behind operation plans, allowlists, user approval, validation, and audit logging.
