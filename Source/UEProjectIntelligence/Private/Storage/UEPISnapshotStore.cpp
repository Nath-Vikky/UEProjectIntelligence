#include "UEPISnapshotStore.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString NormalizeFullPath(const FString& Path)
{
	return NormalizePathForUEPI(FPaths::ConvertRelativePathToFull(Path));
}

FString JsonObjectToString(const TSharedRef<FJsonObject>& Object)
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Object, Writer);
	return Output;
}

TSharedPtr<FJsonObject> LoadJsonObject(const FString& Path)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Object))
	{
		return nullptr;
	}
	return Object;
}

bool SaveJsonAtomicallyInternal(const TSharedRef<FJsonObject>& Object, const FString& OutputPath, FText& OutError)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);

	const FString Json = JsonObjectToString(Object);
	const FString TempPath = OutputPath + TEXT(".tmp-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (!FFileHelper::SaveStringToFile(Json, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FText::Format(
			NSLOCTEXT("UEProjectIntelligence", "SnapshotStoreTempWriteFailed", "Failed to write temporary UEPI Snapshot file: {0}."),
			FText::FromString(TempPath));
		return false;
	}

	if (!IFileManager::Get().Move(*OutputPath, *TempPath, true, true))
	{
		IFileManager::Get().Delete(*TempPath);
		OutError = FText::Format(
			NSLOCTEXT("UEProjectIntelligence", "SnapshotStoreAtomicMoveFailed", "Failed to atomically replace UEPI Snapshot file: {0}."),
			FText::FromString(OutputPath));
		return false;
	}

	OutError = FText::GetEmpty();
	return true;
}

TArray<TSharedPtr<FJsonValue>> SnapshotStringArrayToJsonValues(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

TArray<TSharedPtr<FJsonValue>> EntityArrayToJsonValues(const TArray<FEntityRecord>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const FEntityRecord& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueObject>(Value.ToJson()));
	}
	return Result;
}

TArray<TSharedPtr<FJsonValue>> RelationArrayToJsonValues(const TArray<FRelationRecord>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const FRelationRecord& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueObject>(Value.ToJson()));
	}
	return Result;
}

TArray<TSharedPtr<FJsonValue>> DiagnosticArrayToJsonValues(const TArray<FDiagnostic>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const FDiagnostic& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueObject>(Value.ToJson()));
	}
	return Result;
}

TArray<TSharedPtr<FJsonValue>> CopyObjectArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	if (!Object.IsValid())
	{
		return Result;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return Result;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		if (!Value.IsValid())
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
		if (Value->TryGetObject(ObjectValue) && ObjectValue && ObjectValue->IsValid())
		{
			Result.Add(MakeShared<FJsonValueObject>(*ObjectValue));
		}
	}
	return Result;
}

TArray<TSharedPtr<FJsonValue>> CopyStringArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	if (!Object.IsValid())
	{
		return Result;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return Result;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString Text;
		if (Value.IsValid() && Value->TryGetString(Text))
		{
			Result.Add(MakeShared<FJsonValueString>(Text));
		}
	}
	return Result;
}

bool SaveStoreObject(
	const FSnapshotStorePaths& Paths,
	const FString& ProjectId,
	const FString& FragmentKind,
	const TSharedRef<FJsonObject>& Object,
	FString& OutHash,
	FString& OutPath,
	FText& OutError)
{
	const FString Json = JsonObjectToString(Object);
	OutHash = MakeStableId(ProjectId, FragmentKind, Json);
	const FString FragmentPrefix = OutHash.Left(2).IsEmpty() ? TEXT("00") : OutHash.Left(2);
	const FString FragmentDirectory = FPaths::Combine(Paths.ObjectsDir, FragmentPrefix);
	if (!IFileManager::Get().MakeDirectory(*FragmentDirectory, true))
	{
		OutError = FText::Format(
			NSLOCTEXT("UEProjectIntelligence", "SnapshotStoreObjectDirectoryFailed", "Failed to create UEPI Snapshot object directory: {0}."),
			FText::FromString(FragmentDirectory));
		return false;
	}

	OutPath = FPaths::Combine(FragmentDirectory, OutHash + TEXT(".json"));
	if (!FPaths::FileExists(OutPath) && !SaveJsonAtomicallyInternal(Object, OutPath, OutError))
	{
		return false;
	}
	return true;
}

