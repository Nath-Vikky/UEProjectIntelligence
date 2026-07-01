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
The default codex profile remains read-only and exposes exactly ten tools.
The codex_write_alpha profile exposes five extra edit tools, but edit_apply rejects by default.
```
