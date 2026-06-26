# UEPI Agent Guide

UEPI is read-only for Unreal project content. Do not add MCP tools that save, delete, rename, compile, or execute project assets unless the write-operation phase is explicitly opened.

Recommended MCP flow:

1. Call `uepi_status`.
2. Use `uepi_search` or `uepi_context` to identify the smallest useful asset set.
3. Use one narrow read tool for the domain: `uepi_asset`, `uepi_blueprint`, `uepi_blueprint_trace`, `uepi_animation`, or `uepi_impact`.
4. If a response contains `UEPI_REFRESH_REQUESTED`, retry the same read after the editor plugin processes the request.
5. If a response contains `UEPI_SNAPSHOT_STALE`, use the saved data with that caveat or ask the user to open the editor/plugin.

Facts come from Snapshot fragments and the rebuildable SQLite cache. Cache files can be deleted and regenerated with `python -m uepi sync`.
