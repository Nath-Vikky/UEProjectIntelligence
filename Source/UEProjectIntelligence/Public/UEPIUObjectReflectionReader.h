#pragma once

#include "CoreMinimal.h"
#include "UEPITypes.h"

struct FAssetData;

namespace UE::ProjectIntelligence
{
class FUObjectReflectionReader
{
public:
	static void ReadAssetIntoEntity(const FAssetData& AssetData, const FScanOptions& Options, FEntityRecord& Entity);
};
}
