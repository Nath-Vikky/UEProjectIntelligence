# UEPI Definition Of Done

A feature is done when:

- It preserves the read-only project boundary.
- It emits stable IDs and deterministic JSON for stable inputs.
- It records evidence for every derived or non-obvious entity/relation.
- It reports partial support through completeness and diagnostics.
- It has at least one validation path: schema, golden, smoke, security, performance, or build gate.
- It is reachable through the appropriate CLI/HTTP/MCP/UI surface when user-facing.
- It is documented in README, runbook, or the relevant developer document.
- It passes UBT for `GasDemoEditor Win64 Development`.

The whole plugin is done only when the DOCX checklist, v1.0 release checklist, and current validation suite all agree.
