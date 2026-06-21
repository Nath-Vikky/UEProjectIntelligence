#include "UEPIEditorSubsystem.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UEPIAssetRegistryScanner.h"

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

	FString UEPINormalizeDaemonUrl(FString DaemonUrl)
	{
		DaemonUrl.TrimStartAndEndInline();
		if (DaemonUrl.IsEmpty())
		{
			DaemonUrl = TEXT("http://127.0.0.1:8765");
		}
		while (DaemonUrl.EndsWith(TEXT("/")))
		{
			DaemonUrl.LeftChopInline(1);
		}
		if (!DaemonUrl.EndsWith(TEXT("/v1")))
		{
			DaemonUrl += TEXT("/v1");
		}
		return DaemonUrl;
	}

	TSharedPtr<FJsonObject> UEPIResponseJson(FHttpResponsePtr Response)
	{
		if (!Response.IsValid())
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
		if (!FJsonSerializer::Deserialize(Reader, Object))
		{
			return nullptr;
		}
		return Object;
	}

	FString UEPIJsonStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		if (!Object.IsValid())
		{
			return FString();
		}
		FString Value;
		Object->TryGetStringField(FieldName, Value);
		return Value;
	}
}

void UUEPIEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	RegisterIncrementalDelegates();
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

	OutReportPath = FPaths::ConvertRelativePathToFull(Options.OutputPath);
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
		OutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("incremental_events_snapshot.json"));
	}

	TArray<TSharedPtr<FJsonValue>> EventValues;
	EventValues.Reserve(IncrementalEvents.Num());
	for (const FUEPIIncrementalEvent& Event : IncrementalEvents)
	{
		EventValues.Add(MakeShared<FJsonValueObject>(UEPIIncrementalEventToJson(Event)));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema_version"), TEXT("uepi.incremental_events.v1"));
	Root->SetStringField(TEXT("project_name"), FApp::GetProjectName());
	Root->SetStringField(TEXT("generated_at_utc"), FDateTime::UtcNow().ToIso8601());
	Root->SetNumberField(TEXT("event_count"), IncrementalEvents.Num());
	Root->SetArrayField(TEXT("events"), EventValues);

	FString JsonText;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	FJsonSerializer::Serialize(Root, Writer);

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
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("incremental_events.jsonl"));
}

bool UUEPIEditorSubsystem::RegisterWorkerSession(FString DaemonUrl, FString WorkerId, FString& OutError)
{
	WorkerSessionStatus.DaemonUrl = UEPINormalizeDaemonUrl(DaemonUrl);
	if (WorkerId.IsEmpty())
	{
		WorkerId = FString::Printf(TEXT("%s-editor-%s"), FApp::GetProjectName(), *FPlatformProcess::ComputerName());
	}
	WorkerSessionStatus.WorkerId = WorkerId;
	WorkerSessionStatus.Status = TEXT("registering");
	WorkerSessionStatus.LastError.Reset();
	WorkerSessionToken.Reset();

	TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	Capabilities->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Capabilities->SetStringField(TEXT("project_name"), FApp::GetProjectName());
	Capabilities->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
	Capabilities->SetBoolField(TEXT("metadata_scan"), true);
	Capabilities->SetBoolField(TEXT("incremental_events"), true);
	Capabilities->SetBoolField(TEXT("read_only"), true);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("worker_id"), WorkerId);
	Payload->SetStringField(TEXT("worker_type"), TEXT("editor"));
	Payload->SetObjectField(TEXT("capabilities"), Capabilities);
	Payload->SetNumberField(TEXT("ttl_seconds"), 60);

	return PostWorkerJson(
		TEXT("/workers/register"),
		Payload,
		FHttpRequestCompleteDelegate::CreateUObject(this, &UUEPIEditorSubsystem::HandleWorkerRegisterResponse),
		OutError);
}

