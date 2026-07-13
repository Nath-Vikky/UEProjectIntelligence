# UEPI Multi-Asset Atomic Transaction Golden

Date: 2026-07-13

## Environment

- Unreal Engine: 5.3.2, Win64 Development Editor.
- Project: clean `UEPIBlank` test project.
- Editor Operation Registry: 64 operations.
- Guarded transaction limits: 96 operations and 12 affected assets.
- Online Doctor: 17/17 checks passed before and after restart.

## Four-Asset Success And Rollback

- Immutable transaction: `uepi-tx:003e48cbb52749f284fa22c626f2caba`.
- Plan hash: `sha256:beb951fe9ced4ebc7077f0a431061e4cd35cd4106232762b2296dbbff18d8347`.
- Scope: eight operations over four new `PrimaryAssetLabel` assets under `/Game/UEPI/AtomicBudget`.
- Apply created all four assets, set `bIsRuntimeLabel=True`, validated, saved only the four touched packages, completed targeted refresh, and produced a four-asset semantic transaction diff.
- Exact current reads returned all four assets with the expected class and property value.
- Explicit rollback returned `undone=true` and `files_restored=true`, removed all four package files, completed targeted refresh, and wrote a `rolled_back` journal.
- After a normal Editor restart, all four exact reads returned `UEPI_ASSET_TOMBSTONED` and no package files existed.

## Injected Failure Compensation

- Immutable transaction: `uepi-tx:376d9eafff9a4396a22da971e89fd7d9`.
- Plan hash: `sha256:a0a07bb59c174f78f69e5c7234742cf04a52975c217e38588a033e84b421902e`.
- Scope: eight operations over four new assets under `/Game/UEPI/AtomicFailure`.
- The non-Shipping test switch injected a failure after operation four, after two assets had been created and configured but before the remaining two creates.
- The transaction journal entered `failed_rolled_back` with `saved=false`; every target participated in backup/restore and all four package files were absent.
- Online exact reads returned tombstones for the two touched targets and not-found for the two operations that never ran. The same result survived a normal Editor restart.
- MCP error envelopes now retain non-empty structured failure results so `compensation_attempted`, `compensation_ok`, `atomicity_restored`, and `journal_phase` remain Agent-visible. The synthetic MCP v2 regression covers this contract.

The generated test assets, transaction plans, journals, Snapshot data, and Editor logs are local evidence and are not included in release archives.
