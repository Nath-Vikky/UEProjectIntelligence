# UEPI Migration Guide

## Snapshot Store

UEPI 2.0-dev treats `Saved/UEProjectIntelligence/store` as the source of truth. Before replacing the plugin, close the editor and back up:

```text
Saved/UEProjectIntelligence/store/
Saved/UEProjectIntelligence/cache/
```

The SQLite cache under `cache/` is rebuildable and may be deleted if it becomes incompatible.

## Plugin

For source upgrades, replace `Plugins/UEProjectIntelligence`, regenerate project files if needed, and rebuild the editor target.

## Rebuild Derived Cache

After upgrading, rebuild the MCP query cache:

```powershell
python -B -m uepi sync --project "__PROJECT_ROOT__\__PROJECT_NAME__.uproject"
```

Then run `uepi_status` from the MCP client and confirm the Snapshot generation and cache generation match.

## Scan Artifacts

Scan JSON artifacts and Snapshot fragments are immutable evidence. Keep old scans when comparing revisions or validating migrations. If a schema changes intentionally, update golden summaries and record the change in `CHANGELOG.md`.

## Rollback

1. Close the editor.
2. Restore the previous plugin directory.
3. Restore the backed-up `Saved/UEProjectIntelligence/store/` directory if needed.
4. Delete `Saved/UEProjectIntelligence/cache/` and rebuild it after the rollback.
5. Rebuild the editor target.
