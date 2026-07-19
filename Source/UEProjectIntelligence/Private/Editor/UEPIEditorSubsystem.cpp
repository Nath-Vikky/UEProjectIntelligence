#include "UEPIEditorSubsystem.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Bridge/UEPIEditorCommandBridge.h"
#include "Common/UEPIHash.h"
#include "UObject/UObjectIterator.h"
#include "UEPIAssetRegistryScanner.h"
#include "UEPISettings.h"
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

	FString UEPIRequestsDirectory()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("requests"));
	}

	FString UEPIProjectBindingId()
	{
		FString ProjectFile = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		FPaths::NormalizeFilename(ProjectFile);
		FPaths::CollapseRelativeDirectories(ProjectFile);
#if PLATFORM_WINDOWS
		ProjectFile = ProjectFile.ToLower();
#endif
		ProjectFile.RemoveFromEnd(TEXT("/"));
		FTCHARToUTF8 Utf8(*ProjectFile);
		const FString Digest = UE::ProjectIntelligence::Sha256Hex(Utf8.Get(), static_cast<uint64>(Utf8.Length()));
		return Digest.IsEmpty() ? FString() : TEXT("sha256:") + Digest;
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
		return EventType.Equals(TEXT("package_saved"), ESearchCase::IgnoreCase) ||
			EventType.Equals(TEXT("asset_renamed"), ESearchCase::IgnoreCase);
	}

	bool UEPIIsScannableInvalidationEvent(const FString& EventType)
	{
		return EventType.Equals(TEXT("blueprint_compiled"), ESearchCase::IgnoreCase) ||
			EventType.Equals(TEXT("asset_renamed"), ESearchCase::IgnoreCase) ||
			EventType.Equals(TEXT("package_saved"), ESearchCase::IgnoreCase);
	}

	bool UEPIAssetRegistryReady(FString& OutError)
	{
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
		{
			OutError = TEXT("UEPI_DEPENDENCY_MODULE_NOT_READY: Asset Registry module is not loaded.");
			return false;
		}
		IAssetRegistry& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		if (AssetRegistry.IsLoadingAssets())
		{
			OutError = TEXT("UEPI_DEPENDENCY_MODULE_NOT_READY: Asset Registry discovery is still in progress.");
			return false;
		}
		OutError.Reset();
		return true;
	}

	FString UEPIInvalidationKey(const FUEPIIncrementalEvent& Event)
	{
		if (!Event.AssetPath.IsEmpty())
		{
			return Event.AssetPath;
		}
		return Event.PackageName;
	}

	bool UEPILoadJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *Path))
		{
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	bool UEPISaveJsonObject(const TSharedRef<FJsonObject>& Object, const FString& Path)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		if (!FJsonSerializer::Serialize(Object, Writer))
		{
			return false;
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveStringToFile(Output, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	TArray<FString> UEPIStringArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		TArray<FString> Values;
		if (!Object.IsValid())
		{
			return Values;
		}

		const TArray<TSharedPtr<FJsonValue>>* JsonValues = nullptr;
		if (!Object->TryGetArrayField(FieldName, JsonValues) || JsonValues == nullptr)
		{
			return Values;
		}

		for (const TSharedPtr<FJsonValue>& Value : *JsonValues)
		{
			FString Text;
			if (Value.IsValid() && Value->TryGetString(Text) && !Text.IsEmpty())
			{
				Values.AddUnique(Text);
			}
		}
		return Values;
	}

	void UEPIWriteProjectModuleManifest()
	{
		TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(TEXT("schema_version"), TEXT("uepi.project-modules.v1"));
		Manifest->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		Manifest->SetStringField(TEXT("project_binding_id"), UEPIProjectBindingId());

		TArray<TSharedPtr<FJsonValue>> Modules;
		TArray<TSharedPtr<FJsonValue>> Plugins;
		TArray<TSharedPtr<FJsonValue>> AssetRoots = { MakeShared<FJsonValueString>(TEXT("/Game")) };
		TArray<TSharedPtr<FJsonValue>> SourceRoots;
		TSharedRef<FJsonObject> ProjectSource = MakeShared<FJsonObject>();
		ProjectSource->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"))));
		ProjectSource->SetStringField(TEXT("kind"), TEXT("project"));
		SourceRoots.Add(MakeShared<FJsonValueObject>(ProjectSource));

		TSharedPtr<FJsonObject> ProjectDescriptor;
		if (UEPILoadJsonObject(FPaths::GetProjectFilePath(), ProjectDescriptor) && ProjectDescriptor.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ModuleValues = nullptr;
			if (ProjectDescriptor->TryGetArrayField(TEXT("Modules"), ModuleValues) && ModuleValues)
			{
				for (const TSharedPtr<FJsonValue>& ModuleValue : *ModuleValues)
				{
					if (const TSharedPtr<FJsonObject> Module = ModuleValue.IsValid() ? ModuleValue->AsObject() : nullptr)
					{
						TSharedRef<FJsonObject> ModuleObject = MakeShared<FJsonObject>();
						FString ModuleName;
						FString ModuleType;
						Module->TryGetStringField(TEXT("Name"), ModuleName);
						Module->TryGetStringField(TEXT("Type"), ModuleType);
						ModuleObject->SetStringField(TEXT("name"), ModuleName);
						ModuleObject->SetStringField(TEXT("type"), ModuleType);
						ModuleObject->SetStringField(TEXT("owner"), TEXT("project"));
						Modules.Add(MakeShared<FJsonValueObject>(ModuleObject));
					}
				}
			}
		}

		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			if (Plugin->GetLoadedFrom() != EPluginLoadedFrom::Project)
			{
				continue;
			}
			TSharedRef<FJsonObject> PluginObject = MakeShared<FJsonObject>();
			PluginObject->SetStringField(TEXT("name"), Plugin->GetName());
			PluginObject->SetStringField(TEXT("descriptor"), Plugin->GetDescriptorFileName());
			PluginObject->SetStringField(TEXT("type"), TEXT("project_plugin"));
			PluginObject->SetBoolField(TEXT("enabled"), true);
			PluginObject->SetBoolField(TEXT("can_contain_content"), Plugin->CanContainContent());
			PluginObject->SetStringField(TEXT("mounted_asset_root"), Plugin->CanContainContent() ? Plugin->GetMountedAssetPath() : FString());
			PluginObject->SetStringField(TEXT("content_directory"), Plugin->GetContentDir());
			PluginObject->SetStringField(TEXT("source_directory"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source")));
			Plugins.Add(MakeShared<FJsonValueObject>(PluginObject));
			if (Plugin->CanContainContent())
			{
				AssetRoots.Add(MakeShared<FJsonValueString>(Plugin->GetMountedAssetPath().LeftChop(1)));
			}
			const FString PluginSourcePath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source"));
			if (IFileManager::Get().DirectoryExists(*PluginSourcePath))
			{
				TSharedRef<FJsonObject> SourceRoot = MakeShared<FJsonObject>();
				SourceRoot->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(PluginSourcePath));
				SourceRoot->SetStringField(TEXT("kind"), TEXT("project_plugin"));
				SourceRoot->SetStringField(TEXT("plugin"), Plugin->GetName());
				SourceRoots.Add(MakeShared<FJsonValueObject>(SourceRoot));
			}
			for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
			{
				TSharedRef<FJsonObject> ModuleObject = MakeShared<FJsonObject>();
				ModuleObject->SetStringField(TEXT("name"), Module.Name.ToString());
				ModuleObject->SetStringField(TEXT("owner"), Plugin->GetName());
				Modules.Add(MakeShared<FJsonValueObject>(ModuleObject));
			}
		}
		Manifest->SetArrayField(TEXT("modules"), Modules);
		Manifest->SetArrayField(TEXT("plugins"), Plugins);
		Manifest->SetArrayField(TEXT("asset_roots"), AssetRoots);
		Manifest->SetArrayField(TEXT("source_roots"), SourceRoots);
		Manifest->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
		UEPISaveJsonObject(Manifest, FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("project-modules.json")));
	}
}

void UUEPIEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (!IsRunningCommandlet())
	{
		UEPIWriteProjectModuleManifest();
		StartLiveSession();
		StartEditorBridgeIfEnabled();
		RegisterIncrementalDelegates();
		RegisterLoadedBlueprintCompileDelegates();
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
	StopEditorBridge();
	StopLiveSession();
	IncrementalEvents.Reset();
	PendingInvalidations.Reset();
	PendingInvalidationTimes.Reset();
	Super::Deinitialize();
}

bool UUEPIEditorSubsystem::RunMetadataScan(FString OutputPath, FString& OutReportPath, FString& OutError)
{
	if (!UEPIAssetRegistryReady(OutError)) { OutReportPath.Reset(); return false; }
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
	if (!UEPIAssetRegistryReady(OutError)) { OutReportPath.Reset(); LastCollectorError = OutError; return false; }
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
	// Targeted scans update one slice of the current view. Replacing live.json
	// here evicts assets refreshed earlier in the same Editor session.
	CommitOptions.bMergeWithExisting = true;

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
	OutStatus.PendingRefreshRequests = PendingRefreshRequests;
	OutStatus.IncrementalEvents = IncrementalEvents.Num();
	OutStatus.LastAutoScanUtc = LastAutoScanUtc;
	OutStatus.LastAutoScanMode = LastAutoScanMode;
	OutStatus.LastAutoScanManifestPath = LastAutoScanManifestPath;
	OutStatus.LastRefreshRequestUtc = LastRefreshRequestUtc;
	OutStatus.LastRefreshRequestPath = LastRefreshRequestPath;
	OutStatus.LastError = LastCollectorError;
	OutStatus.bBridgeActive = EditorBridge.IsValid() && EditorBridge->IsActive();
	OutStatus.BridgeSessionPath = EditorBridge.IsValid() ? EditorBridge->GetSessionPath() : FString();
	OutStatus.BridgePort = EditorBridge.IsValid() ? EditorBridge->GetPort() : 0;
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

void UUEPIEditorSubsystem::StartEditorBridgeIfEnabled()
{
	const UUEPISettings* Settings = GetDefault<UUEPISettings>();
	if (!Settings || !Settings->bEnableLiveEditorBridge)
	{
		return;
	}

	EditorBridge = MakeUnique<UE::ProjectIntelligence::FUEPIEditorCommandBridge>();
	FString Error;
	if (!EditorBridge->Start(LiveSessionId, Settings->LiveEditorBridgePort, Error))
	{
		LastCollectorError = Error;
		EditorBridge.Reset();
	}
}

void UUEPIEditorSubsystem::StopEditorBridge()
{
	if (EditorBridge.IsValid())
	{
		EditorBridge->Stop();
		EditorBridge.Reset();
	}
}

bool UUEPIEditorSubsystem::TickCollector(float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();
	if (Now - LastHeartbeatSeconds >= GUEPIHeartbeatSeconds)
	{
		WriteLiveSessionState(TEXT("active"));
	}

	RegisterLoadedBlueprintCompileDelegates();
	if (EditorBridge.IsValid())
	{
		EditorBridge->TickHeartbeat();
	}
	ProcessInvalidationQueue();
	ProcessRefreshRequests();
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
	FString ReadinessError;
	if (!UEPIAssetRegistryReady(ReadinessError)) { LastCollectorError = ReadinessError; return; }

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

void UUEPIEditorSubsystem::ProcessRefreshRequests()
{
	FString ReadinessError;
	if (!UEPIAssetRegistryReady(ReadinessError)) { LastCollectorError = ReadinessError; return; }
	const FString RequestsDir = UEPIRequestsDirectory();
	IFileManager::Get().MakeDirectory(*RequestsDir, true);

	TArray<FString> RequestFiles;
	IFileManager::Get().FindFiles(RequestFiles, *FPaths::Combine(RequestsDir, TEXT("*.json")), true, false);
	RequestFiles.Sort();

	int32 PendingCount = 0;
	int32 ProcessedCount = 0;
	for (const FString& RequestFile : RequestFiles)
	{
		const FString RequestPath = FPaths::Combine(RequestsDir, RequestFile);
		TSharedPtr<FJsonObject> Request;
		if (!UEPILoadJsonObject(RequestPath, Request) || !Request.IsValid())
		{
			continue;
		}

		FString SchemaVersion;
		Request->TryGetStringField(TEXT("schema_version"), SchemaVersion);
		const bool bRequestV2 = SchemaVersion.Equals(TEXT("uepi.refresh-request.v2"), ESearchCase::IgnoreCase);
		if (!bRequestV2 && !SchemaVersion.Equals(TEXT("uepi.refresh-request.v1"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (bRequestV2)
		{
			FString RequestBindingId;
			Request->TryGetStringField(TEXT("project_binding_id"), RequestBindingId);
			if (!RequestBindingId.IsEmpty() && RequestBindingId != UEPIProjectBindingId())
			{
				Request->SetStringField(TEXT("status"), TEXT("failed"));
				Request->SetStringField(TEXT("error"), TEXT("UEPI_PROJECT_BINDING_MISMATCH"));
				Request->SetStringField(TEXT("completed_at"), FDateTime::UtcNow().ToIso8601());
				UEPISaveJsonObject(Request.ToSharedRef(), RequestPath);
				continue;
			}
		}

		FString Status;
		Request->TryGetStringField(TEXT("status"), Status);
		if (Status.IsEmpty())
		{
			Status = TEXT("pending");
		}
		if (!Status.Equals(TEXT("queued"), ESearchCase::IgnoreCase) && !Status.Equals(TEXT("pending"), ESearchCase::IgnoreCase) && !Status.Equals(TEXT("running"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		++PendingCount;
		if (ProcessedCount >= 4)
		{
			continue;
		}

		TArray<FString> TargetObjectPaths = UEPIStringArrayField(Request, TEXT("target_object_paths"));
		for (const FString& Target : UEPIStringArrayField(Request, TEXT("targets")))
		{
			TargetObjectPaths.AddUnique(Target);
		}
		FString SingleTarget;
		if (Request->TryGetStringField(TEXT("target_object_path"), SingleTarget) && !SingleTarget.IsEmpty())
		{
			TargetObjectPaths.AddUnique(SingleTarget);
		}
		if (Request->TryGetStringField(TEXT("asset"), SingleTarget) && !SingleTarget.IsEmpty())
		{
			TargetObjectPaths.AddUnique(SingleTarget);
		}

		FString DataMode;
		if (!Request->TryGetStringField(TEXT("data_mode"), DataMode) || DataMode.IsEmpty())
		{
			DataMode = TEXT("live");
		}

		Request->SetStringField(TEXT("status"), TEXT("running"));
		Request->SetStringField(bRequestV2 ? TEXT("started_at") : TEXT("started_at_utc"), FDateTime::UtcNow().ToIso8601());
		Request->SetStringField(TEXT("editor_session_id"), LiveSessionId);
		FString ActiveRequestPath = RequestPath;
		if (bRequestV2 && RequestPath.EndsWith(TEXT(".queued.json")))
		{
			const FString RunningPath = RequestPath.LeftChop(12) + TEXT(".running.json");
			if (IFileManager::Get().Move(*RunningPath, *RequestPath, true, true, false, true))
			{
				ActiveRequestPath = RunningPath;
			}
		}
		UEPISaveJsonObject(Request.ToSharedRef(), ActiveRequestPath);

		FString ReportPath;
		FString Error;
		const bool bOk = RunTargetedSnapshotScan(TargetObjectPaths, DataMode, ReportPath, Error);
		Request->SetStringField(TEXT("status"), bOk ? (bRequestV2 ? TEXT("succeeded") : TEXT("completed")) : TEXT("failed"));
		Request->SetStringField(bRequestV2 ? TEXT("completed_at") : TEXT("completed_at_utc"), FDateTime::UtcNow().ToIso8601());
		Request->SetStringField(TEXT("editor_session_id"), LiveSessionId);
		Request->SetStringField(TEXT("manifest_path"), ReportPath);
		Request->SetStringField(TEXT("error"), Error);
		FString CompletedRequestPath = ActiveRequestPath;
		if (bRequestV2 && ActiveRequestPath.EndsWith(TEXT(".running.json")))
		{
			CompletedRequestPath = ActiveRequestPath.LeftChop(13) + TEXT(".completed.json");
		}
		UEPISaveJsonObject(Request.ToSharedRef(), ActiveRequestPath);
		if (CompletedRequestPath != ActiveRequestPath)
		{
			IFileManager::Get().Move(*CompletedRequestPath, *ActiveRequestPath, true, true, false, true);
		}

		LastRefreshRequestUtc = FDateTime::UtcNow().ToIso8601();
		LastRefreshRequestPath = CompletedRequestPath;
		++ProcessedCount;
	}

	PendingRefreshRequests = PendingCount;
}

void UUEPIEditorSubsystem::CommitAssetTombstoneFromEvent(const FUEPIIncrementalEvent& Event, const FString& Reason)
{
	UE::ProjectIntelligence::FSnapshotTombstoneOptions Options;
	Options.DataMode = TEXT("saved");
	Options.WriterMode = TEXT("editor");
	Options.SessionId = LiveSessionId;
	Options.AssetKey = !Event.OldObjectPath.IsEmpty() ? Event.OldObjectPath : Event.AssetPath;
	Options.AssetName = FPaths::GetBaseFilename(Options.AssetKey);
	Options.PackageName = Event.PackageName;
	Options.ClassPath = Event.ClassPath;
	Options.Reason = Reason;
	Options.EventType = Event.EventType;
	Options.OldObjectPath = Event.OldObjectPath;
	Options.NewObjectPath = Event.AssetPath;
	Options.SourceEventSequence = Event.Sequence;

	if (Options.AssetKey.IsEmpty())
	{
		return;
	}

	UE::ProjectIntelligence::FSnapshotCommitResult CommitResult;
	FText ErrorText;
	if (!UE::ProjectIntelligence::FSnapshotStore::CommitAssetTombstone(Options, CommitResult, ErrorText))
	{
		LastCollectorError = ErrorText.ToString();
		return;
	}

	LastCollectorError.Reset();
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

	RegisterLoadedBlueprintCompileDelegates();
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

	for (TObjectIterator<UBlueprint> It; It; ++It)
	{
		if (UBlueprint* Blueprint = *It)
		{
			Blueprint->OnCompiled().RemoveAll(this);
		}
	}
}

void UUEPIEditorSubsystem::RegisterLoadedBlueprintCompileDelegates()
{
	for (TObjectIterator<UBlueprint> It; It; ++It)
	{
		RegisterBlueprintCompileDelegate(*It);
	}
}

void UUEPIEditorSubsystem::RegisterBlueprintCompileDelegate(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return;
	}

	if (!Blueprint->OnCompiled().IsBoundToObject(this))
	{
		Blueprint->OnCompiled().AddUObject(this, &UUEPIEditorSubsystem::HandleBlueprintCompiled);
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
	if (IncrementalEvents.Num() > 0)
	{
		CommitAssetTombstoneFromEvent(IncrementalEvents.Last(), TEXT("asset_removed"));
	}
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
	if (IncrementalEvents.Num() > 0)
	{
		CommitAssetTombstoneFromEvent(IncrementalEvents.Last(), TEXT("asset_renamed_old_path"));
	}
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

void UUEPIEditorSubsystem::HandleBlueprintCompiled(UBlueprint* CompiledBlueprint)
{
	if (!CompiledBlueprint)
	{
		RecordIncrementalEvent(TEXT("blueprint_compiled"), FString(), FString(), TEXT("UBlueprint"), FString(), FString());
		return;
	}

	const FString AssetPath = CompiledBlueprint->GetPathName();
	const FString PackageName = CompiledBlueprint->GetOutermost() ? CompiledBlueprint->GetOutermost()->GetName() : FString();
	RecordIncrementalEvent(
		TEXT("blueprint_compiled"),
		AssetPath,
		PackageName,
		CompiledBlueprint->GetClass() ? CompiledBlueprint->GetClass()->GetPathName() : TEXT("UBlueprint"),
		FString(),
		FString());
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
