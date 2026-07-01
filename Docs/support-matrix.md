# Support Matrix

## Supported In 2.0-dev

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

## Experimental Write Alpha

- Blueprint write alpha can add variables, set variable defaults, add simple components, set simple component properties, and compile through the live bridge when write settings are explicitly enabled.
- Blueprint graph write alpha can create function graphs, add custom events, add function-call, variable get/set, branch, and PrintString nodes, and connect pins by node GUID plus pin name/id.
- Actor write alpha can set transforms and simple reflected actor properties when explicitly enabled.
- Material write alpha can set scalar, vector, and texture parameters on `UMaterialInstanceConstant` assets when explicitly enabled.

## Not Supported In 2.0-dev

- General asset saves, deletes, broad arbitrary Blueprint graph rewriting, Content Browser import/rename, UMG creation, and Enhanced Input writes.
- Runtime pose evaluation.
- Daemon, HTTP, Web UI, worker queue, or remote service deployment.
- Third-party extension SDK.
- Optional reader coverage for disabled plugins is downgraded to generic Asset Registry and reflection metadata.
