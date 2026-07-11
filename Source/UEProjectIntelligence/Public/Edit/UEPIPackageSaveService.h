#pragma once

#include "CoreMinimal.h"

namespace UE::ProjectIntelligence
{
	struct FUEPISavedFileHash
	{
		FString File;
		FString Md5;
	};

	class FUEPIPackageSaveService
	{
	public:
		static bool SaveTouched(const TArray<FString>& AffectedAssets, TArray<FUEPISavedFileHash>& OutHashes, FString& OutError);
	};
}
