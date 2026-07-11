# LLMNPCDemo Fixtures

This directory freezes the UEPI vNext pre-refactor MCP contract and known failure modes.

Binary Unreal assets are intentionally not copied here. Snapshot fragments for `ABP_Manny1`, `Waving`, `BP_LLMNPC_Manny`, and `LLMNPCActionLayer` must come from the real LLMNPCDemo test project and may be added only after verifying that they contain no unrelated project data.

Regenerate the contract with:

```powershell
python -B Tools/dump_mcp_contract.py
```
