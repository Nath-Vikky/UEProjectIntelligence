#pragma once

#include "CoreMinimal.h"
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

	TArray<FUEPIIncrementalEvent> IncrementalEvents;
	int64 NextIncrementalEventSequence = 0;
	FDelegateHandle PackageSavedHandle;
	FDelegateHandle AssetAddedHandle;
	FDelegateHandle AssetRemovedHandle;
	FDelegateHandle AssetRenamedHandle;
	FDelegateHandle AssetUpdatedHandle;
	FDelegateHandle BlueprintCompiledHandle;
};
