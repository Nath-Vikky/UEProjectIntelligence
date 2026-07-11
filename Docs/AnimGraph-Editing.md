# AnimGraph Editing

AnimGraph is treated as a pose graph domain, not as an Event Graph with different labels. Use exact Blueprint graph evidence and the active operation catalog.

## P0 Slot Workflow

The P0 closed loop supports:

1. `animation.create_slot_group` on the exact Skeleton.
2. `animgraph.add_slot_node` on the exact Animation Blueprint graph.
3. `animgraph.connect_pose` using Graph Schema-compatible pose pins.
4. Blueprint compile, touched-only save, targeted refresh, and diff.

`animgraph.add_slot_node` accepts the slot/group identity and position. `animgraph.connect_pose` requires exact source and target node/pin identities returned by a read or prior operation ref. Skeleton compatibility is checked by the Editor.

## Validation

Animation Blueprint validation compiles the Blueprint and reports graph/compiler failures. Skeleton changes and the Animation Blueprint package are both included in affected assets, backups, saving, and rollback when touched.

## Scope

This alpha does not claim generic state-machine creation, arbitrary Control Rig graph authoring, montage section editing, or bulk animation key editing. Those operations must appear in Discover before an Agent uses them.
