#include "Edit/UEPIBackupService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/Package.h"

namespace UE::ProjectIntelligence
{
	bool FUEPIBackupService::Create(const FString& TransactionId, const TArray<FString>& AffectedAssets, TMap<FString, FString>& OutBackupFiles, FString& OutDirectory, FString& OutError)
	{
		OutBackupFiles.Reset();
		OutDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("backups"), FPaths::MakeValidFileName(TransactionId));
		IFileManager::Get().MakeDirectory(*OutDirectory, true);
		for (const FString& AssetPath : AffectedAssets)
		{
			if (!AssetPath.StartsWith(TEXT("/"))) continue;
			const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
			const FString PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
			if (!IFileManager::Get().FileExists(*PackageFile))
			{
				OutBackupFiles.Add(PackageFile, FString());
				continue;
			}
			const FString BackupFile = FPaths::Combine(OutDirectory, FPaths::MakeValidFileName(PackageName) + FPackageName::GetAssetPackageExtension());
			if (IFileManager::Get().Copy(*BackupFile, *PackageFile, true, true) != COPY_OK)
			{
				OutError = FString::Printf(TEXT("Could not back up target package: %s"), *PackageFile);
				return false;
			}
			OutBackupFiles.Add(PackageFile, BackupFile);
		}
		OutError.Reset();
		return true;
	}

	bool FUEPIBackupService::Restore(const TMap<FString, FString>& BackupFiles, const TArray<FString>& AffectedAssets, FString& OutError)
	{
		bool bOk = true;
		for (const TPair<FString, FString>& Pair : BackupFiles)
		{
			if (Pair.Value.IsEmpty())
			{
				if (IFileManager::Get().FileExists(*Pair.Key) && !IFileManager::Get().Delete(*Pair.Key, false, true, true)) bOk = false;
			}
			else if (IFileManager::Get().Copy(*Pair.Key, *Pair.Value, true, true) != COPY_OK)
			{
				bOk = false;
			}
		}
		TArray<UPackage*> PackagesToUnload;
		TArray<UPackage*> PackagesToReload;
		for (const FString& AssetPath : AffectedAssets)
		{
			if (!AssetPath.StartsWith(TEXT("/"))) continue;
			const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
			if (UPackage* Package = FindPackage(nullptr, *PackageName))
			{
				Package->SetDirtyFlag(false);
				const FString PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
				const FString* BackupFile = BackupFiles.Find(PackageFile);
				if (BackupFile && BackupFile->IsEmpty())
				{
					if (UObject* NewAsset = FindObject<UObject>(nullptr, *AssetPath))
					{
						FAssetRegistryModule::AssetDeleted(NewAsset);
					}
					PackagesToUnload.AddUnique(Package);
				}
				else
				{
					PackagesToReload.AddUnique(Package);
				}
			}
		}
		if (PackagesToUnload.Num() > 0)
		{
			FText UnloadError;
			if (!UPackageTools::UnloadPackages(PackagesToUnload, UnloadError, true))
			{
				bOk = false;
				OutError = UnloadError.ToString();
			}
		}
		if (PackagesToReload.Num() > 0)
		{
			FText ReloadError;
			if (!UPackageTools::ReloadPackages(PackagesToReload, ReloadError, EReloadPackagesInteractionMode::AssumePositive))
			{
				bOk = false;
				OutError = ReloadError.ToString();
			}
		}
		if (bOk) OutError.Reset(); else if (OutError.IsEmpty()) OutError = TEXT("One or more package files could not be restored.");
		return bOk;
	}
}
