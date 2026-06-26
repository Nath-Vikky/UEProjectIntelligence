# Operations Runbook

## Build

```powershell
& "<UE_ROOT>\Engine\Build\BatchFiles\Build.bat" `
  <PROJECT_NAME>Editor Win64 Development `
  "-Project=<PROJECT_ROOT>\<PROJECT_NAME>.uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

## Generate Snapshot

```powershell
& "<UE_ROOT>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "<PROJECT_ROOT>\<PROJECT_NAME>.uproject" `
  -run=UEPIIndex -UEPILevel=L2 -unattended -nop4 -nosplash -NullRHI
```

## Validate MCP

```powershell
python -m compileall "<PROJECT_ROOT>\Plugins\UEProjectIntelligence\Services\uepi\src"
python "<PROJECT_ROOT>\Plugins\UEProjectIntelligence\Tools\test_snapshot_mcp_v2.py"
```

## Inspect Snapshot

```powershell
$env:PYTHONPATH="<PROJECT_ROOT>\Plugins\UEProjectIntelligence\Services\uepi\src"
python -m uepi status --project "<PROJECT_ROOT>\<PROJECT_NAME>.uproject"
```
