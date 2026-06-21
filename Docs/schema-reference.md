# UEPI Schema Reference

Schema files live in `Plugins/UEProjectIntelligence/Schemas`.

Core schemas:

- `entity.schema.json`: stable entity identity, kind, canonical key, source layer, attributes, snapshots, completeness, diagnostics, and evidence.
- `relation.schema.json`: typed directed edges, endpoints, confidence, derived flag, attributes, and evidence.
- `diagnostic.schema.json`: severity, code, message, and context.
- `asset-scan.schema.json`: top-level scan envelope.

Domain schemas:

- Blueprint: `blueprint-graph.schema.json`
- Animation: `animation.schema.json`
- Data: `data.schema.json`
- World: `world.schema.json`
- UI/CommonUI: `ui.schema.json`, `common_ui.schema.json`
- AI/StateTree/GAS/Input: `ai.schema.json`, `state_tree.schema.json`, `gas.schema.json`, `input.schema.json`
- Render/Material/Niagara/PCG/Audio/MetaSound/Cinematics: corresponding schema files.

Compatibility rules:

- Additive fields are preferred.
- Stable IDs and canonical keys must not be display-name dependent.
- Unknown fields must be tolerated by readers unless a schema explicitly rejects them.
- Every partial extraction must use completeness metadata instead of silently omitting unsupported details.
