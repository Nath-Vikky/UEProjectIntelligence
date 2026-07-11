#pragma once

#include "CoreMinimal.h"

namespace UE::ProjectIntelligence
{
	class FUEPIBackupService
	{
	public:
		static bool Create(const FString& TransactionId, const TArray<FString>& AffectedAssets, TMap<FString, FString>& OutBackupFiles, FString& OutDirectory, FString& OutError);
		static bool Restore(const TMap<FString, FString>& BackupFiles, const TArray<FString>& AffectedAssets, FString& OutError);
	};
}
