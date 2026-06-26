#include "UEPIEditorSubsystem.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UEPIAssetRegistryScanner.h"
#include "UEPISnapshotStore.h"

namespace
{
	constexpr int32 GUEPIMaxIncrementalEventsInMemory = 2048;

	TSharedRef<FJsonObject> UEPIIncrementalEventToJson(const FUEPIIncrementalEvent& Event)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("sequence"), static_cast<double>(Event.Sequence));
		Object->SetStringField(TEXT("event_type"), Event.EventType);
		Object->SetStringField(TEXT("timestamp_utc"), Event.TimestampUtc);
		Object->SetStringField(TEXT("asset_path"), Event.AssetPath);
		Object->SetStringField(TEXT("package_name"), Event.PackageName);
		Object->SetStringField(TEXT("class_path"), Event.ClassPath);
		Object->SetStringField(TEXT("old_object_path"), Event.OldObjectPath);
		Object->SetStringField(TEXT("package_file_name"), Event.PackageFileName);
		return Object;
	}

	FString UEPIJsonObjectToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}
}

void UUEPIEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (!IsRunningCommandlet())
	{
		RegisterIncrementalDelegates();
	}
}

void UUEPIEditorSubsystem::Deinitialize()
{
	UnregisterIncrementalDelegates();
	IncrementalEvents.Reset();
	Super::Deinitialize();
}

bool UUEPIEditorSubsystem::RunMetadataScan(FString OutputPath, FString& OutReportPath, FString& OutError)
{
	UE::ProjectIntelligence::FScanOptions Options = UE::ProjectIntelligence::FAssetRegistryScanner::MakeOptionsFromSettings();
	if (OutputPath.IsEmpty())
	{
		OutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("last_scan.json"));
	}
	Options.OutputPath = OutputPath;

	const UE::ProjectIntelligence::FProjectScanResult Result = UE::ProjectIntelligence::FAssetRegistryScanner::ScanProject(Options);

	FText ErrorText;
	if (!UE::ProjectIntelligence::FAssetRegistryScanner::WriteScanResultJson(Result, Options.OutputPath, ErrorText))
	{
		OutReportPath.Reset();
		OutError = ErrorText.ToString();
		return false;
	}

	UE::ProjectIntelligence::FSnapshotCommitOptions CommitOptions;
	CommitOptions.DataMode = TEXT("saved");
	CommitOptions.WriterMode = TEXT("editor");
	CommitOptions.SourceScanPath = Options.OutputPath;

	UE::ProjectIntelligence::FSnapshotCommitResult CommitResult;
	if (!UE::ProjectIntelligence::FSnapshotStore::CommitProjectScan(Result, CommitOptions, CommitResult, ErrorText))
	{
		OutReportPath.Reset();
		OutError = ErrorText.ToString();
		return false;
	}

	OutReportPath = CommitResult.ManifestPath;
	OutError.Reset();
	return Result.Diagnostics.Num() == 0;
}

void UUEPIEditorSubsystem::GetIncrementalEvents(TArray<FUEPIIncrementalEvent>& OutEvents) const
{
	OutEvents = IncrementalEvents;
}

void UUEPIEditorSubsystem::ClearIncrementalEvents()
{
	IncrementalEvents.Reset();
}

bool UUEPIEditorSubsystem::WriteIncrementalEventsSnapshot(FString OutputPath, FString& OutReportPath, FString& OutError) const
{
	if (OutputPath.IsEmpty())
	{
		OutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("sessions"), TEXT("incremental_events_snapshot.json"));
	}

	TArray<TSharedPtr<FJsonValue>> EventValues;
	EventValues.Reserve(IncrementalEvents.Num());
	for (const FUEPIIncrementalEvent& Event : IncrementalEvents)
	{
		EventValues.Add(MakeShared<FJsonValueObject>(UEPIIncrementalEventToJson(Event)));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema_version"), TEXT("uepi.incremental-events.v2"));
	Root->SetStringField(TEXT("project_name"), FApp::GetProjectName());
	Root->SetStringField(TEXT("generated_at_utc"), FDateTime::UtcNow().ToIso8601());
	Root->SetNumberField(TEXT("event_count"), IncrementalEvents.Num());
	Root->SetArrayField(TEXT("events"), EventValues);

	const FString JsonText = UEPIJsonObjectToString(Root);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
	if (!FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutReportPath.Reset();
		OutError = FString::Printf(TEXT("Failed to write incremental event snapshot: %s"), *OutputPath);
		return false;
	}

	OutReportPath = FPaths::ConvertRelativePathToFull(OutputPath);
	OutError.Reset();
	return true;
}

FString UUEPIEditorSubsystem::GetIncrementalEventLogPath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("logs"), TEXT("incremental_events.jsonl"));
}

