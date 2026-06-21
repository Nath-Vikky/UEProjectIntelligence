# Architecture Decision Records

## ADR-001: UE Native Loading Is Authoritative

UEPI uses Unreal Editor or commandlet loading as the authoritative path for asset structure. External `.uasset` parsing can be a metadata aid only when labeled unverified.

## ADR-002: MCP Is An Adapter

MCP, HTTP, CLI, and Web UI all sit above the same scan/index/query core. Protocol changes must not force scanner or schema rewrites.

## ADR-003: Hybrid Storage

SQLite stores entity/relation/history data, JSON stores snapshots/artifacts, and optional columnar formats handle large numeric datasets.

## ADR-004: Blueprint Pins Are First-Class

Pin-level graph data is canonical. Node-level CFG/DFG/call relations are derived projections with evidence.

## ADR-005: v1.0 Is Read-Only

The plugin must not save packages, rename assets, delete assets, compile Blueprints by default, or silently mutate project data.

## ADR-006: Daemon Is A Separate Process

The local daemon owns indexing/query/MCP/Web concerns so database or protocol failures do not crash the editor.

## ADR-007: Derived Relations Require Evidence

Every derived edge must point back to source entities, links, pins, properties, or scan evidence.

## ADR-008: Incomplete Results Are Explicit

Unsupported or partial extraction must be represented by diagnostics and completeness metadata.

## ADR-009: SQLite Is The Default Graph Store

External graph databases are optional exports, not required runtime dependencies.

## ADR-010: Large Results Become Artifacts

Large graph, animation, table, and MCP results must be range-readable artifacts or exported files instead of oversized inline payloads.

## ADR-011: Optional UE Domains Use Extensions

Optional plugin domains should be isolated behind adapters or dedicated readers so the core plugin can load in smaller projects.

## ADR-012: UE5.8 Official MCP Is Future Adapter Work

UE5.8 Toolset integration should adapt to UEPI's query/extraction facade rather than backporting UE5.8 editor modules to UE5.3.
