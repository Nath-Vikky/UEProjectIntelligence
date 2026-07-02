# Optional Live Bridge

The stable UEPI read path does not require a daemon, worker, Web UI, HTTP service, or live bridge.

The live bridge starts with the editor by default and is reserved for editor-online read capabilities plus guarded edit apply:

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

In the current build, the Unreal side starts a localhost TCP bridge by default when the editor session starts. It writes a project-local session file, a project-local token file, and a user-local active-session registry entry, then accepts length-prefixed JSON requests. Users can still opt out in Project Settings.

Implemented commands:

```text
editor.get_status
editor.get_selection
editor.read_output_log
asset.refresh_now
```

`editor.capture_viewport` captures the active editor viewport to:

```text
__PROJECT_ROOT__/Saved/UEProjectIntelligence/artifacts/screenshots/viewport-*.png
```

It returns the artifact path, `uepi://artifact/screenshots/...` URI, PNG dimensions, and byte count. Snapshot tools remain available when the bridge is off.

## Session Path

Editor bridge sessions use:

```text
__PROJECT_ROOT__/Saved/UEProjectIntelligence/store/sessions/editor-bridge.json
```

Session schema:

```json
{
  "schema_version": "uepi.editor-bridge-session.v1",
  "active": true,
  "host": "127.0.0.1",
  "port": 48735,
  "session_id": "...",
  "project_file": "...",
  "project_root": "...",
  "store_root": "...",
  "session_path": "...",
  "token_path": "...",
  "token_hash": "...",
  "transport_ready": true,
  "started_at": "...",
  "last_heartbeat": "..."
}
```

When the configured bridge port is `0`, UEPI tries localhost ports starting at `48735` so multiple open UE projects can publish separate online sessions.

## Safety Rules

- Bind only to localhost.
- Use a per-session capability token.
- Execute editor commands sequentially on the game thread.
- Return structured errors and timeouts.
- Keep edit apply behind preview, user approval, validation, and rollback/diff reporting.
