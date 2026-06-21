#include "UEPIAssetRegistryScanner.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "ProjectDescriptor.h"
#include "Scan/UEPIDirtyPackageGuard.h"
#include "Serialization/JsonSerializer.h"
#include "UEPISettings.h"
#include "UEPIAIReader.h"
#include "UEPIAudioReader.h"
#include "UEPIAnimationReader.h"
#include "UEPIBlueprintGraphReader.h"
#include "UEPICinematicsReader.h"
#include "UEPICommonUIReader.h"
#include "UEPIDataReader.h"
#include "UEPIGASReader.h"
#include "UEPIInputReader.h"
#include "UEPIMaterialReader.h"
#include "UEPIMetaSoundReader.h"
#include "UEPINiagaraReader.h"
#include "UEPIPCGReader.h"
#include "UEPIRenderAssetReader.h"
#include "UEPIStateTreeReader.h"
#include "UEPIUIReader.h"
#include "UEPIUObjectReflectionReader.h"
#include "UEPIWorldReader.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString TargetObjectPathToPackageName(const FString& TargetObjectPath)
{
	int32 DotIndex = INDEX_NONE;
	if (TargetObjectPath.FindChar(TEXT('.'), DotIndex))
	{
		return TargetObjectPath.Left(DotIndex);
	}

	int32 ColonIndex = INDEX_NONE;
	if (TargetObjectPath.FindChar(TEXT(':'), ColonIndex))
	{
		return TargetObjectPath.Left(ColonIndex);
	}

	return TargetObjectPath;
}

bool IsExternalWorldPartitionPackageForTarget(const FString& PackageName, const FString& TargetPackageName)
{
	if (!TargetPackageName.StartsWith(TEXT("/Game/")))
	{
		return false;
	}

	const FString RelativeTargetPackage = TargetPackageName.RightChop(6);
	return PackageName.StartsWith(TEXT("/Game/__ExternalObjects__/") + RelativeTargetPackage + TEXT("/")) ||
		PackageName.StartsWith(TEXT("/Game/__ExternalActors__/") + RelativeTargetPackage + TEXT("/"));
}

bool ShouldIncludePackage(const FString& PackageName, const FScanOptions& Options)
{
	if (Options.TargetObjectPaths.Num() > 0)
	{
		for (const FString& TargetObjectPath : Options.TargetObjectPaths)
		{
			const FString TargetPackageName = TargetObjectPathToPackageName(TargetObjectPath);
			if (TargetPackageName == PackageName ||
				TargetObjectPath.StartsWith(PackageName + TEXT(".")) ||
				IsExternalWorldPartitionPackageForTarget(PackageName, TargetPackageName))
			{
				return true;
			}
		}

		return false;
	}

	if (PackageName.StartsWith(TEXT("/Game")))
	{
		return Options.bIncludeGameContent;
	}

	if (PackageName.StartsWith(TEXT("/Engine")))
	{
		return Options.bIncludeEngineContent;
	}

	return Options.bIncludeProjectPluginContent;
}

FString GetProjectFile()
{
	FString ProjectFile = FPaths::GetProjectFilePath();
	return NormalizePathForUEPI(FPaths::ConvertRelativePathToFull(ProjectFile));
}

FString MakeProjectId()
{
	const FString CanonicalProjectFile = GetProjectFile();
	return MakeStableId(TEXT("uepi"), TEXT("project"), CanonicalProjectFile);
}

FString GetOutputPathFromOptions(const FScanOptions& Options)
{
	if (!Options.OutputPath.IsEmpty())
	{
		return NormalizePathForUEPI(FPaths::ConvertRelativePathToFull(Options.OutputPath));
	}

	return NormalizePathForUEPI(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("last_scan.json")));
}

FString AssetClassPathToString(const FAssetData& AssetData)
{
	return AssetData.AssetClassPath.ToString();
}

bool IsRedirectorAsset(const FAssetData& AssetData)
{
	const FString ClassPath = AssetClassPathToString(AssetData);
	return ClassPath.Equals(TEXT("/Script/CoreUObject.ObjectRedirector"), ESearchCase::IgnoreCase) ||
		ClassPath.EndsWith(TEXT(".ObjectRedirector"), ESearchCase::IgnoreCase) ||
		ClassPath.Contains(TEXT("ObjectRedirector"), ESearchCase::IgnoreCase);
}

FString RedirectorDestinationObjectPath(const FAssetData& AssetData)
{
	FString DestinationObjectPath;
	if (AssetData.GetTagValue(FName(TEXT("DestinationObject")), DestinationObjectPath))
	{
		return DestinationObjectPath;
	}

	if (AssetData.GetTagValue(FName(TEXT("DestinationObjectPath")), DestinationObjectPath))
	{
		return DestinationObjectPath;
	}

	return FString();
}

FString NormalizedFullPath(const FString& Path)
{
	return NormalizePathForUEPI(FPaths::ConvertRelativePathToFull(Path));
}

bool LoadJsonObjectFile(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *Path))
	{
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

void AddFilesystemEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::Filesystem),
		Path,
		Detail
	});
}

