#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "UEPIIndexCommandlet.generated.h"

UCLASS()
class UEPROJECTINTELLIGENCE_API UUEPIIndexCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UUEPIIndexCommandlet();

	virtual int32 Main(const FString& Params) override;
};
