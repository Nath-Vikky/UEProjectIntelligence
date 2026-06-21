# UE Project Intelligence

UE Project Intelligence is a read-only Unreal Editor plugin for extracting project intelligence from UE 5.3.2 projects.

For the end-to-end MCP workflow, including a Codex configuration example, see [Docs/user-guide.md](Docs/user-guide.md).

Current implementation focuses on the first development-plan spine:

- L0 Asset Registry scan.
- L1 targeted UObject reflection snapshots with structured struct serialization, large collection sidecar artifacts, CDO/Super default comparison, direct subobject-reference marking, direct reference-cycle guards, and collection capacity metadata.
- L2 targeted Blueprint Graph/Node/Pin extraction, including split/sub-pin parent metadata and SimpleConstructionScript component templates.
- L2 targeted World/Level/Actor/Component, LevelScriptActor, attach/ownership, Streaming Level, Level Instance reference, WorldDataLayers/Data Layer container, and World Partition Actor Descriptor extraction.
- L2 targeted Enhanced Input Action and Mapping Context extraction.
- L2 targeted CommonUI input data, default action row handles, and CommonUI input action data table extraction.
- Static AI extraction for Blackboard, BehaviorTree, and EQS assets.
- Static StateTree extraction for editor state hierarchy, task/evaluator/condition/transition metadata, property bindings, external/context data, and compiled summary counts.
- Static Gameplay Ability System extraction for GameplayAbility, GameplayEffect, and GameplayCueNotify assets or Blueprint generated-class default objects.
- L2 targeted Skeleton hierarchy, reference local/component poses, virtual bones, Skeleton/Mesh sockets, SkeletalMesh, AnimSequence frame/time raw-local and component-space samples, AnimMontage/AnimComposite timelines, BlendSpace, PoseAsset, IK Rig, IK Retargeter, PhysicsAsset, AnimBlueprint, and Control Rig Blueprint static animation extraction.
- L2 targeted DataAsset/PrimaryDataAsset metadata, PrimaryAssetId, AssetManager rules and bundle members, UserDefinedStruct/UserDefinedEnum, StringTable entries/metadata, DataTable row/schema, CompositeDataTable parent dependencies, CurveTable rows, CurveFloat, CurveVector, CurveLinearColor channel/key/tangent, and CurveLinearColorAtlas slot/reference extraction.
- L2 targeted WidgetBlueprint static WidgetTree extraction.
- L2 targeted Niagara System, Emitter, Script, Parameter Definitions, parameter, renderer, event handler, simulation-stage, and VectorFieldStatic static VFX extraction.
- L2 targeted PCG static graph node/pin/edge/settings extraction with UniversalGraphIR projection.
- L2 targeted MetaSound Frontend Document graph/node/vertex/edge/interface/literal/dependency extraction with UniversalGraphIR projection.
- L2 targeted SoundCue node graph, SoundWave metadata, SoundSubmix hierarchy/effect-chain, and SoundSubmix effect preset static audio extraction.
- L2 targeted LevelSequence/MovieScene timing, binding, track, section, channel, camera-cut, subsequence, audio-section, animation-section, binding-tag, key-time row, and key artifact manifest extraction.
- L2 targeted StaticMesh, Texture2D, TextureCube, and generic UTexture render asset extraction.
- L2 targeted Material, MaterialInstance, MaterialFunction, and MaterialParameterCollection graph/default-parameter extraction.
- First-pass L3 semantic references for common Blueprint nodes, including function calls with owner classes, variable reads/writes with declaring classes, casts, SpawnActor targets, and LoadAsset targets.
- Derived node-level flow projection from canonical Pin links.
- Derived CFG basic blocks from Node-level execution flow.
- Derived DFG values and def-use relations from data Pin links and Blueprint variable accesses.
- Relation attributes for Pin provenance, branch labels, projection kind, and value kind.
- Read-only project descriptor, plugin descriptor, module, source file, and config file graph.
- Config-file Unreal path references via `config_references_asset`.
- Asset Registry redirector metadata with `asset_redirector` entities and `redirects_to` relations when destination tags are available.
- Local SQLite ingest and query CLI.
- Local HTTP query API over the same SQLite index.
- SQLite-backed UE worker protocol for registration, session-token heartbeat, long-poll job leasing, state transitions, cancellation, retry/recovery, trace events, and chunked artifact uploads, with Editor subsystem outbound registration/heartbeat helpers and a read-only `UEPIIndex` commandlet worker for queued `metadata_scan` jobs.
- Local asset revision history for ingested scans, with current/previous revision tracking.
- Offline source/config index for UHT symbols, owner-qualified Blueprint-callable C++ functions/properties, C++/Config asset path references, Build.cs/Target.cs discovery, compile database discovery, effective Unreal config values, and Blueprint call-to-C++ symbol link queries.
- Animation, data, and cinematics query APIs over ingested scan entities, with snapshot opt-in, data snapshot collection cursor paging plus optional JSON collection artifacts, Sequencer key-time cursor paging plus optional JSON key artifacts, graph JSON cursor paging, and MCP token-budget artifact handling for large responses.
- Local scan diff and impact reports for comparing two ingested scan artifacts, including Blueprint, DataTable, and Animation manifest delta summaries.
- Stale checks for detecting missing/changed source scan JSON and changed file-backed UE package assets after ingest.
- Optional Git commit/branch/dirty-state association for ingested scans when the project directory is a Git worktree.
- Editor incremental event logging for package saves, asset add/remove/rename/update, and global Blueprint compile notifications.
- Zero-dependency scan validator covering L2 Blueprint, World, WorldDataLayers, World Partition Actor Descriptor, Enhanced Input, CommonUI, AI, StateTree, Gameplay Ability System, Animation, PhysicsAsset, Control Rig, UserDefinedStruct/Enum, StringTable, DataTable, Data Curve, WidgetBlueprint, Niagara/VFX, PCG, MetaSound, Audio, Cinematics, Render, Material, and MaterialParameterCollection snapshots, with golden summary fixtures for the currently available sample assets.

