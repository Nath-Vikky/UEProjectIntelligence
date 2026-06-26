#include "UEPISettings.h"

UUEPISettings::UUEPISettings()
	: bIncludeGameContent(true)
	, bIncludeProjectPluginContent(true)
	, bIncludeEngineContent(false)
	, MaxAssetsPerBatch(50)
	, MaxInlineCollectionItems(1000)
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
