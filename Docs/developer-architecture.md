# UEPI Developer Architecture

UEPI is split into three layers:

- Unreal editor plugin: read-only asset scanning, static graph extraction, dirty package guard, editor dashboard, and content browser entry points.
- Local daemon: SQLite index, history, diff, stale checks, query/export APIs, Web UI serving, MCP stdio adapter, safety checks, and release tooling.
- Artifacts: JSON scan snapshots, golden summaries, performance baselines, MCP result artifacts, release manifests, and optional graph exports.

## Data Flow

1. Commandlet or editor subsystem writes a scan JSON artifact.
2. `uepi_daemon.py ingest` stores scan, entity, relation, diagnostic, and asset revision rows in SQLite.
3. CLI, HTTP, MCP, and Web UI call the same query helpers.
4. Large or budgeted results are served as artifacts rather than oversized inline responses.

## Read-Only Boundary

The scanner may load assets for inspection but does not save packages, compile Blueprints by default, rename assets, delete assets, or mutate content. Dirty package state is checked after scans and surfaced as diagnostics.

## Extension Points

`UEPIExtensionInterfaces.h` defines:

- `UE::ProjectIntelligence::IAssetAdapter`
- `UE::ProjectIntelligence::INodeSemanticAdapter`

Adapters should register through Unreal modular features and emit UEPI entities/relations with stable IDs, evidence, and completeness metadata.
