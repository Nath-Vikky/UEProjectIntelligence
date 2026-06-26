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
#include "UEPISettings.h"
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

	bool UEPILiveJsonBoolField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, bool DefaultValue)
	{
		if (!Object.IsValid())
		{
			return DefaultValue;
		}
		bool Value = DefaultValue;
		return Object->TryGetBoolField(FieldName, Value) ? Value : DefaultValue;
	}

	int32 UEPILiveJsonIntField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, int32 DefaultValue, int32 MinValue = 0)
	{
		if (!Object.IsValid())
		{
			return DefaultValue;
		}
		int32 Value = DefaultValue;
		if (!Object->TryGetNumberField(FieldName, Value))
		{
			return DefaultValue;
		}
		return FMath::Max(Value, MinValue);
	}

	void UEPILiveAddDelimitedStrings(const FString& Text, TArray<FString>& OutValues)
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() == 0 && !Text.IsEmpty())
		{
			Parts.Add(Text);
		}
		for (FString Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (!Part.IsEmpty())
			{
				OutValues.AddUnique(Part);
			}
		}
	}

	bool UEPILiveAddStringArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FString>& OutValues)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}
		bool bAdded = false;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			FString Text;
			if (Value->TryGetString(Text))
			{
				UEPILiveAddDelimitedStrings(Text, OutValues);
				bAdded = true;
			}
		}
		return bAdded;
	}

	void UEPIApplyLevelToOptions(UE::ProjectIntelligence::FScanOptions& Options, const FString& Level)
	{
		if (Level.Equals(TEXT("L0"), ESearchCase::IgnoreCase))
		{
			Options.bReadUObjectReflection = false;
			Options.bReadBlueprintGraphs = false;
		}
		else if (Level.Equals(TEXT("L1"), ESearchCase::IgnoreCase))
		{
			Options.bReadUObjectReflection = true;
			Options.bReadBlueprintGraphs = false;
		}
		else if (Level.Equals(TEXT("L2"), ESearchCase::IgnoreCase)
			|| Level.Equals(TEXT("L3"), ESearchCase::IgnoreCase)
			|| Level.Equals(TEXT("L4"), ESearchCase::IgnoreCase))
		{
			Options.bReadUObjectReflection = true;
			Options.bReadBlueprintGraphs = true;
		}
	}

	bool UEPILiveIsMetadataScanJobType(const FString& JobType)
	{
		return JobType.Equals(TEXT("metadata_scan"), ESearchCase::IgnoreCase)
			|| JobType.Equals(TEXT("uepi.metadata_scan"), ESearchCase::IgnoreCase)
			|| JobType.Equals(TEXT("uepi_scan"), ESearchCase::IgnoreCase)
			|| JobType.Equals(TEXT("scan"), ESearchCase::IgnoreCase);
	}

	TSharedRef<FJsonObject> UEPILiveWorkerError(const FString& Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("code"), Code);
		Object->SetStringField(TEXT("message"), Message);
		return Object;
	}

	FString UEPIDaemonUrlFromSettings()
	{
		const UUEPISettings* Settings = GetDefault<UUEPISettings>();
		if (!Settings)
		{
			return TEXT("http://127.0.0.1:8765");
		}
		return FString::Printf(TEXT("http://%s:%d"), *Settings->DaemonHost, Settings->DaemonPort);
	}
}

void UUEPIEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (IsRunningCommandlet())
	{
		return;
	}

	RegisterIncrementalDelegates();
	if (!WorkerTickerHandle.IsValid())
	{
		WorkerTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UUEPIEditorSubsystem::TickLiveWorker), 1.0f);
	}

}