## Commandlet

Full L0 metadata scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\last_scan.json"
```

Targeted L1 UObject reflection:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L1 `
  -UEPIAsset="/Game/ThirdPerson/Blueprints/BP_ThirdPersonGameMode.BP_ThirdPersonGameMode" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l1_scan.json"
```

Targeted L2 Blueprint graph and first-pass semantics:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_character_scan.json"
```

Multiple targeted assets can be passed with semicolons in `-UEPIAsset`.

Editor sessions also record lightweight incremental events through `UUEPIEditorSubsystem`.
Events are kept in memory for editor queries and appended to:

```text
Saved/UEProjectIntelligence/incremental_events.jsonl
```

The event log is read-only with respect to project assets; it records observations and does not trigger package saves, Blueprint compiles, or automatic rescans by itself.

Editor Live Worker mode:

1. Start the local daemon on `127.0.0.1:8765`.
2. Open the project in Unreal Editor.
3. Open `Tools > UE Project Intelligence` and click `Start Live Worker` if auto-connect did not already register.

The editor worker registers as `worker_type=editor`, keeps heartbeat alive, long-polls `/v1/jobs/poll`, executes read-only `metadata_scan` jobs inside the open editor process, writes scan JSON artifacts, and posts `succeeded` or `failed` updates back to the daemon. It does not compile Blueprints, save packages, rename assets, delete assets, launch shell commands, or expose an HTTP server inside Unreal.

Commandlet worker mode:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPIWorker `
  -UEPIWorkerUrl="http://127.0.0.1:8765" `
  -UEPIWorkerMaxJobs=1
```

The commandlet worker registers as `worker_type=commandlet`, sends heartbeat updates, long-polls `/v1/jobs/poll`, executes leased `metadata_scan` jobs, writes the scan JSON, and posts `running`/`succeeded` or `failed` state updates back to the daemon. Job requests accept fields such as `level`, `asset` or `assets`, `output_path`, `read_blueprints`, `read_uobject_reflection`, and scope/budget overrides.

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py job-submit `
  --type metadata_scan `
  --request-json "{`"level`":`"L2`",`"asset`":`"/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter`",`"output_path`":`"F:/Epic Games/UE5project/GasDemo/Saved/UEProjectIntelligence/job_character_scan.json`"}"
```

Targeted L2 World/Level/Actor/Component scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Maps/ThirdPersonMap.ThirdPersonMap" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_world_scan.json"
```

Targeted L2 WorldDataLayers/Data Layer container scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Maps/ThirdPersonMap.ThirdPersonMap:PersistentLevel.WorldDataLayers" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_world_data_layers_scan.json"
```

Targeted L2 World Partition Actor Descriptor scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Maps/ThirdPersonMap.ThirdPersonMap" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_world_partition_scan.json"
```

Targeted L2 Enhanced Input Mapping Context scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Input/IMC_Default.IMC_Default" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_input_mapping_scan.json"
```

Targeted L2 CommonUI input data and action table scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/CommonUI/GenericInputData.GenericInputData;/CommonUI/GenericInputActionDataTable.GenericInputActionDataTable" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_common_ui_scan.json"
```

Targeted L2 AnimSequence scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Characters/Mannequins/Animations/Manny/MM_Walk_Fwd.MM_Walk_Fwd" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_anim_sequence_scan.json"
```

Targeted L2 BlendSpace scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Characters/Mannequins/Animations/Manny/BS_MM_WalkRun.BS_MM_WalkRun" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_blend_space_scan.json"
```

Targeted L2 PoseAsset scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Characters/Mannequins/Rigs/Poses/Manny/Manny_foot_l_pose.Manny_foot_l_pose" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_pose_asset_scan.json"
```

