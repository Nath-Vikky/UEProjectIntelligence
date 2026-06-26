// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UEProjectIntelligence : ModuleRules
{
	public UEProjectIntelligence(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"DeveloperSettings",
				"Json",
				"JsonUtilities"
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",
				"AIModule",
				"AnimGraph",
				"AnimGraphRuntime",
				"BlueprintGraph",
				"CommonInput",
				"CommonUI",
				"ControlRig",
				"ControlRigDeveloper",
				"CoreUObject",
				"EditorSubsystem",
				"EnhancedInput",
				"Engine",
				"GameplayAbilities",
				"GameplayTags",
				"GameplayTasks",
				"IKRig",
				"InputCore",
				"LevelEditor",
				"LevelSequence",
				"MetasoundEngine",
				"MetasoundFrontend",
				"MovieScene",
				"MovieSceneTracks",
				"Niagara",
				"PCG",
				"Projects",
				"RigVM",
				"RigVMDeveloper",
				"Slate",
				"SlateCore",
				"StateTreeEditorModule",
				"StateTreeModule",
				"StructUtils",
				"StructUtilsEngine",
				"ToolMenus",
				"UMG",
				"UMGEditor",
				"UnrealEd"
			}
			);
	}
}
