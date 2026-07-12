# Known Limitations

UEPI 2.0 Beta is release-qualified for Unreal Engine 5.3.2 and Codex on Windows. The following boundaries are intentional.

- Offline reads require a previously generated Saved Snapshot. Live refresh, Schema, writes, and PIE require the matching Editor project to be open.
- Only one exact Editor session may satisfy a write plan. Restarting the Editor invalidates plans and Runtime Tickets.
- Writes are limited to the versioned Operation Catalog. There is no arbitrary Python, shell, console, C++ expression, function-call, save-all, or unrestricted delete surface.
- Runtime invoke is limited to non-Exec, non-Latent Blueprint-callable functions explicitly allowlisted by exact project configuration and approved in the Runtime Ticket.
- Dirty target packages are blocked. UEPI does not merge user-unsaved asset changes.
- Source-control checkout, locking, changelists, conflict resolution, and submit are not automated.
- Rollback covers supported touched packages through transaction backup/restore and compensation. It is not a database-style global transaction across external tools or source control.
- Generic Blueprint authoring supports registered node kinds and real reflected pins; arbitrary node classes, State Machine authoring, Control Rig, Behavior Tree, Niagara, PCG, MetaSound, Sequencer, Landscape, and Foliage writes are outside this Beta.
- Animation reconstruction artifacts describe sampled source motion and can drive procedural recreation, but UEPI does not evaluate final runtime poses through every AnimGraph, Control Rig, IK, physics, or retargeting layer.
- Optional plugin readers compile only when their project plugins are enabled. Otherwise UEPI returns generic Asset Registry/reflection evidence.
- Third-party MCP clients and UE versions other than 5.3.2 have not passed the release matrix.
