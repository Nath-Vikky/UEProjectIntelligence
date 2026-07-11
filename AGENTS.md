# UEPI Agent Guide

UEPI is read-only by default. Controlled project writes are allowed only through the versioned edit contract: discover, preview, one explicit user approval, preflight, apply, typed validation, touched-only save, targeted refresh, and transaction diff. Never bypass that contract with Python, console commands, shell execution, unrestricted function calls, or save-all.

Recommended MCP flow:

1. Call `uepi_status`.
2. Use `uepi_search` or `uepi_context` to identify the smallest useful asset set.
3. Use one narrow read tool for the domain: `uepi_asset`, `uepi_blueprint`, `uepi_blueprint_trace`, `uepi_animation`, or `uepi_impact`.
4. If a response contains `UEPI_REFRESH_REQUESTED`, use the refresh job status/wait workflow instead of sleeping and guessing.
5. If a response contains `UEPI_SNAPSHOT_STALE`, use the saved data with that caveat or ask the user to open the editor/plugin.

Write flow:

1. Read exact targets and required property/node schemas.
2. Build one complete edit plan and call `uepi_edit_preview`.
3. Ask the user for one approval covering the immutable plan.
4. Apply only the approved plan identity; do not silently alter operations or targets.
5. Require validation, touched-only save, targeted refresh, and semantic diff in the result.
6. Use controlled runtime verification only when the approved plan grants it.

Facts come from Snapshot fragments and the rebuildable SQLite cache. Cache files can be deleted and regenerated with `python -m uepi sync`.