Targeted L2 PhysicsAsset scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Characters/Mannequins/Rigs/PA_Mannequin.PA_Mannequin" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_physics_asset_scan.json"
```

Targeted L2 IK Rig scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Characters/Mannequins/Rigs/IK_Mannequin.IK_Mannequin" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_ik_rig_scan.json"
```

Targeted L2 IK Retargeter scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Characters/Mannequins/Rigs/RTG_Mannequin.RTG_Mannequin" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_ik_retargeter_scan.json"
```

Targeted L2 AnimBlueprint static scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Characters/Mannequins/Animations/ABP_Manny.ABP_Manny" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_anim_blueprint_scan.json"
```

Targeted L2 Control Rig Blueprint static scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Characters/Mannequins/Rigs/CR_Mannequin_BasicFootIK.CR_Mannequin_BasicFootIK" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_control_rig_scan.json"
```

Targeted L2 DataTable static scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/DatasmithContent/Datasmith/AreaLightsTable.AreaLightsTable" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_data_table_scan.json"
```

Targeted L2 UserDefinedStruct static scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/DatasmithContent/Datasmith/AreaLightsStruct.AreaLightsStruct" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_user_defined_struct_scan.json"
```

Targeted L2 UserDefinedEnum static scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Niagara/Enums/Angles/ENiagara_AngleInput.ENiagara_AngleInput" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_user_defined_enum_scan.json"
```

Targeted L2 Curve static scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Characters/Mannequins/Materials/Functions/ChromaticCurve.ChromaticCurve" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_curve_scan.json"
```

Targeted L2 CurveLinearColorAtlas static scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/Characters/Mannequins/Materials/Functions/CA_Mannequin.CA_Mannequin" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_curve_atlas_scan.json"
```

Targeted L2 WidgetBlueprint static scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/AudioWidgets/AudioButtonMatrix/AudioButtonMatrix.AudioButtonMatrix" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_widget_blueprint_scan.json"
```

Targeted L2 Niagara Emitter static scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_niagara_emitter_scan.json"
```

Targeted L2 Niagara Parameter Definitions static scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Niagara/ParameterDefinitions/CoreParameters.CoreParameters" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_niagara_parameter_definitions_scan.json"
```

Targeted L2 VectorFieldStatic VFX scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Niagara/VectorFields/Assets/TilingCurl16.TilingCurl16" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_vector_field_static_scan.json"
```

Targeted L2 PCG graph scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/UEPI/Fixtures/PCG/SimpleForest.SimpleForest" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_pcg_simple_forest_scan.json"
```

Targeted L2 MetaSound source graph scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/UEPI/Fixtures/MetaSound/MS_UEPI_Tone.MS_UEPI_Tone" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_metasound_uepi_tone_scan.json"
```

Targeted L2 LevelSequence/MovieScene scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/UEPI/Fixtures/Cinematics/LS_UEPI_Simple.LS_UEPI_Simple" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_level_sequence_uepi_simple_scan.json"
```

Targeted L2 StaticMesh render-asset scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/LevelPrototyping/Meshes/SM_Cube.SM_Cube" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_static_mesh_scan.json"
```

Targeted L2 TextureCube render-asset scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Niagara/DefaultAssets/ScenePreviewCube.ScenePreviewCube" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_texture_cube_scan.json"
```

Targeted L2 generic UTexture render-asset scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/MediaPlate/T_MediaPlate.T_MediaPlate" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_texture_generic_scan.json"
```

Targeted L2 SoundWave audio scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/UEPI/Fixtures/Audio/SW_UEPI_Tone.SW_UEPI_Tone" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_sound_wave_uepi_tone_scan.json"
```

Targeted L2 SoundCue audio scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/UEPI/Fixtures/Audio/SC_UEPI_Tone.SC_UEPI_Tone" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_sound_cue_uepi_tone_scan.json"
```

Targeted L2 SoundSubmix audio scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/ResonanceAudio/ResonanceSubmixDefault.ResonanceSubmixDefault" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_sound_submix_scan.json"
```

Targeted L2 SoundSubmix effect preset audio scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/ResonanceAudio/ResonanceReverbDefault.ResonanceReverbDefault" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_sound_submix_effect_scan.json"
```

Targeted L2 Material graph scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Game/ThirdPerson/LevelPrototyping/Materials/M_PrototypeGrid.M_PrototypeGrid" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_material_scan.json"
```

Targeted L2 MaterialParameterCollection scan:

```powershell
& "F:\Epic Games\don\UE_5.3\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "F:\Epic Games\UE5project\GasDemo\GasDemo.uproject" `
  -run=UEPIIndex -unattended -nop4 -nosplash -NullRHI `
  -UEPILevel=L2 `
  -UEPIAsset="/Fab/MaterialParameterCollection/QMPC_GlobalFoliageActor.QMPC_GlobalFoliageActor" `
  -UEPIOutput="F:\Epic Games\UE5project\GasDemo\Saved\UEProjectIntelligence\l2_material_parameter_collection_scan.json"
```