bool IsAssetEntityKind(const FString& Kind)
{
	return Kind == TEXT("asset") || Kind == TEXT("asset_redirector");
}

TSharedRef<FJsonObject> MakeAssetFragmentObject(
	const FProjectScanResult& ScanResult,
	const FEntityRecord& AssetEntity,
	const TMap<FString, const FEntityRecord*>& EntityById,
	const TMultiMap<FString, const FRelationRecord*>& RelationsByEntity)
{
	TSet<FString> IncludedEntityIds;
	IncludedEntityIds.Add(AssetEntity.Id);

	TArray<FString> Queue;
	Queue.Add(AssetEntity.Id);
	for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
	{
		TArray<const FRelationRecord*> AdjacentRelations;
		RelationsByEntity.MultiFind(Queue[QueueIndex], AdjacentRelations);
		for (const FRelationRecord* Relation : AdjacentRelations)
		{
			if (!Relation)
			{
				continue;
			}

			const FString NextId = Relation->FromId == Queue[QueueIndex] ? Relation->ToId : Relation->FromId;
			if (IncludedEntityIds.Contains(NextId))
			{
				continue;
			}

			const FEntityRecord* const* NextEntity = EntityById.Find(NextId);
			if (!NextEntity || !*NextEntity)
			{
				continue;
			}

			if (IsAssetEntityKind((*NextEntity)->Kind))
			{
				continue;
			}

			IncludedEntityIds.Add(NextId);
			Queue.Add(NextId);
		}
	}

	TArray<FEntityRecord> FragmentEntities;
	FragmentEntities.Reserve(IncludedEntityIds.Num());
	for (const FString& EntityId : IncludedEntityIds)
	{
		const FEntityRecord* const* Entity = EntityById.Find(EntityId);
		if (Entity && *Entity)
		{
			FragmentEntities.Add(**Entity);
		}
	}
	FragmentEntities.Sort([](const FEntityRecord& Left, const FEntityRecord& Right)
	{
		return Left.Id < Right.Id;
	});

	TArray<FRelationRecord> FragmentRelations;
	for (const FRelationRecord& Relation : ScanResult.Relations)
	{
		const bool bFromIncluded = IncludedEntityIds.Contains(Relation.FromId);
		const bool bToIncluded = IncludedEntityIds.Contains(Relation.ToId);
		if ((bFromIncluded && bToIncluded) || Relation.FromId == AssetEntity.Id || Relation.ToId == AssetEntity.Id)
		{
			FragmentRelations.Add(Relation);
		}
	}
	FragmentRelations.Sort([](const FRelationRecord& Left, const FRelationRecord& Right)
	{
		return Left.Id < Right.Id;
	});

	TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
	AssetObject->SetStringField(TEXT("id"), AssetEntity.Id);
	AssetObject->SetStringField(TEXT("canonical_key"), AssetEntity.CanonicalKey);
	AssetObject->SetStringField(TEXT("display_name"), AssetEntity.DisplayName);
	AssetObject->SetStringField(TEXT("kind"), AssetEntity.Kind);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema_version"), TEXT("uepi.asset-fragment.v2"));
	Root->SetStringField(TEXT("project_id"), ScanResult.ProjectId);
	Root->SetStringField(TEXT("project_name"), ScanResult.ProjectName);
	Root->SetStringField(TEXT("project_file"), ScanResult.ProjectFile);
	Root->SetStringField(TEXT("engine_version"), ScanResult.EngineVersion);
	Root->SetStringField(TEXT("source_scan_finished_at_utc"), ScanResult.FinishedAtUtc);
	Root->SetObjectField(TEXT("asset"), AssetObject);
	Root->SetArrayField(TEXT("entities"), EntityArrayToJsonValues(FragmentEntities));
	Root->SetArrayField(TEXT("relations"), RelationArrayToJsonValues(FragmentRelations));
	Root->SetArrayField(TEXT("diagnostics"), DiagnosticArrayToJsonValues(ScanResult.Diagnostics));
	return Root;
}

