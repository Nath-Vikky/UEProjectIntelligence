#include "UEPIIndexCommandlet.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UEPIAssetRegistryScanner.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEPIIndexCommandlet, Log, All);

namespace
{
struct FUEPICommandletWorkerSession
{
	FString DaemonUrl;
	FString WorkerId;
	FString SessionId;
	FString SessionToken;
};

FString UEPICommandletJsonObjectToString(const TSharedRef<FJsonObject>& Object)
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Object, Writer);
	return Output;
}

TSharedPtr<FJsonObject> UEPIJsonObjectFromString(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Object))
	{
		return nullptr;
	}
	return Object;
}

FString UEPICommandletNormalizeDaemonUrl(FString DaemonUrl)
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

FString UEPICommandletJsonStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
{
	if (Object.IsValid() && Object->HasTypedField<EJson::String>(FieldName))
	{
		return Object->GetStringField(FieldName);
	}
	return FString();
}

bool UEPIJsonBoolField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, bool DefaultValue)
{
	if (Object.IsValid() && Object->HasTypedField<EJson::Boolean>(FieldName))
	{
		return Object->GetBoolField(FieldName);
	}
	return DefaultValue;
}

int32 UEPIJsonIntField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, int32 DefaultValue, int32 MinValue)
{
	if (!Object.IsValid())
	{
		return DefaultValue;
	}

	double NumberValue = 0.0;
	if (!Object->TryGetNumberField(FieldName, NumberValue))
	{
		return DefaultValue;
	}
	return FMath::Max(MinValue, static_cast<int32>(NumberValue));
}

TSharedRef<FJsonObject> UEPIWorkerError(const FString& Code, const FString& Message)
{
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetStringField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), Message);
	return Error;
}

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

bool UEPIAddStringArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FString>& OutValues)
{
	if (!Object.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values))
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		if (Value.IsValid() && Value->Type == EJson::String)
		{
			FString Text = Value->AsString();
			Text.TrimStartAndEndInline();
			if (!Text.IsEmpty())
			{
				OutValues.AddUnique(Text);
			}
		}
	}
	return true;
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

FString UEPIJobScanOutputPath(const FString& JobId)
{
	FString SafeJobId = FPaths::MakeValidFileName(JobId);
	if (SafeJobId.IsEmpty())
	{
		SafeJobId = TEXT("job_scan");
	}
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), SafeJobId + TEXT("_scan.json"));
}

bool UEPIPostJsonSync(
	const FString& Url,
	const TSharedRef<FJsonObject>& Payload,
	const FString& SessionToken,
	TSharedPtr<FJsonObject>& OutObject,
	FString& OutError,
	double TimeoutSeconds = 30.0)
{
	FString Body = UEPICommandletJsonObjectToString(Payload);
	bool bCompleted = false;
	bool bRequestSucceeded = false;
	int32 ResponseCode = 0;
	FString ResponseText;

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
	if (!SessionToken.IsEmpty())
	{
		Request->SetHeader(TEXT("X-UEPI-Session-Token"), SessionToken);
	}
	Request->SetContentAsString(Body);
	Request->OnProcessRequestComplete().BindLambda(
		[&bCompleted, &bRequestSucceeded, &ResponseCode, &ResponseText](FHttpRequestPtr CompletedRequest, FHttpResponsePtr Response, bool bWasSuccessful)
		{
			bRequestSucceeded = bWasSuccessful;
			if (Response.IsValid())
			{
				ResponseCode = Response->GetResponseCode();
				ResponseText = Response->GetContentAsString();
			}
			bCompleted = true;
		});

	if (!Request->ProcessRequest())
	{
		OutError = FString::Printf(TEXT("Failed to start UEPI HTTP request: %s"), *Url);
		return false;
	}

	const double Deadline = FPlatformTime::Seconds() + FMath::Max(1.0, TimeoutSeconds);
	while (!bCompleted && FPlatformTime::Seconds() < Deadline)
	{
		FHttpModule::Get().GetHttpManager().Tick(0.05f);
		FPlatformProcess::Sleep(0.01f);
	}

	if (!bCompleted)
	{
		Request->CancelRequest();
		OutError = FString::Printf(TEXT("Timed out waiting for UEPI daemon response: %s"), *Url);
		return false;
	}

	if (!bRequestSucceeded || ResponseCode < 200 || ResponseCode >= 300)
	{
		OutError = FString::Printf(TEXT("UEPI daemon request failed. HTTP=%d Url=%s Body=%s"), ResponseCode, *Url, *ResponseText);
		return false;
	}

	OutObject = UEPIJsonObjectFromString(ResponseText);
	if (!OutObject.IsValid())
	{
		OutError = FString::Printf(TEXT("UEPI daemon response was not a JSON object. Url=%s Body=%s"), *Url, *ResponseText);
		return false;
	}

	OutError.Reset();
	return true;
}

