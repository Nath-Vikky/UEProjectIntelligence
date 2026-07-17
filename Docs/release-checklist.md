# Release Checklist

## Repository

- [x] `LICENSE` exists.
- [x] `README.md` has no local absolute paths.
- [x] `Docs/user-guide.md` has no local absolute paths.
- [x] `Resources/codex-config.template.toml` uses placeholders.
- [x] `UEProjectIntelligence.uplugin` `VersionName` is updated.
- [x] `CHANGELOG.md` is updated.

## Python

- [x] `python -B -m compileall Services/uepi/src/uepi Tools/test_snapshot_mcp_v2.py` passes.
- [x] `python -B Tools/test_snapshot_mcp_v2.py` passes.
- [x] Cache can be deleted and rebuilt with `python -m uepi sync`.

## Unreal

- [x] UE5.3.2 blank project builds.
- [x] UE5.3.2 Third Person project builds.
- [x] Plugin enables successfully without optional plugins.
- [x] `Run Snapshot Scan` writes `saved.json`.
- [x] `UEPIIndex` commandlet writes Snapshot data.

## Codex

- [x] `.codex/config.toml` template works.
- [x] Codex shows UEPI MCP.
- [x] `uepi_status` works.
- [x] Unified `--tool-profile codex` lists fifteen read/live tools plus five guarded edit tools.
- [x] `Tools/uepi_doctor.py --require-editor` passes for the selected project.
- [x] `Tools/test_live_read_contract.py --project <UPROJECT>` passes against the rebuilt matching Editor.
- [x] Example question set in `Docs/codex-example-questions.md` passes.

## Snapshot Correctness

- [x] Live overlay works.
- [x] Package save promotes saved Snapshot.
- [x] Rename old path is tombstoned.
- [x] Delete path is tombstoned.
- [x] Offline saved query works.

## Package

- [x] `python Tools/package_release.py --version <VERSION> --kind source --out Dist` generates the Source zip, manifest, and SHA256SUMS.
- [x] A `--kind prebuilt` archive is produced only from UE5.3.2 Win64 binaries.
- [x] Zip extracts to `UEProjectIntelligence/`.
- [x] Source zip excludes `Binaries`; both archives exclude `Saved`, `Intermediate`, generated cache files, and `__pycache__`.
- [x] Zip install tested in a clean project.

## Real Machine Beta Gate

- [x] LLMNPCDemo Golden passes and is recorded.
- [x] Third Person Golden passes and is recorded.
- [x] Minimal blank Blueprint project passes and is recorded.
- [x] Two-project routing and mismatch rejection pass.
- [x] Editor restart confirms touched assets persisted.
- [x] Owned PIE cleanup and rollback failure paths pass.
