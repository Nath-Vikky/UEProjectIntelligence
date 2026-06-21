#pragma once

#include "CoreMinimal.h"
#include "UEPITypes.h"

namespace UE::ProjectIntelligence
{
struct FCommonUIReader
{
	static bool AppendCommonUIAsset(
		UObject& Asset,
		const FString& ProjectId,
		FEntityRecord& AssetEntity,
		TArray<FEntityRecord>& OutEntities,
		TArray<FRelationRecord>& OutRelations);
};
}
