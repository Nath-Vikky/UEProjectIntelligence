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

In the current foundation build, `ready` is normally `false`. Snapshot tools remain available.

## Session Path

Future editor bridge sessions use:

```text
__PROJECT_ROOT__/Saved/UEProjectIntelligence/runtime/editor-bridge.json
```

Expected future schema:

```json
{
  "schema_version": "uepi.editor-bridge-session.v1",
  "host": "127.0.0.1",
  "port": 0,
  "session_id": "...",
  "token_hint": "...",
  "started_at_utc": "..."
}
```

## Safety Rules

- Bind only to localhost.
- Use a per-session capability token.
- Execute editor commands sequentially on the game thread.
- Return structured errors and timeouts.
- Keep write commands behind `codex_write_alpha` and explicit user approval.
