# UEPI Real Machine Test: Two Project Routing

## Build

- UE version: `5.3.2-29314046+++UE5+Release-5.3`.
- Projects: `GasDemo` and clean `UEPIBlank` opened simultaneously.
- Fix commit: `11aa73c`.

## Initial Failure

Both Editors published different project bindings and session IDs but listened on `127.0.0.1:48735`. `FTcpListener` had address/port reuse enabled, so Windows accepted both listeners and bridge requests could reach the wrong project. The second project's Doctor session checks passed while its bridge probe failed.

## Fix

The UEPI bridge now disables TCP address/port reuse. With the default dynamic-port setting, the first Editor takes `48735` and the next Editor advances to `48736`. A user-configured fixed port still fails clearly on conflict instead of sharing a listener.

## Actual

- `UEPIBlank`: unique session, PID `16908`, port `48735`.
- `GasDemo`: unique session, PID `6660`, port `48736`.
- Both online Doctors reached the correct project binding, token, bridge, and 64-operation catalog.
- GasDemo Doctor passed 17/17 checks while both Editors were online.
- The Codex MCP remained bound to GasDemo after UEPIBlank started; it did not switch to the newest global session.
- A GasDemo-bound MCP request naming the UEPIBlank project was rejected with `UEPI_PROJECT_MISMATCH` before any bridge operation.
- No write was routed to the wrong project.

## Result

- [x] Pass
- [ ] Fail
