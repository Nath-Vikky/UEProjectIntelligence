# UEPI Tests

This folder stores lightweight conformance assets that can run without launching Unreal Editor.

- `golden/*.summary.json` files are scan-summary fixtures consumed by `Tools/validate_scan.py --golden`.
- Full scan JSON artifacts are generated under the project `Saved/UEProjectIntelligence` directory and are not checked into this plugin folder.

Current fixtures cover:

- `bp_third_person_character_l2.summary.json` for targeted Blueprint graph/semantic extraction.
- `third_person_map_l2.summary.json` for targeted World/Level/Actor/Component, LevelScriptActor, attach/ownership, Streaming Level, and Level Instance reference extraction.
- `third_person_map_world_data_layers_l2.summary.json` for targeted WorldDataLayers/Data Layer container extraction.
- `third_person_map_world_partition_l2.summary.json` for targeted World Partition Actor Descriptor extraction.
- `ia_move_l2.summary.json` for targeted Enhanced Input Action extraction.
- `imc_default_l2.summary.json` for targeted Enhanced Input Mapping Context extraction.
- `common_ui_input_data_l2.summary.json` for targeted CommonUI input data and input action table extraction.
- `blackboard_uepi_agent_l2.summary.json` for targeted Blackboard key/type extraction.
- `behavior_tree_uepi_patrol_l2.summary.json` for targeted BehaviorTree asset extraction.
- `env_query_uepi_find_point_l2.summary.json` for targeted EQS asset extraction.
- `state_tree_uepi_minimal_l2.summary.json` for targeted StateTree asset extraction.
- `gameplay_ability_uepi_pulse_l2.summary.json` for targeted GameplayAbility Blueprint extraction.
- `gameplay_effect_uepi_pulse_l2.summary.json` for targeted GameplayEffect Blueprint extraction.
- `gameplay_cue_notify_uepi_pulse_static_l2.summary.json` for targeted GameplayCueNotify Static Blueprint extraction.
- `sk_mannequin_l2.summary.json` for targeted Skeleton bone hierarchy extraction.
- `skm_manny_l2.summary.json` for targeted SkeletalMesh metadata extraction.
- `mm_walk_fwd_l2.summary.json` for targeted AnimSequence track metadata extraction.
- `bs_mm_walkrun_l2.summary.json` for targeted BlendSpace axis/sample extraction.
- `manny_foot_l_pose_l2.summary.json` for targeted PoseAsset pose/track/source-animation extraction.
- `ik_mannequin_l2.summary.json` for targeted IK Rig chain/goal/solver extraction.
- `rtg_mannequin_l2.summary.json` for targeted IK Retargeter source/target rig and chain-map extraction.
- `pa_mannequin_l2.summary.json` for targeted PhysicsAsset body/shape/constraint/bone-binding extraction.
- `abp_manny_l2.summary.json` for targeted AnimBlueprint state-machine/transition/asset-player extraction.
- `cr_mannequin_basic_foot_ik_l2.summary.json` for targeted Control Rig Blueprint RigVM graph/hierarchy extraction.
- `area_lights_struct_l2.summary.json` for targeted UserDefinedStruct field metadata extraction.
- `eniagara_angle_input_l2.summary.json` for targeted UserDefinedEnum entry metadata extraction.
- `area_lights_table_l2.summary.json` for targeted DataTable row/schema extraction.
- `chromatic_curve_l2.summary.json` for targeted Curve channel/key/tangent extraction.
- `ca_mannequin_l2.summary.json` for targeted CurveLinearColorAtlas slot/reference extraction.
- `audio_button_matrix_l2.summary.json` for targeted WidgetBlueprint static WidgetTree extraction.
- `fountain_l2.summary.json` for targeted Niagara Emitter renderer/script metadata extraction.
- `core_parameters_l2.summary.json` for targeted Niagara Parameter Definitions script-variable metadata extraction.
- `tiling_curl16_l2.summary.json` for targeted VectorFieldStatic dimensions/bounds/source-data extraction.
- `pcg_simple_forest_l2.summary.json` for targeted PCG graph node/pin/edge/settings extraction plus UniversalGraphIR projection.
- `metasound_uepi_tone_l2.summary.json` for targeted MetaSound Frontend Document node/vertex/edge/interface/literal/dependency extraction plus UniversalGraphIR projection.
- `level_sequence_uepi_simple_l2.summary.json` for targeted LevelSequence/MovieScene binding, camera-cut, transform-track, section, channel, key, and marked-frame extraction.
- `sound_wave_uepi_tone_l2.summary.json` for targeted SoundWave duration/sample-rate/channel/subtitle metadata extraction.
- `sound_cue_uepi_tone_l2.summary.json` for targeted SoundCue Mixer-to-WavePlayer node graph and SoundWave reference extraction.
- `resonance_submix_default_l2.summary.json` for targeted SoundSubmix hierarchy/effect-chain extraction.
- `resonance_reverb_default_l2.summary.json` for targeted SoundSubmix effect preset setting-summary extraction.
- `sm_cube_l2.summary.json` for targeted StaticMesh LOD/material-slot extraction.
- `t_gridchecker_a_l2.summary.json` for targeted Texture2D metadata extraction.
- `scene_preview_cube_l2.summary.json` for targeted TextureCube metadata extraction.
- `t_media_plate_l2.summary.json` for targeted generic UTexture metadata extraction.
- `m_prototype_grid_l2.summary.json` for targeted Material expression graph extraction.
- `mi_prototype_grid_gray_l2.summary.json` for targeted MaterialInstance parameter override extraction.
- `mf_proc_grid_l2.summary.json` for targeted MaterialFunction expression graph extraction.
- `qmpc_global_foliage_actor_l2.summary.json` for targeted MaterialParameterCollection default-parameter extraction.

The checked BehaviorTree, EQS, StateTree, and Gameplay Ability System fixtures are valid commandlet-generated minimal assets. Richer authored samples with non-empty tree nodes, EQS options/tests, StateTree states, and registered GameplayTags remain useful future regression targets.
