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
blueprint.create_function
blueprint.add_event_node              # custom_event only
blueprint.add_function_call_node
blueprint.add_variable_get_node
blueprint.add_variable_set_node
blueprint.add_branch_node
blueprint.add_print_string_node
blueprint.connect_pins
blueprint.compile
actor.set_transform
actor.set_property
material.set_scalar_parameter
material.set_vector_parameter
material.set_texture_parameter
```

Blueprint graph operations return the created node GUIDs, pin IDs, pin names, directions, and links so the Agent can connect pins without guessing UI labels.
`blueprint.add_event_node` is intentionally limited to custom events in this alpha.

Package saving remains disabled by default. Unsupported or out-of-scope graph operations return structured diagnostics instead of partially editing graphs.
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
