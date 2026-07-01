// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
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
				"AssetTools",
				"AssetRegistry",
				"AIModule",
				"AnimGraph",
				"AnimGraphRuntime",
				"BlueprintGraph",
				"CoreUObject",
				"EditorSubsystem",
				"Engine",
				"InputCore",
				"LevelEditor",
				"LevelSequence",
				"MessageLog",
				"MovieScene",
				"MovieSceneTracks",
				"Projects",
				"Networking",
				"Slate",
				"SlateCore",
				"Sockets",
				"ToolMenus",
				"UMG",
				"UMGEditor",
				"UnrealEd"
			}
			);

		AddOptionalReader(Target, "EnhancedInput", "UEPI_WITH_ENHANCED_INPUT", "EnhancedInput", "InputEditor");
		AddOptionalReader(Target, "CommonUI", "UEPI_WITH_COMMON_UI", "CommonInput", "CommonUI", "EnhancedInput");
		AddOptionalReader(Target, "GameplayAbilities", "UEPI_WITH_GAMEPLAY_ABILITIES", "GameplayAbilities", "GameplayTags", "GameplayTasks");
		AddOptionalReader(Target, "StateTree", "UEPI_WITH_STATE_TREE", "StateTreeEditorModule", "StateTreeModule", "StructUtils", "StructUtilsEngine");
		AddOptionalReader(Target, "IKRig", "UEPI_WITH_IK_RIG", "IKRig");
		AddOptionalReader(Target, "ControlRig", "UEPI_WITH_CONTROL_RIG", "ControlRig", "ControlRigDeveloper", "RigVM", "RigVMDeveloper");
		AddOptionalReader(Target, "Niagara", "UEPI_WITH_NIAGARA", "Niagara");
		AddOptionalReader(Target, "PCG", "UEPI_WITH_PCG", "PCG");
		AddOptionalReader(Target, "Metasound", "UEPI_WITH_METASOUND", "MetasoundEngine", "MetasoundFrontend");
	}

	private void AddOptionalReader(ReadOnlyTargetRules Target, string PluginName, string DefinitionName, params string[] ModuleNames)
	{
		bool bEnabled = IsOptionalPluginEnabled(Target, PluginName);
		PublicDefinitions.Add(DefinitionName + "=" + (bEnabled ? "1" : "0"));
		if (bEnabled)
		{
			PrivateDependencyModuleNames.AddRange(ModuleNames);
		}
	}

	private bool IsOptionalPluginEnabled(ReadOnlyTargetRules Target, string PluginName)
	{
		string OverrideValue = Environment.GetEnvironmentVariable("UEPI_OPTIONAL_READERS") ?? "";
		if (!string.IsNullOrWhiteSpace(OverrideValue))
		{
			foreach (string Token in OverrideValue.Split(new char[] { ',', ';', ' ' }, StringSplitOptions.RemoveEmptyEntries))
			{
				if (Token.Equals("all", StringComparison.OrdinalIgnoreCase) ||
					Token.Equals(PluginName, StringComparison.OrdinalIgnoreCase))
				{
					return true;
				}
			}
		}

		foreach (string DescriptorPath in ProjectDescriptorPaths(Target))
		{
			if (DescriptorEnablesPlugin(DescriptorPath, PluginName))
			{
				return true;
			}
		}

		return false;
	}

	private IEnumerable<string> ProjectDescriptorPaths(ReadOnlyTargetRules Target)
	{
		if (Target.ProjectFile != null && File.Exists(Target.ProjectFile.FullName))
		{
			yield return Target.ProjectFile.FullName;
		}
	}

	private bool DescriptorEnablesPlugin(string DescriptorPath, string PluginName)
	{
		string Text = File.ReadAllText(DescriptorPath);
		string PluginObjectPattern = "\\{[^{}]*\"Name\"\\s*:\\s*\"" + Regex.Escape(PluginName) + "\"[^{}]*\\}";
		foreach (Match Match in Regex.Matches(Text, PluginObjectPattern, RegexOptions.IgnoreCase | RegexOptions.Singleline))
		{
			if (Regex.IsMatch(Match.Value, "\"Enabled\"\\s*:\\s*true", RegexOptions.IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}
}