void UUEPIEditorSubsystem::Deinitialize()
{
	if (WorkerTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(WorkerTickerHandle);
		WorkerTickerHandle.Reset();
	}
	StopLiveWorker();
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
		const FString ComputerName = FPlatformProcess::ComputerName();
		WorkerId = FString::Printf(TEXT("%s-editor-%s"), FApp::GetProjectName(), *ComputerName);
	}
	WorkerSessionStatus.WorkerId = WorkerId;
	WorkerSessionStatus.Status = TEXT("registering");
	WorkerSessionStatus.LastError.Reset();
	WorkerSessionStatus.bPollingEnabled = bLiveWorkerEnabled;
	WorkerSessionToken.Reset();
	bWorkerRegisterInFlight = true;
	LastWorkerRegisterAttemptSeconds = FPlatformTime::Seconds();

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

	const bool bStarted = PostWorkerJson(
		TEXT("/workers/register"),
		Payload,
		FHttpRequestCompleteDelegate::CreateUObject(this, &UUEPIEditorSubsystem::HandleWorkerRegisterResponse),
		OutError);
	if (!bStarted)
	{
		bWorkerRegisterInFlight = false;
		WorkerSessionStatus.LastError = OutError;
	}
	return bStarted;
}

bool UUEPIEditorSubsystem::StartLiveWorker(FString DaemonUrl, FString WorkerId, FString& OutError)
{
	bLiveWorkerEnabled = true;
	WorkerSessionStatus.bPollingEnabled = true;
	if (DaemonUrl.IsEmpty())
	{
		DaemonUrl = UEPIDaemonUrlFromSettings();
	}
	return RegisterWorkerSession(DaemonUrl, WorkerId, OutError);
}

void UUEPIEditorSubsystem::StopLiveWorker()
{
	bLiveWorkerEnabled = false;
	bWorkerPollInFlight = false;
	bWorkerJobInProgress = false;
	WorkerSessionStatus.bPollingEnabled = false;
	WorkerSessionStatus.bJobInProgress = false;
	WorkerSessionStatus.ActiveJobId.Reset();
	if (!WorkerSessionStatus.Status.Equals(TEXT("error"), ESearchCase::IgnoreCase))
	{
		WorkerSessionStatus.Status = WorkerSessionStatus.bHasSession ? TEXT("stopped") : WorkerSessionStatus.Status;
	}
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

bool UUEPIEditorSubsystem::PollWorkerJobs(FString& OutError)
{
	if (!WorkerSessionStatus.bHasSession || WorkerSessionStatus.SessionId.IsEmpty() || WorkerSessionToken.IsEmpty())
	{
		OutError = TEXT("No active UEPI worker session. Register with the daemon first.");
		WorkerSessionStatus.LastError = OutError;
		return false;
	}
	if (bWorkerPollInFlight || bWorkerJobInProgress)
	{
		OutError = TEXT("UEPI worker poll or job is already in progress.");
		return false;
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("session_id"), WorkerSessionStatus.SessionId);
	Payload->SetStringField(TEXT("session_token"), WorkerSessionToken);
	Payload->SetNumberField(TEXT("limit"), 1);
	Payload->SetNumberField(TEXT("wait_seconds"), 2);

	bWorkerPollInFlight = true;
	LastWorkerPollSeconds = FPlatformTime::Seconds();
	const bool bStarted = PostWorkerJson(
		TEXT("/jobs/poll"),
		Payload,
		FHttpRequestCompleteDelegate::CreateUObject(this, &UUEPIEditorSubsystem::HandleWorkerPollResponse),
		OutError);
	if (!bStarted)
	{
		bWorkerPollInFlight = false;
		WorkerSessionStatus.LastError = OutError;
	}
	return bStarted;
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
	bWorkerRegisterInFlight = false;
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
	WorkerSessionStatus.bPollingEnabled = bLiveWorkerEnabled;
	WorkerSessionToken = SessionToken;
	LastWorkerHeartbeatSeconds = FPlatformTime::Seconds();
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
	LastWorkerHeartbeatSeconds = FPlatformTime::Seconds();
}

void UUEPIEditorSubsystem::HandleWorkerPollResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	bWorkerPollInFlight = false;
	const int32 ResponseCode = Response.IsValid() ? Response->GetResponseCode() : 0;
	if (!bWasSuccessful || !Response.IsValid() || ResponseCode < 200 || ResponseCode >= 300)
	{
		WorkerSessionStatus.Status = TEXT("error");
		WorkerSessionStatus.LastError = FString::Printf(TEXT("UEPI worker job poll failed. HTTP=%d"), ResponseCode);
		return;
	}

	const TSharedPtr<FJsonObject> Object = UEPIResponseJson(Response);
	const TArray<TSharedPtr<FJsonValue>>* Jobs = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("jobs"), Jobs) || !Jobs || Jobs->Num() == 0)
	{
		return;
	}

	const TSharedPtr<FJsonObject> Job = (*Jobs)[0].IsValid() ? (*Jobs)[0]->AsObject() : nullptr;
	if (!Job.IsValid())
	{
		WorkerSessionStatus.LastError = TEXT("UEPI worker received a non-object job payload.");
		return;
	}
	ExecuteWorkerJob(Job);
}

