# Write Safety

UEPI write support must never bypass these rules.

## Non-Negotiable Rules

- Read before mutate.
- Describe before mutate.
- Preview before apply.
- Authorization policy decision before apply.
- Use GUIDs and pin names returned by UEPI, never guessed display text.
- Wrap editor changes in a scoped transaction.
- Write backup artifacts before touching assets.
- Compile or validate touched assets.
- Run targeted Snapshot refresh after apply.
- Return `uepi_diff` evidence after apply.
- Keep rollback available for every applied transaction.

## Always-On Safety Gates

Project settings are Agent-ready by default, while the non-negotiable gates remain on:

```text
bEnableLiveEditorBridge = true
bEnableWriteTools = true
bRequirePreviewBeforeApply = true
bRequireSnapshotDiffAfterApply = true
bAllowSavingPackages = true
```

Saving is touched-only. UEPI resolves and backs up every target package before mutation, validates it, saves only packages changed by the approved plan, records hashes, and can restore saved files plus reload their packages. Dirty targets and read-only package files block preflight.

The Agent-ready default atomic budget is 96 operations and 12 affected assets. Projects may configure up to 256 operations and 64 assets, but UEPI does not treat a higher cap as weaker safety: Preview blocks over-budget plans before approval, larger accepted plans are marked high-risk, and every target still participates in preflight, backup, validation, touched-only save, refresh, diff, and rollback.

If execution or validation fails after mutation begins, UEPI cancels the Editor transaction, restores every target from the transaction backup, unloads newly created packages, refreshes the affected paths, and returns explicit `compensation_attempted`, `compensation_ok`, and `atomicity_restored` evidence. A compensated failed transaction never replaces the last successful transaction available to explicit rollback.

## Authorization Modes

- `ReviewEachPlan`: default. One explicit approval is required for the immutable Preview.
- `TrustedSession`: an in-policy plan may apply automatically only in the exact current Editor session.
- `TrustedProject`: an in-policy plan may apply automatically when `TrustedProjectBindingId` exactly matches the MCP-bound project.

Trusted modes do not bypass Preview, preflight, project/session guards, risk and scope checks, backup, validation, touched-only save, refresh, diff, or reporting. A plan outside the configured roots, domains, risk ceiling, destructive-operation policy, or asset budget is rejected with `UEPI_EDIT_TRUST_POLICY_REJECTED`; explicit approval cannot override that trusted policy decision.

Recommended project configuration:

```text
WriteAuthorizationMode = TrustedProject
TrustedProjectBindingId = <uepi_status.project.project_binding_id>
AllowedAssetRoots = /Game/LLMNPC, /Game/ThirdPerson, /LLMNPCActionLayer
AllowedOperationDomains = blueprint, animgraph, animation, asset, content, config
MaximumRiskLevel = High
AllowAssetDelete = false
AllowAssetRename = true
AllowRuntimeControl = true
MaximumAssetsPerTransaction = 12
AlwaysCreateBackup = true
AlwaysReportAfterApply = true
```

In `ReviewEachPlan`, approval is required exactly once and is not approval to write generally. In trusted modes, the Agent should invoke Apply immediately after a successful unchanged Preview. In all modes it finishes validation, save, refresh, diff, runtime verification, and reporting without extra phase confirmations.

If Status reports `recovery_required`, mutation remains disabled. Call `uepi_recovery_inspect` and compare normalized current/backup fingerprints. Use `uepi_recovery_finalize` only when every current package matches its backup. Rollback restores old bytes and may overwrite later valid work. For an obsolete marker, ask the user to confirm retaining the inspected current bytes, then call `uepi_recovery_discard` with the returned confirmation token; both service and Editor recheck every fingerprint before clearing the marker.

## Rejected Commands

These capabilities must not be exposed directly as MCP tools:

```text
create_blueprint
add_node
connect_pin
save_asset
run_python
run_console_command
delete_asset
```

They may exist internally only behind operation plans, the Editor-exported registry, authorization policy, validation, and audit logging. Controlled PIE actions are additionally bound to a Runtime Preview or Apply-issued ticket and the configured verification mode.
