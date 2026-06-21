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

	UPROPERTY(Config, EditAnywhere, Category="Daemon")
	bool bAutoConnectDaemon;

	UPROPERTY(Config, EditAnywhere, Category="Daemon")
	FString DaemonHost;

	UPROPERTY(Config, EditAnywhere, Category="Daemon", meta=(ClampMin="1", ClampMax="65535"))
	int32 DaemonPort;

	UPROPERTY(Config, EditAnywhere, Category="Daemon")
	bool bAutoStartDaemon;

	UPROPERTY(Config, EditAnywhere, Category="Indexing")
	bool bEnableLiveIncrementalIndex;

	UPROPERTY(Config, EditAnywhere, Category="Read Only Guard")
	bool bAllowBlueprintCompile;

	UPROPERTY(Config, EditAnywhere, Category="Read Only Guard")
	bool bAllowPackageSave;

	UPROPERTY(Config, EditAnywhere, Category="Runtime Evaluation")
	bool bEnableRuntimeEvaluation;

	UPROPERTY(Config, EditAnywhere, Category="Scope")
	bool bIncludeGameContent;

	UPROPERTY(Config, EditAnywhere, Category="Scope")
	bool bIncludeProjectPluginContent;

	UPROPERTY(Config, EditAnywhere, Category="Scope")
	bool bIncludeEngineContent;

	UPROPERTY(Config, EditAnywhere, Category="Budgets", meta=(ClampMin="1"))
	int32 MaxAssetsPerBatch;

	UPROPERTY(Config, EditAnywhere, Category="Budgets", meta=(ClampMin="0.1"))
	float MaxGameThreadBudgetMs;

	UPROPERTY(Config, EditAnywhere, Category="Budgets", meta=(ClampMin="1"))
	int32 MaxPropertyDepth;

	UPROPERTY(Config, EditAnywhere, Category="Budgets", meta=(ClampMin="1"))
	int32 MaxInlineCollectionItems;

	UPROPERTY(Config, EditAnywhere, Category="Budgets", meta=(ClampMin="1"))
	int32 GarbageCollectEveryBatches;
};
