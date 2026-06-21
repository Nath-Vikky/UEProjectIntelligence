# UEPI Troubleshooting

## Daemon Says Scan Is Outside Sandbox

Run the daemon from the project root or place scan artifacts under `Saved/UEProjectIntelligence`. The sandbox allows the workspace, plugin root, and UEPI data directory.

## Web UI Cannot Connect

- Confirm the daemon is running.
- Check the API base field, usually `http://127.0.0.1:8765/v1`.
- If token auth is enabled, paste the token into the Web UI token field.

## Integrity Fails

Run:

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 recover
```

If integrity still fails, keep the database for inspection and re-ingest the latest scan into a fresh database.

## Dirty Package Diagnostic Appears

Stop using the generated scan as a golden reference until the dirty source is understood. Run `dirty_package_regression.py` against stored scans and compare the scan target with recent editor operations.

## Optional Dependencies Missing

The daemon runs without third-party packages. Optional features:

- `pyarrow` for real Parquet export.
- `mcp` from `requirements-mcp.txt` for official MCP SDK host experiments.
