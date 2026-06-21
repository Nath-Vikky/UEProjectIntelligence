# UEPI Operations Runbook

## Health

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 health
```

## Integrity And Recovery

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 integrity

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 recover
```

`recover` checkpoints WAL, runs SQLite optimize, and returns integrity status.

## Security Audit

```powershell
python Plugins\UEProjectIntelligence\Tools\security_fuzz_audit.py
```

## Performance Baseline

```powershell
python Plugins\UEProjectIntelligence\Tools\benchmark_daemon.py `
  --entities 2000 `
  --report Saved\UEProjectIntelligence\performance_baseline.json
```

## Release Package

```powershell
python Plugins\UEProjectIntelligence\Tools\package_release.py
```

The generated manifest includes install, upgrade, and uninstall plans.
