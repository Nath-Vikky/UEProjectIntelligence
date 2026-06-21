# UEPI v1.0 Support Matrix

Baseline:

- Unreal Engine: UE 5.3.x Win64.
- Project content: `/Game` and project plugin content.
- Engine content: reference tracking by default, deep scans only by explicit scope.
- Runtime mutation: out of scope for v1.0.

Supported surfaces:

- CLI: commandlet scan, daemon ingest/query/export/report/security tools.
- HTTP: localhost query API and Web UI.
- MCP: stdio adapter for query, report, artifact, integrity, and security workflows.
- Editor: dashboard tab, content browser entry, metadata scan, incremental event log.

Coverage:

- Blueprint source graph and projected graph relations.
- UObject reflection snapshots.
- World/Actor/component static structure.
- DataTable, UDS, enum, curves, CurveLinearColorAtlas.
- Skeleton, skeletal mesh, animation sequence, blend space, pose asset, IK, retargeter, physics asset, AnimBP, Control Rig.
- Enhanced Input, CommonUI, WidgetBlueprint.
- AI, StateTree, Gameplay Ability System.
- Material, render assets, Niagara, PCG, MetaSound, Audio, LevelSequence.
- Config, project/plugin descriptors, source file summaries, and asset references.

Known v1.0 limits:

- No automatic asset edits or refactors.
- No default Blueprint compile.
- No runtime gameplay execution.
- AnimBP dynamic final pose remains context-dependent and must report missing runtime context.
- Optional Parquet and official MCP SDK packages are host-installed extras.
