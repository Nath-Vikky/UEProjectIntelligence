# UEPI Fixture Inventory

Primary in-project fixtures and stored scan artifacts cover:

- Blueprint: `BP_ThirdPersonCharacter`
- World: `ThirdPersonMap`, WorldDataLayers, World Partition descriptors
- Input/CommonUI: `IA_Move`, `IMC_Default`, `GenericInputData`, `GenericInputActionDataTable`
- AI/StateTree/GAS: `BB_UEPI_Agent`, `BT_UEPI_Patrol`, `EQS_UEPI_FindPoint`, `ST_UEPI_Minimal`, `GA_UEPI_Pulse`, `GE_UEPI_Pulse`, `GCN_UEPI_Pulse_Static`
- Animation: mannequin skeleton, skeletal mesh, anim sequence, blend space, pose asset, IK rig, retargeter, physics asset, AnimBP, Control Rig
- Data/UI/VFX: UDS, enum, DataTable, curves, CurveLinearColorAtlas, WidgetBlueprint, Niagara emitter, Niagara parameter definitions, VectorField
- PCG/MetaSound/Cinematics/Audio: SimpleForest PCG, MS_UEPI_Tone, LS_UEPI_Simple, SoundWave, SoundCue, SoundSubmix, SoundSubmix effect preset
- Render/Material: static mesh, textures, material, material instance, material function, material parameter collection

Golden summaries live in `Tests/golden`. README validation commands are the authoritative current regression list.
