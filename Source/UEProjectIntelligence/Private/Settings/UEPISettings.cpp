#include "UEPISettings.h"

UUEPISettings::UUEPISettings()
	: bAutoConnectDaemon(true)
	, DaemonHost(TEXT("127.0.0.1"))
	, DaemonPort(17831)
	, bAutoStartDaemon(true)
	, bEnableLiveIncrementalIndex(true)
	, bAllowBlueprintCompile(false)
	, bAllowPackageSave(false)
	, bEnableRuntimeEvaluation(false)
	, bIncludeGameContent(true)
	, bIncludeProjectPluginContent(true)
	, bIncludeEngineContent(false)
	, MaxAssetsPerBatch(50)
	, MaxGameThreadBudgetMs(8.0f)
	, MaxPropertyDepth(16)
	, MaxInlineCollectionItems(1000)
	, GarbageCollectEveryBatches(10)
{
}

FName UUEPISettings::GetContainerName() const
{
	return TEXT("Project");
}

FName UUEPISettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FName UUEPISettings::GetSectionName() const
{
	return TEXT("UEProjectIntelligence");
}

FText UUEPISettings::GetSectionText() const
{
	return NSLOCTEXT("UEProjectIntelligence", "SettingsSectionText", "UE Project Intelligence");
}

FText UUEPISettings::GetSectionDescription() const
{
	return NSLOCTEXT("UEProjectIntelligence", "SettingsSectionDescription", "Read-only project scan settings for UE Project Intelligence.");
}
