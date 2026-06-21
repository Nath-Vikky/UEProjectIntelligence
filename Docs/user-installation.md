# UEPI User Installation

## Requirements

- Unreal Engine 5.3.x on Win64.
- Project plugin location: `Plugins/UEProjectIntelligence`.
- Python 3.11+ for daemon, CLI, MCP, Web UI serving, validation, and packaging tools.

## Install From Source

1. Copy `UEProjectIntelligence` into the project `Plugins` directory.
2. Regenerate project files if your workflow requires it.
3. Build `GasDemoEditor Win64 Development`.
4. Enable the plugin in the editor if it is not already enabled.
5. Open `Tools > UE Project Intelligence`.

## First Scan

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\last_scan.json"
```

## Local Daemon And Web UI

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 `
  ingest --scan Saved\UEProjectIntelligence\last_scan.json

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 `
  serve --host 127.0.0.1 --port 8765
```

Open `http://127.0.0.1:8765/v1/ui`.

## Optional Token

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 `
  serve --host 127.0.0.1 --port 8765 --token auto
```

`--token auto` writes `uepi_http_token.txt` next to the database. Use that value in the Web UI token field or as `Authorization: Bearer <token>`.
