# UE Project Intelligence Development Status

`main` is the experimental `2.0.0-alpha.2` vNext line for UE 5.3.2 and Codex.

## Implemented

- One project-local Python stdio MCP; no daemon, HTTP API, Web UI, external worker queue, or remote registration.
- Snapshot Store v2 with immutable fragments, live overlay, tombstones, incremental events, targeted requests, and rebuildable SQLite cache.
- Project binding hash and exact Editor session guards across status, live calls, plans, apply, and runtime tickets.
- MCP envelope v2, hard scope, exact object-path reads, Blueprint graph/node filters, animation exact mode, projection, byte/item budgets, and opaque generation-bound cursors.
- Fifteen read/live tools and five guarded edit facade tools in the single `codex` profile.
- Live Editor status/selection/log cursor/viewport, World actors/components, targeted Refresh jobs, Reflection Schema, and controlled PIE Runtime.
- Project plugin content mount discovery plus project/plugin C++ source and module-manifest indexing.
- Editor-exported versioned operation catalog and hash, Plan v2, plan expiry, idempotency, affected assets, before fingerprints, dirty/read-only/path/budget preflight, and one explicit approval.
- Plan v2 includes nested project/editor/base state, dependency-checked operation order and references, predicted objects/packages, preconditions, validation/save/verification plans, risk, and hash-bound approval.
- Dedicated transaction journal, backup/restore service, touched-only package save service with hashes, Validator Registry, memory undo, package reload, targeted post-apply refresh, and transaction diff.
- Reflected typed property codec for scalar, enum/name/text, object/soft-object, struct, array, set, and map values.
- Generic DataAsset creation/property writes; Blueprint variables/components/functions/events/generic nodes/pin defaults/connections/removal/layout/comments/compile; AnimGraph Slot and pose-link operations.
- Guarded Actor, Material Instance, scoped Content, UMG, and Enhanced Input operations behind the same project/session/plan/preflight/save/rollback pipeline.
- Actor, Content/DataAsset/property, Material Instance/assignment/parameter, and Enhanced Input operations execute through concrete Registry handlers with domain-owned preflight; their legacy Bridge apply branches have been removed.
- Transaction-bound UEPI-owned PIE status/start/stop/input/parameterless invoke/read/wait/assert and cleanup.
- Project-local Codex setup script, machine-readable Doctor, public schemas, v2 contract snapshot, release packaging, architecture/safety/edit/runtime guides, and real-machine report templates.

## Verified In This Development Pass

- UE 5.3.2 `GasDemoEditor Win64 Development` source build completed successfully after vNext runtime changes.
- Python package/tool compilation passes.
- Snapshot MCP v2 synthetic regression is the required automated gate.
- Offline GasDemo Doctor resolves the exact project binding, saved generation, MCP config, store access, and installed project plugins.

## Beta Blockers

- Run and record the LLMNPCDemo, Third Person, blank-project, and two-project real-machine matrix.
- Exercise every migrated write domain through Discover -> Preview -> one approval -> Apply -> Validate -> Save -> restart -> Diff/Rollback.
- Complete structural extraction of Blueprint, Animation, and UMG executor branches into domain operation handlers; Actor, Content, Material, and Enhanced Input are migrated, but the remaining apply implementation still contains a large domain dispatch body.
- Produce and install-test the source release zip. Produce a prebuilt UE5.3.2 Win64 zip only from clean matching-engine binaries.

The plugin remains honestly marked experimental alpha until these items pass. Do not create a `v2.x-beta` tag before that gate.
