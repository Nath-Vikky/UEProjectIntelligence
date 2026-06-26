# Definition Of Done

A UEPI 2.0-dev feature is done when:

- It preserves the read-only boundary.
- It writes only under `Saved/UEProjectIntelligence`.
- It is represented in Snapshot data or derived from Snapshot data.
- It is reachable through the stdio MCP surface when user-facing.
- It reports completeness, omissions, diagnostics, or stale state when data is incomplete.
- It has a focused smoke, unit, or golden test appropriate to its risk.
