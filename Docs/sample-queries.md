# Sample Queries

Start every new MCP session with:

```text
uepi_status
uepi_overview
```

Find assets:

```text
uepi_search {"query": "BP_ThirdPersonCharacter", "limit": 10}
```

Read an asset:

```text
uepi_asset {"asset": "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter"}
```

Read Blueprint structure:

```text
uepi_blueprint {"asset": "BP_ThirdPersonCharacter", "limit": 300}
```

Trace BeginPlay-style static flow:

```text
uepi_blueprint_trace {"asset": "BP_ThirdPersonCharacter", "start": "BeginPlay", "relation_types": ["exec_flows_to", "calls_function"], "max_depth": 8, "max_paths": 20}
```

Read animation context:

```text
uepi_animation {"asset": "MM_Run_Fwd", "include": ["summary", "tracks", "notifies", "curves", "pose_samples"]}
```

Build a context bundle:

```text
uepi_context {"question": "BP_ThirdPersonCharacter 的 BeginPlay 连接了哪些节点？", "scope": ["blueprint"], "max_items": 80}
```

If the result diagnostics include `UEPI_REFRESH_REQUESTED`, wait for the editor plugin to process the request and retry the same call.