void UUEPIEditorSubsystem::RegisterIncrementalDelegates()
{
	if (!PackageSavedHandle.IsValid())
	{
		PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddUObject(this, &UUEPIEditorSubsystem::HandlePackageSaved);
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	if (!AssetAddedHandle.IsValid())
	{
		AssetAddedHandle = AssetRegistry.OnAssetAdded().AddUObject(this, &UUEPIEditorSubsystem::HandleAssetAdded);
	}
	if (!AssetRemovedHandle.IsValid())
	{
		AssetRemovedHandle = AssetRegistry.OnAssetRemoved().AddUObject(this, &UUEPIEditorSubsystem::HandleAssetRemoved);
	}
	if (!AssetRenamedHandle.IsValid())
	{
		AssetRenamedHandle = AssetRegistry.OnAssetRenamed().AddUObject(this, &UUEPIEditorSubsystem::HandleAssetRenamed);
	}
	if (!AssetUpdatedHandle.IsValid())
	{
		AssetUpdatedHandle = AssetRegistry.OnAssetUpdated().AddUObject(this, &UUEPIEditorSubsystem::HandleAssetUpdated);
	}

	if (GEditor && !BlueprintCompiledHandle.IsValid())
	{
		BlueprintCompiledHandle = GEditor->OnBlueprintCompiled().AddUObject(this, &UUEPIEditorSubsystem::HandleBlueprintCompiled);
	}
}

void UUEPIEditorSubsystem::UnregisterIncrementalDelegates()
{
	if (PackageSavedHandle.IsValid())
	{
		UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
		PackageSavedHandle.Reset();
	}

	if (FAssetRegistryModule* AssetRegistryModule = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
	{
		IAssetRegistry& AssetRegistry = AssetRegistryModule->Get();
		if (AssetAddedHandle.IsValid())
		{
			AssetRegistry.OnAssetAdded().Remove(AssetAddedHandle);
			AssetAddedHandle.Reset();
		}
		if (AssetRemovedHandle.IsValid())
		{
			AssetRegistry.OnAssetRemoved().Remove(AssetRemovedHandle);
			AssetRemovedHandle.Reset();
		}
		if (AssetRenamedHandle.IsValid())
		{
			AssetRegistry.OnAssetRenamed().Remove(AssetRenamedHandle);
			AssetRenamedHandle.Reset();
		}
		if (AssetUpdatedHandle.IsValid())
		{
			AssetRegistry.OnAssetUpdated().Remove(AssetUpdatedHandle);
			AssetUpdatedHandle.Reset();
		}
	}

	if (GEditor && BlueprintCompiledHandle.IsValid())
	{
		GEditor->OnBlueprintCompiled().Remove(BlueprintCompiledHandle);
		BlueprintCompiledHandle.Reset();
	}
}

void UUEPIEditorSubsystem::HandlePackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext ObjectSaveContext)
{
	const FString PackageName = Package ? Package->GetName() : FString();
	RecordIncrementalEvent(TEXT("package_saved"), PackageName, PackageName, TEXT("UPackage"), FString(), PackageFileName);
}

void UUEPIEditorSubsystem::HandleAssetAdded(const FAssetData& AssetData)
{
	RecordIncrementalEvent(
		TEXT("asset_added"),
		AssetData.GetSoftObjectPath().ToString(),
		AssetData.PackageName.ToString(),
		AssetData.AssetClassPath.ToString(),
		FString(),
		FString());
}

void UUEPIEditorSubsystem::HandleAssetRemoved(const FAssetData& AssetData)
{
	RecordIncrementalEvent(
		TEXT("asset_removed"),
		AssetData.GetSoftObjectPath().ToString(),
		AssetData.PackageName.ToString(),
		AssetData.AssetClassPath.ToString(),
		FString(),
		FString());
}

void UUEPIEditorSubsystem::HandleAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath)
{
	RecordIncrementalEvent(
		TEXT("asset_renamed"),
		AssetData.GetSoftObjectPath().ToString(),
		AssetData.PackageName.ToString(),
		AssetData.AssetClassPath.ToString(),
		OldObjectPath,
		FString());
}

void UUEPIEditorSubsystem::HandleAssetUpdated(const FAssetData& AssetData)
{
	RecordIncrementalEvent(
		TEXT("asset_updated"),
		AssetData.GetSoftObjectPath().ToString(),
		AssetData.PackageName.ToString(),
		AssetData.AssetClassPath.ToString(),
		FString(),
		FString());
}

void UUEPIEditorSubsystem::HandleBlueprintCompiled()
{
	RecordIncrementalEvent(TEXT("blueprint_compiled"), FString(), FString(), TEXT("UBlueprint"), FString(), FString());
}

void UUEPIEditorSubsystem::RecordIncrementalEvent(
	const FString& EventType,
	const FString& AssetPath,
	const FString& PackageName,
	const FString& ClassPath,
	const FString& OldObjectPath,
	const FString& PackageFileName)
{
	FUEPIIncrementalEvent Event;
	Event.Sequence = ++NextIncrementalEventSequence;
	Event.EventType = EventType;
	Event.TimestampUtc = FDateTime::UtcNow().ToIso8601();
	Event.AssetPath = AssetPath;
	Event.PackageName = PackageName;
	Event.ClassPath = ClassPath;
	Event.OldObjectPath = OldObjectPath;
	Event.PackageFileName = PackageFileName;

	IncrementalEvents.Add(Event);
	if (IncrementalEvents.Num() > GUEPIMaxIncrementalEventsInMemory)
	{
		IncrementalEvents.RemoveAt(0, IncrementalEvents.Num() - GUEPIMaxIncrementalEventsInMemory, false);
	}

	const FString EventLogPath = GetIncrementalEventLogPath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(EventLogPath), true);
	const FString Line = UEPIJsonObjectToString(UEPIIncrementalEventToJson(Event)) + LINE_TERMINATOR;
	FFileHelper::SaveStringToFile(Line, *EventLogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
}
