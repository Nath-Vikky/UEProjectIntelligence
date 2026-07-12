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

		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");

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
			bool? DescriptorState = DescriptorPluginState(DescriptorPath, PluginName);
			if (DescriptorState.HasValue)
			{
				return DescriptorState.Value;
			}
		}

		return ProjectReferencesPlugin(Target, PluginName);
	}

	private bool ProjectReferencesPlugin(ReadOnlyTargetRules Target, string PluginName)
	{
		if (Target.ProjectFile == null)
		{
			return false;
		}

		string ProjectDirectory = Target.ProjectFile.Directory.FullName;
		foreach (string RelativeRoot in new string[] { "Config", "Source" })
		{
			string SearchRoot = Path.Combine(ProjectDirectory, RelativeRoot);
			if (!Directory.Exists(SearchRoot))
			{
				continue;
			}

			foreach (string FilePath in Directory.EnumerateFiles(SearchRoot, "*", SearchOption.AllDirectories))
			{
				string Extension = Path.GetExtension(FilePath);
				if (!Extension.Equals(".ini", StringComparison.OrdinalIgnoreCase) &&
					!Extension.Equals(".cs", StringComparison.OrdinalIgnoreCase) &&
					!Extension.Equals(".h", StringComparison.OrdinalIgnoreCase) &&
					!Extension.Equals(".hpp", StringComparison.OrdinalIgnoreCase) &&
					!Extension.Equals(".cpp", StringComparison.OrdinalIgnoreCase))
				{
					continue;
				}

				if (File.ReadAllText(FilePath).IndexOf(PluginName, StringComparison.OrdinalIgnoreCase) >= 0)
				{
					return true;
				}
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

	private bool? DescriptorPluginState(string DescriptorPath, string PluginName)
	{
		string Text = File.ReadAllText(DescriptorPath);
		string PluginObjectPattern = "\\{[^{}]*\"Name\"\\s*:\\s*\"" + Regex.Escape(PluginName) + "\"[^{}]*\\}";
		foreach (Match Match in Regex.Matches(Text, PluginObjectPattern, RegexOptions.IgnoreCase | RegexOptions.Singleline))
		{
			Match EnabledMatch = Regex.Match(Match.Value, "\"Enabled\"\\s*:\\s*(true|false)", RegexOptions.IgnoreCase);
			if (EnabledMatch.Success)
			{
				return EnabledMatch.Groups[1].Value.Equals("true", StringComparison.OrdinalIgnoreCase);
			}
		}

		return null;
	}
}
