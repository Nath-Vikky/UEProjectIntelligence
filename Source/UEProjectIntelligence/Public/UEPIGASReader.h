#pragma once

#include "CoreMinimal.h"
#include "UEPITypes.h"

namespace UE::ProjectIntelligence
{
struct FGASReader
{
	static bool AppendGASAsset(
		UObject& Asset,
		const FString& ProjectId,
		FEntityRecord& AssetEntity,
		TArray<FEntityRecord>& OutEntities,
		TArray<FRelationRecord>& OutRelations);
};
}
