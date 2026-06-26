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
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UEPIAssetRegistryScanner.h"
#include "UEPISnapshotStore.h"

namespace
{
	constexpr int32 GUEPIMaxIncrementalEventsInMemory = 2048;
	constexpr double GUEPIDebounceSeconds = 2.0;
	constexpr double GUEPIHeartbeatSeconds = 5.0;
	constexpr float GUEPICollectorTickSeconds = 1.0f;
	constexpr int32 GUEPIMaxInvalidationsPerScan = 16;

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

	FString UEPISessionsDirectory()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("sessions"));
	}

	FString UEPIEditorSessionPath()
	{
		return FPaths::Combine(UEPISessionsDirectory(), TEXT("editor-session.json"));
	}

	FString UEPILiveScanArtifactPath(const FString& SessionId, int64 Sequence)
	{
		return FPaths::Combine(
			UEPISessionsDirectory(),
			FString::Printf(TEXT("live_scan_%s_%lld.json"), *SessionId, Sequence));
	}

	bool UEPIIsSavedPromotionEvent(const FString& EventType)
	{
		return EventType.Equals(TEXT("package_saved"), ESearchCase::IgnoreCase);
	}

	bool UEPIIsScannableInvalidationEvent(const FString& EventType)
	{
		return EventType.Equals(TEXT("asset_added"), ESearchCase::IgnoreCase) ||
			EventType.Equals(TEXT("asset_updated"), ESearchCase::IgnoreCase) ||
			EventType.Equals(TEXT("asset_renamed"), ESearchCase::IgnoreCase) ||
			EventType.Equals(TEXT("package_saved"), ESearchCase::IgnoreCase);
	}

	FString UEPIInvalidationKey(const FUEPIIncrementalEvent& Event)
	{
		if (!Event.AssetPath.IsEmpty())
		{
			return Event.AssetPath;
		}
		return Event.PackageName;
	}
}

void UUEPIEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (!IsRunningCommandlet())
	{
		StartLiveSession();
		RegisterIncrementalDelegates();
		CollectorTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &UUEPIEditorSubsystem::TickCollector),
			GUEPICollectorTickSeconds);
	}
}

