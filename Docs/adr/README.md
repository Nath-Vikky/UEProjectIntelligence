# Architecture Decision Records

## ADR-001: Unreal Loading Is Authoritative

UEPI reads internal asset structure only through Unreal Editor or commandlet APIs.

## ADR-002: UEPI Is A Read-Only MCP Product

The public AI entry point is stdio MCP. MCP tools do not mutate assets.

## ADR-003: Snapshot Is The Source Of Truth

Immutable Snapshot objects and manifests are the fact source. Caches are rebuildable.

## ADR-004: Saved And Live Are Separate

Saved Snapshot data remains queryable while the editor is closed. Live overlay work must not overwrite saved facts.

## ADR-005: No Daemon, HTTP, Web Or Worker Mainline

The default product does not include a local daemon, HTTP API, Web UI, worker queue, or lease protocol.

## ADR-006: Public MCP Surface Is Small

The public tool list is the ten high-level UE project understanding tools in `Services/uepi`.