void UUEPIEditorSubsystem::HandleWorkerJobUpdateResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	const int32 ResponseCode = Response.IsValid() ? Response->GetResponseCode() : 0;
	if (!bWasSuccessful || !Response.IsValid() || ResponseCode < 200 || ResponseCode >= 300)
	{
		WorkerSessionStatus.LastError = FString::Printf(TEXT("UEPI worker job update failed. HTTP=%d"), ResponseCode);
		return;
	}
	WorkerSessionStatus.LastError.Reset();
}

bool UUEPIEditorSubsystem::TickLiveWorker(float DeltaTime)
{
	if (!bLiveWorkerEnabled)
	{
		return true;
	}

	const double Now = FPlatformTime::Seconds();
	if (!WorkerSessionStatus.bHasSession)
	{
		if (!bWorkerRegisterInFlight && Now - LastWorkerRegisterAttemptSeconds > 5.0)
		{
			FString Error;
			RegisterWorkerSession(WorkerSessionStatus.DaemonUrl.IsEmpty() ? UEPIDaemonUrlFromSettings() : WorkerSessionStatus.DaemonUrl, WorkerSessionStatus.WorkerId, Error);
		}
		return true;
	}

	if (Now - LastWorkerHeartbeatSeconds > 20.0)
	{
		FString Error;
		SendWorkerHeartbeat(Error);
	}

	if (!bWorkerPollInFlight && !bWorkerJobInProgress && Now - LastWorkerPollSeconds > 1.0)
	{
		FString Error;
		PollWorkerJobs(Error);
	}
	return true;
}

void UUEPIEditorSubsystem::ExecuteWorkerJob(const TSharedPtr<FJsonObject>& Job)
{
	const FString JobId = UEPIJsonStringField(Job, TEXT("job_id"));
	const FString JobType = UEPIJsonStringField(Job, TEXT("job_type"));
	if (JobId.IsEmpty())
	{
		WorkerSessionStatus.LastError = TEXT("UEPI worker received a job without job_id.");
		return;
	}

	bWorkerJobInProgress = true;
	WorkerSessionStatus.bJobInProgress = true;
	WorkerSessionStatus.ActiveJobId = JobId;
	WorkerSessionStatus.Status = TEXT("running");

	FString ErrorText;
	if (!UEPILiveIsMetadataScanJobType(JobType))
	{
		TArray<TSharedPtr<FJsonValue>> Artifacts;
		SendWorkerJobUpdate(
			JobId,
			TEXT("failed"),
			nullptr,
			UEPILiveWorkerError(TEXT("UEPI_UNSUPPORTED_JOB_TYPE"), FString::Printf(TEXT("Editor Live Worker only supports metadata_scan jobs; got '%s'."), *JobType)),
			Artifacts,
			ErrorText);
	}
	else
	{
		ExecuteMetadataScanJob(Job, ErrorText);
	}

	bWorkerJobInProgress = false;
	WorkerSessionStatus.bJobInProgress = false;
	WorkerSessionStatus.ActiveJobId.Reset();
	if (!ErrorText.IsEmpty())
	{
		WorkerSessionStatus.LastError = ErrorText;
		WorkerSessionStatus.Status = TEXT("error");
	}
}

