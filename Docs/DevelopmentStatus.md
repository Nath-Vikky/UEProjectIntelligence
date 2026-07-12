# UE Project Intelligence Development Status

`main` is the `2.0.0-beta.1` release-candidate line for UE 5.3.2 and Codex.

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
- Actor, Content/DataAsset/property, Material Instance/assignment/parameter, UMG WidgetTree/layout/delegate, Enhanced Input, Animation/Montage, Blueprint, and AnimGraph operations execute through concrete Registry handlers with domain-owned preflight; all legacy Bridge apply branches and fallback handlers have been removed.
- Blueprint/AnimGraph preflight tracks same-plan variables, functions, components, graph nodes, and node references before the first mutation; Apply resolves the corresponding live objects for node creation, pin maintenance, typed properties, compile, validation, and touched-only save.
- Operation descriptors separate write targets from read-only dependencies. Both participate in stale-plan fingerprints, while only write targets consume mutation budgets and enter backup, validation, save, refresh, and rollback sets.
- Exact Blueprint reads traverse only structural containment relations, and Blueprint node schemas return stable/reflected real pin names without constructing unsafe transient graph nodes.
- Transaction-bound UEPI-owned PIE status/start/stop/input/typed allowlisted invoke/read/wait/assert and cleanup. Runtime tickets bind exact targets, functions, arguments, observations, and maps; function Schema and typed outputs are available to the Agent.
- Project-local Codex setup script, machine-readable Doctor, public schemas, v2 contract snapshot, release packaging, architecture/safety/edit/runtime guides, and real-machine report templates.

## Verified In This Development Pass

- UE 5.3.2 `GasDemoEditor Win64 Development` source build completed successfully after vNext runtime changes.
- Python package/tool compilation passes.
- Snapshot MCP v2 synthetic regression is the required automated gate.
- Offline GasDemo Doctor resolves the exact project binding, saved generation, MCP config, store access, and installed project plugins.
- Online GasDemo Doctor passes 17/17 checks with all 64 Registry operations discoverable and write/save/PIE gates enabled.
- The Third Person Golden transaction duplicated `BP_ThirdPersonGameMode`, added a variable, custom event, Print String, and real-pin connection, compiled and saved the touched asset, refreshed its Snapshot, produced a semantic diff, and completed UEPI-owned PIE start/stop after one approval.
- Editor restart preserved the Golden package hash and the Saved Snapshot could read back the event, screen message, nodes, pins, and links; the old runtime ticket was rejected in the new Editor session.
- The `2.0.0-alpha.4` Source and UE5.3.2 Win64 archives pass version/root/exclusion/hash inspection, and the extracted Source archive passes an isolated UE5.3.2 Win64 `BuildPlugin` build.
- A clean blank Blueprint project starts from the alpha.4 Win64 archive, generates saved generation 1 through `UEPIIndex`, passes offline Doctor with no failures, and answers offline Status/Overview queries.
- Two simultaneous Editor projects bind distinct localhost bridge ports, each exact-project Doctor reaches its own 64-operation catalog, and a request naming the other project is rejected with `UEPI_PROJECT_MISMATCH`.
- The blank-project DataAsset transaction creates, types, validates, saves, reads, diffs, and rolls back a new `PrimaryAssetLabel`; the package is removed, the journal is `rolled_back`, and exact current-view reads return `UEPI_ASSET_TOMBSTONED` without diagnostics.
- The LLMNPCDemo Golden exact-read the existing `DefaultGroup.DefaultSlot`, reflected writable MotionTemplate and runtime function schemas, created and persisted `MT_Wave_Asset_Manny_v1` after one approval, and returned a complete property diff.
- Controlled PIE invoked `SubmitPublishedTemplate` with typed arguments, observed Waving enter `Playing` and finish `Completed`, captured a non-empty viewport, found no new runtime error log lines, and stopped cleanly. Editor restart preserved every configured property and 40 exact-session status samples had zero failures.
- Golden hardening corrected `BlueprintReadOnly` reflection semantics, added parameterized allowlisted invoke and typed return values, made Bridge session JSON replacement atomic, and deferred new PrimaryDataAsset registration until same-plan identity properties are final.
- The `2.0.0-beta.1` Source archive passes an isolated UE5.3.2 Win64 `BuildPlugin`; the Win64 archive contains that isolated DLL and passes root, version, exclusion, manifest, and checksum inspection.
- A clean UEPIBlank reinstall from the Beta.1 Win64 archive starts beside LLMNPCDemo, binds its own Editor session, and passes online Doctor 17/17 with exact-project Bridge routing and all 64 operations.
- Post-Beta hardening raises the default atomic budget to 96 operations/12 assets, checks active Editor limits during Preview, reports large-transaction risk, scales Apply/Refresh timeouts, and makes the one-approval Agent continuation contract explicit. Online GasDemo Doctor reports the new limits and the 13-asset dry-run is blocked before approval.

## Beta.1 Release Gate

- All P0 Golden, routing, restart, rollback, isolated build, archive, and clean-install requirements in `DOCX/UEPINEXT.md` are complete.
- Release artifacts and their checksums are recorded in `Docs/Releases/2.0.0-beta.1.md`.
- Broader per-operation real-machine coverage continues after Beta and does not block this release.

The `v2.0.0-beta.1` tag may be created after this verification record is committed.