FEntityRecord MakeFilesystemEntity(
	const FString& ProjectId,
	const FString& Kind,
	const FString& CanonicalPath,
	const FString& DisplayName,
	const FString& EvidenceDetail)
{
	FEntityRecord Entity;
	Entity.Id = MakeStableId(ProjectId, Kind, CanonicalPath);
	Entity.Kind = Kind;
	Entity.CanonicalKey = CanonicalPath;
	Entity.DisplayName = DisplayName;
	Entity.SourceLayer = LexToString(ESourceLayer::Filesystem);
	Entity.Attributes.Add(TEXT("file_path"), CanonicalPath);
	Entity.Attributes.Add(TEXT("file_name"), FPaths::GetCleanFilename(CanonicalPath));
	Entity.Attributes.Add(TEXT("extension"), FPaths::GetExtension(CanonicalPath));
	Entity.Attributes.Add(TEXT("file_size"), FString::Printf(TEXT("%lld"), IFileManager::Get().FileSize(*CanonicalPath)));
	Entity.Attributes.Add(TEXT("modified_utc"), IFileManager::Get().GetTimeStamp(*CanonicalPath).ToIso8601());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Registry));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("file_metadata") };
	AddFilesystemEvidence(Entity, CanonicalPath, EvidenceDetail);
	return Entity;
}

void AddFilesystemRelation(
	const FString& ProjectId,
	const FString& Type,
	const FString& FromId,
	const FString& ToId,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FRelationRecord>& OutRelations)
{
	FRelationRecord Relation;
	Relation.Id = MakeRelationId(ProjectId, Type, FromId, ToId);
	Relation.Type = Type;
	Relation.FromId = FromId;
	Relation.ToId = ToId;
	Relation.SourceLayer = LexToString(ESourceLayer::Filesystem);
	Relation.bDerived = false;
	Relation.Confidence = 1.0f;
	Relation.Evidence.Add({
		LexToString(ESourceLayer::Filesystem),
		EvidencePath,
		EvidenceDetail
	});
	OutRelations.Add(MoveTemp(Relation));
}

void RestoreLoadedAssetPackageDirtyStateAfterRead(const UObject& Asset, bool bWasDirtyBeforeLoad)
{
	UPackage* Package = Asset.GetOutermost();
	if (Package && !bWasDirtyBeforeLoad && Package->IsDirty())
	{
		Package->SetDirtyFlag(false);
	}
}

void RestorePackageDirtyStateAfterRead(const FString& PackageName, bool bWasDirtyBeforeLoad)
{
	UPackage* Package = FindPackage(nullptr, *PackageName);
	if (Package && !bWasDirtyBeforeLoad && Package->IsDirty())
	{
		Package->SetDirtyFlag(false);
	}
}

TArray<FString> ExtractQuotedStrings(const FString& Text)
{
	TArray<FString> Values;
	int32 Index = 0;
	while (Index < Text.Len())
	{
		int32 OpenQuote = INDEX_NONE;
		if (!Text.FindChar(TEXT('"'), OpenQuote) || OpenQuote < Index)
		{
			const FString Remaining = Text.RightChop(Index);
			const int32 RelativeOpen = Remaining.Find(TEXT("\""));
			if (RelativeOpen == INDEX_NONE)
			{
				break;
			}
			OpenQuote = Index + RelativeOpen;
		}

		const FString AfterOpen = Text.RightChop(OpenQuote + 1);
		const int32 RelativeClose = AfterOpen.Find(TEXT("\""));
		if (RelativeClose == INDEX_NONE)
		{
			break;
		}

		const FString Value = AfterOpen.Left(RelativeClose);
		if (!Value.IsEmpty())
		{
			Values.AddUnique(Value);
		}
		Index = OpenQuote + RelativeClose + 2;
	}
	Values.Sort();
	return Values;
}

FString ModuleKey(const FString& OwnerPath, const FString& ModuleName)
{
	return TEXT("module:") + ModuleName;
}

FString ModuleReferenceKey(const FString& ModuleName)
{
	return TEXT("module_reference:") + ModuleName;
}

FString PluginReferenceKey(const FString& OwnerPath, const FString& PluginName)
{
	return OwnerPath + TEXT(":plugin_reference:") + PluginName;
}

FString AssetReferenceKey(const FString& ReferencePath)
{
	return TEXT("asset_reference:") + ReferencePath;
}

bool IsReferenceTerminator(TCHAR Character)
{
	return FChar::IsWhitespace(Character) ||
		Character == TEXT('"') ||
		Character == TEXT('\'') ||
		Character == TEXT(')') ||
		Character == TEXT('(') ||
		Character == TEXT(',') ||
		Character == TEXT(']') ||
		Character == TEXT('[') ||
		Character == TEXT(';');
}

void ExtractReferencesWithPrefix(const FString& Text, const FString& Prefix, TArray<FString>& OutReferences)
{
	int32 SearchIndex = 0;
	while (SearchIndex < Text.Len())
	{
		const int32 PrefixIndex = Text.Find(Prefix, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchIndex);
		if (PrefixIndex == INDEX_NONE)
		{
			break;
		}

		int32 EndIndex = PrefixIndex;
		while (EndIndex < Text.Len() && !IsReferenceTerminator(Text[EndIndex]))
		{
			++EndIndex;
		}

		FString Reference = Text.Mid(PrefixIndex, EndIndex - PrefixIndex);
		Reference.TrimStartAndEndInline();
		while (Reference.EndsWith(TEXT(".")) || Reference.EndsWith(TEXT(":")))
		{
			Reference = Reference.LeftChop(1);
		}
		if (!Reference.IsEmpty() && Reference != Prefix)
		{
			OutReferences.AddUnique(Reference);
		}

		SearchIndex = EndIndex + 1;
	}
}

TArray<FString> ExtractUnrealPathReferences(const FString& Text)
{
	TArray<FString> References;
	ExtractReferencesWithPrefix(Text, TEXT("/Game/"), References);
	ExtractReferencesWithPrefix(Text, TEXT("/Engine/"), References);
	References.Sort();
	return References;
}