## SQLite Query CLI

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 `
  ingest --scan Saved\UEProjectIntelligence\l2_character_scan.json

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 summary

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 health

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 scans --limit 25

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 entities --kind blueprint_node --limit 100

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 relations --relation-type contains_node --limit 100

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 diff --limit 50

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 `
  history "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter"

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 stale --limit 100

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 search u_function --limit 5

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 api-docs

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 integrity

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 recover
```

Use `--include-snapshot` on `search` or `related` when you need full reflection or Blueprint graph snapshots.

Bounded graph query and exports:

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 `
  subgraph "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter" `
  --depth 2 --relation-type contains_graph --relation-type contains_node

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 `
  export-dot "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter" `
  --depth 2 --limit 300 `
  --output Saved\UEProjectIntelligence\l2_character.dot

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 `
  graph-query "from /Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter depth 2 relation contains_graph,contains_node limit 300"

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 `
  export-graph "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter" `
  --format graphml --depth 2 --limit 300 `
  --output Saved\UEProjectIntelligence\l2_character.graphml

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 `
  artifact-range --offset 0 --length 4096

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 `
  report --limit 25 --output Saved\UEProjectIntelligence\l2_report.md
```

`export-graph` supports `json`, `dot`, `mermaid`, `graphml`, `cytoscape`, and `parquet`. Parquet writes real Parquet bytes when the optional `pyarrow` dependency is installed; the daemon otherwise remains Python-standard-library only.

## Local HTTP Query API

The same SQLite query layer can be served over localhost:

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 `
  serve --host 127.0.0.1 --port 8765

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_daemon.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3 `
  serve --host 127.0.0.1 --port 8765 --token auto
```

Supported endpoints:

- `GET /v1/health`
- `GET /v1/summary`
- `GET /v1/scans?limit=50&cursor=<cursor>`
- `GET /v1/entities?scan=<scan-id-or-prefix>&kind=<kind>&limit=100&cursor=<cursor>`
- `GET /v1/relations?scan=<scan-id-or-prefix>&relation_type=<type>&limit=100&cursor=<cursor>`
- `GET /v1/search?q=<text>&limit=20`
- `GET /v1/related?entity=<id-or-canonical-key>&limit=50`
- `GET /v1/subgraph?entity=<id-or-canonical-key>&depth=1&relation_type=exec_flows_to`
- `GET /v1/graph-page?entity=<id-or-canonical-key>&collection=edges&limit=100`
- `GET /v1/graph-query?q=from%20<entity>%20depth%202%20relation%20contains_node`
- `GET /v1/export-dot?entity=<id-or-canonical-key>&depth=1&relation_type=exec_flows_to`
- `GET /v1/export-graph?entity=<id-or-canonical-key>&format=graphml&depth=1`
- `GET /v1/data-query?asset=<asset-key>&include_snapshot=false`
- `GET /v1/data-page?entity=<asset-key>&collection=rows&limit=100`
- `GET /v1/cinematics-key-page?entity=<level-sequence-key>&section=<section-id>&channel=<channel-id>&limit=100`
- `GET /v1/artifact-range?scan=<scan-id-or-prefix>&offset=0&length=4096&encoding=text`
- `GET /v1/report?scan=<scan-id-or-prefix>&limit=25`
- `GET /v1/integrity`
- `POST /v1/recover`
- `GET /v1/diff?base=<scan-id-or-prefix>&compare=<scan-id-or-prefix>&limit=100`
- `GET /v1/stale?scan=<scan-id-or-prefix>&limit=100`
- `GET /v1/history?asset=<asset-key-or-revision-prefix>&limit=50`
- `GET /v1/openapi`
- `GET /v1/ui`
- `POST /v1/ingest` with JSON `{ "scan_path": "Saved/UEProjectIntelligence/l2_character_scan.json" }`

List endpoints return `items`, `count`, `limit`, `next_cursor`, and `has_more`; pass `next_cursor` back as `cursor` to continue. Graph DSL queries use `from <entity> depth <n> relation <type[,type]> limit <n>`. Artifact range reads are capped at 1 MiB per request. The HTTP server supports optional bearer token auth with `--token`, `--token-file`, or `--token auto`; `/health` and `/v1/ui` remain reachable for local startup checks. The HTTP API queries, diffs, exports, reports, and ingests existing scan artifacts only; it does not launch the editor, compile Blueprints, or mutate project assets.

Open the local Web UI from the daemon at `http://127.0.0.1:8765/v1/ui`. It includes an asset browser, SVG graph viewer, Blueprint/CFG/DFG relation filters, animation/data table tables, diff/stale/report panels, diagnostics/coverage summary, path copy, and `uepi://asset/...` links for editor-side handoff.

## MCP Stdio Server

The same read/query/export surface is available over MCP stdio:

```powershell
python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_mcp_server.py `
  --db Saved\UEProjectIntelligence\l2_index.sqlite3

