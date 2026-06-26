#include "UEPIIndexCommandlet.h"

#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UEPIAssetRegistryScanner.h"
#include "UEPISnapshotStore.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEPIIndexCommandlet, Log, All);

namespace
{
void UEPIAddDelimitedStrings(const FString& Text, TArray<FString>& OutValues)
{
	TArray<FString> Parts;
	Text.ParseIntoArray(Parts, TEXT(";"), true);
	for (FString Part : Parts)
	{
		Part.TrimStartAndEndInline();
		if (!Part.IsEmpty())
		{
			OutValues.AddUnique(Part);
		}
	}
}

void UEPIApplyLevelToOptions(UE::ProjectIntelligence::FScanOptions& Options, const FString& Level, bool bOverrideExisting)
{
	if (Level.IsEmpty())
	{
		return;
	}

	if (bOverrideExisting)
	{
		Options.bReadBlueprintGraphs = false;
		Options.bReadUObjectReflection = false;
	}

	const bool bBlueprintLevel = Level.Equals(TEXT("L2"), ESearchCase::IgnoreCase)
		|| Level.Equals(TEXT("Structural"), ESearchCase::IgnoreCase)
		|| Level.Equals(TEXT("Blueprint"), ESearchCase::IgnoreCase);
	const bool bReflectionLevel = bBlueprintLevel
		|| Level.Equals(TEXT("L1"), ESearchCase::IgnoreCase)
		|| Level.Equals(TEXT("Reflection"), ESearchCase::IgnoreCase)
		|| Level.Equals(TEXT("UObject"), ESearchCase::IgnoreCase);

	Options.bReadBlueprintGraphs = Options.bReadBlueprintGraphs || bBlueprintLevel;
	Options.bReadUObjectReflection = Options.bReadUObjectReflection || bReflectionLevel;
}

FString UEPIDefaultScanArtifactPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("last_scan.json"));
}
}

UUEPIIndexCommandlet::UUEPIIndexCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UUEPIIndexCommandlet::Main(const FString& Params)
{
	FString OutputPath;
	FParse::Value(*Params, TEXT("Output="), OutputPath);
	if (OutputPath.IsEmpty())
	{
		FParse::Value(FCommandLine::Get(), TEXT("UEPIOutput="), OutputPath);
	}

	UE::ProjectIntelligence::FScanOptions Options = UE::ProjectIntelligence::FAssetRegistryScanner::MakeOptionsFromSettings();
	FString Level;
	FParse::Value(FCommandLine::Get(), TEXT("UEPILevel="), Level);
	UEPIApplyLevelToOptions(Options, Level, false);
	Options.bReadBlueprintGraphs = Options.bReadBlueprintGraphs || FParse::Param(FCommandLine::Get(), TEXT("UEPIReadBlueprints"));
	Options.bReadUObjectReflection = Options.bReadUObjectReflection
		|| Options.bReadBlueprintGraphs
		|| FParse::Param(FCommandLine::Get(), TEXT("UEPILoadAssets"));

	FString TargetAssets;
	FParse::Value(FCommandLine::Get(), TEXT("UEPIAsset="), TargetAssets);
	if (!TargetAssets.IsEmpty())
	{
		UEPIAddDelimitedStrings(TargetAssets, Options.TargetObjectPaths);
	}

	if (OutputPath.IsEmpty())
	{
		OutputPath = UEPIDefaultScanArtifactPath();
	}
	Options.OutputPath = OutputPath;

	UE_LOG(LogUEPIIndexCommandlet, Display, TEXT("UEPI one-shot metadata scan started. Output=%s"), *Options.OutputPath);

	const UE::ProjectIntelligence::FProjectScanResult Result = UE::ProjectIntelligence::FAssetRegistryScanner::ScanProject(Options);

	FText ErrorText;
	if (!UE::ProjectIntelligence::FAssetRegistryScanner::WriteScanResultJson(Result, Options.OutputPath, ErrorText))
	{
		UE_LOG(LogUEPIIndexCommandlet, Error, TEXT("%s"), *ErrorText.ToString());
		return 1;
	}

	UE::ProjectIntelligence::FSnapshotCommitOptions CommitOptions;
	CommitOptions.DataMode = TEXT("saved");
	CommitOptions.WriterMode = TEXT("commandlet");
	CommitOptions.SourceScanPath = Options.OutputPath;
	CommitOptions.TargetObjectPaths = Options.TargetObjectPaths;
	CommitOptions.bMergeWithExisting = Options.TargetObjectPaths.Num() > 0;

	UE::ProjectIntelligence::FSnapshotCommitResult CommitResult;
	if (!UE::ProjectIntelligence::FSnapshotStore::CommitProjectScan(Result, CommitOptions, CommitResult, ErrorText))
	{
		UE_LOG(LogUEPIIndexCommandlet, Error, TEXT("%s"), *ErrorText.ToString());
		return 1;
	}

	UE_LOG(
		LogUEPIIndexCommandlet,
		Display,
		TEXT("UEPI one-shot metadata scan completed. Assets=%d Relations=%d Diagnostics=%d Output=%s SnapshotManifest=%s Generation=%lld"),
		Result.Entities.Num(),
		Result.Relations.Num(),
		Result.Diagnostics.Num(),
		*Options.OutputPath,
		*CommitResult.ManifestPath,
		CommitResult.Generation);

	return Result.Diagnostics.Num() == 0 ? 0 : 2;
}