TSharedRef<FJsonObject> MakeProjectFragmentObject(const FProjectScanResult& ScanResult)
{
	TSet<FString> ProjectEntityIds;
	TArray<FEntityRecord> FragmentEntities;
	for (const FEntityRecord& Entity : ScanResult.Entities)
	{
		if (IsAssetEntityKind(Entity.Kind))
		{
			continue;
		}
		ProjectEntityIds.Add(Entity.Id);
		FragmentEntities.Add(Entity);
	}
	FragmentEntities.Sort([](const FEntityRecord& Left, const FEntityRecord& Right)
	{
		return Left.Id < Right.Id;
	});

	TArray<FRelationRecord> FragmentRelations;
	for (const FRelationRecord& Relation : ScanResult.Relations)
	{
		if (ProjectEntityIds.Contains(Relation.FromId) && ProjectEntityIds.Contains(Relation.ToId))
		{
			FragmentRelations.Add(Relation);
		}
	}
	FragmentRelations.Sort([](const FRelationRecord& Left, const FRelationRecord& Right)
	{
		return Left.Id < Right.Id;
	});

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema_version"), TEXT("uepi.project-fragment.v2"));
	Root->SetStringField(TEXT("project_id"), ScanResult.ProjectId);
	Root->SetStringField(TEXT("project_name"), ScanResult.ProjectName);
	Root->SetStringField(TEXT("project_file"), ScanResult.ProjectFile);
	Root->SetStringField(TEXT("engine_version"), ScanResult.EngineVersion);
	Root->SetStringField(TEXT("source_scan_started_at_utc"), ScanResult.StartedAtUtc);
	Root->SetStringField(TEXT("source_scan_finished_at_utc"), ScanResult.FinishedAtUtc);
	Root->SetObjectField(TEXT("completeness"), ScanResult.Completeness.ToJson());
	Root->SetArrayField(TEXT("entities"), EntityArrayToJsonValues(FragmentEntities));
	Root->SetArrayField(TEXT("relations"), RelationArrayToJsonValues(FragmentRelations));
	Root->SetArrayField(TEXT("diagnostics"), DiagnosticArrayToJsonValues(ScanResult.Diagnostics));
	return Root;
}

TSharedRef<FJsonObject> MakeTombstoneObject(const FSnapshotTombstoneOptions& Options)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema_version"), TEXT("uepi.asset-tombstone.v2"));
	Root->SetStringField(TEXT("project_id"), Options.ProjectId);
	Root->SetStringField(TEXT("project_name"), Options.ProjectName);
	Root->SetStringField(TEXT("project_file"), Options.ProjectFile);
	Root->SetStringField(TEXT("engine_version"), Options.EngineVersion);
	Root->SetStringField(TEXT("created_at_utc"), FDateTime::UtcNow().ToIso8601());
	Root->SetStringField(TEXT("asset_key"), Options.AssetKey);
	Root->SetStringField(TEXT("asset_name"), Options.AssetName);
	Root->SetStringField(TEXT("asset_id"), Options.AssetId);
	Root->SetStringField(TEXT("package_name"), Options.PackageName);
	Root->SetStringField(TEXT("class_path"), Options.ClassPath);
	Root->SetStringField(TEXT("reason"), Options.Reason);
	Root->SetStringField(TEXT("event_type"), Options.EventType);
	Root->SetStringField(TEXT("old_object_path"), Options.OldObjectPath);
	Root->SetStringField(TEXT("new_object_path"), Options.NewObjectPath);
	Root->SetNumberField(TEXT("source_event_sequence"), static_cast<double>(Options.SourceEventSequence));
	return Root;
}

FString ProjectIdFromOptions(const FSnapshotTombstoneOptions& Options)
{
	if (!Options.ProjectId.IsEmpty())
	{
		return Options.ProjectId;
	}
	return MakeStableId(TEXT("uepi"), TEXT("project"), NormalizeFullPath(FPaths::GetProjectFilePath()));
}
}

FString FSnapshotStore::DefaultRootDir()
{
	return NormalizeFullPath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence")));
}

