#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "EditorSubsystem.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UEPITypes.h"
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
struct FUEPIWorkerSessionStatus
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	bool bHasSession = false;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString WorkerId;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString Status;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString DaemonUrl;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString ExpiresAtUtc;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString LastError;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	bool bPollingEnabled = false;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	bool bJobInProgress = false;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString ActiveJobId;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	FString LastCompletedJobId;

	UPROPERTY(BlueprintReadOnly, Category="UE Project Intelligence")
	int32 CompletedJobCount = 0;
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
	void GetIncrementalEvents(TArray<FUEPIIncrementalEvent>& OutEvents) const;

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	void ClearIncrementalEvents();

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	bool WriteIncrementalEventsSnapshot(FString OutputPath, FString& OutReportPath, FString& OutError) const;

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	FString GetIncrementalEventLogPath() const;

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	bool RegisterWorkerSession(FString DaemonUrl, FString WorkerId, FString& OutError);

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	bool StartLiveWorker(FString DaemonUrl, FString WorkerId, FString& OutError);

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	void StopLiveWorker();

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	bool SendWorkerHeartbeat(FString& OutError);

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	bool PollWorkerJobs(FString& OutError);

	UFUNCTION(BlueprintCallable, Category="UE Project Intelligence")
	FUEPIWorkerSessionStatus GetWorkerSessionStatus() const;

private:
	void RegisterIncrementalDelegates();
	void UnregisterIncrementalDelegates();

	void HandlePackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext ObjectSaveContext);
	void HandleAssetAdded(const struct FAssetData& AssetData);
	void HandleAssetRemoved(const struct FAssetData& AssetData);
	void HandleAssetRenamed(const struct FAssetData& AssetData, const FString& OldObjectPath);
	void HandleAssetUpdated(const struct FAssetData& AssetData);
	void HandleBlueprintCompiled();

	void RecordIncrementalEvent(
		const FString& EventType,
		const FString& AssetPath,
		const FString& PackageName,
		const FString& ClassPath,
		const FString& OldObjectPath,
		const FString& PackageFileName);

	void HandleWorkerRegisterResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	void HandleWorkerHeartbeatResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	void HandleWorkerPollResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	void HandleWorkerJobUpdateResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	bool PostWorkerJson(const FString& EndpointPath, const TSharedRef<class FJsonObject>& Payload, FHttpRequestCompleteDelegate CompletionDelegate, FString& OutError) const;
	bool TickLiveWorker(float DeltaTime);
	void ExecuteWorkerJob(const TSharedPtr<class FJsonObject>& Job);
	bool ExecuteMetadataScanJob(const TSharedPtr<class FJsonObject>& Job, FString& OutError);
	bool BuildScanOptionsFromWorkerJob(const FString& JobId, const TSharedPtr<class FJsonObject>& Job, UE::ProjectIntelligence::FScanOptions& Options, FString& OutError) const;
	bool SendWorkerJobUpdate(const FString& JobId, const FString& State, const TSharedPtr<class FJsonObject>& Result, const TSharedPtr<class FJsonObject>& Error, const TArray<TSharedPtr<class FJsonValue>>& Artifacts, FString& OutError);

	TArray<FUEPIIncrementalEvent> IncrementalEvents;
	int64 NextIncrementalEventSequence = 0;
	FUEPIWorkerSessionStatus WorkerSessionStatus;
	FString WorkerSessionToken;
	FTSTicker::FDelegateHandle WorkerTickerHandle;
	bool bLiveWorkerEnabled = false;
	bool bWorkerRegisterInFlight = false;
	bool bWorkerPollInFlight = false;
	bool bWorkerJobInProgress = false;
	double LastWorkerRegisterAttemptSeconds = 0.0;
	double LastWorkerHeartbeatSeconds = 0.0;
	double LastWorkerPollSeconds = 0.0;
	FDelegateHandle PackageSavedHandle;
	FDelegateHandle AssetAddedHandle;
	FDelegateHandle AssetRemovedHandle;
	FDelegateHandle AssetRenamedHandle;
	FDelegateHandle AssetUpdatedHandle;
	FDelegateHandle BlueprintCompiledHandle;
};
