# MCP Tools

All read tools accept common v2 options such as `expected_project_file`, `expected_editor_session_id`, `exact`, `refresh`, `compact`, field projection, evidence level, page size, cursor, and payload budget where applicable.

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
| `uepi_runtime` | Transaction-bound controlled PIE verification | Yes |

For `uepi_animation`, use `mode="exact_asset"` unless dependency/playback context is explicitly needed. Start procedural reconstruction with `reconstruction_profile` or `driver_track_curves`; request full-pose artifacts only for high-fidelity work.

## Edit Facade

| Tool | Purpose |
|---|---|
| `uepi_edit_discover` | Read the exact Editor's versioned operation catalog |
| `uepi_edit_preview` | Build immutable Plan v2 and run dry preflight |
| `uepi_edit_apply` | Apply one approved plan after all guards repeat |
| `uepi_edit_validate` | Re-run transaction validation |
| `uepi_edit_rollback` | Undo/restore the applied transaction |

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
one user approval
uepi_edit_apply
uepi_edit_validate
uepi_diff(mode="transaction")
uepi_runtime only when a returned ticket requires runtime proof
```

`UEPI_REFRESH_REQUESTED` means retry after the Editor processes the targeted request. `UEPI_SNAPSHOT_STALE` means offline evidence is older than a known change.