FSnapshotStorePaths FSnapshotStore::MakePaths(const FString& RootDir)
{
	FSnapshotStorePaths Paths;
	Paths.RootDir = RootDir.IsEmpty() ? DefaultRootDir() : NormalizeFullPath(RootDir);
	Paths.StoreDir = FPaths::Combine(Paths.RootDir, TEXT("store"));
	Paths.ObjectsDir = FPaths::Combine(Paths.StoreDir, TEXT("objects"));
	Paths.ArtifactsDir = FPaths::Combine(Paths.StoreDir, TEXT("artifacts"));
	Paths.ManifestsDir = FPaths::Combine(Paths.StoreDir, TEXT("manifests"));
	Paths.SessionsDir = FPaths::Combine(Paths.StoreDir, TEXT("sessions"));
	Paths.RequestsDir = FPaths::Combine(Paths.StoreDir, TEXT("requests"));
	Paths.LocksDir = FPaths::Combine(Paths.StoreDir, TEXT("locks"));
	Paths.LogsDir = FPaths::Combine(Paths.StoreDir, TEXT("logs"));
	Paths.CacheDir = FPaths::Combine(Paths.RootDir, TEXT("cache"));
	return Paths;
}

bool FSnapshotStore::EnsureLayout(const FSnapshotStorePaths& Paths, FText& OutError)
{
	const TArray<FString> Directories = {
		Paths.RootDir,
		Paths.StoreDir,
		Paths.ObjectsDir,
		Paths.ArtifactsDir,
		Paths.ManifestsDir,
		Paths.SessionsDir,
		Paths.RequestsDir,
		Paths.LocksDir,
		Paths.LogsDir,
		Paths.CacheDir
	};

	for (const FString& Directory : Directories)
	{
		if (Directory.IsEmpty() || !IFileManager::Get().MakeDirectory(*Directory, true))
		{
			OutError = FText::Format(
				NSLOCTEXT("UEProjectIntelligence", "SnapshotStoreDirectoryFailed", "Failed to create UEPI Snapshot Store directory: {0}."),
				FText::FromString(Directory));
			return false;
		}
	}

	OutError = FText::GetEmpty();
	return true;
}

