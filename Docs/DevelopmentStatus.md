# UE Project Intelligence Development Status

`main` is the `2.0.0-beta.5` release-candidate line for UE 5.3.2 and Codex.

## Implemented

- One project-local Python stdio MCP; no daemon, HTTP API, Web UI, external worker queue, or remote registration.
- Snapshot Store v2 with immutable fragments, live overlay, tombstones, incremental events, targeted requests, and rebuildable SQLite cache.
- Project binding hash and exact Editor session guards across status, live calls, plans, apply, and runtime tickets.
- MCP envelope v2, hard scope, exact object-path reads, Blueprint graph/node filters, animation exact mode, projection, byte/item budgets, and opaque generation-bound cursors.
- Fifteen read/live tools and five guarded edit facade tools in the single `codex` profile.
- Live Editor status/selection/log cursor/viewport, World actors/components, targeted Refresh jobs, Reflection Schema, and controlled PIE Runtime.
- Project plugin content mount discovery plus project/plugin C++ source and module-manifest indexing.
- Editor-exported versioned operation catalog and hash, Plan v2, plan expiry, idempotency, affected assets, before fingerprints, dirty/read-only/path/budget preflight, and policy-bound authorization.
- Plan v2 includes nested project/editor/base state, dependency-checked operation order and references, predicted objects/packages, preconditions, validation/save/verification plans, risk, and hash-bound approval.
- Dedicated transaction journal, backup/restore service, touched-only package save service with hashes, Validator Registry, memory undo, package reload, targeted post-apply refresh, and transaction diff.
- Reflected typed property codec for scalar, enum/name/text, object/soft-object, struct, array, set, and map values.
- Generic DataAsset creation/property writes; Blueprint variables/components/functions/events/generic nodes/pin defaults/connections/removal/layout/comments/compile; AnimGraph Slot and pose-link operations.
- Guarded Actor, Material Instance, scoped Content, UMG, and Enhanced Input operations behind the same project/session/plan/preflight/save/rollback pipeline.
- Actor, Content/DataAsset/property, Material Instance/assignment/parameter, UMG WidgetTree/layout/delegate, Enhanced Input, Animation/Montage, Blueprint, and AnimGraph operations execute through concrete Registry handlers with domain-owned preflight; all legacy Bridge apply branches and fallback handlers have been removed.
- Blueprint/AnimGraph preflight tracks same-plan variables, functions, components, graph nodes, and node references before the first mutation; Apply resolves the corresponding live objects for node creation, pin maintenance, typed properties, compile, validation, and touched-only save.
- Operation descriptors separate write targets from read-only dependencies. Both participate in stale-plan fingerprints, while only write targets consume mutation budgets and enter backup, validation, save, refresh, and rollback sets.
- Exact Blueprint reads traverse only structural containment relations, and Blueprint node schemas return stable/reflected real pin names without constructing unsafe transient graph nodes.
- Standalone, explicitly approved UEPI-owned PIE verification plus edit-bound verification. Runtime Preview/Approve tickets bind exact project, Editor session, map, input delivery, InputAction/key/value, functions, arguments, observations, delays, and timeouts without requiring a dummy asset edit.
- Gameplay input ownership and cross-asset effect tracing combine live GameMode/Possession/InputComponent evidence with cached Blueprint calls, Custom Events, interface candidates, terminal effects, and duplicate input-path diagnostics.
- Blueprint Pin snapshots retain container/element/Map terminal types, qualifiers, wildcard state, raw links, and incomplete-projection diagnostics; focused node reads prioritize complete Pins and direct one-hop links.
- Editor-backed Preview locks graph/node/pin targets for Apply. Dependency readiness, baseline compile, disk-first compensation, deferred reload, persistent recovery markers, targeted-refresh tombstones, and operation-level failure evidence protect edit atomicity.
- Project-local Codex setup script, machine-readable Doctor, public schemas, v2 contract snapshot, release packaging, architecture/safety/edit/runtime guides, and real-machine report templates.
- Whole-envelope response projection/pagination/budget enforcement, artifact-backed large animation payloads, strict operation contracts, and end-to-end timing diagnostics.
- Exact World actor/component reads and filters plus viewport resize, camera metadata, absolute artifacts, and inline MCP image content.
- Consistent multi-asset live generations, atomic Hard Scope refresh, stable `FKey` routing, Focus/Pin projection preservation, and accurate pre/post-projection payload accounting.
- Source-hashed Python service identity, patch-specific Editor build IDs, actionable transaction recovery inspection/finalize/rollback, and discriminated edit operation wrappers.
- Typed Runtime assertions, truthful input-delivery evidence, safe BlueprintPure observations, objective/human/hybrid verification modes, and human-owned visual acceptance.
- Project settings authorization modes (`ReviewEachPlan`, `TrustedSession`, `TrustedProject`) with bounded roots/domains/risk/destructive/runtime policy and complete post-action reports.

