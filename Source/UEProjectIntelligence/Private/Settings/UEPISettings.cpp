#include "UEPISettings.h"

UUEPISettings::UUEPISettings()
	: bIncludeGameContent(true)
	, bIncludeProjectPluginContent(true)
	, bIncludeEngineContent(false)
	, MaxAssetsPerBatch(50)
	, MaxInlineCollectionItems(1000)
	, bEnableLiveEditorBridge(false)
	, LiveEditorBridgePort(0)
	, bEnableWriteTools(false)
	, bAllowBlueprintEdits(false)
	, bAllowActorEdits(false)
	, bAllowContentEdits(false)
	, bAllowMaterialEdits(false)
	, bAllowUMGEdits(false)
	, bAllowInputEdits(false)
	, bAllowSavingPackages(false)
	, bRequirePreviewBeforeApply(true)
	, bRequireSnapshotDiffAfterApply(true)
	, MaxWriteOperationsPerTransaction(8)
	, MaxWriteAssetsPerTransaction(3)
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
	return NSLOCTEXT("UEProjectIntelligence", "SettingsSectionDescription", "Snapshot read settings plus disabled-by-default experimental bridge/write safety gates for UE Project Intelligence.");
}
