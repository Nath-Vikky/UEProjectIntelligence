# UEPI Real Machine Test: GasDemo Third Person Golden

## Build

- Commit under test: `858371b` plus the `2.0.0-alpha.4` release metadata commit.
- UE version: `5.3.2-29314046+++UE5+Release-5.3`.
- Project: `GasDemo`, Third Person template content.
- Configuration: `GasDemoEditor Win64 Development`.

## Settings

- Guarded write, Blueprint/content edits, package save, PIE control, and runtime invoke enabled.
- Online Doctor: 17 passed, 0 warnings, 0 failed.
- Operation Catalog: 64 operations; `content.duplicate_asset` writes `destination_asset` and fingerprints read-only `source` as a dependency.

## Codex Prompt

Execute the approved Golden transaction once: duplicate `BP_ThirdPersonGameMode`, add an integer variable, add a custom smoke-test event and on-screen Print String, connect their real execution pins, compile, save, refresh, diff, and run controlled PIE verification.

## Tool Calls

```text
uepi_status
uepi_edit_discover
uepi_schema(blueprint_node)
uepi_edit_preview
uepi_edit_apply
uepi_edit_validate
uepi_diff(transaction)
uepi_runtime(start/status/stop/status)
uepi_blueprint(exact, refresh=never)
```

## Expected

- One immutable Preview and one user approval.
- Source Blueprint remains byte-identical.
- Only the duplicated Golden package is modified and saved.
- Custom event `then` connects to Print String `execute` using returned real pins.
- Blueprint validation succeeds with no compile warnings or errors.
- Transaction diff reports two added nodes, one new link, and the created Golden asset.
- UEPI-owned PIE starts on `ThirdPersonMap` and stops cleanly.
- Editor restart preserves the saved package and offline Snapshot readback.
- Old session-bound runtime tickets cannot be replayed after restart.

## Actual

- Apply and independent Validate both returned success; six operations completed.
- Source SHA-256 remained `81282d51d1af1c5a9348747d04ab760282b6abea5c14f2b3ce2d82dc747f8cae`.
- Golden package SHA-256 after save and after Editor restart was `769b8d1d69750c4c7b341eb749cf84df5a448bfb351f2762c56aa4a0987a92b3`.
- Compile status was up to date with zero errors and warnings.
- PIE reached `running` on `/Game/ThirdPerson/Maps/UEDPIE_0_ThirdPersonMap`, then returned to `stopped`.
- Exact offline readback returned 89 Blueprint entities and 123 structural relations, including `UEPI_GoldenSmokeTest`, the Print String message, and their pins/links.
- A post-restart start attempt using the old runtime ticket was rejected with `UEPI_EDITOR_SESSION_MISMATCH`.
- During the run, transaction scope defects were found and fixed: duplicate sources are now read-only dependencies, and duplicate/fingerprint creation is included in semantic diff.

## Artifacts

- Plan: `Saved/UEProjectIntelligence/store/edit/uepi-tx-60e55b664b884b7fbd5587d55e121b58.plan.json`.
- Result: `Saved/UEProjectIntelligence/store/edit/uepi-tx-60e55b664b884b7fbd5587d55e121b58.result.json`.
- Golden asset: `/Game/UEPI/Golden/BP_UEPI_ThirdPersonGolden.BP_UEPI_ThirdPersonGolden`.
- Runtime session: `uepi-runtime:A9B2AD424F52A35D1F298F907F4B0F26`.

The `Saved` paths are local evidence references and are not included in release archives.

## Result

- [x] Pass
- [ ] Fail

## Follow-up

- Blueprint member variables are validated by Apply/compile but are not yet emitted as first-class entities by the Blueprint Snapshot reader.
- Saved rollback remains intentionally limited to the most recent UEPI transaction in the same Editor session; historical rollback after Editor restart is not promised by the alpha contract.
- Run the remaining LLMNPCDemo, blank-project, and two-project Beta matrix before changing the release label from Alpha.
