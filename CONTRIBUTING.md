# Contributing

UE Project Intelligence is currently converging toward the `2.0-dev` architecture in `DOCX/Improvement.md`.

## Development Rules

- Keep the public product path read-only.
- Do not add write-asset, shell, arbitrary-code, Blueprint compile, or package-save MCP tools.
- Prefer Snapshot Store and stdio MCP flows over daemon, worker, HTTP, queue, or Web UI flows.
- Keep generated data under `Saved/UEProjectIntelligence`.
- Preserve `v1.0.0` as the legacy stable reference point.

## Validation

Before submitting changes, run the narrowest useful checks for the affected layer:

```powershell
python -m compileall Plugins\UEProjectIntelligence\Services\uepi\src
python Plugins\UEProjectIntelligence\Tools\test_snapshot_mcp_v2.py
```

For C++ changes, close Unreal Editor and build the editor target:

```powershell
& "<UE_ROOT>\Engine\Build\BatchFiles\Build.bat" `
  <PROJECT_NAME>Editor Win64 Development `
  "-Project=<PROJECT_ROOT>\<PROJECT_NAME>.uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

## Commit Style

Use small staged commits that match the migration phases:

- public boundary and docs;
- model and schema;
- snapshot storage;
- commandlet collection;
- editor live/saved state;
- Python query core;
- MCP tool surface;
- cleanup and release.
