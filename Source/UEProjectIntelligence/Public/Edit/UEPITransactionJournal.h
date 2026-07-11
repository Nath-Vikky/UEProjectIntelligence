#pragma once

#include "CoreMinimal.h"

namespace UE::ProjectIntelligence
{
	class FUEPITransactionJournal
	{
	public:
		static bool Write(const FString& TransactionId, const FString& Phase, const TArray<FString>& AffectedAssets, const TMap<FString, FString>& BackupFiles, bool bSaved, const FString& Message, FString& OutPath, FString& OutError);
	};
}