python Plugins\UEProjectIntelligence\Services\uepi_daemon\uepi_mcp_server.py --sdk-status

python Plugins\UEProjectIntelligence\Tools\test_mcp_stdio.py
```

The server exposes `tools/list`, `tools/call`, `resources/list`, `resources/read`, `resources/templates/list`, `prompts/list`, and `prompts/get`. Tools include LLM-facing project status, refresh, asset context, Blueprint context, and animation context tools (`uepi_project_status`, `uepi_project_refresh`, `uepi_read_asset_context`, `uepi_read_blueprint`, `uepi_read_animation`), plus health, ingest, summary, paginated scans/entities/relations, search, related, subgraph, graph JSON paging, graph DSL, graph export, artifact range, Markdown report, diff, stale, history, animation/data query, data snapshot paging with optional collection artifacts, Sequencer key-time paging plus optional JSON key artifacts, source index/search, Blueprint-to-C++ symbol links, config effective values, security audit, worker protocol/session tools, SQLite-backed queue tools, and MCP job start/get. Tool calls return MCP `structuredContent` plus a text copy, advertise input schemas, and honor `token_budget`; oversized structured results are written to `uepi://mcp-artifact/{artifact_id}` with a small preview in the tool response. `tools/list` omits non-essential `outputSchema` fields and empty `required` arrays by default for broad client compatibility; pass `--include-output-schema` only for clients known to accept them. For Codex Desktop, pass `--tool-profile codex` to initialize with tool-only capabilities and advertise the high-level read-only loop first (`uepi_health`, `uepi_project_status`, `uepi_project_refresh`, `uepi_read_asset_context`, `uepi_read_blueprint`, `uepi_read_animation`, `uepi_summary`, `uepi_search`, `uepi_graph_query`, `uepi_security_audit`) with simplified input schemas while keeping the full toolset available to clients that can ingest it. Add `--trace-file Saved/UEProjectIntelligence/mcp_trace.jsonl` when diagnosing Codex startup; the trace records `initialize`, notification, and `tools/list` events without logging tool result payloads.

Resources include `uepi://summary`, `uepi://scans`, `uepi://report`, `uepi://openapi`, `uepi://security-audit`, `uepi://agent-protocol`, `uepi://workers`, `uepi://jobs`, `uepi://animation-query`, `uepi://data-query`, `uepi://source-symbols`, `uepi://source-references`, `uepi://blueprint-cpp-links`, `uepi://config-values`, `uepi://history/{asset}`, `uepi://artifact/{scan}?offset={offset}&length={length}`, and `uepi://mcp-artifact/{artifact_id}`. Prompt templates include scan triage and asset review workflows. The MCP adapter does not expose shell execution, arbitrary code execution, editor launch, Blueprint compile, package save, rename, delete, or write-asset tools; `uepi_project_refresh` and `uepi_read_*` with `refresh=true` only write queued job rows, scan artifacts, MCP artifacts, and the selected SQLite index. `requirements-mcp.txt` records the optional official Python MCP SDK dependency for host setups that want it, while the compatibility server itself remains standard-library only.

Example MCP client config:

```json
{
  "mcpServers": {
    "ue-project-intelligence": {
      "command": "python",
      "args": [
        "F:/Epic Games/UE5project/GasDemo/Plugins/UEProjectIntelligence/Services/uepi_daemon/uepi_mcp_server.py",
        "--db",
        "F:/Epic Games/UE5project/GasDemo/Saved/UEProjectIntelligence/index.sqlite3",
        "--tool-profile",
        "codex",
        "--trace-file",
        "F:/Epic Games/UE5project/GasDemo/Saved/UEProjectIntelligence/mcp_trace.jsonl"
      ]
    }
  }
}
```

## Validation

Validate a scan artifact against the built-in UEPI conformance checks:

```powershell
python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_character_scan.json
```

Security, fuzz, and performance checks:

```powershell
python Plugins\UEProjectIntelligence\Tools\security_fuzz_audit.py

python Plugins\UEProjectIntelligence\Tools\test_worker_protocol.py

python Plugins\UEProjectIntelligence\Tools\test_source_index.py

python Plugins\UEProjectIntelligence\Tools\test_data_page.py

python Plugins\UEProjectIntelligence\Tools\test_graph_page.py

python Plugins\UEProjectIntelligence\Tools\test_cinematics_key_page.py

python Plugins\UEProjectIntelligence\Tools\test_mcp_stdio.py

python Plugins\UEProjectIntelligence\Tools\run_golden_regression.py

python Plugins\UEProjectIntelligence\Tools\benchmark_daemon.py `
  --entities 2000 `
  --report Saved\UEProjectIntelligence\performance_baseline.json

python Plugins\UEProjectIntelligence\Tools\dirty_package_regression.py

python Plugins\UEProjectIntelligence\Tools\package_release.py --dry-run
```

## Documentation

