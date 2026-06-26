#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "EditorSubsystem.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UEPIEditorSubsystem.generated.h"

USTRUCT(BlueprintType)
struct FUEPIIncrementalEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	int64 Sequence = 0;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString EventType;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString TimestampUtc;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString AssetPath;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString PackageName;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString ClassPath;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString OldObjectPath;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString PackageFileName;
};

USTRUCT(BlueprintType)
struct FUEPICollectorStatus
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString SessionPath;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString LastHeartbeatUtc;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	int32 PendingInvalidations = 0;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	int32 PendingRefreshRequests = 0;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	int32 IncrementalEvents = 0;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString LastAutoScanUtc;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString LastAutoScanMode;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString LastAutoScanManifestPath;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString LastRefreshRequestUtc;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString LastRefreshRequestPath;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString LastError;
};

UCLASS()
class UEPROJECTINTELLIGENCE_API UUEPIEditorSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	bool RunMetadataScan(FString OutputPath, FString& OutReportPath, FString& OutError);

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	bool RunTargetedSnapshotScan(const TArray<FString>& TargetObjectPaths, const FString& DataMode, FString& OutReportPath, FString& OutError);

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	void GetCollectorStatus(FUEPICollectorStatus& OutStatus) const;

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	void GetIncrementalEvents(TArray<FUEPIIncrementalEvent>& OutEvents) const;

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	void ClearIncrementalEvents();

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	bool WriteIncrementalEventsSnapshot(FString OutputPath, FString& OutReportPath, FString& OutError) const;

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	FString GetIncrementalEventLogPath() const;

private:
	void RegisterIncrementalDelegates();
	void UnregisterIncrementalDelegates();
	void RegisterLoadedBlueprintCompileDelegates();
	void RegisterBlueprintCompileDelegate(class UBlueprint* Blueprint);
	void StartLiveSession();
	void WriteLiveSessionState(const FString& State);
	void StopLiveSession();
	bool TickCollector(float DeltaTime);
	void EnqueueInvalidation(const FUEPIIncrementalEvent& Event);
	void ProcessInvalidationQueue();
	void ProcessRefreshRequests();
	void CommitAssetTombstoneFromEvent(const FUEPIIncrementalEvent& Event, const FString& Reason);

	void HandlePackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext ObjectSaveContext);
	void HandleAssetAdded(const struct FAssetData& AssetData);
	void HandleAssetRemoved(const struct FAssetData& AssetData);
	void HandleAssetRenamed(const struct FAssetData& AssetData, const FString& OldObjectPath);
	void HandleAssetUpdated(const struct FAssetData& AssetData);
	void HandleBlueprintCompiled(class UBlueprint* CompiledBlueprint);

	void RecordIncrementalEvent(
		const FString& EventType,
		const FString& AssetPath,
		const FString& PackageName,
		const FString& ClassPath,
		const FString& OldObjectPath,
		const FString& PackageFileName);

	TArray<FUEPIIncrementalEvent> IncrementalEvents;
	TMap<FString, FUEPIIncrementalEvent> PendingInvalidations;
	TMap<FString, double> PendingInvalidationTimes;
	int64 NextIncrementalEventSequence = 0;
	int64 NextAutoScanSequence = 0;
	FString LiveSessionId;
	FString LastHeartbeatUtc;
	FString LastAutoScanUtc;
	FString LastAutoScanMode;
	FString LastAutoScanManifestPath;
	FString LastRefreshRequestUtc;
	FString LastRefreshRequestPath;
	FString LastCollectorError;
	int32 PendingRefreshRequests = 0;
	double LastHeartbeatSeconds = 0.0;
	FTSTicker::FDelegateHandle CollectorTickerHandle;
	FDelegateHandle PackageSavedHandle;
	FDelegateHandle AssetAddedHandle;
	FDelegateHandle AssetRemovedHandle;
	FDelegateHandle AssetRenamedHandle;
	FDelegateHandle AssetUpdatedHandle;
};
