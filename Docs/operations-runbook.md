# Operations Runbook

## Build

```powershell
& "__UE_ROOT__\Engine\Build\BatchFiles\Build.bat" `
  __PROJECT_NAME__Editor Win64 Development `
  "-Project=__PROJECT_ROOT__\__PROJECT_NAME__.uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

## Generate Snapshot

```powershell
& "__UE_ROOT__\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "__PROJECT_ROOT__\__PROJECT_NAME__.uproject" `
  -run=UEPIIndex -UEPILevel=L2 -unattended -nop4 -nosplash -NullRHI
```

## Validate MCP

```powershell
python -m compileall "__PROJECT_ROOT__\Plugins\UEProjectIntelligence\Services\uepi\src"
python "__PROJECT_ROOT__\Plugins\UEProjectIntelligence\Tools\test_snapshot_mcp_v2.py"
```

## Inspect Snapshot

```powershell
$env:PYTHONPATH="__PROJECT_ROOT__\Plugins\UEProjectIntelligence\Services\uepi\src"
python -m uepi status --project "__PROJECT_ROOT__\__PROJECT_NAME__.uproject"
```
