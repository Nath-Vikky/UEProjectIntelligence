# UEPI Real Machine Test: LLMNPCDemo Golden

## Build

- UE version: `5.3.2-29314046+++UE5+Release-5.3`.
- Project: `LLMNPCDemo` with project plugin `LLMNPCActionLayer`.
- Configuration: `LLMNPCDemoEditor Win64 Development`.
- UEPI baseline: post-`4a3013c` vNext source plus the runtime/schema hardening recorded by this test.

## Scope

- Exact-read `/Game/LLMNPC/Animation/ABP_Manny1.ABP_Manny1` and confirm `DefaultGroup.DefaultSlot`.
- Read authoritative `ULLMNPCMotionTemplate` and runtime function schemas.
- Create, configure, validate, save, refresh, and diff one custom MotionTemplate DataAsset after one approval.
- Start UEPI-owned PIE, invoke `SubmitPublishedTemplate`, prove Waving starts and completes, capture the viewport, inspect only new log output, and stop PIE.
- Restart the Editor and re-read every persisted Golden property.

## Transaction

- Transaction: `uepi-tx:80ac148465a8418eb497137c39442393`.
- Plan hash: `sha256:19bdde64280bc3e173ad686efc51eab562de8870a77e5e1029425d7fd860f609`.
- Asset: `/Game/LLMNPCActionLayer/MotionTemplates/MT_Wave_Asset_Manny_v1.MT_Wave_Asset_Manny_v1`.
- Runtime ticket: `uepi-runtime-ticket:16fddf3f1be741b6b6cdf4fa27a09afd`.
- Saved file MD5: `19c7585df781cb570624b9d11e6db25b`.
- Apply result: applied, typed validation, touched-only save, targeted refresh, and transaction diff all succeeded.

## Read Evidence

- `ABP_Manny1` exact read returned the existing `AnimGraphNode_Slot` with node GUID `A8BF30A3-4EDE-3A28-57A3-7395EE93926E` and `DefaultGroup.DefaultSlot`; no AnimGraph edit was needed.
- The reflected MotionTemplate schema correctly exposed `Edit + BlueprintVisible + BlueprintReadOnly` properties as Editor-writable. `BlueprintReadOnly` no longer incorrectly blocks guarded Editor reflection writes.
- `SubmitPublishedTemplate` and `GetDebugState` returned real parameter/return schemas and were enabled only by exact project allowlist keys.
- Waving reconstruction evidence contained 143 source keys and 30 driver curves.

## Runtime Evidence

- PIE reached `running` on `/Game/LLMNPC/Maps/UEDPIE_0_M_LLMNPC_Test`.
- The exact `ULLMNPCMotionComponent` selector resolved one component.
- `SubmitPublishedTemplate(gesture.wave.asset.manny.v1, default modifiers)` returned `true`.
- `GetDebugState` observed `bAnimationAssetPlaying=true`, then `false`; terminal `AnimationPlaybackState` was `Completed` and `LastAnimationError` was empty.
- The post-cursor log query returned zero new Fatal, Ensure, Blueprint Runtime Error, or `LogLLMNPCActionLayer: Error` lines.
- PIE stopped under UEPI ownership.
- Viewport artifact: `Saved/UEProjectIntelligence/artifacts/screenshots/viewport-20260712T070215Z.png`, 1200x724, 1,138,257 bytes.

## Restart Evidence

- New Editor session: `944485AE-4483-CE6E-324D-4D9BAE706B57`.
- Reopened asset preserved `Kind=AnimationAsset`, published metadata, `full_body`, `ue5_manny.v1`, the Waving soft-object path, `DefaultSlot`, blend values, five-second maximum duration, provenance, and validation report.
- Forty consecutive exact-session Status samples produced zero failures.
- Fresh startup log contained no PrimaryAssetId mismatch, Fatal, Ensure, or Blueprint Runtime Error.

## Defects Found And Fixed

- `BlueprintReadOnly` was incorrectly treated as an Editor reflection write prohibition. Property Schema and typed writes now use `CPF_Edit` while retaining the real flag list for evidence.
- Runtime invocation was parameterless-only. It now exposes authoritative function Schema, decodes typed arguments, returns typed outputs, resolves an approved exact target selector, and binds target/function/arguments into the Runtime Ticket.
- Direct heartbeat JSON replacement could briefly expose an empty file and create a false session mismatch. Bridge JSON writes are now atomic and Python session reads retry transient decode failures.
- DataAsset creation registered a temporary PrimaryAssetId before same-plan property writes. New DataAssets are now registered only after all operations succeed and final identity fields are set.

## Result

- [x] Pass
- [ ] Fail

The local project asset, transaction files, logs, and screenshots are test evidence and are not included in release archives.
