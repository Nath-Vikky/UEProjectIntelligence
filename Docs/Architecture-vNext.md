# UEPI vNext Architecture

UEPI is one project-local stdio MCP server backed by an Unreal Editor plugin. It does not use a daemon, HTTP API, external worker queue, or remote discovery service.

```text
Codex
  -> Services/uepi (stdio MCP, request guards, bounded query layer)
  -> Saved/UEProjectIntelligence/store (saved evidence and rebuildable cache)
  -> exact-project localhost Editor Bridge (short-lived reads and guarded writes)
  -> Unreal Editor APIs (the only authority that mutates assets)
```

## Project Binding

The canonical absolute `.uproject` path is hashed into `project_binding_id`. Every live request checks the configured project, active Editor session, plan, catalog, and runtime ticket against that binding. An online exact-project Editor session is authoritative. With no matching Editor, `--project` selects one offline Snapshot store.

## Read Path

Saved Snapshot fragments are immutable evidence. A SQLite cache is derived from those fragments and can be rebuilt. Exact object paths are exact by default; bounded search and context tools are used to discover paths. Saved data works with the Editor closed. Selection, output log, viewport, live world, refresh, property schema, and current graph state require the Editor Bridge.

The response envelope is `uepi.mcp-envelope.v2`. Projection, item limits, byte budgets, and opaque cursors bound large responses. A cursor is tied to its query and Snapshot generation.

## Write Path

The Editor-exported Operation Registry is the capability source. The Agent reads before writing, calls Discover, constructs one complete immutable Plan v2, and receives a hash, nonce, expiry, before fingerprints, and affected assets. One explicit user approval authorizes only that exact plan.

Apply repeats all guards before mutation, creates a journal and file backups, uses an Unreal transaction, validates touched objects, saves touched packages only, requests targeted refresh, and writes a transaction diff. Failure triggers memory undo or backup restore and package reload. Arbitrary Python, shell, console commands, save-all, source-control submission, broad delete, and plugin enablement are not exposed.

Backup/restore, touched-package save, transaction journal, and validation are independent Editor services. The Validator Registry currently selects Blueprint/AnimBlueprint compile, Animation/Montage skeleton/slot/segment checks, Material Instance parent checks, DataAsset checks, or a generic UObject validity fallback.

Domain handlers own operation-specific preflight and mutation. Actor, Content/Asset, Material, and Enhanced Input operations are fully migrated: Registry entries execute exact target/type/path checks, typed probes where applicable, and apply logic. The Bridge supplies transaction context, including planned asset classes, captures created-asset references, marks Blueprint validation targets, and records the handler result; remaining legacy domains are being moved through the same boundary before Beta.

## Runtime Path

Successful Apply can return a transaction-bound runtime verification ticket. UEPI may then own a PIE session and perform allowlisted status, input, parameterless Blueprint-callable invoke, read, wait, assert, and stop actions. It never takes over a PIE session it did not start and always attempts cleanup.

## Source Layout

- `Services/uepi/src/uepi`: MCP protocol, identity, query, planning, diff, runtime, and bridge client.
- `Source/UEProjectIntelligence`: Editor collection, local bridge, registry, property codec, validation, save, and rollback.
- `Schemas`: versioned public schemas.
- `Tools`: setup, doctor, contract export, packaging, and regression checks.
- `Saved/UEProjectIntelligence`: per-project runtime data; never packaged or committed.

## Current Release Gate

The source is an experimental alpha until the real-machine matrix in `Docs/RealMachineTests/README.md` passes on UE 5.3.2. Compilation and synthetic tests do not promote it to Beta by themselves.