bool FSnapshotStore::CommitProjectScan(
	const FProjectScanResult& ScanResult,
	const FSnapshotCommitOptions& Options,
	FSnapshotCommitResult& OutResult,
	FText& OutError)
{
	const FSnapshotStorePaths Paths = MakePaths();
	if (!EnsureLayout(Paths, OutError))
	{
		return false;
	}

	const TSharedRef<FJsonObject> ProjectFragmentObject = MakeProjectFragmentObject(ScanResult);
	FString ProjectFragmentHash;
	FString ProjectFragmentPath;
	if (!SaveStoreObject(Paths, ScanResult.ProjectId, TEXT("project_fragment"), ProjectFragmentObject, ProjectFragmentHash, ProjectFragmentPath, OutError))
	{
		return false;
	}

	TArray<FString> AssetEntityIds;
	AssetEntityIds.Reserve(ScanResult.Entities.Num());
	for (const FEntityRecord& Entity : ScanResult.Entities)
	{
		if (Entity.Kind == TEXT("asset") || Entity.Kind == TEXT("asset_redirector"))
		{
			AssetEntityIds.Add(Entity.Id);
		}
	}
	AssetEntityIds.Sort();

	const bool bLiveMode = Options.DataMode.Equals(TEXT("live"), ESearchCase::IgnoreCase);
	const FString ManifestPath = FPaths::Combine(Paths.ManifestsDir, bLiveMode ? TEXT("live.json") : TEXT("saved.json"));
	const int64 Generation = ReadCurrentGeneration(ManifestPath) + 1;
	const FString VersionedManifestPath = FPaths::Combine(
		Paths.ManifestsDir,
		FString::Printf(TEXT("%s-%lld.json"), bLiveMode ? TEXT("live") : TEXT("saved"), Generation));

	TSharedPtr<FJsonObject> ExistingManifest;
	if (Options.bMergeWithExisting)
	{
		ExistingManifest = LoadJsonObject(ManifestPath);
	}
	const TSharedPtr<FJsonObject> SavedBaselineManifest = bLiveMode
		? LoadJsonObject(FPaths::Combine(Paths.ManifestsDir, TEXT("saved.json")))
		: ExistingManifest;

	TSharedRef<FJsonObject> ProjectObject = MakeShared<FJsonObject>();
	ProjectObject->SetStringField(TEXT("id"), ScanResult.ProjectId);
	ProjectObject->SetStringField(TEXT("name"), ScanResult.ProjectName);
	ProjectObject->SetStringField(TEXT("project_file"), ScanResult.ProjectFile);
	ProjectObject->SetStringField(TEXT("engine_version"), ScanResult.EngineVersion);

	TSharedRef<FJsonObject> FragmentObject = MakeShared<FJsonObject>();
	FragmentObject->SetStringField(TEXT("kind"), TEXT("project_fragment"));
	FragmentObject->SetStringField(TEXT("schema_version"), TEXT("uepi.project-fragment.v2"));
	FragmentObject->SetStringField(TEXT("hash"), ProjectFragmentHash);
	FragmentObject->SetStringField(TEXT("path"), ProjectFragmentPath);
	FragmentObject->SetNumberField(TEXT("entity_count"), ProjectFragmentObject->GetArrayField(TEXT("entities")).Num());
	FragmentObject->SetNumberField(TEXT("relation_count"), ProjectFragmentObject->GetArrayField(TEXT("relations")).Num());
	FragmentObject->SetNumberField(TEXT("diagnostic_count"), ScanResult.Diagnostics.Num());

	TArray<TSharedPtr<FJsonValue>> FragmentValues = CopyObjectArrayField(ExistingManifest, TEXT("fragments"));
	FragmentValues.Add(MakeShared<FJsonValueObject>(FragmentObject));

	TMap<FString, const FEntityRecord*> EntityById;
	for (const FEntityRecord& Entity : ScanResult.Entities)
	{
		EntityById.Add(Entity.Id, &Entity);
	}

	TMultiMap<FString, const FRelationRecord*> RelationsByEntity;
	for (const FRelationRecord& Relation : ScanResult.Relations)
	{
		RelationsByEntity.Add(Relation.FromId, &Relation);
		RelationsByEntity.Add(Relation.ToId, &Relation);
	}

	for (const FEntityRecord& Entity : ScanResult.Entities)
	{
		if (!IsAssetEntityKind(Entity.Kind))
		{
			continue;
		}

		const TSharedRef<FJsonObject> AssetFragmentObject = MakeAssetFragmentObject(ScanResult, Entity, EntityById, RelationsByEntity);
		FString AssetFragmentHash;
		FString AssetFragmentPath;
		if (!SaveStoreObject(Paths, ScanResult.ProjectId, TEXT("asset_fragment"), AssetFragmentObject, AssetFragmentHash, AssetFragmentPath, OutError))
		{
			return false;
		}

		TSharedRef<FJsonObject> AssetFragmentRef = MakeShared<FJsonObject>();
		AssetFragmentRef->SetStringField(TEXT("kind"), TEXT("asset_fragment"));
		AssetFragmentRef->SetStringField(TEXT("schema_version"), TEXT("uepi.asset-fragment.v2"));
		AssetFragmentRef->SetStringField(TEXT("hash"), AssetFragmentHash);
		AssetFragmentRef->SetStringField(TEXT("path"), AssetFragmentPath);
		AssetFragmentRef->SetStringField(TEXT("asset_id"), Entity.Id);
		AssetFragmentRef->SetStringField(TEXT("asset_key"), Entity.CanonicalKey);
		AssetFragmentRef->SetStringField(TEXT("asset_name"), Entity.DisplayName);
		FragmentValues.Add(MakeShared<FJsonValueObject>(AssetFragmentRef));
	}

	TSharedRef<FJsonObject> CountsObject = MakeShared<FJsonObject>();
	CountsObject->SetNumberField(TEXT("entities"), ScanResult.Entities.Num());
	CountsObject->SetNumberField(TEXT("relations"), ScanResult.Relations.Num());
	CountsObject->SetNumberField(TEXT("diagnostics"), ScanResult.Diagnostics.Num());
	CountsObject->SetNumberField(TEXT("asset_entities"), AssetEntityIds.Num());

	TSharedRef<FJsonObject> SourceObject = MakeShared<FJsonObject>();
	SourceObject->SetStringField(TEXT("scan_started_at_utc"), ScanResult.StartedAtUtc);
	SourceObject->SetStringField(TEXT("scan_finished_at_utc"), ScanResult.FinishedAtUtc);
	if (!Options.SourceScanPath.IsEmpty())
	{
		SourceObject->SetStringField(TEXT("scan_path"), NormalizeFullPath(Options.SourceScanPath));
	}

	TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("schema_version"), TEXT("uepi.snapshot-manifest.v2"));
	Manifest->SetStringField(TEXT("data_mode"), Options.DataMode.IsEmpty() ? TEXT("saved") : Options.DataMode);
	Manifest->SetStringField(TEXT("writer_mode"), Options.WriterMode.IsEmpty() ? TEXT("editor") : Options.WriterMode);
	Manifest->SetStringField(TEXT("session_id"), Options.SessionId);
	Manifest->SetNumberField(TEXT("generation"), Generation);
	if (SavedBaselineManifest.IsValid())
	{
		double SavedGeneration = 0.0;
		if (SavedBaselineManifest->TryGetNumberField(TEXT("generation"), SavedGeneration))
		{
			Manifest->SetNumberField(TEXT("base_saved_generation"), SavedGeneration);
		}
	}
	Manifest->SetStringField(TEXT("created_at_utc"), FDateTime::UtcNow().ToIso8601());
	Manifest->SetBoolField(TEXT("is_overlay"), Options.TargetObjectPaths.Num() > 0 || bLiveMode);
	Manifest->SetStringField(TEXT("merge_strategy"), Options.bMergeWithExisting ? TEXT("append_snapshot_fragments") : TEXT("replace"));
	Manifest->SetStringField(TEXT("counts_scope"), Options.bMergeWithExisting ? TEXT("latest_fragment") : TEXT("manifest"));
	Manifest->SetArrayField(TEXT("target_object_paths"), SnapshotStringArrayToJsonValues(Options.TargetObjectPaths));
	Manifest->SetObjectField(TEXT("project"), ProjectObject);
	Manifest->SetObjectField(TEXT("counts"), CountsObject);
	Manifest->SetObjectField(TEXT("source"), SourceObject);
	Manifest->SetObjectField(TEXT("completeness"), ScanResult.Completeness.ToJson());
	Manifest->SetArrayField(TEXT("asset_entity_ids"), SnapshotStringArrayToJsonValues(AssetEntityIds));
	Manifest->SetArrayField(TEXT("fragments"), FragmentValues);

	if (!SaveJsonAtomically(Manifest, VersionedManifestPath, OutError))
	{
		return false;
	}

	if (!SaveJsonAtomically(Manifest, ManifestPath, OutError))
	{
		return false;
	}

	OutResult.ManifestPath = ManifestPath;
	OutResult.VersionedManifestPath = VersionedManifestPath;
	OutResult.ObjectPath = ProjectFragmentPath;
	OutResult.FragmentHash = ProjectFragmentHash;
	OutResult.Generation = Generation;
	OutError = FText::GetEmpty();
	return true;
}