FString InferModuleNameFromSourcePath(const FString& SourceRoot, const FString& SourcePath)
{
	FString RelativePath = SourcePath;
	FString NormalizedRoot = SourceRoot;
	FPaths::NormalizeFilename(RelativePath);
	FPaths::NormalizeFilename(NormalizedRoot);
	if (!NormalizedRoot.EndsWith(TEXT("/")))
	{
		NormalizedRoot += TEXT("/");
	}

	if (!RelativePath.StartsWith(NormalizedRoot))
	{
		return FString();
	}

	RelativePath = RelativePath.RightChop(NormalizedRoot.Len());
	TArray<FString> Parts;
	RelativePath.ParseIntoArray(Parts, TEXT("/"), true);
	if (Parts.Num() < 2)
	{
		return FString();
	}

	return Parts[0];
}

FString AddModuleEntity(
	const FString& ProjectId,
	const FString& OwnerPath,
	const FString& ModuleName,
	const FString& ModuleType,
	const FString& LoadingPhase,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = ModuleKey(OwnerPath, ModuleName);
	const FString ModuleId = MakeStableId(ProjectId, TEXT("module"), CanonicalKey);
	for (const FEntityRecord& Entity : OutEntities)
	{
		if (Entity.Id == ModuleId)
		{
			return ModuleId;
		}
	}

	FEntityRecord Entity;
	Entity.Id = ModuleId;
	Entity.Kind = TEXT("module");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = ModuleName;
	Entity.SourceLayer = LexToString(ESourceLayer::Filesystem);
	Entity.Attributes.Add(TEXT("module_name"), ModuleName);
	Entity.Attributes.Add(TEXT("module_type"), ModuleType);
	Entity.Attributes.Add(TEXT("loading_phase"), LoadingPhase);
	Entity.Attributes.Add(TEXT("descriptor_path"), OwnerPath);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Registry));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("module_descriptor") };
	AddFilesystemEvidence(Entity, EvidencePath, TEXT("Module entry declared in project or plugin descriptor."));
	OutEntities.Add(MoveTemp(Entity));
	return ModuleId;
}

FString AddReferenceEntity(
	const FString& ProjectId,
	const FString& Kind,
	const FString& CanonicalKey,
	const FString& DisplayName,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString EntityId = MakeStableId(ProjectId, Kind, CanonicalKey);
	for (const FEntityRecord& Entity : OutEntities)
	{
		if (Entity.Id == EntityId)
		{
			return EntityId;
		}
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = Kind;
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = DisplayName;
	Entity.SourceLayer = LexToString(ESourceLayer::Filesystem);
	Entity.Attributes.Add(TEXT("name"), DisplayName);
	if (Kind == TEXT("asset_reference"))
	{
		Entity.Attributes.Add(TEXT("reference_path"), DisplayName);
	}
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Registry));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("descriptor_reference") };
	AddFilesystemEvidence(Entity, EvidencePath, TEXT("External module or plugin reference declared by a project file."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddAssetRegistryReferenceEntity(
	const FString& ProjectId,
	const FString& CanonicalKey,
	const FString& DisplayName,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FEntityRecord>& OutEntities)
{
	const FString EntityId = MakeStableId(ProjectId, TEXT("asset_reference"), CanonicalKey);
	for (const FEntityRecord& Entity : OutEntities)
	{
		if (Entity.Id == EntityId)
		{
			return EntityId;
		}
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("asset_reference");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = DisplayName;
	Entity.SourceLayer = LexToString(ESourceLayer::AssetRegistry);
	Entity.Attributes.Add(TEXT("reference_path"), DisplayName);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Registry));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("asset_reference_metadata") };
	Entity.Evidence.Add({
		LexToString(ESourceLayer::AssetRegistry),
		EvidencePath,
		EvidenceDetail
	});
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

void AddAssetRegistryRelation(
	const FString& ProjectId,
	const FString& Type,
	const FString& FromId,
	const FString& ToId,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FRelationRecord>& OutRelations)
{
	FRelationRecord Relation;
	Relation.Id = MakeRelationId(ProjectId, Type, FromId, ToId);
	Relation.Type = Type;
	Relation.FromId = FromId;
	Relation.ToId = ToId;
	Relation.SourceLayer = LexToString(ESourceLayer::AssetRegistry);
	Relation.bDerived = false;
	Relation.Confidence = 1.0f;
	Relation.Evidence.Add({
		LexToString(ESourceLayer::AssetRegistry),
		EvidencePath,
		EvidenceDetail
	});
	OutRelations.Add(MoveTemp(Relation));
}

void AppendUnrealPathReferenceRelations(
	const FString& ProjectId,
	const FString& SourceId,
	const FString& RelationType,
	const FString& Text,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	for (const FString& ReferencePath : ExtractUnrealPathReferences(Text))
	{
		const FString ReferenceId = AddReferenceEntity(
			ProjectId,
			TEXT("asset_reference"),
			AssetReferenceKey(ReferencePath),
			ReferencePath,
			EvidencePath,
			OutEntities);
		AddFilesystemRelation(ProjectId, RelationType, SourceId, ReferenceId, EvidencePath, EvidenceDetail, OutRelations);
	}
}

void AppendModuleDependenciesFromBuildFile(
	const FString& ProjectId,
	const FString& ModuleId,
	const FString& BuildFilePath,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	FString BuildText;
	if (!FFileHelper::LoadFileToString(BuildText, *BuildFilePath))
	{
		return;
	}

	for (const FString& DependencyName : ExtractQuotedStrings(BuildText))
	{
		if (DependencyName.IsEmpty())
		{
			continue;
		}

		const FString DependencyId = AddReferenceEntity(
			ProjectId,
			TEXT("module_reference"),
			ModuleReferenceKey(DependencyName),
			DependencyName,
			BuildFilePath,
			OutEntities);
		AddFilesystemRelation(
			ProjectId,
			TEXT("module_depends_on"),
			ModuleId,
			DependencyId,
			BuildFilePath,
			TEXT("Module dependency name parsed from Build.cs string literals."),
			OutRelations);
	}
}

void AppendDescriptorModules(
	const FString& ProjectId,
	const FString& DescriptorPath,
	const FString& DescriptorId,
	const TSharedPtr<FJsonObject>& Descriptor,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
	if (!Descriptor.IsValid() || !Descriptor->TryGetArrayField(TEXT("Modules"), Modules))
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& ModuleValue : *Modules)
	{
		const TSharedPtr<FJsonObject> ModuleObject = ModuleValue.IsValid() ? ModuleValue->AsObject() : nullptr;
		if (!ModuleObject.IsValid())
		{
			continue;
		}

		const FString ModuleName = ModuleObject->GetStringField(TEXT("Name"));
		const FString ModuleType = ModuleObject->HasTypedField<EJson::String>(TEXT("Type")) ? ModuleObject->GetStringField(TEXT("Type")) : FString();
		const FString LoadingPhase = ModuleObject->HasTypedField<EJson::String>(TEXT("LoadingPhase")) ? ModuleObject->GetStringField(TEXT("LoadingPhase")) : FString();
		const FString ModuleId = AddModuleEntity(ProjectId, DescriptorPath, ModuleName, ModuleType, LoadingPhase, DescriptorPath, OutEntities);
		AddFilesystemRelation(ProjectId, TEXT("declares_module"), DescriptorId, ModuleId, DescriptorPath, TEXT("Descriptor declares an Unreal module."), OutRelations);
		AddFilesystemRelation(ProjectId, TEXT("declared_in"), ModuleId, DescriptorId, DescriptorPath, TEXT("Module declaration is stored in this descriptor."), OutRelations);
	}
}

