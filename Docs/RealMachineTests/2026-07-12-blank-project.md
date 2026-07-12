# UEPI Real Machine Test: Clean Blank Project

## Build

- UE version: `5.3.2-29314046+++UE5+Release-5.3`.
- Project: clean Blueprint-only `UEPIBlank` project.
- Initial install: `UEProjectIntelligence-UE5.3.2-Win64-v2.0.0-alpha.4.zip`.
- Rollback fixes under test: `110b055` and `48ea0df`.

## Install And Snapshot

- The prebuilt archive extracted under `Project/Plugins/UEProjectIntelligence` and the Editor started without project modules or a compile prompt.
- A project-local `.codex/config.toml` targeted the exact project, server, and `codex` profile.
- The first online Doctor reached the exact Editor session and 64-operation catalog; only the expected missing Saved Snapshot check failed.
- `UEPIIndex -unattended -nop4 -nosplash -NullRHI` completed with 0 errors and wrote saved generation 1.
- With the Editor closed, Doctor had 0 failures and Status/Overview queried the Saved Snapshot without a daemon.

## Golden Transaction

The immutable plan affected only:

```text
/Game/UEPI/DA_UEPIBlankRollbackProbe.DA_UEPIBlankRollbackProbe
```

Operations:

```text
content.create_asset class=/Script/Engine.PrimaryAssetLabel
asset.set_properties bIsRuntimeLabel=true
```

## Actual

- Apply, typed DataAsset validation, touched-only save, targeted refresh, and transaction diff passed.
- The property diff reported `bIsRuntimeLabel: false -> true`.
- The saved package was 1,568 bytes and exact readback identified a `PrimaryAssetLabel` DataAsset.
- Current-session rollback returned `undone=true` and `files_restored=true`.
- The package file was deleted and the transaction journal phase became `rolled_back`.
- Rollback targeted refresh completed successfully.
- Exact current-view read returned `UEPI_ASSET_TOMBSTONED` with reason `asset_removed` and no diagnostics.

## Defects Found And Fixed

- New assets have an intentionally empty backup path. Rollback now unloads those packages instead of trying to reload a deleted file (`110b055`).
- New assets notify Asset Registry deletion before package unload so targeted refresh deterministically emits a tombstone (`48ea0df`).
- Python rollback waits for and reports the post-rollback refresh result (`110b055`).

## Result

- [x] Pass
- [ ] Fail

The generated test project and its Saved evidence are local test artifacts and are not included in release archives.
