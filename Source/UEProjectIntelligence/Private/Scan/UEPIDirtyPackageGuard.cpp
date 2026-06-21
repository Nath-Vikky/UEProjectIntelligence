#include "Scan/UEPIDirtyPackageGuard.h"

#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace UE::ProjectIntelligence
{
FDirtyPackageGuard::FDirtyPackageGuard()
{
	CaptureDirtyPackages(InitialDirtyPackages);
}

bool FDirtyPackageGuard::ValidateUnchanged(TArray<FString>& OutChangedPackages) const
{
	TSet<FName> CurrentDirtyPackages;
	CaptureDirtyPackages(CurrentDirtyPackages);

	for (const FName& PackageName : CurrentDirtyPackages)
	{
		if (!InitialDirtyPackages.Contains(PackageName))
		{
			OutChangedPackages.Add(FString::Printf(TEXT("+%s"), *PackageName.ToString()));
		}
	}

	for (const FName& PackageName : InitialDirtyPackages)
	{
		if (!CurrentDirtyPackages.Contains(PackageName))
		{
			OutChangedPackages.Add(FString::Printf(TEXT("-%s"), *PackageName.ToString()));
		}
	}

	OutChangedPackages.Sort();
	return OutChangedPackages.Num() == 0;
}

void FDirtyPackageGuard::CaptureDirtyPackages(TSet<FName>& OutPackages)
{
	OutPackages.Reset();

	for (TObjectIterator<UPackage> PackageIt; PackageIt; ++PackageIt)
	{
		const UPackage* Package = *PackageIt;
		if (Package && Package->IsDirty())
		{
			OutPackages.Add(Package->GetFName());
		}
	}
}
}