void AppendProjectPluginReferences(
	const FString& ProjectId,
	const FString& ProjectDescriptorPath,
	const FString& ProjectDescriptorId,
	const TSharedPtr<FJsonObject>& ProjectDescriptor,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const TArray<TSharedPtr<FJsonValue>>* Plugins = nullptr;
	if (!ProjectDescriptor.IsValid() || !ProjectDescriptor->TryGetArrayField(TEXT("Plugins"), Plugins))
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& PluginValue : *Plugins)
	{
		const TSharedPtr<FJsonObject> PluginObject = PluginValue.IsValid() ? PluginValue->AsObject() : nullptr;
		if (!PluginObject.IsValid())
		{
			continue;
		}

		const FString PluginName = PluginObject->GetStringField(TEXT("Name"));
		const FString PluginId = AddReferenceEntity(
			ProjectId,
			TEXT("plugin_reference"),
			PluginReferenceKey(ProjectDescriptorPath, PluginName),
			PluginName,
			ProjectDescriptorPath,
			OutEntities);
		if (PluginObject->HasTypedField<EJson::Boolean>(TEXT("Enabled")))
		{
			if (FEntityRecord* PluginEntity = OutEntities.FindByPredicate([&PluginId](const FEntityRecord& Entity) { return Entity.Id == PluginId; }))
			{
				PluginEntity->Attributes.Add(TEXT("enabled"), PluginObject->GetBoolField(TEXT("Enabled")) ? TEXT("true") : TEXT("false"));
			}
		}
		AddFilesystemRelation(ProjectId, TEXT("plugin_depends_on"), ProjectDescriptorId, PluginId, ProjectDescriptorPath, TEXT("Project descriptor declares a plugin dependency."), OutRelations);
	}
}

