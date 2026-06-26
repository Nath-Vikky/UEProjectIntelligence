# Troubleshooting

## Codex Does Not Show UEPI Tools

- Confirm the MCP command points to `Services/uepi/src/uepi/mcp_server.py`.
- Use `--project <path-to-uproject>` or `--store <Saved/UEProjectIntelligence>`.
- Restart the Codex conversation after changing MCP configuration.

## `uepi_status` Reports Missing Snapshot

Generate a saved snapshot first from the UE dashboard or the `UEPIIndex` commandlet. The required file is:

```text
Saved/UEProjectIntelligence/store/manifests/saved.json
```

## Blueprint Or Animation Details Are Missing

The current Snapshot may be L0 metadata only. Regenerate with a targeted L2 commandlet scan for the asset you need, then call `uepi_blueprint` or `uepi_animation` again.

## Editor Is Closed

This is supported. The MCP server reads the last saved Snapshot and reports `editor_connected: false`.
