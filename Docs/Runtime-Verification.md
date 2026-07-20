# Runtime Verification

Runtime verification may be transaction-bound or created independently with `uepi_runtime_preview`. The immutable ticket fixes the map, actions, functions, keys, delivery method, and object/property reads allowed by the active authorization policy. The Agent must pass it to `uepi_runtime`; a general remote-control session is not exposed.

```json
{
  "verification_plan": {
    "map": "/Game/Maps/M_Test",
    "timeout_seconds": 60,
    "verification_mode": "hybrid",
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

UEPI only controls a PIE session it started. Start fails if unrelated PIE is active. Stop, bridge action failure, wait timeout, and assertion failure all attempt to end owned PIE and invalidate runtime state.

## Actions

- `status`: ownership and PIE state.
- `start`: start PIE for the ticket and optional permitted map.
- `input`: send an allowlisted key or Enhanced Input action. Results distinguish API acceptance, delivery attempt, known binding handling, and actual Enhanced Input injection.
- `invoke`: invoke an allowlisted, non-latent Blueprint-callable function with Schema-validated typed arguments and typed return/out values.
- `read`: read a permitted runtime property/object state, or invoke an allowlisted `BlueprintPure` debug function through the observation path.
- `wait`: bounded polling for a permitted condition.
- `assert`: record a structured expected/actual result.
- `stop`: end owned PIE and clean up.

Runtime object handles are session-scoped and must not be reused after stop. Raw memory access, arbitrary reflection invocation, console commands, Python, shell, and taking over user-owned PIE are prohibited.

Invoke must pass both gates: it appears in the approved verification ticket and its exact `FunctionOwnerClassPath:FunctionName` is present in Project Settings `AllowedRuntimeFunctions`. An empty project allowlist permits no invoke calls.

`wait` and `assert` automatically unwrap UEPI typed values such as `{"type":"bool","value":false}` before comparing them with `equals`. Input API acceptance alone is not gameplay proof; objective verification should assert a resulting gameplay state such as an active template ID.

## Verification Modes

- `agent_objective`: UEPI may control its own PIE session and evaluates only machine-verifiable state.
- `human_pie`: the user owns PIE; UEPI is observation-only and never starts, stops, injects input, or invokes gameplay functions.
- `hybrid`: default. UEPI completes objective checks, then returns concise human PIE steps for final visual acceptance.

For `human_pie` and `hybrid`, results always separate technical state from visual judgment:

```text
visual_acceptance = human_required
visual_status = unreviewed | accepted | rejected
visual_reviewer = user
```

## Evidence

A complete verification report includes the transaction ID/ticket, verification mode, PIE ownership, action results, assertions, truthful input evidence, relevant output-log cursor range, optional PIE viewport artifact, cleanup status, and separate human visual status. A failed runtime assertion does not silently roll back a saved edit; the Agent reports it and uses the explicit transaction rollback workflow when appropriate.