- [User Installation](Docs/user-installation.md)
- [Support Matrix](Docs/support-matrix.md)
- [Terminology](Docs/terminology.md)
- [Definition Of Done](Docs/definition-of-done.md)
- [Fixture Inventory](Docs/fixture-inventory.md)
- [ADR](Docs/adr/README.md)
- [Developer Architecture](Docs/developer-architecture.md)
- [Extension SDK](Docs/extension-sdk.md)
- [Schema Reference](Docs/schema-reference.md)
- [Operations Runbook](Docs/operations-runbook.md)
- [Troubleshooting](Docs/troubleshooting.md)
- [Migration Guide](Docs/migration-guide.md)
- [Sample Queries](Docs/sample-queries.md)
- [Changelog](CHANGELOG.md)
- [Third-Party Notices](THIRD_PARTY_NOTICES.md)

Run the current L2 golden summaries:

```powershell
python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_character_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\bp_third_person_character_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_world_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\third_person_map_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_world_data_layers_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\third_person_map_world_data_layers_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_world_partition_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\third_person_map_world_partition_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_input_action_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\ia_move_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_input_mapping_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\imc_default_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_common_ui_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\common_ui_input_data_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_blackboard_uepi_agent_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\blackboard_uepi_agent_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_behavior_tree_uepi_patrol_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\behavior_tree_uepi_patrol_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_env_query_uepi_find_point_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\env_query_uepi_find_point_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_state_tree_uepi_minimal_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\state_tree_uepi_minimal_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_gameplay_ability_uepi_pulse_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\gameplay_ability_uepi_pulse_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_gameplay_effect_uepi_pulse_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\gameplay_effect_uepi_pulse_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_gameplay_cue_notify_uepi_pulse_static_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\gameplay_cue_notify_uepi_pulse_static_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_skeleton_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\sk_mannequin_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_skeletal_mesh_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\skm_manny_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_anim_sequence_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\mm_walk_fwd_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_blend_space_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\bs_mm_walkrun_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_pose_asset_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\manny_foot_l_pose_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_ik_rig_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\ik_mannequin_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_ik_retargeter_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\rtg_mannequin_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_physics_asset_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\pa_mannequin_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_anim_blueprint_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\abp_manny_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_control_rig_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\cr_mannequin_basic_foot_ik_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_user_defined_struct_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\area_lights_struct_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_user_defined_enum_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\eniagara_angle_input_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_data_table_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\area_lights_table_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_curve_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\chromatic_curve_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_curve_atlas_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\ca_mannequin_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_widget_blueprint_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\audio_button_matrix_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_niagara_emitter_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\fountain_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_niagara_parameter_definitions_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\core_parameters_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_vector_field_static_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\tiling_curl16_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_pcg_simple_forest_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\pcg_simple_forest_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_metasound_uepi_tone_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\metasound_uepi_tone_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_level_sequence_uepi_simple_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\level_sequence_uepi_simple_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_static_mesh_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\sm_cube_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_texture_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\t_gridchecker_a_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_texture_cube_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\scene_preview_cube_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_texture_generic_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\t_media_plate_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_sound_wave_uepi_tone_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\sound_wave_uepi_tone_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_sound_cue_uepi_tone_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\sound_cue_uepi_tone_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_sound_submix_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\resonance_submix_default_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_sound_submix_effect_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\resonance_reverb_default_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_material_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\m_prototype_grid_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_material_instance_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\mi_prototype_grid_gray_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_material_function_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\mf_proc_grid_l2.summary.json

python Plugins\UEProjectIntelligence\Tools\validate_scan.py `
  --scan Saved\UEProjectIntelligence\l2_material_parameter_collection_scan.json `
  --golden Plugins\UEProjectIntelligence\Tests\golden\qmpc_global_foliage_actor_l2.summary.json
```

## Read-Only Contract

The implemented scans do not save packages, compile Blueprints, or expose write/delete/rename operations. Each scan checks the dirty package set and reports `UEPI_DIRTY_PACKAGE_DETECTED` if the set changes.

## Current Boundaries

