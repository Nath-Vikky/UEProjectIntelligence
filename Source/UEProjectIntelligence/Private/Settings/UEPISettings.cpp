#include "UEPISettings.h"

UUEPISettings::UUEPISettings()
	: bIncludeGameContent(true)
	, bIncludeProjectPluginContent(true)
	, bIncludeEngineContent(false)
	, MaxAssetsPerBatch(50)
	, MaxInlineCollectionItems(1000)
	, bEnableLiveEditorBridge(true)
	, LiveEditorBridgePort(0)
	, bEnableWriteTools(true)
	, bAllowBlueprintEdits(true)
	, bAllowActorEdits(true)
	, bAllowContentEdits(true)
	, bAllowMaterialEdits(true)
	, bAllowUMGEdits(true)
	, bAllowInputEdits(true)
	, bAllowSavingPackages(false)
	, bRequirePreviewBeforeApply(true)
	, bRequireSnapshotDiffAfterApply(true)
	, MaxWriteOperationsPerTransaction(32)
	, MaxWriteAssetsPerTransaction(3)
	, bAllowPIEControl(true)
	, bAllowRuntimeInvoke(true)
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
	return NSLOCTEXT("UEProjectIntelligence", "SettingsSectionDescription", "Snapshot read settings plus Agent-ready live bridge and guarded edit-apply safety gates for UE Project Intelligence.");
}
