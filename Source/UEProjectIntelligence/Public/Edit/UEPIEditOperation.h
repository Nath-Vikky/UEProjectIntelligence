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
		TMap<FString, FString> ResolvedAssets;
		TMap<FString, UClass*> ResolvedAssetClasses;
		TMap<FString, TSet<FName>> PlannedSkeletonSlots;
		TMap<FString, FString> PlannedAssetSkeletons;
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

	struct FUEPIEditOperationDescriptor
	{
		FString Name;
		int32 Version = 1;
		FString Domain;
		FString Summary;
		FString Risk = TEXT("medium");
		FString RollbackMode = TEXT("editor_transaction");
		FString ValidationMode = TEXT("generic_uobject");
		TArray<FString> TargetFields;
		TArray<FString> RequiredCapabilities;
		TArray<FString> SupportedEngineVersions = { TEXT("5.3.2") };
		TArray<FString> SupportedAssetClasses;
		TArray<FString> SupportedGraphSchemas;
		FString RequiredPlugin;
		FString SaveBehavior = TEXT("touched_only_after_validation");
		FString IdempotencyBehavior = TEXT("transaction_and_plan_hash");
		bool bRequiresSave = true;
		bool bAtomicSupported = true;
	};

	class IUEPIEditOperation
	{
	public:
		virtual ~IUEPIEditOperation() = default;

		virtual FString GetOperationType() const = 0;
		virtual FUEPIEditOperationDescriptor GetDescriptor() const = 0;
		virtual FUEPIEditResult Validate(const FUEPIEditContext& Context, const FJsonObject& Params) = 0;
		virtual FUEPIEditResult Preview(const FUEPIEditContext& Context, const FJsonObject& Params) = 0;
		virtual FUEPIEditResult Apply(const FUEPIEditContext& Context, const FJsonObject& Params) = 0;
	};
}