bool FSnapshotStore::CommitAssetTombstone(
	const FSnapshotTombstoneOptions& Options,
	FSnapshotCommitResult& OutResult,
	FText& OutError)
{
	FSnapshotTombstoneOptions NormalizedOptions = Options;
	NormalizedOptions.ProjectId = ProjectIdFromOptions(NormalizedOptions);
	if (NormalizedOptions.ProjectName.IsEmpty())
	{
		NormalizedOptions.ProjectName = FApp::GetProjectName();
	}
	if (NormalizedOptions.ProjectFile.IsEmpty())
	{
		NormalizedOptions.ProjectFile = NormalizeFullPath(FPaths::GetProjectFilePath());
	}
	if (NormalizedOptions.EngineVersion.IsEmpty())
	{
		NormalizedOptions.EngineVersion = FEngineVersion::Current().ToString();
	}
	if (NormalizedOptions.AssetKey.IsEmpty())
	{
		NormalizedOptions.AssetKey = !NormalizedOptions.OldObjectPath.IsEmpty() ? NormalizedOptions.OldObjectPath : NormalizedOptions.PackageName;
	}
	if (NormalizedOptions.AssetName.IsEmpty())
	{
		NormalizedOptions.AssetName = FPaths::GetBaseFilename(NormalizedOptions.AssetKey);
	}
	if (NormalizedOptions.AssetId.IsEmpty() && !NormalizedOptions.AssetKey.IsEmpty())
	{
		NormalizedOptions.AssetId = MakeStableId(NormalizedOptions.ProjectId, TEXT("asset"), NormalizedOptions.AssetKey);
	}

	const FSnapshotStorePaths Paths = MakePaths();
	if (!EnsureLayout(Paths, OutError))
	{
		return false;
	}

	const bool bLiveMode = NormalizedOptions.DataMode.Equals(TEXT("live"), ESearchCase::IgnoreCase);
	const FString ManifestPath = FPaths::Combine(Paths.ManifestsDir, bLiveMode ? TEXT("live.json") : TEXT("saved.json"));
	const int64 Generation = ReadCurrentGeneration(ManifestPath) + 1;
	const FString VersionedManifestPath = FPaths::Combine(
		Paths.ManifestsDir,
		FString::Printf(TEXT("%s-%lld.json"), bLiveMode ? TEXT("live") : TEXT("saved"), Generation));

	TSharedPtr<FJsonObject> ExistingManifest;
	if (NormalizedOptions.bMergeWithExisting)
	{
		ExistingManifest = LoadJsonObject(ManifestPath);
	}
	const TSharedPtr<FJsonObject> SavedBaselineManifest = bLiveMode
		? LoadJsonObject(FPaths::Combine(Paths.ManifestsDir, TEXT("saved.json")))
		: ExistingManifest;

	const TSharedRef<FJsonObject> TombstoneObject = MakeTombstoneObject(NormalizedOptions);
	FString TombstoneHash;
	FString TombstonePath;
	if (!SaveStoreObject(Paths, NormalizedOptions.ProjectId, TEXT("asset_tombstone"), TombstoneObject, TombstoneHash, TombstonePath, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> TombstoneRef = MakeShared<FJsonObject>();
	TombstoneRef->SetStringField(TEXT("kind"), TEXT("asset_tombstone"));
	TombstoneRef->SetStringField(TEXT("schema_version"), TEXT("uepi.asset-tombstone.v2"));
	TombstoneRef->SetStringField(TEXT("hash"), TombstoneHash);
	TombstoneRef->SetStringField(TEXT("path"), TombstonePath);
	TombstoneRef->SetStringField(TEXT("asset_id"), NormalizedOptions.AssetId);
	TombstoneRef->SetStringField(TEXT("asset_key"), NormalizedOptions.AssetKey);
	TombstoneRef->SetStringField(TEXT("asset_name"), NormalizedOptions.AssetName);
	TombstoneRef->SetStringField(TEXT("reason"), NormalizedOptions.Reason);

	TArray<TSharedPtr<FJsonValue>> FragmentValues = CopyObjectArrayField(ExistingManifest, TEXT("fragments"));
	FragmentValues.Add(MakeShared<FJsonValueObject>(TombstoneRef));

	TSharedRef<FJsonObject> ProjectObject = MakeShared<FJsonObject>();
	ProjectObject->SetStringField(TEXT("id"), NormalizedOptions.ProjectId);
	ProjectObject->SetStringField(TEXT("name"), NormalizedOptions.ProjectName);
	ProjectObject->SetStringField(TEXT("project_file"), NormalizedOptions.ProjectFile);
	ProjectObject->SetStringField(TEXT("engine_version"), NormalizedOptions.EngineVersion);

	TSharedRef<FJsonObject> CountsObject = MakeShared<FJsonObject>();
	if (ExistingManifest.IsValid() && ExistingManifest->HasTypedField<EJson::Object>(TEXT("counts")))
	{
		CountsObject = ExistingManifest->GetObjectField(TEXT("counts")).ToSharedRef();
	}
	else
	{
		CountsObject->SetNumberField(TEXT("entities"), 0);
		CountsObject->SetNumberField(TEXT("relations"), 0);
		CountsObject->SetNumberField(TEXT("diagnostics"), 0);
		CountsObject->SetNumberField(TEXT("asset_entities"), 0);
	}

	TSharedRef<FJsonObject> SourceObject = MakeShared<FJsonObject>();
	SourceObject->SetStringField(TEXT("event_type"), NormalizedOptions.EventType);
	SourceObject->SetStringField(TEXT("asset_key"), NormalizedOptions.AssetKey);
	SourceObject->SetStringField(TEXT("old_object_path"), NormalizedOptions.OldObjectPath);
	SourceObject->SetStringField(TEXT("new_object_path"), NormalizedOptions.NewObjectPath);

	TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("schema_version"), TEXT("uepi.snapshot-manifest.v2"));
	Manifest->SetStringField(TEXT("data_mode"), NormalizedOptions.DataMode.IsEmpty() ? TEXT("saved") : NormalizedOptions.DataMode);
	Manifest->SetStringField(TEXT("writer_mode"), NormalizedOptions.WriterMode.IsEmpty() ? TEXT("editor") : NormalizedOptions.WriterMode);
	Manifest->SetStringField(TEXT("session_id"), NormalizedOptions.SessionId);
	Manifest->SetNumberField(TEXT("generation"), Generation);
	if (SavedBaselineManifest.IsValid())
	{
		double SavedGeneration = 0.0;
		if (SavedBaselineManifest->TryGetNumberField(TEXT("generation"), SavedGeneration))
		{
			Manifest->SetNumberField(TEXT("base_saved_generation"), SavedGeneration);
		}
	}
	Manifest->SetStringField(TEXT("created_at_utc"), FDateTime::UtcNow().ToIso8601());
	Manifest->SetBoolField(TEXT("is_overlay"), bLiveMode);
	Manifest->SetStringField(TEXT("merge_strategy"), NormalizedOptions.bMergeWithExisting ? TEXT("append_snapshot_fragments") : TEXT("replace"));
	Manifest->SetStringField(TEXT("counts_scope"), NormalizedOptions.bMergeWithExisting ? TEXT("current_view") : TEXT("manifest"));
	TArray<FString> TargetObjectPaths;
	TargetObjectPaths.Add(NormalizedOptions.AssetKey);
	Manifest->SetArrayField(TEXT("target_object_paths"), SnapshotStringArrayToJsonValues(TargetObjectPaths));
	Manifest->SetObjectField(TEXT("project"), ProjectObject);
	Manifest->SetObjectField(TEXT("counts"), CountsObject);
	Manifest->SetObjectField(TEXT("source"), SourceObject);
	Manifest->SetArrayField(TEXT("asset_entity_ids"), CopyStringArrayField(ExistingManifest, TEXT("asset_entity_ids")));
	Manifest->SetArrayField(TEXT("fragments"), FragmentValues);

	if (!SaveJsonAtomically(Manifest, VersionedManifestPath, OutError))
	{
		return false;
	}
	if (!SaveJsonAtomically(Manifest, ManifestPath, OutError))
	{
		return false;
	}

	OutResult.ManifestPath = ManifestPath;
	OutResult.VersionedManifestPath = VersionedManifestPath;
	OutResult.ObjectPath = TombstonePath;
	OutResult.FragmentHash = TombstoneHash;
	OutResult.Generation = Generation;
	OutError = FText::GetEmpty();
	return true;
}

bool FSnapshotStore::SaveJsonAtomically(const TSharedRef<FJsonObject>& Object, const FString& OutputPath, FText& OutError)
{
	return SaveJsonAtomicallyInternal(Object, OutputPath, OutError);
}

int64 FSnapshotStore::ReadCurrentGeneration(const FString& ManifestPath)
{
	const TSharedPtr<FJsonObject> Manifest = LoadJsonObject(ManifestPath);
	if (!Manifest.IsValid())
	{
		return 0;
	}

	double Generation = 0.0;
	if (!Manifest->TryGetNumberField(TEXT("generation"), Generation))
	{
		return 0;
	}
	return static_cast<int64>(Generation);
}
}
