# Real-Machine Test Log

## 2026-07-01

Environment:

```text
Unreal Engine 5.3.2
Project: GasDemo
Client: Codex
Profile: codex
```

Validated:

```text
Codex can discover UEPI MCP tools.
uepi_status reads Snapshot state.
Saved Snapshot can be queried with the editor closed.
Live editor session can service targeted refresh requests when the editor/plugin is open.
BP_ThirdPersonGameMode Blueprint nodes can be queried after Snapshot scan.
BP_ThirdPersonGameMode saved changes are detected as fresher incremental events before a new Snapshot scan.
MM_Run_Fwd animation summary and changing-bone motion evidence can be queried.
```

Current automated regression:

```powershell
python -B -m compileall Plugins\UEProjectIntelligence\Services\uepi\src\uepi
python -B Plugins\UEProjectIntelligence\Tools\test_snapshot_mcp_v2.py
```

Expected result:

```text
snapshot MCP v2 assertions ok
```

Notes:

```text
The default codex profile exposes read and guarded edit tools together so the Agent can choose the workflow without profile switching.
edit_apply requires the live bridge, preview, and user approval; the current alpha executor covers Blueprint variables, components, custom events, function graphs, common graph nodes, pin connections, Actor spawn/transform/property edits, Material Instance create/parameter/apply edits, scoped /Game Content operations, basic UMG Widget Blueprint edits, and Enhanced Input asset/key-mapping edits when EnhancedInput is enabled.
```
