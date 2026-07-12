# UEPI Migration Guide

## Snapshot Store

UEPI 2.0 Beta uses MCP envelope v2, Editor bridge protocol v2, Edit Plan v2, and the Editor-exported operation catalog. Before replacing the plugin, close the editor and back up:

```text
Saved/UEProjectIntelligence/store/
Saved/UEProjectIntelligence/cache/
```

The SQLite cache under `cache/` is rebuildable and may be deleted if it becomes incompatible.

Old v1 daemon/HTTP/worker configuration is not migrated. Remove those MCP entries and configure the project-local stdio server with `Tools/setup_codex.py`. The legacy `codex_write_alpha` profile remains an alias, but new configurations use `codex`.

## Plugin

For source upgrades, replace `Plugins/UEProjectIntelligence`, regenerate project files if needed, and rebuild the editor target.

## Rebuild Derived Cache

After upgrading, rebuild the MCP query cache:

```powershell
python -B -m uepi sync --project "__PROJECT_ROOT__\__PROJECT_NAME__.uproject"
```

Then run `uepi_status` from the MCP client and confirm the Snapshot generation and cache generation match.

Open the Editor once after upgrading so it publishes a v2 exact-project session and operation catalog. Plans created before the upgrade are intentionally invalid because their session/catalog hashes no longer match.

## Alpha.4 To Beta.1

- Replace the entire plugin directory; do not mix Alpha.4 binaries with Beta.1 source.
- Regenerate the Codex block with `Tools/setup_codex.py` if it still uses a daemon, `codex_write_alpha`, or a legacy database argument. The alias remains readable, but `codex` is the release profile.
- Runtime allowlists now use exact `ClassPath:FunctionName` entries. Parameterized calls are accepted only when the function Schema, project allowlist, approved target, typed arguments, and Runtime Ticket all agree.
- `BlueprintReadOnly` no longer blocks guarded Editor writes when the property is also `EditAnywhere`; this changes incorrect Alpha.4 reflection output.
- New PrimaryDataAssets are registered only after same-plan property writes finish, preventing temporary PrimaryAssetId registration.
- Old plans and runtime tickets must not be reused after upgrade or Editor restart.

## Scan Artifacts

Scan JSON artifacts and Snapshot fragments are immutable evidence. Keep old scans when comparing revisions or validating migrations. If a schema changes intentionally, update golden summaries and record the change in `CHANGELOG.md`.

## Rollback

1. Close the editor.
2. Restore the previous plugin directory.
3. Restore the backed-up `Saved/UEProjectIntelligence/store/` directory if needed.
4. Delete `Saved/UEProjectIntelligence/cache/` and rebuild it after the rollback.
5. Rebuild the editor target.
