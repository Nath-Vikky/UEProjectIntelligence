# Blueprint Editing

Blueprint changes use exact graph, node, and pin evidence. Read the Blueprint first, request a node/operation schema when needed, and submit one complete Plan v2.

## Design Before Operations

Choose an idiomatic graph shape before listing nodes. Repeated or timed behavior should normally use a variable plus timer/loop/custom event, not one duplicated chain per iteration. If the current catalog cannot express the better design, report the limitation rather than hiding it.

## Generic Nodes

`blueprint.add_node` is the generic entry point for catalog-advertised node kinds, including function calls, variable get/set, branch, print, input key, and make-struct. Existing specialized operation names remain available for compatibility. Nodes can declare a transaction-local `ref`, then later operations can refer to `node_ref`.

Connections use Graph Schema compatibility and exact source/target node GUID or ref plus pin ID/name. UEPI does not guess localized pin labels.

```json
{
  "type": "blueprint.connect_pins",
  "params": {
    "asset": "/Game/Blueprints/BP_Example.BP_Example",
    "graph": "EventGraph",
    "source": {"node_guid": "...", "pin_name": "then"},
    "target": {"node_ref": "print.location", "pin_name": "execute"}
  }
}
```

## Supported Graph Maintenance

UEPI can set pin defaults, connect/disconnect pins, remove/move nodes, add comments, create variables/components/functions/events, and compile. Apply validates touched Blueprints and returns compile diagnostics. Post-apply refresh makes the changed graph available to read tools and transaction diff.

## Safety

Preview resolves affected assets, target packages, budget, dirty/read-only state, plan identity, and fingerprints before any `Modify()`. Apply repeats preflight. Touched packages are backed up, saved individually, and eligible for transaction undo or file restore/package reload.
