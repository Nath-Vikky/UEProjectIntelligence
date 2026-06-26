# Release Checklist

## Required Checks

- `python -B -m compileall Services/uepi/src/uepi Tools/test_snapshot_mcp_v2.py`
- `python -B Tools/test_snapshot_mcp_v2.py`
- `GasDemoEditor Win64 Development` builds with UnrealBuildTool.
- `uepi_status` returns the expected Snapshot generation and cache state.
- Codex can list exactly the ten public UEPI tools.

## Manual Smoke

- Generate a saved Snapshot from the editor panel.
- Query a Blueprint with `uepi_blueprint`.
- Query an animation with `uepi_animation`.
- Rename or remove a test asset and confirm old paths return tombstone data.
- Save or update a test asset, call the relevant read tool, and confirm `UEPI_REFRESH_REQUESTED` appears when the editor is active.

## Packaging

- Include `README.md`, `Docs/user-guide.md`, `Docs/troubleshooting.md`, `Resources/codex-config.template.toml`, `LICENSE`, `SECURITY.md`, and `THIRD_PARTY_NOTICES.md`.
- Exclude generated `Intermediate`, `Binaries`, `Saved`, `DerivedDataCache`, and Python `__pycache__`.
