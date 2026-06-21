#pragma once

#include "CoreMinimal.h"

namespace UE::ProjectIntelligence
{
class FDirtyPackageGuard
{
public:
	FDirtyPackageGuard();

	bool ValidateUnchanged(TArray<FString>& OutChangedPackages) const;

private:
	TSet<FName> InitialDirtyPackages;

	static void CaptureDirtyPackages(TSet<FName>& OutPackages);
};
}