void AppendProjectFileGraph(FProjectScanResult& Result)
{
	const FString ProjectDescriptorPath = Result.ProjectFile;
	TSharedPtr<FJsonObject> ProjectDescriptor;
	FEntityRecord ProjectEntity = MakeFilesystemEntity(
		Result.ProjectId,
		TEXT("project_descriptor"),
		ProjectDescriptorPath,
		FPaths::GetCleanFilename(ProjectDescriptorPath),
		TEXT("Project descriptor read from the filesystem."));
	ProjectEntity.Attributes.Add(TEXT("descriptor_type"), TEXT("uproject"));
	if (LoadJsonObjectFile(ProjectDescriptorPath, ProjectDescriptor))
	{
		ProjectEntity.Snapshot = ProjectDescriptor;
		ProjectEntity.Completeness.Covered.AddUnique(TEXT("descriptor_json"));
	}
	const FString ProjectDescriptorId = ProjectEntity.Id;
	Result.Entities.Add(MoveTemp(ProjectEntity));

	AppendDescriptorModules(Result.ProjectId, ProjectDescriptorPath, ProjectDescriptorId, ProjectDescriptor, Result.Entities, Result.Relations);
	AppendProjectPluginReferences(Result.ProjectId, ProjectDescriptorPath, ProjectDescriptorId, ProjectDescriptor, Result.Entities, Result.Relations);

	TArray<FString> PluginDescriptorPaths;
	IFileManager::Get().FindFilesRecursive(PluginDescriptorPaths, *FPaths::ProjectPluginsDir(), TEXT("*.uplugin"), true, false);
	PluginDescriptorPaths.Sort();
	for (const FString& PluginDescriptorFile : PluginDescriptorPaths)
	{
		const FString PluginDescriptorPath = NormalizedFullPath(PluginDescriptorFile);
		TSharedPtr<FJsonObject> PluginDescriptor;
		FEntityRecord PluginEntity = MakeFilesystemEntity(
			Result.ProjectId,
			TEXT("plugin_descriptor"),
			PluginDescriptorPath,
			FPaths::GetBaseFilename(PluginDescriptorPath),
			TEXT("Plugin descriptor read from the filesystem."));
		PluginEntity.Attributes.Add(TEXT("descriptor_type"), TEXT("uplugin"));
		if (LoadJsonObjectFile(PluginDescriptorPath, PluginDescriptor))
		{
			PluginEntity.Snapshot = PluginDescriptor;
			PluginEntity.Completeness.Covered.AddUnique(TEXT("descriptor_json"));
			if (PluginDescriptor->HasTypedField<EJson::String>(TEXT("FriendlyName")))
			{
				PluginEntity.DisplayName = PluginDescriptor->GetStringField(TEXT("FriendlyName"));
			}
		}

		const FString PluginDescriptorId = PluginEntity.Id;
		Result.Entities.Add(MoveTemp(PluginEntity));
		AddFilesystemRelation(Result.ProjectId, TEXT("contains_subobject"), ProjectDescriptorId, PluginDescriptorId, PluginDescriptorPath, TEXT("Project contains a project plugin descriptor."), Result.Relations);
		AppendDescriptorModules(Result.ProjectId, PluginDescriptorPath, PluginDescriptorId, PluginDescriptor, Result.Entities, Result.Relations);
	}

	TArray<FString> ConfigPaths;
	IFileManager::Get().FindFilesRecursive(ConfigPaths, *FPaths::ProjectConfigDir(), TEXT("*.ini"), true, false);
	ConfigPaths.Sort();
	for (const FString& ConfigFile : ConfigPaths)
	{
		const FString ConfigPath = NormalizedFullPath(ConfigFile);
		FEntityRecord ConfigEntity = MakeFilesystemEntity(Result.ProjectId, TEXT("config_file"), ConfigPath, FPaths::GetCleanFilename(ConfigPath), TEXT("Project configuration file discovered on disk."));
		const FString ConfigId = ConfigEntity.Id;
		Result.Entities.Add(MoveTemp(ConfigEntity));
		AddFilesystemRelation(Result.ProjectId, TEXT("configured_by"), ProjectDescriptorId, ConfigId, ConfigPath, TEXT("Project is configured by this ini file."), Result.Relations);

		FString ConfigText;
		if (FFileHelper::LoadFileToString(ConfigText, *ConfigPath))
		{
			AppendUnrealPathReferenceRelations(
				Result.ProjectId,
				ConfigId,
				TEXT("config_references_asset"),
				ConfigText,
				ConfigPath,
				TEXT("Unreal asset path reference parsed from a config file value."),
				Result.Entities,
				Result.Relations);
		}
	}

	TArray<FString> SourceRoots = { FPaths::Combine(FPaths::ProjectDir(), TEXT("Source")), FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UEProjectIntelligence"), TEXT("Source")) };
	for (const FString& SourceRoot : SourceRoots)
	{
		TArray<FString> SourceFiles;
		IFileManager::Get().FindFilesRecursive(SourceFiles, *SourceRoot, TEXT("*.*"), true, false);
		SourceFiles.Sort();
		for (const FString& SourceFile : SourceFiles)
		{
			const FString Extension = FPaths::GetExtension(SourceFile).ToLower();
			if (Extension != TEXT("cs") && Extension != TEXT("cpp") && Extension != TEXT("h"))
			{
				continue;
			}

			const FString SourcePath = NormalizedFullPath(SourceFile);
			FEntityRecord SourceEntity = MakeFilesystemEntity(Result.ProjectId, TEXT("source_file"), SourcePath, FPaths::GetCleanFilename(SourcePath), TEXT("Project source file discovered on disk."));
			const FString SourceId = SourceEntity.Id;
			Result.Entities.Add(MoveTemp(SourceEntity));

			const FString ModuleName = InferModuleNameFromSourcePath(NormalizedFullPath(SourceRoot), SourcePath);
			if (!ModuleName.IsEmpty())
			{
				const FString ModuleId = AddModuleEntity(Result.ProjectId, SourcePath, ModuleName, FString(), FString(), SourcePath, Result.Entities);
				AddFilesystemRelation(Result.ProjectId, TEXT("belongs_to_module"), SourceId, ModuleId, SourcePath, TEXT("Source file belongs to the module inferred from its Source/<ModuleName> path."), Result.Relations);
			}

			FString SourceText;
			if (FFileHelper::LoadFileToString(SourceText, *SourcePath))
			{
				AppendUnrealPathReferenceRelations(
					Result.ProjectId,
					SourceId,
					TEXT("possible_string_reference"),
					SourceText,
					SourcePath,
					TEXT("Possible Unreal asset path reference parsed from source text."),
					Result.Entities,
					Result.Relations);
			}

			if (Extension == TEXT("cs") && SourcePath.EndsWith(TEXT(".Build.cs")))
			{
				const FString BuildRulesModuleName = FPaths::GetBaseFilename(FPaths::GetBaseFilename(SourcePath));
				const FString ModuleId = AddModuleEntity(Result.ProjectId, SourcePath, BuildRulesModuleName, FString(), FString(), SourcePath, Result.Entities);
				AddFilesystemRelation(Result.ProjectId, TEXT("declared_in"), ModuleId, SourceId, SourcePath, TEXT("Build.cs declares Unreal module build rules."), Result.Relations);
				AddFilesystemRelation(Result.ProjectId, TEXT("implemented_in"), ModuleId, SourceId, SourcePath, TEXT("Module build rules are implemented in this source file."), Result.Relations);
				AppendModuleDependenciesFromBuildFile(Result.ProjectId, ModuleId, SourcePath, Result.Entities, Result.Relations);
			}
		}
	}
}

void AddDependencyRelations(
	IAssetRegistry& AssetRegistry,
	const FAssetData& AssetData,
	const FString& ProjectId,
	const TMap<FName, FString>& PackageNameToEntityId,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<FName> Dependencies;
	AssetRegistry.GetDependencies(AssetData.PackageName, Dependencies);

	for (const FName& DependencyName : Dependencies)
	{
		const FString* DependencyId = PackageNameToEntityId.Find(DependencyName);
		if (!DependencyId)
		{
			continue;
		}

		const FString* FromId = PackageNameToEntityId.Find(AssetData.PackageName);
		if (!FromId)
		{
			continue;
		}

		FRelationRecord Relation;
		Relation.Type = TEXT("hard_references");
		Relation.FromId = *FromId;
		Relation.ToId = *DependencyId;
		Relation.SourceLayer = LexToString(ESourceLayer::AssetRegistry);
		Relation.bDerived = false;
		Relation.Confidence = 1.0f;
		Relation.Id = MakeRelationId(ProjectId, Relation.Type, Relation.FromId, Relation.ToId);
		Relation.Evidence.Add({
			LexToString(ESourceLayer::AssetRegistry),
			AssetData.PackageName.ToString(),
			TEXT("Package dependency reported by Asset Registry.")
		});

		OutRelations.Add(MoveTemp(Relation));
	}
}
}