bool UEPIRegisterWorker(FUEPICommandletWorkerSession& Session, FString& OutError)
{
	TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	Capabilities->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Capabilities->SetStringField(TEXT("project_name"), FApp::GetProjectName());
	Capabilities->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
	Capabilities->SetBoolField(TEXT("metadata_scan"), true);
	Capabilities->SetBoolField(TEXT("commandlet"), true);
	Capabilities->SetBoolField(TEXT("read_only"), true);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("worker_id"), Session.WorkerId);
	Payload->SetStringField(TEXT("worker_type"), TEXT("commandlet"));
	Payload->SetObjectField(TEXT("capabilities"), Capabilities);
	Payload->SetNumberField(TEXT("ttl_seconds"), 60);

	TSharedPtr<FJsonObject> Response;
	if (!UEPIPostJsonSync(Session.DaemonUrl + TEXT("/workers/register"), Payload, FString(), Response, OutError))
	{
		return false;
	}

	Session.SessionId = UEPICommandletJsonStringField(Response, TEXT("session_id"));
	Session.SessionToken = UEPICommandletJsonStringField(Response, TEXT("session_token"));
	if (Session.SessionId.IsEmpty() || Session.SessionToken.IsEmpty())
	{
		OutError = TEXT("UEPI worker registration response did not include session_id/session_token.");
		return false;
	}
	return true;
}

bool UEPISendWorkerHeartbeat(const FUEPICommandletWorkerSession& Session, const FString& Status, FString& OutError)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("session_id"), Session.SessionId);
	Payload->SetStringField(TEXT("session_token"), Session.SessionToken);
	Payload->SetStringField(TEXT("status"), Status.IsEmpty() ? TEXT("online") : Status);
	Payload->SetNumberField(TEXT("ttl_seconds"), 60);

	TSharedPtr<FJsonObject> Response;
	return UEPIPostJsonSync(Session.DaemonUrl + TEXT("/workers/heartbeat"), Payload, Session.SessionToken, Response, OutError);
}

bool UEPIPollWorkerJobs(
	const FUEPICommandletWorkerSession& Session,
	int32 Limit,
	int32 WaitSeconds,
	TSharedPtr<FJsonObject>& OutResponse,
	FString& OutError)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("session_id"), Session.SessionId);
	Payload->SetStringField(TEXT("session_token"), Session.SessionToken);
	Payload->SetNumberField(TEXT("limit"), FMath::Clamp(Limit, 1, 20));
	Payload->SetNumberField(TEXT("wait_seconds"), FMath::Max(0, WaitSeconds));

	return UEPIPostJsonSync(
		Session.DaemonUrl + TEXT("/jobs/poll"),
		Payload,
		Session.SessionToken,
		OutResponse,
		OutError,
		FMath::Max(5.0, static_cast<double>(WaitSeconds) + 10.0));
}

