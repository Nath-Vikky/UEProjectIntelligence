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
  "schema_version": "uepi.edit-plan.v1",
  "transaction_id": "uepi-preview-...",
  "created_at_utc": "...",
  "status": "preview_only",
  "intent": "...",
  "operations": [],
  "evidence": [],
  "requires_user_approval": true,
  "apply_supported": false,
  "validation_plan": [],
  "rejection_reasons": []
}
```
