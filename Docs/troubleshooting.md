# Troubleshooting

## Codex Does Not Show UEPI Tools

- Run `python Tools/uepi_doctor.py --project <path-to-uproject>`.
- Confirm the MCP command points to `Services/uepi/src/uepi/mcp_server.py`.
- Use `--project <path-to-uproject>` or `--store <Saved/UEProjectIntelligence>`.
- For Codex, use `--tool-profile codex`.
- Restart the Codex conversation after changing MCP configuration.

The MCP server is stdio. A global proxy or TUN normally does not affect Codex-to-UEPI transport. The Editor Bridge is exact-project localhost TCP and rejects non-loopback endpoints.

## `uepi_status` Reports Missing Snapshot

Generate a saved snapshot first from the UE dashboard or the `UEPIIndex` commandlet. The required file is:

```text
Saved/UEProjectIntelligence/store/manifests/saved.json
```

## Blueprint Or Animation Details Are Missing

The current Snapshot may be L0 metadata only. Regenerate with a targeted L2 commandlet scan for the asset you need, then call `uepi_blueprint` or `uepi_animation` again.

## Read Tool Returns `UEPI_REFRESH_REQUESTED`

The MCP server found an incremental editor event newer than the current Snapshot for the requested asset. It queued a targeted refresh request under:

```text
Saved/UEProjectIntelligence/store/requests
```

Keep the editor and UEPI plugin open briefly, then retry the same tool call.

## Read Tool Returns `UEPI_SNAPSHOT_STALE`

The Snapshot is older than a recorded editor event, but there is no active editor session to service a targeted refresh request. Open the editor/plugin or run a targeted `UEPIIndex` commandlet scan.

## Deleted Or Renamed Asset Is Still In A Question

If the old path was observed by UEPI, the asset read tools return `UEPI_ASSET_TOMBSTONED`. Use the tombstone candidate fields to see whether the asset was removed or renamed.

## Editor Is Closed

This is supported for offline reads. The MCP server reads the last saved Snapshot and reports `editor_connected: false`. Live Editor/world/schema reads, write Apply, refresh execution, and runtime verification require the Editor.

## Apply Is Blocked

Run Doctor with `--require-editor`, then call `uepi_edit_discover` again. Plan expiry, changed catalog/session/project, dirty or read-only targets, changed before fingerprints, or disabled domain/save settings require a new Preview after the underlying condition is fixed.
