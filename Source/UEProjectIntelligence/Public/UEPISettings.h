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

	UPROPERTY(Config, EditAnywhere, Category="Experimental Live Bridge")
	bool bEnableLiveEditorBridge;

	UPROPERTY(Config, EditAnywhere, Category="Experimental Live Bridge", meta=(ClampMin="0", ClampMax="65535"))
	int32 LiveEditorBridgePort;

	UPROPERTY(Config, EditAnywhere, Category="Experimental Write Safety")
	bool bEnableWriteTools;

	UPROPERTY(Config, EditAnywhere, Category="Experimental Write Safety")
	bool bRequirePreviewBeforeApply;

	UPROPERTY(Config, EditAnywhere, Category="Experimental Write Safety")
	bool bRequireSnapshotDiffAfterApply;
};
