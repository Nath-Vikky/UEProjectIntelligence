# Definition Of Done

A UEPI 2.0-dev feature is done when:

- It preserves the read-first contract and does not mutate assets unless the feature is explicitly part of guarded edit alpha.
- Python/MCP writes only UEPI-owned artifacts under `Saved/UEProjectIntelligence`; Unreal asset mutation, when allowed, happens only through the live editor bridge with preview, approval, validation, and rollback/diff reporting.
- It is represented in Snapshot data or derived from Snapshot data.
- It is reachable through the stdio MCP surface when user-facing.
- It reports completeness, omissions, diagnostics, or stale state when data is incomplete.
- It has a focused smoke, unit, or golden test appropriate to its risk.