- L0 package dependencies come from Asset Registry.
- Project/config/code metadata comes from filesystem reads of `.uproject`, project `.uplugin`, `Config/*.ini`, and project/plugin `Source` files. Effective config values come from the daemon's indexed `.ini` rows and preserve Unreal array operators.
- Config references currently extract clear `/Game/...` and `/Engine/...` path strings; broad roots such as `/Game/` are ignored.
- L1 reflection is targeted by asset path to avoid uncontrolled full-project loads.
- L2 Blueprint graph extraction currently covers editor source Graph/Node/Pin/Pin-link structure.
- Derived node-level flow edges currently classify Pin links as `exec_flows_to`, `data_flows_to`, or `delegate_flows_to`.
- L2 PCG extraction covers static PCG graph nodes, pins, edges, settings classes, editable setting summaries, optional subgraph references, and UniversalGraphIR. Generated runtime results and world actor/volume resolution are intentionally omitted from static scans.
- L2 MetaSound extraction covers Frontend Document metadata, root/subgraphs, nodes, input/output vertices, edges, interfaces, input literals, dependency classes, preset parent references, and UniversalGraphIR. Runtime operator execution, audio buffers, DSP state, live generator handles, audition output, and editor graph layout are intentionally omitted from static scans.
- Pin-link and projected-flow relations include attributes such as `source_pin_name`, `target_pin_name`, `branch_label`, and `projection`.
- Derived CFG basic blocks are generated from node-level execution flow; loop nesting and dominator analysis are not implemented yet.
- Derived DFG def-use covers direct data Pin links and variable read/write storage access; SSA versioning and path-sensitive def-use are not implemented yet.
- First-pass semantics currently cover call function, latent function, interface call, event, variable get/set, dynamic cast, SpawnActor, LoadAsset, Branch, Switch, Sequence, Macro Instance, collapsed Composite graphs, standard loop macros, and async task nodes.
- C++ source coverage is file/module/dependency level only; Clang AST symbol indexing is not implemented yet.
- World coverage currently reads loaded UWorld persistent levels, actors, components, static transforms, Actor GUIDs, folders, tags, owner/attachment links, LevelScriptActor links, ULevelStreaming descriptors, Level Instance world references, class instance relations, AWorldDataLayers/Data Layer container metadata, and World Partition actor descriptors from Asset Registry metadata in targeted map external actor/object packages. The checked ThirdPersonMap fixture has an empty WorldDataLayers container and no streaming sublevels or LevelInstance actors, so those readers are structurally implemented but not exercised by non-empty samples in this project; runtime actor state, actor-to-Data-Layer assignment expansion, region/Actor-GUID deep loading, World Partition runtime cell streaming state, and HLOD/runtime grid evaluation remain omitted.
- Enhanced Input coverage currently reads UInputAction metadata and UInputMappingContext action-key mappings; runtime mapping resolution and active input state remain omitted.
- CommonUI coverage currently reads CommonUI input data assets or Blueprint generated-class default objects, default click/back FDataTableRowHandle values, optional hold-data class references, optional Enhanced Input action references, and CommonUI generic input action table rows with keyboard/gamepad/touch key and hold metadata. Runtime CommonInput subsystem state, platform-specific resolution, live input reflector state, and widget action routing remain omitted.
- AI coverage currently reads Blackboard keys and parent references, BehaviorTree blackboard/node/composite/decorator/service/task structure, and EQS option/generator/test metadata. Runtime AIController/BrainComponent state, live blackboard values, path following, perception state, service tick state, and EQS scoring results remain omitted.
- StateTree coverage currently reads editor-authored state hierarchy, enter conditions, tasks, single-task state data, transitions and transition conditions, global evaluators/tasks, property bindings, external/context data descriptors, and compiled summary counts. Runtime execution state, evaluator/task instance memory, active state stack, event queues, and live transition evaluation remain omitted.
- Gameplay Ability System coverage currently reads GameplayAbility policy/tag/trigger/cost/cooldown metadata, GameplayEffect duration/period/tag/modifier/execution/cue/overflow/granted-ability metadata, and GameplayCueNotify static/actor notify tags. Runtime AbilitySystemComponent specs, active gameplay effects, prediction keys, aggregators, target data, gameplay event payloads, and cue runtime dispatch remain omitted.
- Revision coverage currently records one `asset_revision` row per asset entity in each ingested scan, stores a canonical content hash over the asset entity plus directly connected relations, records file-backed UE package fingerprints where the package can be resolved under `/Game` or project-plugin content, and closes the previous current revision for the same asset key. Diff coverage compares two already-ingested scan artifacts by stable entity/relation IDs, reports added/removed/changed entities and relations, summarizes Blueprint graph/node/pin/link, DataTable row/column/table, and Animation manifest deltas, and returns directly affected entities for impact review. Stale checks compare the stored scan hash against the current scan JSON path and compare package file size, mtime, and SHA-256 for resolved package files. Ingest records Git commit, branch, commit time, dirty state, and porcelain status when the project directory is a Git worktree; otherwise it records an unavailable reason. Editor incremental event coverage records package saves, asset add/remove/rename/update, and global Blueprint compile notifications into a JSONL event log. Automatic refresh jobs, rename/move heuristics, engine-plugin package fingerprint resolution, and deeper domain semantic diffing remain future work.
- Animation coverage currently reads Skeleton bone hierarchy, local/component reference poses, virtual bones, skeleton sockets, SkeletalMesh skeleton/LOD/material/morph metadata plus mesh/active sockets, AnimSequence track/notify/curve counts with first/mid/last frame-time raw-local and component-space pose samples plus root-motion/additive settings and full-range root motion transform, AnimMontage sections/slots/segment timelines, AnimComposite segment timelines and source-time mapping, BlendSpace axis/sample animation references, PoseAsset pose/curve/track/source-animation metadata, IK Rig chain/goal/solver metadata, IK Retargeter source/target rig, chain map, and current retarget pose metadata, PhysicsAsset body/shape/constraint/bone-binding metadata, AnimBlueprint static state-machine/state/transition/asset-player/cached-pose/slot summaries, and Control Rig Blueprint static RigVM graph/node/pin/link plus hierarchy element metadata; full raw track artifacts, compressed track data, runtime sample weights, runtime IK solving, runtime retarget evaluation, physics simulation, AnimBlueprint final-pose evaluation, and Control Rig runtime evaluation remain omitted.
- Data coverage currently reads DataAsset/PrimaryDataAsset class metadata, PrimaryAssetId, AssetManager rules and bundle members, UserDefinedStruct field types/layout metadata, UserDefinedEnum entries/display names, StringTable entries/source strings/metadata, DataTable row structs, columns, row names, exported field values, CompositeDataTable parent chains, CurveTable row metadata, CurveFloat, CurveVector, and CurveLinearColor channels, rich-curve keys, interpolation modes, tangent modes, tangent weights, extrapolation modes, and time/value ranges, plus CurveLinearColorAtlas dimensions, slots, and curve references; UserDefinedStruct default-instance values, enum editor history, atlas pixel data, material runtime bindings, and runtime curve evaluation remain omitted.
- UI coverage currently reads WidgetBlueprint generated-class WidgetTree archetype templates, parent-child hierarchy, named-slot names, WidgetAnimation names, and binding metadata; runtime UUserWidget instantiation, Slate widget trees, animation playback state, and live binding evaluation remain omitted.
- Niagara/VFX coverage currently reads System/Emitter/Script static metadata, emitter handles, script references, exposed and rapid-iteration parameter metadata, Parameter Definitions script-variable metadata, renderer properties, event handlers, simulation stages, compiled-script summary counts, and VectorFieldStatic dimensions/bounds/source bulk-data and CPU-data summaries only when those reads are already serialized and do not dirty packages; VM bytecode, HLSL, Niagara graph-node semantics, parameter default-value bytes, Parameter Map usage, VectorField vector samples, GPU volume textures, runtime simulation, GPU particle state, shader compilation, and live renderer state remain omitted. Engine-shipped Niagara System templates can be rejected by the dirty-package guard if Unreal upgrades or refreshes them during load; the checked Niagara golden fixtures use the zero-diagnostic `Fountain` Emitter template, `CoreParameters` Parameter Definitions, and `TilingCurl16` VectorFieldStatic.
- MetaSound coverage currently reads MetaSound Source/Patch Frontend Documents, root and subgraph classes, nodes, vertices, edges, interfaces, input literals, dependency classes, preset parent references, and a MetaSound-domain UniversalGraphIR projection. Runtime operator execution, graph compilation products, audio buffers, DSP state, live generator handles, audition output, editor graph layout, and rendered audio remain omitted.
- Audio coverage currently reads SoundCue node graphs, common SoundNode semantics for WavePlayer/Mixer/Random/Modulator/Branch/Switch/Attenuation/WaveParam/SoundClass nodes, SoundWave duration/sample-rate/channel/compression/loading/cue-point/subtitle metadata without decoding PCM, SoundSubmix hierarchy, parent/child references, effect-chain references, auto-disable/background-mute flags, envelope follower timing, and SoundSubmix effect preset class/color plus top-level editable setting summaries. Runtime audio buffers, spectral/envelope runtime values, recording output, effect instances, DSP state, SoundCue runtime parameter values, wave-instance parse state, compressed payload bytes, decoded PCM, and full nested effect schemas remain omitted.
- Cinematics coverage currently reads LevelSequence/MovieScene display and tick resolutions, playback/selection ranges, spawnables, possessables, object bindings, root and binding-owned tracks, sections, channels, per-channel key counts, key time rows, key artifact manifests, effective ranges, camera-cut binding references, subsequence references, audio section sound references, skeletal animation section animation references, binding tags, marked frames, root-folder count, and editor node-group count. Runtime bound world actors, evaluated camera state, director/event execution, Sequencer playback state, Control Rig runtime evaluation, and typed full channel value payloads remain omitted; large key-time payloads can be paged and materialized as daemon JSON artifacts.
- Render coverage currently reads StaticMesh LOD/material-slot metadata, Texture2D dimensions/mip/settings metadata, TextureCube dimensions/face/source summary metadata, and generic UTexture class/surface/source-summary metadata for subclasses such as MediaTexture; raw vertex/index buffers, source pixels, media frames, GPU resources, and runtime rendering state remain omitted.
- Material coverage currently reads Material and MaterialFunction expression graphs, expression input links, function references, texture references, MaterialInstance parameter overrides, and MaterialParameterCollection scalar/vector parameter defaults; raw HLSL, compiled shader maps, runtime collection instances, uniform-buffer layout, and runtime renderer state remain omitted.
- CFG loop analysis, SSA/path-sensitive DFG, deeper Niagara graph semantics, richer non-empty AI/GAS/StateTree fixtures, and UI productization remain future phases.
