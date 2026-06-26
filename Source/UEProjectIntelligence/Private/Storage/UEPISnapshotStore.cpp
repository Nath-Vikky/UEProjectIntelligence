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
	const FString ScanJson = JsonObjectToString(ScanObject);
	const FString FragmentHash = MakeStableId(ScanResult.ProjectId, TEXT("project_scan_fragment"), ScanJson);
	const FString FragmentPrefix = FragmentHash.Left(2).IsEmpty() ? TEXT("00") : FragmentHash.Left(2);
	const FString FragmentDirectory = FPaths::Combine(Paths.ObjectsDir, FragmentPrefix);
	if (!IFileManager::Get().MakeDirectory(*FragmentDirectory, true))
	{
		OutError = FText::Format(
			NSLOCTEXT("UEProjectIntelligence", "SnapshotStoreObjectDirectoryFailed", "Failed to create UEPI Snapshot object directory: {0}."),
			FText::FromString(FragmentDirectory));
		return false;
	}

	const FString FragmentPath = FPaths::Combine(FragmentDirectory, FragmentHash + TEXT(".json"));
	if (!FPaths::FileExists(FragmentPath) && !SaveJsonAtomically(ScanObject, FragmentPath, OutError))
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

	const FString ManifestPath = FPaths::Combine(Paths.ManifestsDir, Options.DataMode.Equals(TEXT("live"), ESearchCase::IgnoreCase) ? TEXT("live.json") : TEXT("saved.json"));
	const int64 Generation = ReadCurrentGeneration(ManifestPath) + 1;
	const FString VersionedManifestPath = FPaths::Combine(
		Paths.ManifestsDir,
		FString::Printf(TEXT("%s-%lld.json"), Options.DataMode.Equals(TEXT("live"), ESearchCase::IgnoreCase) ? TEXT("live") : TEXT("saved"), Generation));

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

	TArray<TSharedPtr<FJsonValue>> FragmentValues;
	FragmentValues.Add(MakeShared<FJsonValueObject>(FragmentObject));

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
	Manifest->SetStringField(TEXT("created_at_utc"), FDateTime::UtcNow().ToIso8601());
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
