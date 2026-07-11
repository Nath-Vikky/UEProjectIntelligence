#pragma once

#include "CoreMinimal.h"

class UObject;

namespace UE::ProjectIntelligence
{
	struct FUEPIValidationResult
	{
		bool bOk = false;
		FString Asset;
		FString ClassPath;
		FString Validator;
		FString Message;
	};

	class IUEPIAssetValidator
	{
	public:
		virtual ~IUEPIAssetValidator() = default;
		virtual bool Supports(const UObject* Object) const = 0;
		virtual FUEPIValidationResult Validate(UObject* Object) const = 0;
	};

	class FUEPIValidatorRegistry
	{
	public:
		static FUEPIValidatorRegistry& Get();
		FUEPIValidationResult Validate(UObject* Object) const;

	private:
		FUEPIValidatorRegistry();
		TArray<TSharedRef<IUEPIAssetValidator>> Validators;
	};
}
