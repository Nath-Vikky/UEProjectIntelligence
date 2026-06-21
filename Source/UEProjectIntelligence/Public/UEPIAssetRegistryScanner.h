#pragma once

#include "CoreMinimal.h"
#include "UEPITypes.h"

namespace UE::ProjectIntelligence
{
class FAssetRegistryScanner
{
public:
	static FScanOptions MakeOptionsFromSettings();
	static FProjectScanResult ScanProject(const FScanOptions& Options);
	static bool WriteScanResultJson(const FProjectScanResult& Result, const FString& OutputPath, FText& OutError);
};
}
