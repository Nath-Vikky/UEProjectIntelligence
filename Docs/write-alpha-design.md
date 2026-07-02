# Write Alpha Design

The stable read path remains Snapshot-first and non-mutating. Guarded write support shares the default Codex profile but keeps a small tool surface:

```text
uepi_edit_discover
uepi_edit_preview
uepi_edit_apply
uepi_edit_validate
uepi_edit_rollback
```

No low-level tools such as `add_node`, `connect_pin`, `save_asset`, `run_python`, or `run_console_command` are exposed directly to Codex.

## Current Alpha Behavior

The default `codex` profile exposes the operation catalog, dry-run plan testing, and explicitly enabled live-bridge alpha edits alongside the stable read tools. `codex_write_alpha` remains accepted as a legacy alias only.

`uepi_edit_apply` is settings-gated by default:

```text
UEPI_EDIT_WRITE_DISABLED
UEPI_EDIT_BLUEPRINT_DISABLED
UEPI_EDIT_APPROVAL_REQUIRED
```

When the live bridge is online and the user has approved the preview, apply supports:

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
actor.spawn
actor.set_transform
actor.set_property
material.create_instance
material.set_scalar_parameter
material.set_vector_parameter
material.set_texture_parameter
material.apply_to_actor
material.apply_to_blueprint_component
content.import
content.create_folder
content.duplicate_asset
content.rename_asset
widget.create
widget.add_text
widget.add_button
widget.set_slot
widget.bind_button_to_custom_event
input.create_action
input.create_mapping_context
input.add_key_mapping
input.remove_key_mapping
```

Blueprint graph operations return the created node GUIDs, pin IDs, pin names, directions, and links so the Agent can connect pins without guessing UI labels. Plans may also assign transaction-local `ref` / `node_ref` aliases to newly created nodes so creation, connection, and compile operations can run in one approved transaction.
`blueprint.add_event_node` is intentionally limited to custom events in this alpha.

Package saving remains disabled by default. Content operations are limited to `/Game` paths, automated imports use an extension allowlist, and broad deletes/save-all operations remain blocked.
UMG operations are limited to Widget Blueprint creation, TextBlock/Button insertion, CanvasPanel slot edits, and Button delegate binding through UE's ComponentBoundEvent path.
Enhanced Input operations are available only when the project enables the EnhancedInput plugin; otherwise discover reports them as apply-unsupported and apply returns a safe unsupported diagnostic.
Unsupported or out-of-scope operations return structured diagnostics instead of partially editing assets.

## Recommended Transaction Flow

```text
read evidence
edit_discover
choose Blueprint design and compare compact vs expanded alternatives
edit_preview with the complete intended plan
one user approval
edit_apply
edit_validate
targeted Snapshot refresh
uepi_diff
report
```

## Blueprint Plan Quality

Write alpha is intentionally operation-based, but Agents should not treat the operation catalog as a macro recorder. For Blueprint graph edits, choose the design first and use operations second.

Preferred designs:

- Use variables for state instead of duplicating constants across many nodes.
- Use timers, loops, custom events, or helper functions for repeated or time-based behavior.
- Keep graphs small enough for a human to inspect, debug, and extend.
- Use expanded node-by-node graphs only for tiny examples, direct user requests, or when UEPI cannot yet express the compact design safely.

`uepi_edit_preview` returns non-blocking quality diagnostics when a plan appears to unroll repeated behavior or create an unusually large number of nodes. Agents should revise the plan or explicitly report the operation-catalog limitation before requesting approval.

## Operation Plan Shape

Plans use `uepi.edit_plan.v1` and are saved under:

```text
__PROJECT_ROOT__/Saved/UEProjectIntelligence/store/edit/plans/
```

The plan must include a `transaction_id`, operation list, affected assets, required evidence, backup artifact, validation plan, and rollback strategy.
