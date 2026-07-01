#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FJsonObject;

namespace UE::ProjectIntelligence
{
	struct FUEPIEditDiagnostic
	{
		FString Code;
		FString Severity;
		FString Message;
		bool bRecoverable = false;
	};

	struct FUEPIEditContext
	{
		FString TransactionId;
		FString ProjectId;
		TArray<FString> AssetAllowList;
		bool bDryRun = true;
		bool bAllowSave = false;
		bool bAllowDelete = false;
	};

	struct FUEPIEditResult
	{
		bool bOk = false;
		FString ErrorCode;
		FString Message;
		TArray<FUEPIEditDiagnostic> Diagnostics;
		TSharedPtr<FJsonObject> Result;
	};

	class IUEPIEditOperation
	{
	public:
		virtual ~IUEPIEditOperation() = default;

		virtual FString GetOperationType() const = 0;
		virtual FUEPIEditResult Validate(const FUEPIEditContext& Context, const FJsonObject& Params) = 0;
		virtual FUEPIEditResult Preview(const FUEPIEditContext& Context, const FJsonObject& Params) = 0;
		virtual FUEPIEditResult Apply(const FUEPIEditContext& Context, const FJsonObject& Params) = 0;
	};
}