void UUEPIEditorSubsystem::Deinitialize()
{
	if (CollectorTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(CollectorTickerHandle);
		CollectorTickerHandle.Reset();
	}
	UnregisterIncrementalDelegates();
	StopLiveSession();
	IncrementalEvents.Reset();
	PendingInvalidations.Reset();
	PendingInvalidationTimes.Reset();
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
	CommitOptions.SessionId = LiveSessionId;
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

bool UUEPIEditorSubsystem::RunTargetedSnapshotScan(const TArray<FString>& TargetObjectPaths, const FString& DataMode, FString& OutReportPath, FString& OutError)
{
	TArray<FString> CleanTargets;
	for (FString Target : TargetObjectPaths)
	{
		Target.TrimStartAndEndInline();
		if (!Target.IsEmpty())
		{
			CleanTargets.AddUnique(Target);
		}
	}

	if (CleanTargets.Num() == 0)
	{
		OutReportPath.Reset();
		OutError = TEXT("No target object paths were supplied for the targeted UEPI scan.");
		return false;
	}

	UE::ProjectIntelligence::FScanOptions Options = UE::ProjectIntelligence::FAssetRegistryScanner::MakeOptionsFromSettings();
	Options.TargetObjectPaths = CleanTargets;
	Options.bReadUObjectReflection = true;
	Options.bReadBlueprintGraphs = true;
	Options.OutputPath = UEPILiveScanArtifactPath(LiveSessionId.IsEmpty() ? TEXT("session") : LiveSessionId, ++NextAutoScanSequence);

	const UE::ProjectIntelligence::FProjectScanResult Result = UE::ProjectIntelligence::FAssetRegistryScanner::ScanProject(Options);

	FText ErrorText;
	if (!UE::ProjectIntelligence::FAssetRegistryScanner::WriteScanResultJson(Result, Options.OutputPath, ErrorText))
	{
		OutReportPath.Reset();
		OutError = ErrorText.ToString();
		LastCollectorError = OutError;
		return false;
	}

	const bool bSavedMode = DataMode.Equals(TEXT("saved"), ESearchCase::IgnoreCase);
	UE::ProjectIntelligence::FSnapshotCommitOptions CommitOptions;
	CommitOptions.DataMode = bSavedMode ? TEXT("saved") : TEXT("live");
	CommitOptions.WriterMode = TEXT("editor");
	CommitOptions.SessionId = LiveSessionId;
	CommitOptions.SourceScanPath = Options.OutputPath;
	CommitOptions.TargetObjectPaths = CleanTargets;
	CommitOptions.bMergeWithExisting = bSavedMode;

	UE::ProjectIntelligence::FSnapshotCommitResult CommitResult;
	if (!UE::ProjectIntelligence::FSnapshotStore::CommitProjectScan(Result, CommitOptions, CommitResult, ErrorText))
	{
		OutReportPath.Reset();
		OutError = ErrorText.ToString();
		LastCollectorError = OutError;
		return false;
	}

	OutReportPath = CommitResult.ManifestPath;
	OutError.Reset();
	LastCollectorError.Reset();
	LastAutoScanUtc = FDateTime::UtcNow().ToIso8601();
	LastAutoScanMode = CommitOptions.DataMode;
	LastAutoScanManifestPath = CommitResult.ManifestPath;
	return Result.Diagnostics.Num() == 0;
}

void UUEPIEditorSubsystem::GetCollectorStatus(FUEPICollectorStatus& OutStatus) const
{
	OutStatus.SessionId = LiveSessionId;
	OutStatus.SessionPath = UEPIEditorSessionPath();
	OutStatus.LastHeartbeatUtc = LastHeartbeatUtc;
	OutStatus.PendingInvalidations = PendingInvalidations.Num();
	OutStatus.IncrementalEvents = IncrementalEvents.Num();
	OutStatus.LastAutoScanUtc = LastAutoScanUtc;
	OutStatus.LastAutoScanMode = LastAutoScanMode;
	OutStatus.LastAutoScanManifestPath = LastAutoScanManifestPath;
	OutStatus.LastError = LastCollectorError;
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

void UUEPIEditorSubsystem::StartLiveSession()
{
	LiveSessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	WriteLiveSessionState(TEXT("active"));
}

void UUEPIEditorSubsystem::WriteLiveSessionState(const FString& State)
{
	if (LiveSessionId.IsEmpty())
	{
		return;
	}

	LastHeartbeatUtc = FDateTime::UtcNow().ToIso8601();
	LastHeartbeatSeconds = FPlatformTime::Seconds();

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema_version"), TEXT("uepi.live-session.v2"));
	Root->SetStringField(TEXT("session_id"), LiveSessionId);
	Root->SetStringField(TEXT("state"), State);
	Root->SetStringField(TEXT("project_name"), FApp::GetProjectName());
	Root->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
	Root->SetStringField(TEXT("last_seen_at_utc"), LastHeartbeatUtc);
	Root->SetNumberField(TEXT("pid"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));

	const FString SessionPath = UEPIEditorSessionPath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SessionPath), true);
	FFileHelper::SaveStringToFile(UEPIJsonObjectToString(Root), *SessionPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void UUEPIEditorSubsystem::StopLiveSession()
{
	WriteLiveSessionState(TEXT("stopped"));
	LiveSessionId.Reset();
}

bool UUEPIEditorSubsystem::TickCollector(float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();
	if (Now - LastHeartbeatSeconds >= GUEPIHeartbeatSeconds)
	{
		WriteLiveSessionState(TEXT("active"));
	}

	ProcessInvalidationQueue();
	return true;
}

void UUEPIEditorSubsystem::EnqueueInvalidation(const FUEPIIncrementalEvent& Event)
{
	if (!UEPIIsScannableInvalidationEvent(Event.EventType))
	{
		return;
	}

	const FString Key = UEPIInvalidationKey(Event);
	if (Key.IsEmpty())
	{
		return;
	}

	PendingInvalidations.Add(Key, Event);
	PendingInvalidationTimes.Add(Key, FPlatformTime::Seconds());
}

void UUEPIEditorSubsystem::ProcessInvalidationQueue()
{
	if (PendingInvalidations.Num() == 0)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	TArray<FString> ReadyKeys;
	for (const TPair<FString, double>& Pair : PendingInvalidationTimes)
	{
		if (Now - Pair.Value >= GUEPIDebounceSeconds)
		{
			ReadyKeys.Add(Pair.Key);
		}
	}

	if (ReadyKeys.Num() == 0)
	{
		return;
	}

	ReadyKeys.Sort();

	TArray<FString> LiveTargets;
	TArray<FString> SavedTargets;
	for (const FString& Key : ReadyKeys)
	{
		if (LiveTargets.Num() + SavedTargets.Num() >= GUEPIMaxInvalidationsPerScan)
		{
			break;
		}

		const FUEPIIncrementalEvent* Event = PendingInvalidations.Find(Key);
		if (!Event)
		{
			continue;
		}

		const FString Target = UEPIInvalidationKey(*Event);
		if (Target.IsEmpty())
		{
			continue;
		}

		if (UEPIIsSavedPromotionEvent(Event->EventType))
		{
			SavedTargets.AddUnique(Target);
		}
		else
		{
			LiveTargets.AddUnique(Target);
		}
	}

	for (const FString& Key : ReadyKeys)
	{
		if (const FUEPIIncrementalEvent* Event = PendingInvalidations.Find(Key))
		{
			const FString Target = UEPIInvalidationKey(*Event);
			if (LiveTargets.Contains(Target) || SavedTargets.Contains(Target))
			{
				PendingInvalidations.Remove(Key);
				PendingInvalidationTimes.Remove(Key);
			}
		}
	}

	FString ReportPath;
	FString Error;
	if (LiveTargets.Num() > 0)
	{
		RunTargetedSnapshotScan(LiveTargets, TEXT("live"), ReportPath, Error);
	}
	if (SavedTargets.Num() > 0)
	{
		RunTargetedSnapshotScan(SavedTargets, TEXT("saved"), ReportPath, Error);
	}
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

	EnqueueInvalidation(Event);
}
