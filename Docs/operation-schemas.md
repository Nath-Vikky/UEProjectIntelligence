# Operation Schemas

This document records the stable envelope and experimental edit plan shapes.

## MCP Envelope

All tools return `content[0].text` and `structuredContent`.

`structuredContent` uses:

```json
{
  "schema_version": "uepi.mcp-envelope.v1",
  "ok": true,
  "tool": "uepi_blueprint",
  "operation": "graph_summary",
  "data_mode": "saved",
  "project": {},
  "snapshot": {},
  "state": {},
  "result": {},
  "evidence": [],
  "diagnostics": [],
  "next_actions": []
}
```

Legacy-compatible fields such as `state`, `omissions`, `truncation`, and `continuation` remain present.

## Diagnostic

```json
{
  "code": "UEPI_REFRESH_REQUESTED",
  "severity": "warning",
  "message": "A targeted refresh request was queued.",
  "recoverable": true,
  "recommended_user_action": "Keep the Unreal Editor open and retry.",
  "recommended_agent_action": {
    "tool": "uepi_status",
    "after_seconds": 2
  }
}
```

## Edit Plan

```json
{
  "schema_version": "uepi.edit_plan.v1",
  "schema_aliases": ["uepi.edit-plan.v1"],
  "transaction_id": "uepi-preview-...",
  "created_at_utc": "...",
  "status": "preview_only",
  "intent": "...",
  "operations": [],
  "evidence": [],
  "affected_assets": ["/Game/BP_Hero.BP_Hero"],
  "risk": {
    "level": "low",
    "requires_user_approval": true,
    "blocked_operation_count": 0
  },
  "safety": {
    "dry_run": true,
    "mutates_unreal_assets": false,
    "requires_editor_bridge": true,
    "allow_saving": false
  },
  "backup": {
    "required": true,
    "artifact_uri": "uepi://artifact/backups/uepi-preview-...",
    "manifest_path": "__PROJECT_ROOT__/Saved/UEProjectIntelligence/artifacts/backups/uepi-preview-.../manifest.json"
  },
  "requires_user_approval": true,
  "apply_supported": false,
  "validation_plan": [],
  "rollback_plan": {
    "strategy": "transaction_undo_or_backup_restore",
    "backup_artifact": "uepi://artifact/backups/uepi-preview-..."
  },
  "diagnostics": [],
  "rejection_reasons": []
}
```

## Blueprint Graph Write Operations

Graph write operations are carried inside the edit plan `operations` array and are applied only by `uepi_edit_apply` after preview and user approval.

Create a function graph:

```json
{
  "type": "blueprint.create_function",
  "params": {
    "asset": "/Game/Blueprints/BP_Test.BP_Test",
    "name": "UEPI_TestFunction"
  }
}
```

Add a PrintString node and return real pin evidence:

```json
{
  "type": "blueprint.add_print_string_node",
  "params": {
    "asset": "/Game/Blueprints/BP_Test.BP_Test",
    "graph": "EventGraph",
    "message": "Hello from UEPI",
    "position": {"x": 600, "y": 120}
  }
}
```

Connect pins using returned node GUIDs and pin names or pin IDs:

```json
{
  "type": "blueprint.connect_pins",
  "params": {
    "asset": "/Game/Blueprints/BP_Test.BP_Test",
    "graph": "EventGraph",
    "source": {"node_guid": "...", "pin_name": "then"},
    "target": {"node_guid": "...", "pin_name": "execute"}
  }
}
```

Successful node operations return a `detail.node` object with `node_guid`, `graph`, `class`, `title`, and a `pins` array containing `pin_id`, `name`, `direction`, `category`, `default_value`, and `linked_to`.

## Editor Content Write Operations

Spawn an actor in the editor world:

```json
{
  "type": "actor.spawn",
  "params": {
    "actor_class": "/Script/Engine.StaticMeshActor",
    "label": "UEPI_TestActor",
    "location": {"x": 0, "y": 0, "z": 120}
  }
}
```

Create a Material Instance:

```json
{
  "type": "material.create_instance",
  "params": {
    "parent": "/Game/Materials/M_Base.M_Base",
    "destination_path": "/Game/UEPIWriteSandbox",
    "name": "MI_UEPI_Test"
  }
}
```

Apply a material to an actor component:

```json
{
  "type": "material.apply_to_actor",
  "params": {
    "material": "/Game/UEPIWriteSandbox/MI_UEPI_Test.MI_UEPI_Test",
    "targets": {"paths": ["MapName:PersistentLevel.Actor_1"]},
    "component": "StaticMeshComponent0",
    "material_index": 0
  }
}
```

Content operations are scoped to `/Game` paths:

```json
{
  "type": "content.duplicate_asset",
  "params": {
    "source": "/Game/Blueprints/BP_Test.BP_Test",
    "destination_path": "/Game/UEPIWriteSandbox",
    "name": "BP_Test_Copy"
  }
}
```

Create a Widget Blueprint with a CanvasPanel root:

```json
{
  "type": "widget.create",
  "params": {
    "destination_path": "/Game/UEPIWriteSandbox",
    "name": "WBP_UEPI_Test"
  }
}
```

Add a button to a Widget Blueprint and set its CanvasPanel slot:

```json
{
  "type": "widget.add_button",
  "params": {
    "asset": "/Game/UEPIWriteSandbox/WBP_UEPI_Test.WBP_UEPI_Test",
    "name": "StartButton",
    "text": "Start",
    "position": {"x": 120, "y": 80},
    "size": {"x": 220, "y": 64},
    "z_order": 1
  }
}
```

Bind a Button's `OnClicked` delegate to a generated ComponentBoundEvent node:

```json
{
  "type": "widget.bind_button_to_custom_event",
  "params": {
    "asset": "/Game/UEPIWriteSandbox/WBP_UEPI_Test.WBP_UEPI_Test",
    "button": "StartButton",
    "delegate": "OnClicked"
  }
}
```

Create Enhanced Input assets and add a key mapping:

```json
{
  "type": "input.create_action",
  "params": {
    "destination_path": "/Game/UEPIWriteSandbox/Input",
    "name": "IA_UEPI_Jump",
    "value_type": "bool"
  }
}
```

```json
{
  "type": "input.add_key_mapping",
  "params": {
    "context": "/Game/UEPIWriteSandbox/Input/IMC_UEPI_Test.IMC_UEPI_Test",
    "action": "/Game/UEPIWriteSandbox/Input/IA_UEPI_Jump.IA_UEPI_Jump",
    "key": "SpaceBar"
  }
}
```

## Edit Audit

Edit preview/apply/validate/rollback records are appended to:

```text
__PROJECT_ROOT__/Saved/UEProjectIntelligence/audit/edit-YYYYMMDD.jsonl
```

Each line uses `uepi.edit-audit.v1` and includes the transaction id, event name, affected assets where available, and the backup artifact URI.
