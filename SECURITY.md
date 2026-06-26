# Security Policy

UE Project Intelligence is designed as a local, read-only Unreal project understanding tool.

## Supported Line

- `main`: active development line for the snapshot-first read-only MCP architecture.
- `v1.0.0`: legacy stable tag for the daemon-compatible read-only MCP workflow.

## Security Boundary

UEPI must not expose tools that:

- execute shell commands or arbitrary code;
- write, save, delete, rename, move, or create Unreal assets;
- compile Blueprints;
- modify levels, actors, animations, materials, data assets, source, or config;
- accept arbitrary filesystem paths from MCP clients.

Allowed writes are limited to UEPI-owned observations and artifacts under `Saved/UEProjectIntelligence`.

## Reporting

Open a private report with reproduction steps, affected commit or tag, and the smallest project or scan artifact needed to reproduce. Do not attach proprietary project content unless it is required and safe to share.

