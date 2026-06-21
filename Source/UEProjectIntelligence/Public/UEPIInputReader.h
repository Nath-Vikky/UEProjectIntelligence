#pragma once

#include "CoreMinimal.h"
#include "UEPITypes.h"

class UInputAction;
class UInputMappingContext;

namespace UE::ProjectIntelligence
{
struct FInputReader
{
	static bool AppendInputAsset(
		UObject& Asset,
		const FString& ProjectId,
		FEntityRecord& AssetEntity,
		TArray<FEntityRecord>& OutEntities,
		TArray<FRelationRecord>& OutRelations);
};
}
