# Write Alpha Design

The stable product remains read-only. Write support is an experimental profile with a small tool surface:

```text
uepi_edit_discover
uepi_edit_preview
uepi_edit_apply
uepi_edit_validate
uepi_edit_rollback
```

No low-level tools such as `add_node`, `connect_pin`, `save_asset`, `run_python`, or `run_console_command` are exposed directly to Codex.

## Current Foundation Behavior

`codex_write_alpha` is available for operation catalog and dry-run plan testing.

`uepi_edit_apply` rejects by default:

```text
UEPI_EDIT_APPLY_DISABLED
```

No Unreal assets are modified by this build.

## Future Transaction Flow

```text
read evidence
edit_discover
edit_preview
user approval
edit_apply
edit_validate
targeted Snapshot refresh
uepi_diff
report
```

## Operation Plan Shape

Plans use `uepi.edit-plan.v1` and are saved under:

```text
__PROJECT_ROOT__/Saved/UEProjectIntelligence/store/edit/
```

The plan must include a `transaction_id`, operation list, required evidence, validation plan, and rollback strategy.
