#pragma once

#include "CoreMinimal.h"
#include "UEPITypes.h"

class UBlueprint;

namespace UE::ProjectIntelligence
{
class FBlueprintGraphReader
{
public:
	static void AppendBlueprintGraph(
		UBlueprint& Blueprint,
		const FString& ProjectId,
		FEntityRecord& AssetEntity,
		TArray<FEntityRecord>& OutEntities,
		TArray<FRelationRecord>& OutRelations);
};
}
