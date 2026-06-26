# Operations Runbook

## Build

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Build\BatchFiles\Build.bat" GasDemoEditor Win64 Development `
  "-Project=F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" -WaitMutex -NoHotReload
```

## Generate Snapshot

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -UEPILevel=L2 -unattended -nop4 -nosplash
```

## Validate MCP

```powershell
python -m compileall Plugins\UEProjectIntelligence\Services\uepi\src
python Plugins\UEProjectIntelligence\Tools\test_snapshot_mcp_v2.py
```

## Inspect Snapshot

```powershell
$env:PYTHONPATH="F:\Epic Games\UE5project\GasDemo\Plugins\UEProjectIntelligence\Services\uepi\src"
python -m uepi status --project "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject"
```