bool UEPIUpdateWorkerJob(
	const FUEPICommandletWorkerSession& Session,
	const FString& JobId,
	const FString& State,
	const TSharedPtr<FJsonObject>& Result,
	const TSharedPtr<FJsonObject>& Error,
	const TArray<TSharedPtr<FJsonValue>>* Artifacts,
	FString& OutError)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("session_id"), Session.SessionId);
	Payload->SetStringField(TEXT("session_token"), Session.SessionToken);
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
	if (Artifacts != nullptr)
	{
		Payload->SetArrayField(TEXT("artifacts"), *Artifacts);
	}

	TSharedPtr<FJsonObject> Response;
	return UEPIPostJsonSync(Session.DaemonUrl + TEXT("/jobs/update"), Payload, Session.SessionToken, Response, OutError);
}

bool UEPIIsMetadataScanJobType(const FString& JobType)
{
	return JobType.Equals(TEXT("metadata_scan"), ESearchCase::IgnoreCase)
		|| JobType.Equals(TEXT("uepi.metadata_scan"), ESearchCase::IgnoreCase)
		|| JobType.Equals(TEXT("uepi_scan"), ESearchCase::IgnoreCase)
		|| JobType.Equals(TEXT("scan"), ESearchCase::IgnoreCase);
}

bool UEPIBuildScanOptionsFromRequest(
	const FString& JobId,
	const TSharedPtr<FJsonObject>& Request,
	UE::ProjectIntelligence::FScanOptions& Options,
	FString& OutError)
{
	const FString Level = UEPICommandletJsonStringField(Request, TEXT("level"));
	UEPIApplyLevelToOptions(Options, Level, true);

	Options.bIncludeGameContent = UEPIJsonBoolField(Request, TEXT("include_game_content"), Options.bIncludeGameContent);
	Options.bIncludeProjectPluginContent = UEPIJsonBoolField(Request, TEXT("include_project_plugin_content"), Options.bIncludeProjectPluginContent);
	Options.bIncludeEngineContent = UEPIJsonBoolField(Request, TEXT("include_engine_content"), Options.bIncludeEngineContent);
	Options.bReadBlueprintGraphs = UEPIJsonBoolField(Request, TEXT("read_blueprints"), Options.bReadBlueprintGraphs);
	Options.bReadBlueprintGraphs = UEPIJsonBoolField(Request, TEXT("read_blueprint_graphs"), Options.bReadBlueprintGraphs);
	Options.bReadUObjectReflection = UEPIJsonBoolField(Request, TEXT("read_uobject_reflection"), Options.bReadUObjectReflection);
	Options.bReadUObjectReflection = UEPIJsonBoolField(Request, TEXT("load_assets"), Options.bReadUObjectReflection);
	if (Options.bReadBlueprintGraphs)
	{
		Options.bReadUObjectReflection = true;
	}
	Options.MaxAssetsPerBatch = UEPIJsonIntField(Request, TEXT("max_assets_per_batch"), Options.MaxAssetsPerBatch, 1);
	Options.MaxInlineCollectionItems = UEPIJsonIntField(Request, TEXT("max_inline_collection_items"), Options.MaxInlineCollectionItems, 1);

	FString OutputPath = UEPICommandletJsonStringField(Request, TEXT("output_path"));
	if (OutputPath.IsEmpty())
	{
		OutputPath = UEPICommandletJsonStringField(Request, TEXT("output"));
	}
	if (OutputPath.IsEmpty())
	{
		OutputPath = UEPICommandletJsonStringField(Request, TEXT("scan_path"));
	}
	Options.OutputPath = OutputPath.IsEmpty() ? UEPIJobScanOutputPath(JobId) : OutputPath;

	TArray<FString> TargetObjectPaths;
	bool bHasTargetSpec = false;
	const FString Asset = UEPICommandletJsonStringField(Request, TEXT("asset"));
	if (!Asset.IsEmpty())
	{
		bHasTargetSpec = true;
		UEPIAddDelimitedStrings(Asset, TargetObjectPaths);
	}
	const FString TargetObjectPath = UEPICommandletJsonStringField(Request, TEXT("target_object_path"));
	if (!TargetObjectPath.IsEmpty())
	{
		bHasTargetSpec = true;
		UEPIAddDelimitedStrings(TargetObjectPath, TargetObjectPaths);
	}
	bHasTargetSpec = UEPIAddStringArrayField(Request, TEXT("assets"), TargetObjectPaths) || bHasTargetSpec;
	bHasTargetSpec = UEPIAddStringArrayField(Request, TEXT("target_object_paths"), TargetObjectPaths) || bHasTargetSpec;
	if (bHasTargetSpec)
	{
		Options.TargetObjectPaths = MoveTemp(TargetObjectPaths);
	}

	if (Options.OutputPath.IsEmpty())
	{
		OutError = TEXT("metadata_scan request did not resolve an output path.");
		return false;
	}

	OutError.Reset();
	return true;
}