bool UUEPIEditorSubsystem::ExecuteMetadataScanJob(const TSharedPtr<FJsonObject>& Job, FString& OutError)
{
	const FString JobId = UEPIJsonStringField(Job, TEXT("job_id"));
	UE::ProjectIntelligence::FScanOptions Options = UE::ProjectIntelligence::FAssetRegistryScanner::MakeOptionsFromSettings();
	if (!BuildScanOptionsFromWorkerJob(JobId, Job, Options, OutError))
	{
		TArray<TSharedPtr<FJsonValue>> Artifacts;
		SendWorkerJobUpdate(JobId, TEXT("failed"), nullptr, UEPILiveWorkerError(TEXT("UEPI_INVALID_SCAN_REQUEST"), OutError), Artifacts, OutError);
		return false;
	}

	const UE::ProjectIntelligence::FProjectScanResult Result = UE::ProjectIntelligence::FAssetRegistryScanner::ScanProject(Options);
	FText WriteError;
	if (!UE::ProjectIntelligence::FAssetRegistryScanner::WriteScanResultJson(Result, Options.OutputPath, WriteError))
	{
		const FString Message = WriteError.ToString();
		TArray<TSharedPtr<FJsonValue>> Artifacts;
		SendWorkerJobUpdate(JobId, TEXT("failed"), nullptr, UEPILiveWorkerError(TEXT("UEPI_SCAN_WRITE_FAILED"), Message), Artifacts, OutError);
		return false;
	}

	TSharedRef<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetStringField(TEXT("schema_version"), TEXT("uepi.worker.metadata_scan_result.v1"));
	ResultObject->SetStringField(TEXT("scan_path"), FPaths::ConvertRelativePathToFull(Options.OutputPath));
	ResultObject->SetNumberField(TEXT("entity_count"), Result.Entities.Num());
	ResultObject->SetNumberField(TEXT("relation_count"), Result.Relations.Num());
	ResultObject->SetNumberField(TEXT("diagnostic_count"), Result.Diagnostics.Num());
	ResultObject->SetStringField(TEXT("completeness_state"), LexToString(Result.Completeness.State));

	TSharedRef<FJsonObject> Artifact = MakeShared<FJsonObject>();
	Artifact->SetStringField(TEXT("artifact_id"), TEXT("scan-json"));
	Artifact->SetStringField(TEXT("artifact_type"), TEXT("uepi.scan.v1"));
	Artifact->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(Options.OutputPath));
	TArray<TSharedPtr<FJsonValue>> Artifacts;
	Artifacts.Add(MakeShared<FJsonValueObject>(Artifact));

	if (!SendWorkerJobUpdate(JobId, TEXT("succeeded"), ResultObject, nullptr, Artifacts, OutError))
	{
		return false;
	}

	WorkerSessionStatus.LastCompletedJobId = JobId;
	WorkerSessionStatus.CompletedJobCount += 1;
	WorkerSessionStatus.Status = TEXT("online");
	return true;
}

