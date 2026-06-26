#include "UEPISnapshotStore.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
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

	const TSharedRef<FJsonObject> ScanObject = ScanResult.ToJson();
	FString FragmentHash;
	FString FragmentPath;
	if (!SaveStoreObject(Paths, ScanResult.ProjectId, TEXT("project_scan_fragment"), ScanObject, FragmentHash, FragmentPath, OutError))
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
	FragmentObject->SetStringField(TEXT("kind"), TEXT("project_scan"));
	FragmentObject->SetStringField(TEXT("schema_version"), ScanResult.SchemaVersion);
	FragmentObject->SetStringField(TEXT("hash"), FragmentHash);
	FragmentObject->SetStringField(TEXT("path"), FragmentPath);
	FragmentObject->SetNumberField(TEXT("entity_count"), ScanResult.Entities.Num());
	FragmentObject->SetNumberField(TEXT("relation_count"), ScanResult.Relations.Num());
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
	Manifest->SetStringField(TEXT("merge_strategy"), Options.bMergeWithExisting ? TEXT("append_project_scan_fragments") : TEXT("replace"));
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
	OutResult.ObjectPath = FragmentPath;
	OutResult.FragmentHash = FragmentHash;
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