FScanOptions FAssetRegistryScanner::MakeOptionsFromSettings()
{
	const UUEPISettings* Settings = GetDefault<UUEPISettings>();

	FScanOptions Options;
	Options.bIncludeGameContent = Settings->bIncludeGameContent;
	Options.bIncludeProjectPluginContent = Settings->bIncludeProjectPluginContent;
	Options.bIncludeEngineContent = Settings->bIncludeEngineContent;
	Options.MaxAssetsPerBatch = Settings->MaxAssetsPerBatch;
	Options.MaxInlineCollectionItems = Settings->MaxInlineCollectionItems;
	return Options;
}

FProjectScanResult FAssetRegistryScanner::ScanProject(const FScanOptions& Options)
{
	FProjectScanResult Result;
	Result.ProjectName = FApp::GetProjectName();
	Result.ProjectFile = GetProjectFile();
	Result.ProjectId = MakeProjectId();
	Result.EngineVersion = FEngineVersion::Current().ToString();
	Result.StartedAtUtc = FDateTime::UtcNow().ToIso8601();
	Result.Completeness.State = ECompletenessState::MetadataOnly;
	Result.Completeness.Covered = {
		TEXT("asset_registry_assets"),
		TEXT("asset_paths"),
		TEXT("asset_class_paths"),
		TEXT("package_dependency_edges"),
		TEXT("project_descriptor"),
		TEXT("plugin_descriptors"),
		TEXT("source_files"),
		TEXT("config_files")
	};
	Result.Completeness.Omitted = {
		TEXT("uobject_reflection"),
		TEXT("blueprint_graphs"),
		TEXT("animation_data"),
		TEXT("runtime_evaluation")
	};

	if (Options.bReadUObjectReflection && Options.TargetObjectPaths.Num() > 0)
	{
		Result.Completeness.State = ECompletenessState::Partial;
		Result.Completeness.Covered.AddUnique(TEXT("uobject_reflection"));
		Result.Completeness.Omitted.Remove(TEXT("uobject_reflection"));
	}

	if (Options.bReadBlueprintGraphs && Options.TargetObjectPaths.Num() > 0)
	{
		Result.Completeness.State = ECompletenessState::Partial;
		Result.Completeness.Covered.AddUnique(TEXT("blueprint_graphs"));
		Result.Completeness.Covered.AddUnique(TEXT("node_level_flow_projection"));
		Result.Completeness.Covered.AddUnique(TEXT("cfg_basic_blocks"));
		Result.Completeness.Covered.AddUnique(TEXT("dfg_def_use"));
		Result.Completeness.Covered.AddUnique(TEXT("blueprint_semantics_first_pass"));
		Result.Completeness.Covered.AddUnique(TEXT("world_structure"));
		Result.Completeness.Covered.AddUnique(TEXT("enhanced_input_assets"));
		Result.Completeness.Covered.AddUnique(TEXT("animation_static_assets"));
		Result.Completeness.Covered.AddUnique(TEXT("data_static_assets"));
				Result.Completeness.Covered.AddUnique(TEXT("niagara_static_assets"));
				Result.Completeness.Covered.AddUnique(TEXT("render_static_assets"));
				Result.Completeness.Covered.AddUnique(TEXT("material_graphs"));
				Result.Completeness.Covered.AddUnique(TEXT("cinematics_static_assets"));
				Result.Completeness.Covered.AddUnique(TEXT("pcg_graphs"));
				Result.Completeness.Covered.AddUnique(TEXT("ui_widget_blueprints"));
		Result.Completeness.Omitted.Remove(TEXT("blueprint_graphs"));
		Result.Completeness.Omitted.Remove(TEXT("animation_data"));
		Result.Completeness.Omitted.Remove(TEXT("cfg_basic_blocks"));
		Result.Completeness.Omitted.Remove(TEXT("dfg_def_use"));
		Result.Completeness.Omitted.AddUnique(TEXT("cfg_loop_nesting"));
		Result.Completeness.Omitted.AddUnique(TEXT("ssa_path_sensitive_def_use"));
		Result.Completeness.Omitted.AddUnique(TEXT("world_partition_cells"));
	}

	AppendProjectFileGraph(Result);

	FDirtyPackageGuard DirtyGuard;

	if (Options.bReadUObjectReflection && Options.TargetObjectPaths.Num() == 0)
	{
		FDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("UEPI_REFLECTION_TARGET_REQUIRED");
		Diagnostic.Severity = TEXT("warning");
		Diagnostic.Message = TEXT("UObject reflection scan requested without target assets; falling back to L0 registry scan.");
		Result.Diagnostics.Add(MoveTemp(Diagnostic));
		Result.Completeness.Warnings.Add(TEXT("reflection_requires_target_assets"));
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	AssetRegistry.SearchAllAssets(true);

	TArray<FAssetData> AssetDataList;
	AssetRegistry.GetAllAssets(AssetDataList, true);
	AssetDataList.Sort([](const FAssetData& Left, const FAssetData& Right)
	{
		return Left.GetObjectPathString() < Right.GetObjectPathString();
	});

	TMap<FName, FString> PackageNameToEntityId;

	for (const FAssetData& AssetData : AssetDataList)
	{
		const FString PackageName = AssetData.PackageName.ToString();
		if (!ShouldIncludePackage(PackageName, Options))
		{
			continue;
		}

		const FString ObjectPath = AssetData.GetObjectPathString();
		const FString EntityId = MakeStableId(Result.ProjectId, TEXT("asset"), ObjectPath);
		PackageNameToEntityId.Add(AssetData.PackageName, EntityId);

		FEntityRecord Entity;
		Entity.Id = EntityId;
		Entity.Kind = TEXT("asset");
		Entity.CanonicalKey = ObjectPath;
		Entity.DisplayName = AssetData.AssetName.ToString();
		Entity.SourceLayer = LexToString(ESourceLayer::AssetRegistry);
		Entity.Attributes.Add(TEXT("object_path"), ObjectPath);
		Entity.Attributes.Add(TEXT("package_name"), PackageName);
		Entity.Attributes.Add(TEXT("package_path"), AssetData.PackagePath.ToString());
		Entity.Attributes.Add(TEXT("asset_name"), AssetData.AssetName.ToString());
		Entity.Attributes.Add(TEXT("asset_class_path"), AssetClassPathToString(AssetData));
		Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Registry));
		Entity.Completeness.State = ECompletenessState::MetadataOnly;
		Entity.Completeness.Covered = { TEXT("asset_registry_metadata") };
		Entity.Completeness.Omitted = { TEXT("loaded_uobject_properties") };
		Entity.Evidence.Add({
			LexToString(ESourceLayer::AssetRegistry),
			PackageName,
			TEXT("AssetData returned by Asset Registry without loading the UObject.")
		});

		if (IsRedirectorAsset(AssetData))
		{
			Entity.Kind = TEXT("asset_redirector");
			Entity.Attributes.Add(TEXT("is_redirector"), TEXT("true"));
			Entity.Completeness.Covered.AddUnique(TEXT("redirector_metadata"));
			Result.Completeness.Covered.AddUnique(TEXT("redirector_metadata"));

			const FString DestinationObjectPath = RedirectorDestinationObjectPath(AssetData);
			if (!DestinationObjectPath.IsEmpty())
			{
				Entity.Attributes.Add(TEXT("destination_object_path"), DestinationObjectPath);
				Entity.Attributes.Add(TEXT("destination_package_name"), TargetObjectPathToPackageName(DestinationObjectPath));
				const FString DestinationId = AddAssetRegistryReferenceEntity(
					Result.ProjectId,
					AssetReferenceKey(DestinationObjectPath),
					DestinationObjectPath,
					PackageName,
					TEXT("Redirector destination object path reported in Asset Registry tags."),
					Result.Entities);
				AddAssetRegistryRelation(
					Result.ProjectId,
					TEXT("redirects_to"),
					EntityId,
					DestinationId,
					PackageName,
					TEXT("ObjectRedirector destination read from Asset Registry tags."),
					Result.Relations);
			}
			else
			{
				Entity.Completeness.Warnings.Add(TEXT("redirector_destination_tag_missing"));
			}

			Entity.Evidence.Add({
				LexToString(ESourceLayer::AssetRegistry),
				PackageName,
				TEXT("Asset class identifies this package as an ObjectRedirector.")
			});
		}

		UPackage* ExistingLoadedPackageBeforeAssetRead = FindPackage(nullptr, *PackageName);
		const bool bWasDirtyBeforeAssetRead = ExistingLoadedPackageBeforeAssetRead && ExistingLoadedPackageBeforeAssetRead->IsDirty();

		if (Options.bReadUObjectReflection && Options.TargetObjectPaths.Num() > 0)
		{
			FUObjectReflectionReader::ReadAssetIntoEntity(AssetData, Options, Entity);
		}

		if (Options.bReadBlueprintGraphs && Options.TargetObjectPaths.Num() > 0)
		{
			const bool bAppendedWorldPartitionActorDesc = FWorldReader::AppendWorldPartitionActorDescAssetData(
				AssetData,
				Result.ProjectId,
				Entity,
				Result.Entities,
				Result.Relations);
			bool bHandledStructuralAsset = bAppendedWorldPartitionActorDesc;
			UObject* LoadedAsset = AssetData.GetAsset();

			if (LoadedAsset && FCommonUIReader::AppendCommonUIAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
				if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset))
				{
					FBlueprintGraphReader::AppendBlueprintGraph(*Blueprint, Result.ProjectId, Entity, Result.Entities, Result.Relations);
				}
			}
			else if (LoadedAsset && FUIReader::AppendUIAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
				if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset))
				{
					FBlueprintGraphReader::AppendBlueprintGraph(*Blueprint, Result.ProjectId, Entity, Result.Entities, Result.Relations);
				}
			}
			else if (LoadedAsset && FGASReader::AppendGASAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
				if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset))
				{
					FBlueprintGraphReader::AppendBlueprintGraph(*Blueprint, Result.ProjectId, Entity, Result.Entities, Result.Relations);
				}
			}
			else if (LoadedAsset && FStateTreeReader::AppendStateTreeAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
			}
			else if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset))
			{
				bHandledStructuralAsset = true;
				FBlueprintGraphReader::AppendBlueprintGraph(*Blueprint, Result.ProjectId, Entity, Result.Entities, Result.Relations);
			}
			else if (LoadedAsset && FWorldReader::AppendWorldAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
			}
			else if (LoadedAsset && FAIReader::AppendAIAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
			}
			else if (LoadedAsset && FAnimationReader::AppendAnimationAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
			}
			else if (LoadedAsset && FInputReader::AppendInputAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
			}
			else if (LoadedAsset && FDataReader::AppendDataAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
			}
			else if (LoadedAsset && FNiagaraReader::AppendNiagaraAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
			}
			else if (LoadedAsset && FPCGReader::AppendPCGAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
				RestoreLoadedAssetPackageDirtyStateAfterRead(*LoadedAsset, bWasDirtyBeforeAssetRead);
			}
			else if (LoadedAsset && FMaterialReader::AppendMaterialAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
			}
			else if (LoadedAsset && FMetaSoundReader::AppendMetaSoundAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
				RestoreLoadedAssetPackageDirtyStateAfterRead(*LoadedAsset, bWasDirtyBeforeAssetRead);
			}
			else if (LoadedAsset && FAudioReader::AppendAudioAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
			}
			else if (LoadedAsset && FCinematicsReader::AppendCinematicsAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
			}
			else if (LoadedAsset && FRenderAssetReader::AppendRenderAsset(*LoadedAsset, Result.ProjectId, Entity, Result.Entities, Result.Relations))
			{
				bHandledStructuralAsset = true;
			}

			if (!bHandledStructuralAsset)
			{
				FDiagnostic Diagnostic;
				Diagnostic.Code = TEXT("UEPI_L2_READER_MISSING");
				Diagnostic.Severity = TEXT("warning");
				Diagnostic.Message = TEXT("L2 structural scan requested for an asset without a specialized reader.");
				Diagnostic.Context.Add(TEXT("object_path"), AssetData.GetObjectPathString());
				Entity.Diagnostics.Add(MoveTemp(Diagnostic));
			}
		}

		if ((Options.bReadUObjectReflection || Options.bReadBlueprintGraphs) && Options.TargetObjectPaths.Num() > 0)
		{
			RestorePackageDirtyStateAfterRead(PackageName, bWasDirtyBeforeAssetRead);
		}

		Result.Entities.Add(MoveTemp(Entity));
	}

	for (const FAssetData& AssetData : AssetDataList)
	{
		if (!PackageNameToEntityId.Contains(AssetData.PackageName))
		{
			continue;
		}

		AddDependencyRelations(AssetRegistry, AssetData, Result.ProjectId, PackageNameToEntityId, Result.Relations);
	}

	TSet<FString> RelationIds;
	Result.Relations.RemoveAllSwap([&RelationIds](const FRelationRecord& Relation)
	{
		const bool bAlreadySeen = RelationIds.Contains(Relation.Id);
		RelationIds.Add(Relation.Id);
		return bAlreadySeen;
	});

	Result.Relations.Sort([](const FRelationRecord& Left, const FRelationRecord& Right)
	{
		return Left.Id < Right.Id;
	});

	TSet<FString> EntityIds;
	Result.Entities.RemoveAllSwap([&EntityIds](const FEntityRecord& Entity)
	{
		const bool bAlreadySeen = EntityIds.Contains(Entity.Id);
		EntityIds.Add(Entity.Id);
		return bAlreadySeen;
	});

	Result.Entities.Sort([](const FEntityRecord& Left, const FEntityRecord& Right)
	{
		return Left.Id < Right.Id;
	});

	TArray<FString> ChangedDirtyPackages;
	if (!DirtyGuard.ValidateUnchanged(ChangedDirtyPackages))
	{
		FDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("UEPI_DIRTY_PACKAGE_DETECTED");
		Diagnostic.Severity = TEXT("error");
		Diagnostic.Message = TEXT("Read-only scan changed the dirty package set.");
		Diagnostic.Context.Add(TEXT("changed_packages"), FString::Join(ChangedDirtyPackages, TEXT(",")));
		Result.Diagnostics.Add(MoveTemp(Diagnostic));
		Result.Completeness.Warnings.Add(TEXT("dirty_package_set_changed"));
		Result.Completeness.State = ECompletenessState::Partial;
	}

	Result.FinishedAtUtc = FDateTime::UtcNow().ToIso8601();
	return Result;
}

bool FAssetRegistryScanner::WriteScanResultJson(const FProjectScanResult& Result, const FString& OutputPath, FText& OutError)
{
	const FString FinalOutputPath = OutputPath.IsEmpty()
		? GetOutputPathFromOptions(FScanOptions())
		: NormalizePathForUEPI(FPaths::ConvertRelativePathToFull(OutputPath));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FinalOutputPath), true);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Result.ToJson(), Writer))
	{
		OutError = NSLOCTEXT("UEProjectIntelligence", "SerializeScanFailed", "Failed to serialize UEPI scan result.");
		return false;
	}

	if (!FFileHelper::SaveStringToFile(Json, *FinalOutputPath, FFileHelper::EEncodingOptions::ForceUTF8))
	{
		OutError = FText::Format(NSLOCTEXT("UEProjectIntelligence", "WriteScanFailed", "Failed to write UEPI scan result to {0}."), FText::FromString(FinalOutputPath));
		return false;
	}

	return true;
}
}