bool UUEPIEditorSubsystem::BuildScanOptionsFromWorkerJob(const FString& JobId, const TSharedPtr<FJsonObject>& Job, UE::ProjectIntelligence::FScanOptions& Options, FString& OutError) const
{
	TSharedPtr<FJsonObject> Request;
	if (Job.IsValid() && Job->HasTypedField<EJson::Object>(TEXT("request")))
	{
		Request = Job->GetObjectField(TEXT("request"));
	}
	if (!Request.IsValid())
	{
		Request = MakeShared<FJsonObject>();
	}

	UEPIApplyLevelToOptions(Options, UEPIJsonStringField(Request, TEXT("level")));
	Options.bIncludeGameContent = UEPILiveJsonBoolField(Request, TEXT("include_game_content"), Options.bIncludeGameContent);
	Options.bIncludeProjectPluginContent = UEPILiveJsonBoolField(Request, TEXT("include_project_plugin_content"), Options.bIncludeProjectPluginContent);
	Options.bIncludeEngineContent = UEPILiveJsonBoolField(Request, TEXT("include_engine_content"), Options.bIncludeEngineContent);
	Options.bReadBlueprintGraphs = UEPILiveJsonBoolField(Request, TEXT("read_blueprints"), Options.bReadBlueprintGraphs);
	Options.bReadBlueprintGraphs = UEPILiveJsonBoolField(Request, TEXT("read_blueprint_graphs"), Options.bReadBlueprintGraphs);
	Options.bReadUObjectReflection = UEPILiveJsonBoolField(Request, TEXT("read_uobject_reflection"), Options.bReadUObjectReflection);
	Options.bReadUObjectReflection = UEPILiveJsonBoolField(Request, TEXT("load_assets"), Options.bReadUObjectReflection);
	if (Options.bReadBlueprintGraphs)
	{
		Options.bReadUObjectReflection = true;
	}
	Options.MaxAssetsPerBatch = UEPILiveJsonIntField(Request, TEXT("max_assets_per_batch"), Options.MaxAssetsPerBatch, 1);
	Options.MaxInlineCollectionItems = UEPILiveJsonIntField(Request, TEXT("max_inline_collection_items"), Options.MaxInlineCollectionItems, 1);

	FString OutputPath = UEPIJsonStringField(Request, TEXT("output_path"));
	if (OutputPath.IsEmpty())
	{
		OutputPath = UEPIJsonStringField(Request, TEXT("output"));
	}
	if (OutputPath.IsEmpty())
	{
		OutputPath = UEPIJsonStringField(Request, TEXT("scan_path"));
	}
	if (OutputPath.IsEmpty())
	{
		const FString SafeJobId = JobId.IsEmpty() ? TEXT("editor_job_scan") : JobId.Replace(TEXT("/"), TEXT("_")).Replace(TEXT("\\"), TEXT("_"));
		OutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), SafeJobId + TEXT("_scan.json"));
	}
	Options.OutputPath = OutputPath;

	TArray<FString> TargetObjectPaths;
	bool bHasTargetSpec = false;
	const FString Asset = UEPIJsonStringField(Request, TEXT("asset"));
	if (!Asset.IsEmpty())
	{
		bHasTargetSpec = true;
		UEPILiveAddDelimitedStrings(Asset, TargetObjectPaths);
	}
	const FString TargetObjectPath = UEPIJsonStringField(Request, TEXT("target_object_path"));
	if (!TargetObjectPath.IsEmpty())
	{
		bHasTargetSpec = true;
		UEPILiveAddDelimitedStrings(TargetObjectPath, TargetObjectPaths);
	}
	bHasTargetSpec = UEPILiveAddStringArrayField(Request, TEXT("assets"), TargetObjectPaths) || bHasTargetSpec;
	bHasTargetSpec = UEPILiveAddStringArrayField(Request, TEXT("target_object_paths"), TargetObjectPaths) || bHasTargetSpec;
	if (bHasTargetSpec)
	{
		Options.TargetObjectPaths = MoveTemp(TargetObjectPaths);
	}

	OutError.Reset();
	return true;
}

bool UUEPIEditorSubsystem::SendWorkerJobUpdate(const FString& JobId, const FString& State, const TSharedPtr<FJsonObject>& Result, const TSharedPtr<FJsonObject>& Error, const TArray<TSharedPtr<FJsonValue>>& Artifacts, FString& OutError)
{
	if (!WorkerSessionStatus.bHasSession || WorkerSessionStatus.SessionId.IsEmpty() || WorkerSessionToken.IsEmpty())
	{
		OutError = TEXT("No active UEPI worker session. Cannot update job.");
		return false;
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("session_id"), WorkerSessionStatus.SessionId);
	Payload->SetStringField(TEXT("session_token"), WorkerSessionToken);
	Payload->SetStringField(TEXT("job_id"), JobId);
	Payload->SetStringField(TEXT("state"), State);
	if (Result.IsValid())
	{
		Payload->SetObjectField(TEXT("result"), Result.ToSharedRef());
	}
	if (Error.IsValid())
	{
		Payload->SetObjectField(TEXT("error"), Error.ToSharedRef());
	}
	if (Artifacts.Num() > 0)
	{
		Payload->SetArrayField(TEXT("artifacts"), Artifacts);
	}

	return PostWorkerJson(
		TEXT("/jobs/update"),
		Payload,
		FHttpRequestCompleteDelegate::CreateUObject(this, &UUEPIEditorSubsystem::HandleWorkerJobUpdateResponse),
		OutError);
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
