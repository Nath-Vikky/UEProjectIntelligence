#pragma once

#include "CoreMinimal.h"
#include "UEPITypes.h"

class FJsonObject;

namespace UE::ProjectIntelligence
{
struct FSnapshotStorePaths
{
	FString RootDir;
	FString StoreDir;
	FString ObjectsDir;
	FString ArtifactsDir;
	FString ManifestsDir;
	FString SessionsDir;
	FString LocksDir;
	FString LogsDir;
	FString CacheDir;
};

struct FSnapshotCommitOptions
{
	FString DataMode = TEXT("saved");
	FString WriterMode = TEXT("editor");
	FString SessionId;
	FString SourceScanPath;
	bool bMergeWithExisting = false;
	TArray<FString> TargetObjectPaths;
};

struct FSnapshotCommitResult
{
	FString ManifestPath;
	FString VersionedManifestPath;
	FString ObjectPath;
	FString FragmentHash;
	int64 Generation = 0;
};

class UEPROJECTINTELLIGENCE_API FSnapshotStore
{
public:
	static FString DefaultRootDir();
	static FSnapshotStorePaths MakePaths(const FString& RootDir = FString());
	static bool EnsureLayout(const FSnapshotStorePaths& Paths, FText& OutError);
	static bool CommitProjectScan(
		const FProjectScanResult& ScanResult,
		const FSnapshotCommitOptions& Options,
		FSnapshotCommitResult& OutResult,
		FText& OutError);

private:
	static bool SaveJsonAtomically(const TSharedRef<FJsonObject>& Object, const FString& OutputPath, FText& OutError);
	static int64 ReadCurrentGeneration(const FString& ManifestPath);
};
}
