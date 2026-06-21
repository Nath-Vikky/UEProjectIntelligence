# UEPI Sample Queries

## CLI

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 entities --kind asset --limit 25

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 search blueprint --limit 20

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 graph-query "from /Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter depth 2 relation contains_graph,contains_node"

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\index.sqlite3 export-graph "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter" --format graphml --output Saved\UEProjectIntelligence\bp.graphml
```

## HTTP

```text
GET /v1/entities?kind=asset&limit=50
GET /v1/search?q=AnimBlueprint&limit=20
GET /v1/subgraph?entity=<id-or-key>&depth=2&relation_type=asset_reference
GET /v1/stale?limit=50
POST /v1/recover
```

## MCP

Use `uepi_summary`, `uepi_entities`, `uepi_related`, `uepi_subgraph`, `uepi_graph_query`, `uepi_report`, `uepi_integrity`, and `uepi_security_audit` for read-oriented workflows. Use `uepi_job_start` for large operations that should keep a retained result.
