#pragma once

#include "CoreMinimal.h"
#include "UEPITypes.h"

class UObject;

namespace UE::ProjectIntelligence
{
struct FUIReader
{
	static bool AppendUIAsset(
		UObject& Asset,
		const FString& ProjectId,
		FEntityRecord& AssetEntity,
		TArray<FEntityRecord>& OutEntities,
		TArray<FRelationRecord>& OutRelations);
};
}