bool UEPIExecuteMetadataScanJob(const FUEPICommandletWorkerSession& Session, const TSharedPtr<FJsonObject>& Job, FString& OutError)
{
	const FString JobId = UEPICommandletJsonStringField(Job, TEXT("job_id"));
	const FString JobType = UEPICommandletJsonStringField(Job, TEXT("job_type"));
	if (JobId.IsEmpty())
	{
		OutError = TEXT("UEPI worker received a job without job_id.");
		return false;
	}

	if (!UEPIIsMetadataScanJobType(JobType))
	{
		const TSharedRef<FJsonObject> Error = UEPIWorkerError(
			TEXT("UEPI_UNSUPPORTED_JOB_TYPE"),
			FString::Printf(TEXT("Commandlet worker only supports metadata_scan jobs; got '%s'."), *JobType));
		return UEPIUpdateWorkerJob(Session, JobId, TEXT("failed"), nullptr, Error, nullptr, OutError);
	}

	if (!UEPIUpdateWorkerJob(Session, JobId, TEXT("running"), nullptr, nullptr, nullptr, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	if (Job.IsValid() && Job->HasTypedField<EJson::Object>(TEXT("request")))
	{
		Request = Job->GetObjectField(TEXT("request"));
	}

	UE::ProjectIntelligence::FScanOptions Options = UE::ProjectIntelligence::FAssetRegistryScanner::MakeOptionsFromSettings();
	FString BuildError;
	if (!UEPIBuildScanOptionsFromRequest(JobId, Request, Options, BuildError))
	{
		const TSharedRef<FJsonObject> Error = UEPIWorkerError(TEXT("UEPI_INVALID_SCAN_REQUEST"), BuildError);
		return UEPIUpdateWorkerJob(Session, JobId, TEXT("failed"), nullptr, Error, nullptr, OutError);
	}

	UE_LOG(LogUEPIIndexCommandlet, Display, TEXT("UEPI commandlet worker scan started. Job=%s Output=%s"), *JobId, *Options.OutputPath);
	const UE::ProjectIntelligence::FProjectScanResult Result = UE::ProjectIntelligence::FAssetRegistryScanner::ScanProject(Options);

	FText ErrorText;
	if (!UE::ProjectIntelligence::FAssetRegistryScanner::WriteScanResultJson(Result, Options.OutputPath, ErrorText))
	{
		const TSharedRef<FJsonObject> Error = UEPIWorkerError(TEXT("UEPI_SCAN_WRITE_FAILED"), ErrorText.ToString());
		return UEPIUpdateWorkerJob(Session, JobId, TEXT("failed"), nullptr, Error, nullptr, OutError);
	}

	TSharedRef<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetStringField(TEXT("scan_path"), FPaths::ConvertRelativePathToFull(Options.OutputPath));
	ResultObject->SetStringField(TEXT("schema_version"), Result.SchemaVersion);
	ResultObject->SetNumberField(TEXT("entity_count"), Result.Entities.Num());
	ResultObject->SetNumberField(TEXT("relation_count"), Result.Relations.Num());
	ResultObject->SetNumberField(TEXT("diagnostic_count"), Result.Diagnostics.Num());

	TSharedRef<FJsonObject> Artifact = MakeShared<FJsonObject>();
	Artifact->SetStringField(TEXT("artifact_id"), TEXT("scan-json"));
	Artifact->SetStringField(TEXT("artifact_type"), TEXT("uepi.scan.v1"));
	Artifact->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(Options.OutputPath));
	Artifact->SetStringField(TEXT("encoding"), TEXT("json"));
	TArray<TSharedPtr<FJsonValue>> Artifacts;
	Artifacts.Add(MakeShared<FJsonValueObject>(Artifact));

	UE_LOG(
		LogUEPIIndexCommandlet,
		Display,
		TEXT("UEPI commandlet worker scan completed. Job=%s Assets=%d Relations=%d Diagnostics=%d Output=%s"),
		*JobId,
		Result.Entities.Num(),
		Result.Relations.Num(),
		Result.Diagnostics.Num(),
		*Options.OutputPath);

	return UEPIUpdateWorkerJob(Session, JobId, TEXT("succeeded"), ResultObject, nullptr, &Artifacts, OutError);
}

int32 UEPIRunCommandletWorker()
{
	FUEPICommandletWorkerSession Session;
	FString DaemonUrl;
	FParse::Value(FCommandLine::Get(), TEXT("UEPIWorkerUrl="), DaemonUrl);
	if (DaemonUrl.IsEmpty())
	{
		FParse::Value(FCommandLine::Get(), TEXT("UEPIDaemonUrl="), DaemonUrl);
	}
	Session.DaemonUrl = UEPICommandletNormalizeDaemonUrl(DaemonUrl);

	FParse::Value(FCommandLine::Get(), TEXT("UEPIWorkerId="), Session.WorkerId);
	if (Session.WorkerId.IsEmpty())
	{
		const FString ComputerName = FPlatformProcess::ComputerName();
		Session.WorkerId = FString::Printf(TEXT("%s-commandlet-%s"), FApp::GetProjectName(), *ComputerName);
	}

	int32 PollLimit = 1;
	FParse::Value(FCommandLine::Get(), TEXT("UEPIWorkerPollLimit="), PollLimit);
	PollLimit = FMath::Clamp(PollLimit, 1, 20);

	int32 WaitSeconds = 10;
	FParse::Value(FCommandLine::Get(), TEXT("UEPIWorkerWaitSeconds="), WaitSeconds);
	WaitSeconds = FMath::Clamp(WaitSeconds, 0, 300);

	int32 MaxJobs = 0;
	FParse::Value(FCommandLine::Get(), TEXT("UEPIWorkerMaxJobs="), MaxJobs);
	if (FParse::Param(FCommandLine::Get(), TEXT("UEPIWorkerOnce")) && MaxJobs <= 0)
	{
		MaxJobs = 1;
	}

	int32 MaxIdlePolls = 0;
	FParse::Value(FCommandLine::Get(), TEXT("UEPIWorkerMaxIdlePolls="), MaxIdlePolls);
	if (FParse::Param(FCommandLine::Get(), TEXT("UEPIWorkerOnce")) && MaxIdlePolls <= 0)
	{
		MaxIdlePolls = 1;
	}

	FString Error;
	if (!UEPIRegisterWorker(Session, Error))
	{
		UE_LOG(LogUEPIIndexCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}

	UE_LOG(
		LogUEPIIndexCommandlet,
		Display,
		TEXT("UEPI commandlet worker registered. WorkerId=%s SessionId=%s Daemon=%s"),
		*Session.WorkerId,
		*Session.SessionId,
		*Session.DaemonUrl);

	int32 CompletedJobs = 0;
	int32 IdlePolls = 0;
	while (!IsEngineExitRequested())
	{
		if (!UEPISendWorkerHeartbeat(Session, TEXT("idle"), Error))
		{
			UE_LOG(LogUEPIIndexCommandlet, Error, TEXT("%s"), *Error);
			return 1;
		}

		TSharedPtr<FJsonObject> PollResponse;
		if (!UEPIPollWorkerJobs(Session, PollLimit, WaitSeconds, PollResponse, Error))
		{
			UE_LOG(LogUEPIIndexCommandlet, Error, TEXT("%s"), *Error);
			return 1;
		}

		const TArray<TSharedPtr<FJsonValue>>* Jobs = nullptr;
		if (!PollResponse.IsValid() || !PollResponse->TryGetArrayField(TEXT("jobs"), Jobs))
		{
			UE_LOG(LogUEPIIndexCommandlet, Error, TEXT("UEPI jobs/poll response did not include a jobs array."));
			return 1;
		}

		if (Jobs->Num() == 0)
		{
			++IdlePolls;
			if (MaxIdlePolls > 0 && IdlePolls >= MaxIdlePolls)
			{
				UE_LOG(LogUEPIIndexCommandlet, Display, TEXT("UEPI commandlet worker exiting after %d idle polls."), IdlePolls);
				return 0;
			}
			continue;
		}

		IdlePolls = 0;
		for (const TSharedPtr<FJsonValue>& JobValue : *Jobs)
		{
			if (!JobValue.IsValid() || JobValue->Type != EJson::Object)
			{
				UE_LOG(LogUEPIIndexCommandlet, Warning, TEXT("UEPI worker ignored a non-object job payload."));
				continue;
			}

			if (!UEPIExecuteMetadataScanJob(Session, JobValue->AsObject(), Error))
			{
				UE_LOG(LogUEPIIndexCommandlet, Error, TEXT("%s"), *Error);
				return 1;
			}

			++CompletedJobs;
			if (MaxJobs > 0 && CompletedJobs >= MaxJobs)
			{
				UE_LOG(LogUEPIIndexCommandlet, Display, TEXT("UEPI commandlet worker completed %d job(s)."), CompletedJobs);
				return 0;
			}
		}
	}

	return 0;
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
	FString WorkerUrlProbe;
	if (FParse::Param(FCommandLine::Get(), TEXT("UEPIWorker"))
		|| FParse::Value(FCommandLine::Get(), TEXT("UEPIWorkerUrl="), WorkerUrlProbe)
		|| FParse::Value(FCommandLine::Get(), TEXT("UEPIDaemonUrl="), WorkerUrlProbe))
	{
		return UEPIRunCommandletWorker();
	}

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
		TargetAssets.ParseIntoArray(Options.TargetObjectPaths, TEXT(";"), true);
		for (FString& TargetObjectPath : Options.TargetObjectPaths)
		{
			TargetObjectPath.TrimStartAndEndInline();
		}
	}

	if (OutputPath.IsEmpty())
	{
		OutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("last_scan.json"));
	}
	Options.OutputPath = OutputPath;

	UE_LOG(LogUEPIIndexCommandlet, Display, TEXT("UEPI metadata scan started. Output=%s"), *Options.OutputPath);

	const UE::ProjectIntelligence::FProjectScanResult Result = UE::ProjectIntelligence::FAssetRegistryScanner::ScanProject(Options);

	FText ErrorText;
	if (!UE::ProjectIntelligence::FAssetRegistryScanner::WriteScanResultJson(Result, Options.OutputPath, ErrorText))
	{
		UE_LOG(LogUEPIIndexCommandlet, Error, TEXT("%s"), *ErrorText.ToString());
		return 1;
	}

	UE_LOG(
		LogUEPIIndexCommandlet,
		Display,
		TEXT("UEPI metadata scan completed. Assets=%d Relations=%d Diagnostics=%d Output=%s"),
		Result.Entities.Num(),
		Result.Relations.Num(),
		Result.Diagnostics.Num(),
		*Options.OutputPath);

	return Result.Diagnostics.Num() == 0 ? 0 : 2;
}
