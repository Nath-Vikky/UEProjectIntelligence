#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"
#include "UEPITypes.h"

class UEdGraphNode;
struct FAssetData;

namespace UE::ProjectIntelligence
{
	class IAssetAdapter : public IModularFeature
	{
	public:
		static FName FeatureName()
		{
			return TEXT("UEPIAssetAdapter");
		}

		virtual FString GetAdapterId() const = 0;
		virtual FString GetAdapterVersion() const = 0;
		virtual bool SupportsAsset(const FAssetData& AssetData) const = 0;
		virtual void ExtractAsset(
			UObject& Asset,
			const FString& ProjectId,
			FEntityRecord& AssetEntity,
			TArray<FEntityRecord>& OutEntities,
			TArray<FRelationRecord>& OutRelations) const = 0;
	};

	class INodeSemanticAdapter : public IModularFeature
	{
	public:
		static FName FeatureName()
		{
			return TEXT("UEPINodeSemanticAdapter");
		}

		virtual FString GetAdapterId() const = 0;
		virtual FString GetAdapterVersion() const = 0;
		virtual bool SupportsNode(const UEdGraphNode& Node) const = 0;
		virtual void ExtractNodeSemantics(
			const UEdGraphNode& Node,
			const FString& ProjectId,
			const FString& GraphEntityId,
			const FString& NodeEntityId,
			TArray<FEntityRecord>& OutEntities,
			TArray<FRelationRecord>& OutRelations) const = 0;
	};
}
