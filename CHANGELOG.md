# Changelog

## 2.0.0-dev

- Started the snapshot-first read-only MCP convergence line.
- Reframed `main` around UE plugin collection, Snapshot Store, and stdio MCP as the only default AI entry point.
- Added the initial Snapshot Store v2 C++ writer skeleton with canonical `Saved/UEProjectIntelligence/store` and `cache` layout.
- Converted the UEPIIndex commandlet toward a one-shot Snapshot writer and removed its daemon worker mode.
- Routed the Editor dashboard scan action through Snapshot Store and removed worker/web controls from the default dashboard UI.
- Added the new Snapshot-backed `Services/uepi` Python query package and ten-tool stdio MCP server.
- Added optional-reader compile gates so ordinary projects do not link EnhancedInput, CommonUI, GAS, StateTree, IKRig, ControlRig, Niagara, PCG, or MetaSound modules unless explicitly enabled.
- Replaced newly written full `project_scan` Snapshot fragments with small `project_fragment` objects while keeping legacy `project_scan` reads for migration.
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
