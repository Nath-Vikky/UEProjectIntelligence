# Sample Queries

After MCP connects, start with:

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

Read animation context:

```text
uepi_animation {"asset": "MM_Run_Fwd", "include": ["summary", "tracks", "notifies", "curves", "pose_samples"]}
```

Build a context bundle:

```text
uepi_context {"question": "BP_ThirdPersonCharacter 的 BeginPlay 连接了哪些节点？", "scope": ["blueprint"], "max_items": 80}
```