## Verified In This Development Pass

- The Beta.5 synthetic Snapshot MCP regression passes with multi-asset ownership, exact input routing, focused Pin projection, service identity, recovery, trusted authorization, typed Runtime values, and post-action-report coverage.
- `GasDemoEditor Win64 Development` compiles successfully against UE 5.3.2 after the Beta.5 recovery, Runtime, authorization, settings, and build-identity changes.
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
- UEPIBlank passes an eight-operation/four-asset success transaction with exact reads, semantic diff, explicit rollback, and restart tombstones. A second transaction injects failure after operation four, restores atomicity across all four targets, writes `failed_rolled_back`, leaves no package files, and preserves its structured failure evidence through the MCP envelope.
- The `2.0.0-beta.2` Source archive passes isolated UE5.3.2 `BuildPlugin`; the Win64 archive contains the isolated DLL, passes root/version/exclusion/hash inspection, and a fresh archive install reaches online Doctor 17/17 after its first `UEPIIndex` Snapshot.
- The 2026-07-17 contract-hardening pass builds on UE5.3.2, passes the synthetic Snapshot MCP regression and online Doctor 17/17, and passes the live read-contract smoke for current map, catalog, exact World actor, strict operation schema/example, 640x360 viewport, camera metadata, absolute screenshot artifact, inline PNG content, and all eight timing stages.
- The Beta.3 recheck hardening replaces mutable Windows cache publication with generation-addressed SQLite files and atomic pointers; two concurrent sync clients publish one generation while an old reader remains usable, and 20 warm fixture engines do not rebuild the cache.
- The LLMNPCDemo live read-contract now resolves `ABP_Manny1`'s `DefaultSlot` through typed graph/class filters, returns an empty non-refreshing result for a missing node class, and consistently selects `Waving.Waving` from a shuffled three-asset Hard Scope.
- Twenty mixed live Status/World/Schema reads complete without `WinError 5` or `UEPI_CACHE_SYNC_FAILED`; tail-window incremental-event parsing lowers the measured warm P95 to 1,021.636 ms on the 2026-07-17 test machine.
- UE 5.3.2 recompiles the Beta.3 Editor module successfully after adding version/build metadata to Bridge Status and session publication.
- The Beta.4 synthetic Golden resolves `BP_ThirdPersonCharacter.Three -> GetAllActorsOfClass -> BP_LLMNPC_Manny.333 -> SubmitPublishedTemplate -> Waving -> Dynamic Montage`, selects the Character as input owner, flags Manny's duplicate local input, preserves `OutActors` as an array, and stays below 30 KB and 2 seconds.
- The Beta.4 Snapshot regression also proves strict tombstone ownership, strong operation schemas, independent Runtime tickets, exact Enhanced Input approval fields, direct Pin disconnect diffs, focused node reads, and cache-only Hard Scope routing.
- `GasDemoEditor Win64 Development` compiles successfully against UE 5.3.2 with the Beta.4 Bridge, Enhanced Input injection, deferred recovery, locked-target preflight, and Blueprint semantic-ID changes.

## Beta.3 Release Gate

- All Beta.2 Golden, routing, restart, rollback, atomicity, and guarded-write requirements remain satisfied.
- Versioned Cache publication, explicit connection closure, typed Blueprint filtering, non-refreshing filter misses, Hard Scope root preservation, animation ranking, and Discover pagination pass isolated regressions.
- The source tree compiles against UE 5.3.2 and the LLMNPCDemo read-only live contract passes against the matching Editor bridge.
- Broader per-operation real-machine coverage continues after Beta and does not block this release candidate.

## Beta.4 Release Gate

- `python -B Tools/test_snapshot_mcp_v2.py` passes.
- `GasDemoEditor Win64 Development` builds against UE 5.3.2.
- The matching plugin must be copied to LLMNPCDemo, rebuilt, and exercised with the read-only live contract before tagging.
- The destructive four-node Apply and standalone PIE input/state assertion remain explicit real-machine checks; they are not claimed by the synthetic gate.

The `v2.0.0-beta.4` tag may be created after the LLMNPCDemo live checks are recorded.

## Beta.5 Release Gate

- `python -B Tools/test_snapshot_mcp_v2.py` must pass with cross-asset ownership, exact input, focused Pin, projection, service identity, recovery, trusted authorization, typed Runtime, and post-action-report assertions.
- `GasDemoEditor Win64 Development` must compile against UE 5.3.2 with the matching Beta.5 Bridge.
- The matching plugin must be copied to LLMNPCDemo and pass Doctor plus `test_live_read_contract.py --llmnpc-regression` from a cold MCP process.
- TrustedProject Apply and objective Runtime execution remain explicit real-machine tests. Final animation quality remains a user-owned PIE visual decision.

The `v2.0.0-beta.5` tag may be created after the LLMNPCDemo live and trusted-write checks are recorded.
