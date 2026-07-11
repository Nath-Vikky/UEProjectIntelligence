#include "Edit/UEPIPackageSaveService.h"

#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"

namespace UE::ProjectIntelligence
{
	bool FUEPIPackageSaveService::SaveTouched(const TArray<FString>& AffectedAssets, TArray<FUEPISavedFileHash>& OutHashes, FString& OutError)
	{
		OutHashes.Reset();
		TArray<UPackage*> Packages;
		for (const FString& AssetPath : AffectedAssets)
		{
			if (!AssetPath.StartsWith(TEXT("/"))) continue;
			const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
			if (UPackage* Package = FindPackage(nullptr, *PackageName)) Packages.AddUnique(Package);
		}
		if (Packages.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(Packages, true))
		{
			OutError = TEXT("Touched-only package save failed.");
			return false;
		}
		for (UPackage* Package : Packages)
		{
			const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
			if (IFileManager::Get().FileExists(*File)) OutHashes.Add({ File, LexToString(FMD5Hash::HashFile(*File)) });
		}
		OutError.Reset();
		return true;
	}
}
