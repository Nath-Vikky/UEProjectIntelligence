#pragma once

#include "CoreMinimal.h"
#include "Edit/UEPIEditOperation.h"

namespace UE::ProjectIntelligence
{
	class FUEPIEditOperationRegistry
	{
	public:
		static FUEPIEditOperationRegistry& Get();

		bool RegisterOperation(TSharedRef<IUEPIEditOperation> Operation);
		TSharedPtr<IUEPIEditOperation> FindOperation(const FString& OperationType) const;
		TArray<FString> GetOperationTypes() const;
		void Reset();

	private:
		TMap<FString, TSharedPtr<IUEPIEditOperation>> OperationsByType;
	};
}
