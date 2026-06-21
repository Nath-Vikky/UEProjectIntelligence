#pragma once

#include "CoreMinimal.h"
#include "UEPITypes.h"

namespace UE::ProjectIntelligence
{
struct FAIReader
{
	static bool AppendAIAsset(
		UObject& Asset,
		const FString& ProjectId,
		FEntityRecord& AssetEntity,
		TArray<FEntityRecord>& OutEntities,
		TArray<FRelationRecord>& OutRelations);
};
}
