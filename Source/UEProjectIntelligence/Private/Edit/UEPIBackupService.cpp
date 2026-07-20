#include "Edit/UEPIBackupService.h"

#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace UE::ProjectIntelligence
{
	namespace
	{
		FString AbsolutePackageFilename(const FString& PackageName)
		{
			FString PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
			if (!IFileManager::Get().FileExists(*PackageFile))
			{
				const FString MapFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
				if (IFileManager::Get().FileExists(*MapFile)) PackageFile = MapFile;
			}
			PackageFile = FPaths::ConvertRelativePathToFull(PackageFile);
			FPaths::NormalizeFilename(PackageFile);
			return PackageFile;
		}
	}

	bool FUEPIBackupService::Create(const FString& TransactionId, const TArray<FString>& AffectedAssets, TMap<FString, FString>& OutBackupFiles, FString& OutDirectory, FString& OutError)
	{
		OutBackupFiles.Reset();
		OutDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("backups"), FPaths::MakeValidFileName(TransactionId)));
		FPaths::NormalizeFilename(OutDirectory);
		IFileManager::Get().MakeDirectory(*OutDirectory, true);
		for (const FString& AssetPath : AffectedAssets)
		{
			if (!AssetPath.StartsWith(TEXT("/"))) continue;
			const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
			const FString PackageFile = AbsolutePackageFilename(PackageName);
			if (!IFileManager::Get().FileExists(*PackageFile))
			{
				OutBackupFiles.Add(PackageFile, FString());
				continue;
			}
			FString BackupFile = FPaths::Combine(OutDirectory, FPaths::MakeValidFileName(PackageName) + FPaths::GetExtension(PackageFile, true));
			BackupFile = FPaths::ConvertRelativePathToFull(BackupFile);
			FPaths::NormalizeFilename(BackupFile);
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
		if (bOk) OutError.Reset(); else if (OutError.IsEmpty()) OutError = TEXT("One or more package files could not be restored.");
		return bOk;
	}
}
