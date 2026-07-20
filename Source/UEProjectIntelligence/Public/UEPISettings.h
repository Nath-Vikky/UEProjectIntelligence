#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UEPISettings.generated.h"

UENUM()
enum class EUEPIWriteAuthorizationMode : uint8
{
	ReviewEachPlan,
	TrustedSession,
	TrustedProject
};

UENUM()
enum class EUEPIMaximumRiskLevel : uint8
{
	Low,
	Medium,
	High,
	Critical
};

UCLASS(Config=EditorPerProjectUserSettings, DefaultConfig, meta=(DisplayName="UE Project Intelligence"))
class UEPROJECTINTELLIGENCE_API UUEPISettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UUEPISettings();

	virtual FName GetContainerName() const override;
	virtual FName GetCategoryName() const override;
	virtual FName GetSectionName() const override;
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;

	UPROPERTY(Config, EditAnywhere, Category="Scope")
	bool bIncludeGameContent;

	UPROPERTY(Config, EditAnywhere, Category="Scope")
	bool bIncludeProjectPluginContent;

	UPROPERTY(Config, EditAnywhere, Category="Scope")
	bool bIncludeEngineContent;

	UPROPERTY(Config, EditAnywhere, Category="Budgets", meta=(ClampMin="1"))
	int32 MaxAssetsPerBatch;

	UPROPERTY(Config, EditAnywhere, Category="Budgets", meta=(ClampMin="1"))
	int32 MaxInlineCollectionItems;

	UPROPERTY(Config, EditAnywhere, Category="Agent Live Bridge")
	bool bEnableLiveEditorBridge;

	UPROPERTY(Config, EditAnywhere, Category="Agent Live Bridge", meta=(ClampMin="0", ClampMax="65535"))
	int32 LiveEditorBridgePort;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply")
	bool bEnableWriteTools;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply")
	bool bAllowBlueprintEdits;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply")
	bool bAllowActorEdits;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply")
	bool bAllowContentEdits;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply")
	bool bAllowMaterialEdits;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply")
	bool bAllowUMGEdits;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply")
	bool bAllowInputEdits;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply")
	bool bAllowSavingPackages;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply")
	bool bRequirePreviewBeforeApply;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply")
	bool bRequireSnapshotDiffAfterApply;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply|Authorization")
	EUEPIWriteAuthorizationMode WriteAuthorizationMode;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply|Authorization", meta=(ToolTip="Exact sha256 project binding shown by uepi_status. Required by TrustedProject."))
	FString TrustedProjectBindingId;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply|Authorization")
	TArray<FString> AllowedAssetRoots;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply|Authorization")
	TArray<FString> AllowedOperationDomains;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply|Authorization")
	EUEPIMaximumRiskLevel MaximumRiskLevel;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply|Authorization")
	bool bAllowAssetDelete;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply|Authorization")
	bool bAllowAssetRename;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply|Authorization")
	bool bAllowRuntimeControl;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply|Authorization", meta=(ClampMin="1", ClampMax="64"))
	int32 MaximumAssetsPerTransaction;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply|Authorization")
	bool bAlwaysCreateBackup;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply|Authorization")
	bool bAlwaysReportAfterApply;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply", meta=(ClampMin="1", ClampMax="256"))
	int32 MaxWriteOperationsPerTransaction;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply", meta=(ClampMin="1", ClampMax="64"))
	int32 MaxWriteAssetsPerTransaction;

	UPROPERTY(Config, EditAnywhere, Category="Controlled Runtime")
	bool bAllowPIEControl;

	UPROPERTY(Config, EditAnywhere, Category="Controlled Runtime")
	bool bAllowRuntimeInvoke;

	UPROPERTY(Config, EditAnywhere, Category="Controlled Runtime", meta=(ToolTip="Exact ClassPath:FunctionName entries allowed for transaction-bound runtime invoke."))
	TArray<FString> AllowedRuntimeFunctions;
};
