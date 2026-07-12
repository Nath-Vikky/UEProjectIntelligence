# Support Matrix

## Supported In 2.0 Beta

- UE 5.3.2 editor and commandlet collection.
- Saved Snapshot querying while the editor is closed.
- Fresh editor live overlay querying when `editor-session.json` heartbeat is current.
- Manifests with `project_fragment` and `asset_fragment` Snapshot fragments.
- Legacy `project_scan` Snapshot objects are readable for migration compatibility.
- Automatic targeted live overlay scans for asset added, updated, and renamed editor events.
- Automatic saved manifest append on package saved events.
- Blueprint static graph entities when captured in an L2 Snapshot.
- Animation static metadata and motion summaries when captured in an L2 Snapshot.
- Asset search, context, impact, and generation diff over Snapshot data.
- `typed_attributes` v2 wrappers are emitted next to the legacy string `attributes` map.
- Synced SQLite v2.1 cache routing for high-volume search and graph-neighborhood reads.
- Codex stdio MCP via `Services/uepi/src/uepi/mcp_server.py`.
- Optional domain plugin references are declared as non-enabled optional dependencies.
- Optional domain readers are compile-gated by the project descriptor and do not link unrelated UE plugin modules by default.
- `UEPI_OPTIONAL_READERS=all` or a comma-separated plugin list can force optional reader compilation for projects that intentionally use those domains.

## Guarded Write Beta

- Every write requires exact project/session routing, an immutable Preview, one explicit user approval, repeated preflight, typed validation, touched-only save, targeted refresh, and transaction diff.
- Reflected DataAsset creation and nested scalar/enum/name/text/object/soft-object/struct/array/set/map property writes are supported.
- Blueprint graph operations cover variables, components, functions, registered node kinds, real-pin defaults/connections/disconnections, node removal/layout/comments, compile, save, and rollback.
- AnimGraph operations cover Slot registration, Slot node creation, pose links, compile, save, and rollback on UE5.3.2.
- Controlled runtime verification supports UEPI-owned PIE, exact target selectors, project-allowlisted Blueprint-callable functions, typed arguments/outputs, reads, waits, assertions, screenshots, logs, and cleanup.
- Actor, Material Instance, scoped `/Game` Content, basic UMG, Enhanced Input, and Animation/Montage operations use the same guarded transaction pipeline.

## Not Supported In 2.0 Beta

- Save-all, unrestricted delete, arbitrary Blueprint node classes, and broad graph rewriting outside the registered operation catalog.
- Runtime pose evaluation.
- Arbitrary Python, shell, console commands, C++ expressions, or unrestricted UObject function calls.
- Automatic source-control checkout, resolve, changelist, submit, or lock management.
- UE versions other than 5.3.2 and MCP clients other than Codex as release-qualified targets.
- Daemon, HTTP, Web UI, worker queue, or remote service deployment.
- Third-party extension SDK.
- Optional reader coverage for disabled plugins is downgraded to generic Asset Registry and reflection metadata.