bool UUEPIEditorSubsystem::SendWorkerHeartbeat(FString& OutError)
{
	if (!WorkerSessionStatus.bHasSession || WorkerSessionStatus.SessionId.IsEmpty() || WorkerSessionToken.IsEmpty())
	{
		OutError = TEXT("No active UEPI worker session. Register with the daemon first.");
		WorkerSessionStatus.LastError = OutError;
		return false;
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("session_id"), WorkerSessionStatus.SessionId);
	Payload->SetStringField(TEXT("session_token"), WorkerSessionToken);
	Payload->SetStringField(TEXT("status"), TEXT("online"));
	Payload->SetNumberField(TEXT("ttl_seconds"), 60);

	return PostWorkerJson(
		TEXT("/workers/heartbeat"),
		Payload,
		FHttpRequestCompleteDelegate::CreateUObject(this, &UUEPIEditorSubsystem::HandleWorkerHeartbeatResponse),
		OutError);
}

FUEPIWorkerSessionStatus UUEPIEditorSubsystem::GetWorkerSessionStatus() const
{
	return WorkerSessionStatus;
}

bool UUEPIEditorSubsystem::PostWorkerJson(
	const FString& EndpointPath,
	const TSharedRef<FJsonObject>& Payload,
	FHttpRequestCompleteDelegate CompletionDelegate,
	FString& OutError) const
{
	if (WorkerSessionStatus.DaemonUrl.IsEmpty())
	{
		OutError = TEXT("UEPI daemon URL is empty.");
		return false;
	}

	FString Body;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Payload, Writer);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(WorkerSessionStatus.DaemonUrl + EndpointPath);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
	if (!WorkerSessionToken.IsEmpty())
	{
		Request->SetHeader(TEXT("X-UEPI-Session-Token"), WorkerSessionToken);
	}
	Request->SetContentAsString(Body);
	Request->OnProcessRequestComplete() = CompletionDelegate;

	if (!Request->ProcessRequest())
	{
		OutError = FString::Printf(TEXT("Failed to start UEPI worker HTTP request: %s"), *Request->GetURL());
		return false;
	}

	OutError.Reset();
	return true;
}

void UUEPIEditorSubsystem::HandleWorkerRegisterResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	const int32 ResponseCode = Response.IsValid() ? Response->GetResponseCode() : 0;
	if (!bWasSuccessful || !Response.IsValid() || ResponseCode < 200 || ResponseCode >= 300)
	{
		WorkerSessionStatus.bHasSession = false;
		WorkerSessionStatus.Status = TEXT("error");
		WorkerSessionStatus.LastError = FString::Printf(TEXT("UEPI worker registration failed. HTTP=%d"), ResponseCode);
		return;
	}

	const TSharedPtr<FJsonObject> Object = UEPIResponseJson(Response);
	const FString SessionId = UEPIJsonStringField(Object, TEXT("session_id"));
	const FString SessionToken = UEPIJsonStringField(Object, TEXT("session_token"));
	if (SessionId.IsEmpty() || SessionToken.IsEmpty())
	{
		WorkerSessionStatus.bHasSession = false;
		WorkerSessionStatus.Status = TEXT("error");
		WorkerSessionStatus.LastError = TEXT("UEPI worker registration response did not include session_id/session_token.");
		return;
	}

	WorkerSessionStatus.bHasSession = true;
	WorkerSessionStatus.SessionId = SessionId;
	WorkerSessionStatus.WorkerId = UEPIJsonStringField(Object, TEXT("worker_id"));
	WorkerSessionStatus.Status = UEPIJsonStringField(Object, TEXT("status"));
	WorkerSessionStatus.ExpiresAtUtc = UEPIJsonStringField(Object, TEXT("expires_at_utc"));
	WorkerSessionStatus.LastError.Reset();
	WorkerSessionToken = SessionToken;
}

void UUEPIEditorSubsystem::HandleWorkerHeartbeatResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	const int32 ResponseCode = Response.IsValid() ? Response->GetResponseCode() : 0;
	if (!bWasSuccessful || !Response.IsValid() || ResponseCode < 200 || ResponseCode >= 300)
	{
		WorkerSessionStatus.Status = TEXT("error");
		WorkerSessionStatus.LastError = FString::Printf(TEXT("UEPI worker heartbeat failed. HTTP=%d"), ResponseCode);
		return;
	}

	const TSharedPtr<FJsonObject> Object = UEPIResponseJson(Response);
	WorkerSessionStatus.Status = UEPIJsonStringField(Object, TEXT("status"));
	WorkerSessionStatus.ExpiresAtUtc = UEPIJsonStringField(Object, TEXT("expires_at_utc"));
	WorkerSessionStatus.LastError.Reset();
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
