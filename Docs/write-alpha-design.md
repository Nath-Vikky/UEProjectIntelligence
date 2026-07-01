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

## Current Alpha Behavior

`codex_write_alpha` is available for operation catalog, dry-run plan testing, and explicitly enabled live-bridge Blueprint alpha edits.

`uepi_edit_apply` is settings-gated by default:

```text
UEPI_EDIT_WRITE_DISABLED
UEPI_EDIT_BLUEPRINT_DISABLED
UEPI_EDIT_APPROVAL_REQUIRED
```

When the live bridge and write settings are enabled, apply supports:

```text
blueprint.add_variable
blueprint.set_variable_default
blueprint.add_component
blueprint.set_component_property
blueprint.compile
actor.set_transform
actor.set_property
material.set_scalar_parameter
material.set_vector_parameter
material.set_texture_parameter
```

Package saving remains disabled by default. Unsupported graph operations return structured diagnostics instead of partially editing graphs.
Content import/rename, UMG, and Enhanced Input operations are discoverable for preview planning but remain apply-unsupported in this alpha.

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

Plans use `uepi.edit_plan.v1` and are saved under:

```text
__PROJECT_ROOT__/Saved/UEProjectIntelligence/store/edit/plans/
```

The plan must include a `transaction_id`, operation list, affected assets, required evidence, backup artifact, validation plan, and rollback strategy.
