# UEPI Daemon Skeleton

This is the first local service layer for UE Project Intelligence. It ingests scan JSON emitted by the `UEPIIndex` commandlet into SQLite and exposes minimal query commands.

## Commands

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 `
  ingest --scan Saved\UEProjectIntelligence\last_scan.json

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 summary

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 health

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 scans --limit 25

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 entities --kind blueprint_node --limit 100

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 relations --relation-type contains_node --limit 100

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 diff --limit 50

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 history BP_Hero

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 stale --limit 100

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 search BP_Hero

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 graph-query "from BP_Hero depth 2 relation contains_node limit 100"

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 export-dot BP_Hero --depth 2 `
  --output Saved\UEProjectIntelligence\bp_hero.dot

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 export-graph BP_Hero --format graphml --depth 2 `
  --output Saved\UEProjectIntelligence\bp_hero.graphml

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 artifact-range --offset 0 --length 4096

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 report --limit 25 `
  --output Saved\UEProjectIntelligence\scan_report.md

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 api-docs

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 protocol

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 worker-register --worker-id editor-a --worker-type editor

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 job-submit --type metadata_scan --request-json "{""asset"":""/Game/Foo.Foo""}"

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 job-poll --session-id <session> --session-token <token>

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 jobs --include-events

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 source-index --project GasDemo.uproject

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 source-search BlueprintCallable

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 config-values --section /Script/EngineSettings.GameMapsSettings --key GameDefaultMap --include-history

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 animation-query --asset MM_Walk_Fwd

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 data-query --asset AreaLights

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 data-page "/Game/Data/ExampleTable.ExampleTable" --collection rows --limit 50 --include-artifact

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 integrity

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 recover
```

The service uses only Python standard library modules by default. SQLite FTS5 is used when available and falls back to `LIKE` search otherwise. `scans`, `entities`, `relations`, `data-page`, and `graph-page` use cursor pagination and return `next_cursor` when more rows are available. `data-page` pages `rows`, `columns`, `entries`, `bundles`, or `parent_tables` from an ingested data snapshot without returning the full snapshot, and `--include-artifact` writes the full collection JSON under the database directory; `graph-page` pages `nodes` or `edges` from a bounded subgraph JSON result. Graph DSL queries use `from <entity> depth <n> relation <type[,type]> limit <n>`. `export-graph` supports JSON, DOT, Mermaid, GraphML, Cytoscape JSON, and optional real Parquet output when `pyarrow` is installed. `artifact-range` reads capped byte ranges from ingested scan JSON artifacts. `source-index` extracts UHT symbols, C++/Config Unreal path references, Build.cs/Target.cs markers, compile database discovery, and indexed `.ini` rows; `config-values` returns raw rows plus effective values with Unreal array operators preserved. Ingest records asset revision history for each `kind=asset` entity and optional Git commit metadata when the project is inside a Git worktree. The `diff` command compares two ingested scans, defaulting to the previous scan versus the latest scan, and includes Blueprint, DataTable, and Animation manifest summaries. The `stale` command verifies that the source scan artifact still exists and still hashes to the ingested scan id, then compares file-backed UE package fingerprints when the package can be resolved under `/Game` or project-plugin content. The worker protocol stores sessions, heartbeats, queued jobs, leases, retry/recovery state, trace events, and chunk uploads in SQLite without exposing package save, delete, rename, compile, shell, or arbitrary-code tools. `integrity` runs SQLite integrity and foreign-key checks. `recover` checkpoints WAL, runs SQLite optimize, and returns integrity status.

When `serve` is running, the local Web UI is available at `/v1/ui`. The page uses the same HTTP API for asset browsing, graph viewing, Blueprint flow filters, animation/data table tables, diff/stale/report panels, diagnostics/coverage, and asset path handoff.

Use `serve --token <value>`, `serve --token-file <path>`, or `serve --token auto` to require bearer or `X-UEPI-Token` auth for query endpoints. The daemon path sandbox allows scan and token reads only from the project/workspace, the plugin root, or the configured UEPI data directory.

## MCP Stdio

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_mcp_server.py `
  --db Saved\UEProjectIntelligence\index.sqlite3

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_mcp_server.py --sdk-status

python Plugins\UEProjectIntelligence\Tools\test_mcp_stdio.py

python Plugins\UEProjectIntelligence\Tools\test_worker_protocol.py

python Plugins\UEProjectIntelligence\Tools\test_data_page.py

python Plugins\UEProjectIntelligence\Tools\test_graph_page.py
```

The MCP adapter exposes tools, resources, resource templates, prompts, output schemas, structured content, token-budgeted results, and synchronous job envelopes over stdio. Large structured responses are written as JSON artifacts under the database directory and returned as `uepi://mcp-artifact/{artifact_id}` resources. The adapter does not provide shell execution, arbitrary code execution, editor launch, Blueprint compile, package save, rename, delete, or write-asset tools. `requirements-mcp.txt` records the optional official Python MCP SDK package for environments that want to install it; the compatibility server remains standard-library only.

## Audit

```powershell
python Plugins\UEProjectIntelligence\Tools\security_fuzz_audit.py

python Plugins\UEProjectIntelligence\Tools\benchmark_daemon.py

python Plugins\UEProjectIntelligence\Tools\dirty_package_regression.py

python Plugins\UEProjectIntelligence\Tools\package_release.py --dry-run
```
