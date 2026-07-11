# Runtime Verification

Runtime verification is transaction-bound. Preview must include a `verification_plan`; only then may successful Apply return a ticket. The ticket fixes the map, actions, functions, keys, and object/property reads approved by the user. The Agent must pass it to `uepi_runtime`; a general remote-control session is not exposed.

```json
{
  "verification_plan": {
    "map": "/Game/Maps/M_Test",
    "timeout_seconds": 60,
    "steps": [
      {"action": "invoke", "function": "SubmitPublishedTemplate"},
      {"action": "assert", "object_path": "/Game/...:PersistentLevel.Target", "property": "bCompleted", "equals": true}
    ]
  }
}
```

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

Invoke must pass both gates: it appears in the approved verification ticket and its exact `FunctionOwnerClassPath:FunctionName` is present in Project Settings `AllowedRuntimeFunctions`. An empty project allowlist permits no invoke calls.

## Evidence

A complete verification report includes the transaction ID/ticket, PIE start and stop, action results, assertions, relevant output-log cursor range, optional PIE viewport artifact, and cleanup status. A failed runtime assertion does not silently roll back a saved edit; the Agent reports it and uses the explicit transaction rollback workflow when appropriate.
