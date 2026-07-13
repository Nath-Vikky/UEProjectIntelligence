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
bAllowSavingPackages = true
```

Saving is touched-only. UEPI resolves and backs up every target package before mutation, validates it, saves only packages changed by the approved plan, records hashes, and can restore saved files plus reload their packages. Dirty targets and read-only package files block preflight.

The Agent-ready default atomic budget is 96 operations and 12 affected assets. Projects may configure up to 256 operations and 64 assets, but UEPI does not treat a higher cap as weaker safety: Preview blocks over-budget plans before approval, larger accepted plans are marked high-risk, and every target still participates in preflight, backup, validation, touched-only save, refresh, diff, and rollback.

If execution or validation fails after mutation begins, UEPI cancels the Editor transaction, restores every target from the transaction backup, unloads newly created packages, refreshes the affected paths, and returns explicit `compensation_attempted`, `compensation_ok`, and `atomicity_restored` evidence. A compensated failed transaction never replaces the last successful transaction available to explicit rollback.

Explicit approval is required exactly once for an immutable Preview. It is not approval to write generally. After that approval, the Agent should invoke Apply and finish every unchanged post-apply phase automatically; asking the user to press or call Apply manually is not part of the normal workflow.

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

They may exist internally only behind operation plans, the Editor-exported registry, user approval, validation, and audit logging. Controlled PIE actions are additionally bound to an Apply-issued runtime ticket and an owned PIE session.
