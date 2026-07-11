# Runtime Verification

Runtime verification is transaction-bound. `uepi_edit_apply` may return a ticket describing allowed checks. The Agent must pass that ticket to `uepi_runtime`; a general remote-control session is not exposed.

## Lifecycle

```text
uepi_runtime(status)
uepi_runtime(start, ticket_id, map?)
uepi_runtime(input | invoke | read | wait | assert)
uepi_editor(output_log | viewport_capture)
uepi_runtime(stop)
```

UEPI only controls a PIE session it started. Start fails if unrelated PIE is active. Stop and failure cleanup remove owned runtime handles and end owned PIE.

## Actions

- `status`: ownership and PIE state.
- `start`: start PIE for the ticket and optional permitted map.
- `input`: send an allowlisted key pressed/released event.
- `invoke`: invoke an allowlisted parameterless, non-latent Blueprint-callable function.
- `read`: read a permitted runtime property/object state.
- `wait`: bounded polling for a permitted condition.
- `assert`: record a structured expected/actual result.
- `stop`: end owned PIE and clean up.

Runtime object handles are session-scoped and must not be reused after stop. Raw memory access, arbitrary reflection invocation, console commands, Python, shell, and taking over user-owned PIE are prohibited.

## Evidence

A complete verification report includes the transaction ID/ticket, PIE start and stop, action results, assertions, relevant output-log cursor range, optional PIE viewport artifact, and cleanup status. A failed runtime assertion does not silently roll back a saved edit; the Agent reports it and uses the explicit transaction rollback workflow when appropriate.
