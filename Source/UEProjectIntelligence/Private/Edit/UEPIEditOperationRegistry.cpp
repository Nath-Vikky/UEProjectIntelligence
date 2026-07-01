#include "Edit/UEPIEditOperationRegistry.h"

namespace UE::ProjectIntelligence
{
	FUEPIEditOperationRegistry& FUEPIEditOperationRegistry::Get()
	{
		static FUEPIEditOperationRegistry Registry;
		return Registry;
	}

	bool FUEPIEditOperationRegistry::RegisterOperation(TSharedRef<IUEPIEditOperation> Operation)
	{
		const FString OperationType = Operation->GetOperationType();
		if (OperationType.IsEmpty() || OperationsByType.Contains(OperationType))
		{
			return false;
		}
		OperationsByType.Add(OperationType, Operation);
		return true;
	}

	TSharedPtr<IUEPIEditOperation> FUEPIEditOperationRegistry::FindOperation(const FString& OperationType) const
	{
		if (const TSharedPtr<IUEPIEditOperation>* Operation = OperationsByType.Find(OperationType))
		{
			return *Operation;
		}
		return nullptr;
	}

	TArray<FString> FUEPIEditOperationRegistry::GetOperationTypes() const
	{
		TArray<FString> Types;
		OperationsByType.GetKeys(Types);
		Types.Sort();
		return Types;
	}

	void FUEPIEditOperationRegistry::Reset()
	{
		OperationsByType.Reset();
	}
}
