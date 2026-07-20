# MCP Tools

All read tools accept common v2 options such as `expected_project_file`, `expected_editor_session_id`, `exact`, `refresh`, `compact`, field projection, evidence level, page size, cursor, and payload budget where applicable.

Every tool result uses MCP envelope v2. `max_payload_bytes` applies to the complete UTF-8 envelope, not only its primary list. Large animation profiles and curve payloads fall back to file-backed artifact manifests when needed. Nested `fields`, `page_size`, and opaque `cursor` options apply to declared result page roots such as matches, graph entities, properties, operations, actors, and components.

## Read And Live Tools

| Tool | Purpose | Editor required |
|---|---|---|
| `uepi_status` | Project binding, Snapshot, bridge, capability, and doctor summary | No |
| `uepi_overview` | Bounded project counts and source overview | No |
| `uepi_search` | Discover exact assets/entities | No |
| `uepi_context` | Question-oriented bounded evidence with hard scope | No |
| `uepi_asset` | Exact asset/entity and nearby relations | No |
| `uepi_blueprint` | Graph/node/pin/CFG/DFG evidence with filters | No |
| `uepi_blueprint_trace` | Static execution/data/delegate/call paths | No |
| `uepi_animation` | Exact animation evidence and reconstruction artifacts | No |
| `uepi_impact` | Incoming/outgoing dependency impact | No |
| `uepi_diff` | Snapshot generation or transaction diff | No |
| `uepi_editor` | Status, selection, incremental log, viewport capture | Yes |
| `uepi_world` | Editor or UEPI-owned PIE actors/components | Yes |
| `uepi_refresh` | Request, inspect, or wait for targeted collection | Yes to execute |
| `uepi_schema` | Property, operation, node, and runtime schemas | Usually yes |
| `uepi_runtime_preview` | Immutable standalone runtime verification plan | Yes |
| `uepi_runtime_approve` | Issue a policy-bound runtime ticket | Yes |
| `uepi_runtime` | Transaction-bound controlled PIE verification | Yes |
| `uepi_recovery_inspect` | Explain unresolved transaction markers and compare fingerprints | No |

For `uepi_animation`, use `mode="exact_asset"` unless dependency/playback context is explicitly needed. Start procedural reconstruction with `reconstruction_profile` or `driver_track_curves`; request full-pose artifacts only for high-fidelity work.

## Edit Facade

| Tool | Purpose |
|---|---|
| `uepi_edit_discover` | Read the exact Editor's versioned operation catalog |
| `uepi_edit_preview` | Build immutable Plan v2 and run dry preflight |
| `uepi_edit_apply` | Apply one approved plan after all guards repeat |
| `uepi_edit_validate` | Re-run transaction validation |
| `uepi_edit_rollback` | Undo/restore the applied transaction |
| `uepi_recovery_finalize` | Acknowledge an already-restored exact recovery transaction |
| `uepi_recovery_rollback` | Restore the exact prepared backup set for a recovery transaction |

Operations remain data inside the edit facade; UEPI does not expose one MCP tool per mutation.

## Recommended Flow

```text
uepi_status
uepi_search / uepi_context
narrow read tool

when the user explicitly requests a change:
uepi_edit_discover
uepi_schema as needed
uepi_edit_preview (one complete plan)
authorization policy decision
one user approval only in ReviewEachPlan
uepi_edit_apply
uepi_edit_validate
uepi_diff(mode="transaction")
uepi_runtime only when a returned ticket requires runtime proof
```

`TrustedSession` and `TrustedProject` permit immediate Apply only when the unchanged plan satisfies every configured scope, domain, risk, destructive-operation, runtime, project, session, and asset-budget constraint. A successful Apply returns a structured post-action report with intent, operation outcomes, affected/compiled/saved assets, validation, semantic diff, backup and rollback state, runtime verification state, and the human visual verification requirement.

`UEPI_REFRESH_REQUESTED` means retry after the Editor processes the targeted request. `UEPI_SNAPSHOT_STALE` means offline evidence is older than a known change.

## Timing

Every MCP tool envelope contains:

```text
timing.total_ms
timing.mcp_queue_ms
timing.snapshot_query_ms
timing.bridge_connect_ms
timing.bridge_wait_ms
timing.editor_dispatch_ms
timing.editor_execute_ms
timing.serialization_ms
```

The fields are non-overlapping server-observed stages. `editor_dispatch_ms` is the time from localhost Bridge acceptance until the Editor game-thread tick dispatches the request. Calls over the configurable `UEPI_SLOW_OPERATION_MS` threshold, 5000 ms by default, include a non-blocking `UEPI_SLOW_OPERATION` diagnostic.

`uepi_world` rejects unknown filter keys. Use `action="read"` for a filtered actor page, `action="actor"` with an exact actor path for details, and `action="component"` with exact actor/component selectors for component state and requested properties. Viewport capture honors `viewport`, `width`, `height`, `include_camera_metadata`, and `inline_image`; the artifact path is absolute and inline mode adds MCP `image/png` content.
