# Real Machine Tests

Beta promotion requires UE 5.3.2 Win64 records for:

1. `LLMNPCDemo`: exact AnimGraph Slot read, DataAsset schema/create/set/save/reopen, one approval, PIE invoke/assert/cleanup.
2. Third Person Template: exact Blueprint read/edit/compile/save/reopen, actor/world read, targeted refresh, transaction diff.
3. Minimal blank Blueprint project: source install, setup, doctor, snapshot, offline read, live bridge, rollback.
4. Two project copies: exact online routing, mismatched binding rejection, explicit offline selection.

Create one Markdown record per run using `template.md`. Attach only sanitized logs/artifact paths; never commit bridge tokens or session files.

Required result: zero wrong-project calls, zero unapproved writes, touched packages persist after restart, validation passes, owned PIE stops, and failed apply leaves no partial product.
