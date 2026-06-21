# UEPI Migration Guide

## Database

The current SQLite schema uses additive migration helpers for new columns. Before upgrading:

1. Stop the daemon.
2. Back up `Saved/UEProjectIntelligence/*.sqlite3*`.
3. Replace the plugin.
4. Run `integrity`.
5. Re-ingest the latest scan if integrity or expected query fields are missing.

## Plugin

For source upgrades, replace `Plugins/UEProjectIntelligence`, regenerate project files if needed, and rebuild the editor target.

## Scan Artifacts

Scan JSON artifacts are immutable evidence. Keep old scans when comparing revisions or validating migrations. If a schema changes intentionally, update golden summaries and record the change in `CHANGELOG.md`.

## Rollback

1. Stop editor and daemon.
2. Restore the previous plugin directory.
3. Restore the previous SQLite files if the newer daemon ingested data with incompatible schema changes.
4. Rebuild the editor target.
