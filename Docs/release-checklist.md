# Release Checklist

## Repository

- [ ] `LICENSE` exists.
- [ ] `README.md` has no local absolute paths.
- [ ] `Docs/user-guide.md` has no local absolute paths.
- [ ] `Resources/codex-config.template.toml` uses placeholders.
- [ ] `UEProjectIntelligence.uplugin` `VersionName` is updated.
- [ ] `CHANGELOG.md` is updated.

## Python

- [ ] `python -B -m compileall Services/uepi/src/uepi Tools/test_snapshot_mcp_v2.py` passes.
- [ ] `python -B Tools/test_snapshot_mcp_v2.py` passes.
- [ ] Cache can be deleted and rebuilt with `python -m uepi sync`.

## Unreal

- [ ] UE5.3.2 blank project builds.
- [ ] UE5.3.2 Third Person project builds.
- [ ] Plugin enables successfully without optional plugins.
- [ ] `Run Snapshot Scan` writes `saved.json`.
- [ ] `UEPIIndex` commandlet writes Snapshot data.

## Codex

- [ ] `.codex/config.toml` template works.
- [ ] Codex shows UEPI MCP.
- [ ] `uepi_status` works.
- [ ] Unified `--tool-profile codex` lists ten read tools plus five guarded edit tools.
- [ ] Example question set in `Docs/codex-example-questions.md` passes.

## Snapshot Correctness

- [ ] Live overlay works.
- [ ] Package save promotes saved Snapshot.
- [ ] Rename old path is tombstoned.
- [ ] Delete path is tombstoned.
- [ ] Offline saved query works.

## Package

- [ ] `python Tools/package_release.py --version <VERSION> --out Dist` generates a zip.
- [ ] Zip extracts to `UEProjectIntelligence/`.
- [ ] Zip excludes `Saved`, `Intermediate`, `Binaries`, generated cache files, and `__pycache__`.
- [ ] Zip install tested in a clean project.
