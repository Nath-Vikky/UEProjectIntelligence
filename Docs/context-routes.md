# Context Routes

`uepi_context` chooses a route for the user's natural-language question and returns a bounded evidence pack. It does not add MCP tools; it makes the existing context tool smarter.

Available routes:

```text
project_overview
gameplay_input_to_effect
input_to_gameplay
blueprint_behavior
animation_playback
ui_flow
asset_dependency_impact
data_driven_behavior
gas_ability_flow
ai_behavior_flow
network_replication_flow
```

## Example

```json
{
  "question": "What does BP_Hero BeginPlay do?",
  "route": "blueprint_behavior",
  "max_items": 40
}
```

The response includes:

```text
result.route
result.route_confidence
result.matches
result.relations
result.sections
next_actions
```

## Route Intent

`project_overview` gives project counts and likely entry assets.

`gameplay_input_to_effect` resolves one stable FKey, selects likely player input ownership, and traces static Blueprint calls to terminal gameplay effects. Prefer structured `input_key` and `excluded_input_keys`; free-form questions support `exclude`, `not`, `except`, `不要`, `排除`, and `不是`. Multiple positive keys or an unknown key fail closed with empty paths and no Runtime next action.

`input_to_gameplay` links input assets, mapping contexts, Blueprint handlers, and side effects.

`blueprint_behavior` summarizes Blueprint entrypoints, function calls, variable reads/writes, class interactions, and flow edges.

`animation_playback` summarizes animation assets, motion summaries, notifies, curves, and references.

`ui_flow` finds WidgetBlueprint, viewport, delegate, and binding evidence.

`asset_dependency_impact` groups incoming/outgoing dependencies and risk hints.

The remaining routes provide focused search packs for data assets/tables, GAS, AI, and networking.
