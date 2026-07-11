#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UEPISettings.generated.h"

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

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply", meta=(ClampMin="1", ClampMax="100"))
	int32 MaxWriteOperationsPerTransaction;

	UPROPERTY(Config, EditAnywhere, Category="Guarded Edit Apply", meta=(ClampMin="1", ClampMax="20"))
	int32 MaxWriteAssetsPerTransaction;

	UPROPERTY(Config, EditAnywhere, Category="Controlled Runtime")
	bool bAllowPIEControl;

	UPROPERTY(Config, EditAnywhere, Category="Controlled Runtime")
	bool bAllowRuntimeInvoke;
};
