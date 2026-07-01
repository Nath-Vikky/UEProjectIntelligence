# Optional Live Bridge

The stable UEPI read path does not require a daemon, worker, Web UI, HTTP service, or live bridge.

The optional live bridge is reserved for editor-online read capabilities:

```text
editor.get_status
editor.get_selection
editor.capture_viewport
editor.read_output_log
asset.refresh_now
```

## Current Build

`uepi_status` returns a `bridge` section with:

```text
supported
enabled
configured
ready
connected
session_path
capabilities
diagnostics
```

In the current foundation build, the Unreal side can write a disabled-by-default session/token skeleton when `bEnableLiveEditorBridge` is enabled. `transport_ready` is still `false`, so MCP reports `ready: false`. Snapshot tools remain available.

## Session Path

Future editor bridge sessions use:

```text
__PROJECT_ROOT__/Saved/UEProjectIntelligence/store/sessions/editor-bridge.json
```

Expected future schema:

```json
{
  "schema_version": "uepi.editor-bridge-session.v1",
  "host": "127.0.0.1",
  "port": 0,
  "session_id": "...",
  "token_path": "...",
  "token_hash": "...",
  "transport_ready": false,
  "started_at": "...",
  "last_heartbeat": "..."
}
```

## Safety Rules

- Bind only to localhost.
- Use a per-session capability token.
- Execute editor commands sequentially on the game thread.
- Return structured errors and timeouts.
- Keep write commands behind `codex_write_alpha` and explicit user approval.
