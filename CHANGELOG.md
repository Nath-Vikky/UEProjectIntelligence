# Changelog

## 2.0.0-alpha.2

- Added exact project/session binding, authoritative Status v2, MCP envelope v2, hard scope, exact reads, projection, payload budgets, and opaque pagination.
- Added live Editor, World, targeted Refresh, Reflection Schema, and controlled Runtime tools to the unified Codex profile.
- Added Editor-exported operation catalog, immutable Plan v2, repeat preflight, idempotency, before fingerprints, one-approval Apply, touched-only save, backups, validators, rollback, and transaction diff.
- Added reflected UObject property schemas and typed value codec, generic DataAsset/property writes, generic Blueprint graph maintenance, and AnimGraph Slot/pose operations.
- Aligned the P0 operation contract with typed `writes`, same-transaction `$ref` assets/nodes, complete descriptor fields, explicit touched-asset save, and AnimGraph/Actor/Widget compatibility operations.
- Added collection write modes, JSON map-key property paths, and complete v2 envelopes for unknown-tool and top-level failure responses.
- Added preflighted AnimMontage creation from Sequence plus slot track, segment, section, blend, preview mesh, same-transaction references, save, and rollback participation.
- Bound Runtime tickets to an explicitly approved verification plan and added per-project exact function allowlisting on top of ticket function/key/read/map guards.
- Extracted backup/restore, touched-package save, phase journal, and typed Validator Registry services from the Editor Bridge apply pipeline.
- Completed nested Plan v2 base/risk/approval/verification structure, dependency-bound refs, Class/SoftClass typed values, richer transaction diff, runtime failure cleanup, and dual-artifact release packaging.
- Migrated Actor write operations from the Bridge apply branch into concrete Registry handlers with authoritative preflight and typed property diffs.
- Migrated Enhanced Input creation and key-mapping operations into concrete Registry handlers with optional-module blocking, destination checks, and same-plan created-asset capture.
- Migrated Material Instance creation, typed parameter writes, and actor/Blueprint component assignment into concrete Registry handlers with full preflight and same-plan references.
- Added project-plugin content/source indexing, project-local Codex setup, machine-readable Doctor, versioned public schemas, v2 contract export, and source/prebuilt release packaging.
- Kept the release marked experimental alpha until the UE5.3.2 real-machine matrix is complete.

## 2.0.0-alpha.1

- Started the snapshot-first read-only MCP convergence line.
- Reframed `main` around UE plugin collection, Snapshot Store, and stdio MCP as the only default AI entry point.
- Added the initial Snapshot Store v2 C++ writer skeleton with canonical `Saved/UEProjectIntelligence/store` and `cache` layout.
- Converted the UEPIIndex commandlet toward a one-shot Snapshot writer and removed its daemon worker mode.
- Routed the Editor dashboard scan action through Snapshot Store and removed worker/web controls from the default dashboard UI.
- Added the new Snapshot-backed `Services/uepi` Python query package and ten-tool stdio MCP server.
- Added optional-reader compile gates so ordinary projects do not link EnhancedInput, CommonUI, GAS, StateTree, IKRig, ControlRig, Niagara, PCG, or MetaSound modules unless explicitly enabled.
- Replaced newly written full `project_scan` Snapshot fragments with small `project_fragment` objects while keeping legacy `project_scan` reads for migration.
- Added `typed_attributes` v2 wrappers and SQLite cache v2.1 routing for search and graph-neighborhood MCP reads.
- Added on-demand targeted refresh requests from MCP reads, editor-side request polling, incremental-event freshness diagnostics, and `asset_tombstone` current-view handling.
- Added cache auto-sync, MCP initialize instructions, Codex config template, release checklist, and refreshed user documentation.
- Added Blueprint compile targeted live refresh, rename tombstone fixture coverage, compact CLI, release packaging checks, and placeholder-only public docs.
- Queued targeted refresh requests when Blueprint/animation domain data is missing from an otherwise current L0 Snapshot, including offline request handoff for the next editor session.
- Removed legacy daemon, HTTP/Web UI, worker/queue tooling, and extension SDK files from the mainline.
- Marked daemon, worker, HTTP API, job queue, and Web UI flows as legacy v1 architecture to be removed from the default product.
- Preserved the previous stable daemon-compatible loop as tag `v1.0.0`.

## 1.0.0

- Stabilized the read-only MCP loop for LLM project understanding.
- Added LLM-friendly Blueprint graph summaries and `event_graph_nodes` snapshots.
- Added AnimSequence all-key bone motion summaries with changing-bone metrics.
- Added short-name and cross-scan asset resolution for high-level MCP read tools.
- Fixed Blueprint graph extraction for assets that also carry World Partition actor descriptor metadata.
- Added the Chinese user guide with a Codex MCP configuration example.

- Added L2 coverage across Blueprint, World, Input, UI, AI, StateTree, GAS, Animation, Data, Niagara, PCG, MetaSound, Audio, Cinematics, Render, and Material domains.
- Added SQLite ingest, search, pagination, diff, stale detection, revision history, graph export, Markdown reports, and DB integrity/recovery commands.
- Added local HTTP API, daemon-served Web UI, and MCP stdio adapter.
- Added editor dashboard tab and Content Browser context menu entry.
- Added security fuzz audit, dirty package regression, synthetic performance baseline, and release packaging manifest tooling.
