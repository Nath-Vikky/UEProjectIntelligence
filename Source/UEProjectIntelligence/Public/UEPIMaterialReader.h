#pragma once

#include "CoreMinimal.h"
#include "UEPITypes.h"

class UObject;

namespace UE::ProjectIntelligence
{
struct FMaterialReader
{
	static bool AppendMaterialAsset(
		UObject& Asset,
		const FString& ProjectId,
		FEntityRecord& AssetEntity,
		TArray<FEntityRecord>& OutEntities,
		TArray<FRelationRecord>& OutRelations);
};
}
