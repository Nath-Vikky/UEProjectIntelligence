# UEPI Contract Hardening Live Read Test

Date: 2026-07-17

## Environment

- Unreal Engine: 5.3.2, Win64 Development Editor.
- Project: `GasDemo` Third Person project.
- Editor Operation Registry: 64 operations with Enhanced Input compiled.
- Guarded transaction limits: 96 operations and 12 affected assets.
- Online Doctor: 17/17 checks passed with zero warnings.

## Automated Gates

- `python -B Tools/test_snapshot_mcp_v2.py` passed.
- Python module compilation passed.
- `GasDemoEditor Win64 Development` rebuilt successfully after the C++ Bridge changes.
- The v2 MCP contract snapshot was regenerated.

## Live Read Contract

`python -B Tools/test_live_read_contract.py --project <GasDemo.uproject>` passed against a newly started Editor session.

- Status returned the exact GasDemo binding, `/Game/ThirdPerson/Maps/ThirdPersonMap`, stopped PIE state, and an authoritative current online catalog.
- World filtering returned only the exact `PlayerStart` actor; exact actor detail and requested properties succeeded.
- `content.create_asset` returned strict machine-readable input schema, required fields, an example, and a contract hash.
- Level viewport capture returned a 640x360 PNG, camera metadata, a normalized absolute artifact path, and non-empty inline MCP `image/png` content.
- Every response returned all eight timing fields. Observed calls were approximately 0.85-1.12 seconds, primarily in `editor_dispatch_ms`; Editor execution ranged from sub-millisecond status/world work to roughly 125 ms for PNG capture and inline encoding.

No Preview, Apply, project asset mutation, PIE start, or Runtime invoke was performed in this read-only hardening pass. The only generated artifact was a screenshot under `Saved/UEProjectIntelligence/artifacts/screenshots`.
