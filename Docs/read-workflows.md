# Read Workflows

UEPI's read workflow is Snapshot Query. A saved Snapshot is enough for offline analysis, and the unified Codex profile can still choose guarded edit tools only when the user asks for a project change.

## Status First

1. Call `uepi_status`.
2. Check `ok`, `data_mode`, `snapshot`, `cache.synced`, and `llm_readiness`.
3. If `freshness` is `stale` or `refresh_requested`, keep the editor open and retry after UEPI processes the targeted request.

## Natural-Language Project Questions

1. Call `uepi_context` with `route: "auto"`.
2. Read `result.route`, `result.sections`, `evidence`, and `next_actions`.
3. Call the recommended narrow tool.

When the editor bridge is enabled and connected, pass `live: true` to include editor status, selection, and output log tail.

## Blueprint Questions

Use:

```json
{"asset": "/Game/Path/BP_Name.BP_Name"}
```

`uepi_blueprint` returns:

```text
blueprint_entities
relations
semantic_summary
call_graph
data_mutations
```

For a specific event or node, call `uepi_blueprint_trace` with `start`.

When you need an immediate editor refresh and the bridge is connected, pass `refresh: "force"` to `uepi_asset`, `uepi_blueprint`, or `uepi_animation`.

## Animation Questions

Use `uepi_animation` for sequence, skeleton, track, notify, curve, and motion summary. Use `uepi_impact` when you need to know who plays or references the animation.

## Impact Questions

Use `uepi_impact` before planning changes. It returns incoming/outgoing relations and affected entities from Snapshot evidence.
