#pragma once

#include "CoreMinimal.h"
#include "UEPITypes.h"

class UWorld;
class UObject;
struct FAssetData;

namespace UE::ProjectIntelligence
{
struct FWorldReader
{
	static bool AppendWorldAsset(
		UObject& Asset,
		const FString& ProjectId,
		FEntityRecord& AssetEntity,
		TArray<FEntityRecord>& OutEntities,
		TArray<FRelationRecord>& OutRelations);

	static bool AppendWorldPartitionActorDescAssetData(
		const FAssetData& AssetData,
		const FString& ProjectId,
		FEntityRecord& AssetEntity,
		TArray<FEntityRecord>& OutEntities,
		TArray<FRelationRecord>& OutRelations);

	static void AppendWorld(
		UWorld& World,
		const FString& ProjectId,
		FEntityRecord& AssetEntity,
		TArray<FEntityRecord>& OutEntities,
		TArray<FRelationRecord>& OutRelations);
};
}
