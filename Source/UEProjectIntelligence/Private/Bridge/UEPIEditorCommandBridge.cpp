#include "Bridge/UEPIEditorCommandBridge.h"

#include "Bridge/UEPIBridgeProtocol.h"
#include "Common/UEPIHash.h"
#include "Components/ActorComponent.h"
#include "Common/TcpListener.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "ImageUtils.h"
#include "InputCoreTypes.h"
#include "K2Node_MakeStruct.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Edit/UEPIEditOperationRegistry.h"
#include "Edit/UEPIBackupService.h"
#include "Edit/UEPIPackageSaveService.h"
#include "Edit/UEPITransactionJournal.h"
#include "Logging/TokenizedMessage.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "ScopedTransaction.h"
#include "PlayInEditorDataTypes.h"
#include "Reflection/UEPIPropertyCodec.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UEPISettings.h"
#include "UEPISnapshotStore.h"
#include "Validation/UEPIValidatorRegistry.h"
#include "UnrealClient.h"
#include "WidgetBlueprint.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace UE::ProjectIntelligence
{
	namespace
	{
		FString UEPICanonicalProjectFile()
		{
			FString ProjectFile = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
			FPaths::NormalizeFilename(ProjectFile);
			FPaths::CollapseRelativeDirectories(ProjectFile);
#if PLATFORM_WINDOWS
			ProjectFile = ProjectFile.ToLower();
#endif
			ProjectFile.RemoveFromEnd(TEXT("/"));
			return ProjectFile;
		}

		FString UEPIProjectBindingId()
		{
			const FString CanonicalProjectFile = UEPICanonicalProjectFile();
			FTCHARToUTF8 Utf8(*CanonicalProjectFile);
			const FString Digest = Sha256Hex(Utf8.Get(), static_cast<uint64>(Utf8.Length()));
			return Digest.IsEmpty() ? FString() : TEXT("sha256:") + Digest;
		}

		FString UEPIFileSha256(const FString& Filename)
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
			{
				return FString();
			}
			const FString Digest = Sha256Hex(Bytes.GetData(), static_cast<uint64>(Bytes.Num()));
			return Digest.IsEmpty() ? FString() : TEXT("sha256:") + Digest;
		}

		FString UEPISessionsDirectory()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("sessions")));
		}

		bool SaveJsonObject(const TSharedRef<FJsonObject>& Object, const FString& Path);

		FString UEPIBridgeSessionPath()
		{
			return FPaths::Combine(UEPISessionsDirectory(), TEXT("editor-bridge.json"));
		}

		FString UEPIBridgeTokenPath()
		{
			return FPaths::Combine(UEPISessionsDirectory(), TEXT("editor-bridge-token.txt"));
		}

		FString UEPIStoreRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence")));
		}

		FString UEPIGlobalSessionsDirectory()
		{
			FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
			if (LocalAppData.IsEmpty())
			{
				LocalAppData = FPaths::ProjectSavedDir();
			}
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(LocalAppData, TEXT("UEProjectIntelligence"), TEXT("sessions")));
		}

		FString UEPIGlobalBridgeSessionPath()
		{
			const FString ProjectHash = UEPIProjectBindingId().Replace(TEXT("sha256:"), TEXT("")).Left(12);
			const FString ProjectName(FApp::GetProjectName());
			const FString FileName = FPaths::MakeValidFileName(FString::Printf(TEXT("%s-%s.json"), *ProjectName, *ProjectHash));
			return FPaths::Combine(UEPIGlobalSessionsDirectory(), FileName);
		}

		FString UEPIRequestsDirectory()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("requests")));
		}

		FString JsonObjectToString(const TSharedRef<FJsonObject>& Object)
		{
			FString Output;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
			FJsonSerializer::Serialize(Object, Writer);
			return Output;
		}

		TArray<TSharedPtr<FJsonValue>> StringArrayToJsonValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			JsonValues.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Value));
			}
			return JsonValues;
		}

		TArray<TSharedPtr<FJsonValue>> EmptyJsonArray()
		{
			return TArray<TSharedPtr<FJsonValue>>();
		}

		TSharedRef<FJsonObject> SuccessResponse(const FString& RequestId, const TSharedRef<FJsonObject>& Result, TArray<TSharedPtr<FJsonValue>> Diagnostics = EmptyJsonArray())
		{
			TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("id"), RequestId);
			Response->SetBoolField(TEXT("ok"), true);
			Response->SetObjectField(TEXT("result"), Result);
			Response->SetArrayField(TEXT("diagnostics"), Diagnostics);
			return Response;
		}

		TSharedRef<FJsonObject> DiagnosticObject(const FString& Code, const FString& Severity, const FString& Message)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("code"), Code);
			Object->SetStringField(TEXT("severity"), Severity);
			Object->SetStringField(TEXT("message"), Message);
			return Object;
		}

		TArray<TSharedPtr<FJsonValue>> DiagnosticsArray(const FString& Code, const FString& Severity, const FString& Message)
		{
			return { MakeShared<FJsonValueObject>(DiagnosticObject(Code, Severity, Message)) };
		}

		FString JsonString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& DefaultValue = FString())
		{
			if (!Object.IsValid())
			{
				return DefaultValue;
			}
			FString Value;
			return Object->TryGetStringField(Field, Value) ? Value : DefaultValue;
		}

		bool JsonBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool bDefaultValue = false)
		{
			if (!Object.IsValid())
			{
				return bDefaultValue;
			}
			bool bValue = false;
			return Object->TryGetBoolField(Field, bValue) ? bValue : bDefaultValue;
		}

		int32 JsonInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 DefaultValue = 0)
		{
			if (!Object.IsValid())
			{
				return DefaultValue;
			}
			int32 Value = 0;
			return Object->TryGetNumberField(Field, Value) ? Value : DefaultValue;
		}

		const TArray<TSharedPtr<FJsonValue>>* JsonArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
		{
			if (!Object.IsValid())
			{
				return nullptr;
			}
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			return Object->TryGetArrayField(Field, Values) ? Values : nullptr;
		}

		TArray<FString> JsonStringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
		{
			TArray<FString> Result;
			if (const TArray<TSharedPtr<FJsonValue>>* Values = JsonArray(Object, Field))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					FString Text;
					if (Value.IsValid() && Value->TryGetString(Text) && !Text.IsEmpty())
					{
						Result.AddUnique(Text);
					}
				}
			}
			return Result;
		}

		TSharedPtr<FJsonObject> JsonObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field);

		FString RuntimeFunctionKey(const UFunction* Function)
		{
			return Function
				? FString::Printf(TEXT("%s:%s"), Function->GetOwnerClass() ? *Function->GetOwnerClass()->GetPathName() : TEXT(""), *Function->GetName())
				: FString();
		}

		bool RuntimeFunctionSemanticsAllowed(const UFunction* Function, FString& OutError)
		{
			if (!Function || !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
			{
				OutError = TEXT("Runtime function must be BlueprintCallable.");
				return false;
			}
			if (Function->HasAnyFunctionFlags(FUNC_Exec) || Function->HasMetaData(TEXT("Latent")))
			{
				OutError = TEXT("Runtime function must be non-Exec and non-Latent.");
				return false;
			}
			return true;
		}

		UObject* ResolveRuntimeObject(UWorld* PIEWorld, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			const FString ObjectPath = JsonString(Params, TEXT("object_path"));
			if (!ObjectPath.IsEmpty())
			{
				UObject* Object = FindObject<UObject>(nullptr, *ObjectPath);
				if (Object && Object->GetWorld() == PIEWorld) return Object;
				OutError = FString::Printf(TEXT("Runtime object was not found in the owned PIE world: %s"), *ObjectPath);
				return nullptr;
			}

			const TSharedPtr<FJsonObject> Target = JsonObjectField(Params, TEXT("target"));
			const FString ClassPath = JsonString(Target, TEXT("class"), JsonString(Target, TEXT("class_path")));
			if (!Target.IsValid() || ClassPath.IsEmpty())
			{
				OutError = TEXT("Runtime object_path or target.class is required.");
				return nullptr;
			}
			UClass* TargetClass = LoadObject<UClass>(nullptr, *ClassPath);
			if (!TargetClass)
			{
				OutError = FString::Printf(TEXT("Runtime target class was not found: %s"), *ClassPath);
				return nullptr;
			}
			const FName ActorTag(*JsonString(Target, TEXT("actor_tag")));
			const FString ObjectName = JsonString(Target, TEXT("object_name"));
			TArray<UObject*> Matches;
			for (TObjectIterator<UObject> It; It; ++It)
			{
				UObject* Candidate = *It;
				if (!IsValid(Candidate) || Candidate->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Candidate->GetWorld() != PIEWorld || !Candidate->IsA(TargetClass)) continue;
				if (!ObjectName.IsEmpty() && !Candidate->GetName().Equals(ObjectName, ESearchCase::CaseSensitive)) continue;
				AActor* OwnerActor = Cast<AActor>(Candidate);
				if (!OwnerActor)
				{
					if (const UActorComponent* Component = Cast<UActorComponent>(Candidate)) OwnerActor = Component->GetOwner();
					else OwnerActor = Candidate->GetTypedOuter<AActor>();
				}
				if (!ActorTag.IsNone() && (!OwnerActor || !OwnerActor->ActorHasTag(ActorTag))) continue;
				Matches.Add(Candidate);
			}
			if (Matches.Num() != 1)
			{
				OutError = FString::Printf(TEXT("Runtime target selector resolved %d objects; exactly one is required."), Matches.Num());
				return nullptr;
			}
			return Matches[0];
		}

		bool InvokeRuntimeFunction(UObject* Object, UFunction* Function, const TSharedPtr<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutResult, FString& OutError)
		{
			if (!Object || !Function)
			{
				OutError = TEXT("Runtime object or function is invalid.");
				return false;
			}
			uint8* Parameters = Function->ParmsSize > 0 ? static_cast<uint8*>(FMemory_Alloca(Function->ParmsSize)) : nullptr;
			if (Parameters) FMemory::Memzero(Parameters, Function->ParmsSize);
			TArray<FProperty*> ParameterProperties;
			for (TFieldIterator<FProperty> It(Function); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property || !Property->HasAnyPropertyFlags(CPF_Parm)) continue;
				ParameterProperties.Add(Property);
				Property->InitializeValue_InContainer(Parameters);
			}

			auto DestroyParameters = [&ParameterProperties, Parameters]()
			{
				for (int32 Index = ParameterProperties.Num() - 1; Index >= 0; --Index) ParameterProperties[Index]->DestroyValue_InContainer(Parameters);
			};
			for (FProperty* Property : ParameterProperties)
			{
				const bool bReturn = Property->HasAnyPropertyFlags(CPF_ReturnParm);
				const bool bOutputOnly = Property->HasAnyPropertyFlags(CPF_OutParm) && !Property->HasAnyPropertyFlags(CPF_ReferenceParm);
				if (bReturn || bOutputOnly) continue;
				const TSharedPtr<FJsonValue> Value = Arguments.IsValid() ? Arguments->TryGetField(Property->GetName()) : nullptr;
				if (!Value.IsValid())
				{
					OutError = FString::Printf(TEXT("Runtime argument is required: %s"), *Property->GetName());
					DestroyParameters();
					return false;
				}
				if (!FUEPIPropertyCodec::WriteValue(Property, Property->ContainerPtrToValuePtr<void>(Parameters), Value, OutError))
				{
					OutError = FString::Printf(TEXT("Runtime argument %s is invalid: %s"), *Property->GetName(), *OutError);
					DestroyParameters();
					return false;
				}
			}

			Object->ProcessEvent(Function, Parameters);
			TSharedRef<FJsonObject> Outputs = MakeShared<FJsonObject>();
			for (FProperty* Property : ParameterProperties)
			{
				if (!Property->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm)) continue;
				Outputs->SetField(Property->HasAnyPropertyFlags(CPF_ReturnParm) ? TEXT("return_value") : Property->GetName(), FUEPIPropertyCodec::ReadValue(Property, Property->ContainerPtrToValuePtr<void>(Parameters)));
			}
			OutResult->SetObjectField(TEXT("outputs"), Outputs);
			DestroyParameters();
			return true;
		}

		TSharedPtr<FJsonObject> JsonObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
		{
			if (!Object.IsValid())
			{
				return nullptr;
			}
			const TSharedPtr<FJsonObject>* Value = nullptr;
			return Object->TryGetObjectField(Field, Value) && Value ? *Value : nullptr;
		}

		FString OperationId(const TSharedPtr<FJsonObject>& Operation)
		{
			return JsonString(Operation, TEXT("operation_id"), JsonString(Operation, TEXT("id"), JsonString(Operation, TEXT("ref"))));
		}

		TArray<FString> OperationReferences(const TSharedPtr<FJsonObject>& Operation, const TSharedPtr<FJsonObject>& Params)
		{
			TArray<FString> Result;
			auto Add = [&Result](const FString& Value) { const FString Clean = Value.TrimStartAndEnd(); if (!Clean.IsEmpty()) Result.AddUnique(Clean); };
			Add(OperationId(Operation));
			for (const TCHAR* Field : { TEXT("ref"), TEXT("node_ref"), TEXT("result_ref") })
			{
				Add(JsonString(Operation, Field));
				Add(JsonString(Params, Field));
			}
			return Result;
		}

		FString BlueprintStatusString(const UBlueprint* Blueprint)
		{
			if (!Blueprint)
			{
				return TEXT("unknown");
			}
			if (const UEnum* Enum = StaticEnum<EBlueprintStatus>())
			{
				return Enum->GetNameStringByValue(static_cast<int64>(Blueprint->Status));
			}
			return FString::FromInt(static_cast<int32>(Blueprint->Status));
		}

		FString MessageSeverityString(EMessageSeverity::Type Severity)
		{
			switch (Severity)
			{
			case EMessageSeverity::Error:
				return TEXT("error");
			case EMessageSeverity::PerformanceWarning:
			case EMessageSeverity::Warning:
				return TEXT("warning");
			case EMessageSeverity::Info:
				return TEXT("info");
			default:
				return TEXT("message");
			}
		}

		TSharedRef<FJsonObject> CompileBlueprintToJson(UBlueprint* Blueprint)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Blueprint)
			{
				Result->SetBoolField(TEXT("ok"), false);
				Result->SetStringField(TEXT("error"), TEXT("Blueprint was null."));
				return Result;
			}

			FCompilerResultsLog Log;
			Log.SetSourcePath(Blueprint->GetPathName());
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Log);

			TArray<TSharedPtr<FJsonValue>> Messages;
			const int32 MaxMessages = FMath::Min(Log.Messages.Num(), 40);
			for (int32 Index = 0; Index < MaxMessages; ++Index)
			{
				const TSharedRef<FTokenizedMessage>& Message = Log.Messages[Index];
				TSharedRef<FJsonObject> MessageObject = MakeShared<FJsonObject>();
				MessageObject->SetStringField(TEXT("severity"), MessageSeverityString(Message->GetSeverity()));
				MessageObject->SetStringField(TEXT("message"), Message->ToText().ToString());
				Messages.Add(MakeShared<FJsonValueObject>(MessageObject));
			}

			Result->SetBoolField(TEXT("ok"), Log.NumErrors == 0);
			Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
			Result->SetStringField(TEXT("status"), BlueprintStatusString(Blueprint));
			Result->SetNumberField(TEXT("error_count"), Log.NumErrors);
			Result->SetNumberField(TEXT("warning_count"), Log.NumWarnings);
			Result->SetNumberField(TEXT("message_count"), Log.Messages.Num());
			Result->SetArrayField(TEXT("messages"), Messages);
			return Result;
		}

		FString NormalizeBlueprintObjectPath(const FString& RawPath)
		{
			FString Path = RawPath.TrimStartAndEnd();
			if (Path.IsEmpty() || Path.Contains(TEXT("."))) return Path;
			FString Package;
			FString AssetName;
			return Path.Split(TEXT("/"), &Package, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !AssetName.IsEmpty()
				? Path + TEXT(".") + AssetName
				: Path;
		}

		FString NormalizedContentPath(const FString& RawPath)
		{
			FString Path = RawPath.TrimStartAndEnd();
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			while (Path.EndsWith(TEXT("/")) && Path.Len() > 5) Path.LeftChopInline(1);
			return Path;
		}

		bool ValidateGameContentPath(const FString& Path, FString& OutError)
		{
			if (Path.IsEmpty() || (!Path.Equals(TEXT("/Game"), ESearchCase::IgnoreCase) && !Path.StartsWith(TEXT("/Game/"))))
			{
				OutError = FString::Printf(TEXT("Write content paths must be under /Game: %s"), *Path);
				return false;
			}
			if (Path.Contains(TEXT("..")) || Path.Contains(TEXT("\\")))
			{
				OutError = FString::Printf(TEXT("Content path contains unsupported traversal or separator characters: %s"), *Path);
				return false;
			}
			return true;
		}

		bool SplitDestinationPath(const TSharedPtr<FJsonObject>& Params, const FString& DefaultAssetName, FString& OutPackagePath, FString& OutAssetName, FString& OutError)
		{
			FString Destination = JsonString(Params, TEXT("destination"), JsonString(Params, TEXT("destination_asset")));
			if (!Destination.IsEmpty())
			{
				Destination = NormalizedContentPath(Destination);
				FString PackageName = Destination;
				int32 DotIndex = INDEX_NONE;
				if (Destination.FindChar(TEXT('.'), DotIndex))
				{
					PackageName = Destination.Left(DotIndex);
				}
				OutPackagePath = FPackageName::GetLongPackagePath(PackageName);
				OutAssetName = FPackageName::GetLongPackageAssetName(PackageName);
			}

			OutPackagePath = NormalizedContentPath(JsonString(Params, TEXT("destination_path"), JsonString(Params, TEXT("folder"), OutPackagePath)));
			OutAssetName = JsonString(Params, TEXT("name"), JsonString(Params, TEXT("asset_name"), OutAssetName.IsEmpty() ? DefaultAssetName : OutAssetName)).TrimStartAndEnd();
			if (OutPackagePath.IsEmpty() || OutAssetName.IsEmpty())
			{
				OutError = TEXT("Destination requires destination/destination_path plus name.");
				return false;
			}
			if (!ValidateGameContentPath(OutPackagePath, OutError))
			{
				return false;
			}
			if (OutAssetName.Contains(TEXT("/")) || OutAssetName.Contains(TEXT(".")) || OutAssetName.Contains(TEXT("\\")))
			{
				OutError = FString::Printf(TEXT("Asset name must not contain path separators or dots: %s"), *OutAssetName);
				return false;
			}
			return true;
		}

		UWorld* EditorWorld()
		{
			return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		}

		void AddOperationResult(TArray<TSharedPtr<FJsonValue>>& OperationResults, int32 Index, const FString& Type, bool bOk, const FString& Message, const TSharedPtr<FJsonObject>& Detail = nullptr)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("index"), Index);
			Object->SetStringField(TEXT("type"), Type);
			Object->SetBoolField(TEXT("ok"), bOk);
			Object->SetStringField(TEXT("message"), Message);
			if (Detail.IsValid())
			{
				Object->SetObjectField(TEXT("detail"), Detail);
			}
			OperationResults.Add(MakeShared<FJsonValueObject>(Object));
		}

		bool WriteRefreshRequest(const TArray<FString>& Targets, const FString& DataMode, FString& OutRequestPath, FString& OutError)
		{
			const FString RequestIdPart = FString::Printf(TEXT("uepi-refresh:%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			const FString CreatedAt = FDateTime::UtcNow().ToIso8601();
			const FString SafeRequestId = RequestIdPart.Replace(TEXT(":"), TEXT("-"));
			const FString RequestPath = FPaths::Combine(UEPIRequestsDirectory(), FString::Printf(TEXT("%s.queued.json"), *SafeRequestId));

			TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
			Request->SetStringField(TEXT("schema_version"), TEXT("uepi.refresh-request.v2"));
			Request->SetStringField(TEXT("request_id"), RequestIdPart);
			Request->SetStringField(TEXT("project_binding_id"), UEPIProjectBindingId());
			Request->SetStringField(TEXT("status"), TEXT("queued"));
			Request->SetStringField(TEXT("created_at"), CreatedAt);
			Request->SetStringField(TEXT("expires_at"), (FDateTime::UtcNow() + FTimespan::FromMinutes(5.0)).ToIso8601());
			Request->SetStringField(TEXT("data_mode"), DataMode);
			Request->SetArrayField(TEXT("targets"), StringArrayToJsonValues(Targets));
			Request->SetArrayField(TEXT("target_object_paths"), StringArrayToJsonValues(Targets));
			Request->SetArrayField(TEXT("domains"), EmptyJsonArray());
			Request->SetArrayField(TEXT("artifacts"), EmptyJsonArray());
			Request->SetStringField(TEXT("reason"), TEXT("bridge_edit_apply"));
			Request->SetStringField(TEXT("tool_name"), TEXT("uepi_bridge"));

			if (!SaveJsonObject(Request, RequestPath))
			{
				OutError = FString::Printf(TEXT("Failed to write refresh request: %s"), *RequestPath);
				return false;
			}
			OutRequestPath = RequestPath;
			OutError.Reset();
			return true;
		}


		FString AbsoluteLogFilename()
		{
			const FString Candidate = FGenericPlatformOutputDevices::GetAbsoluteLogFilename();
			return FPaths::ConvertRelativePathToFull(Candidate);
		}

		void AddBlueprintPinSchema(
			TArray<TSharedPtr<FJsonValue>>& Pins,
			const FName PinName,
			const TCHAR* Direction,
			const FString& Category,
			bool bRequiredForConnection)
		{
			TSharedRef<FJsonObject> PinSchema = MakeShared<FJsonObject>();
			PinSchema->SetStringField(TEXT("pin_name"), PinName.ToString());
			PinSchema->SetStringField(TEXT("direction"), Direction);
			PinSchema->SetStringField(TEXT("category"), Category);
			PinSchema->SetBoolField(TEXT("is_exec"), Category == UEdGraphSchema_K2::PC_Exec.ToString());
			PinSchema->SetBoolField(TEXT("required_for_connection"), bRequiredForConnection);
			Pins.Add(MakeShared<FJsonValueObject>(PinSchema));
		}

		TArray<TSharedPtr<FJsonValue>> BlueprintNodePinSchema(const FString& Kind)
		{
			TArray<TSharedPtr<FJsonValue>> Pins;
			if (Kind == TEXT("custom_event"))
			{
				AddBlueprintPinSchema(Pins, UEdGraphSchema_K2::PN_Then, TEXT("output"), UEdGraphSchema_K2::PC_Exec.ToString(), true);
			}
			else if (Kind == TEXT("print_string"))
			{
				AddBlueprintPinSchema(Pins, UEdGraphSchema_K2::PN_Execute, TEXT("input"), UEdGraphSchema_K2::PC_Exec.ToString(), true);
				AddBlueprintPinSchema(Pins, UEdGraphSchema_K2::PN_Then, TEXT("output"), UEdGraphSchema_K2::PC_Exec.ToString(), true);
				if (const UFunction* Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)))
				{
					for (TFieldIterator<FProperty> It(Function); It; ++It)
					{
						const FProperty* Property = *It;
						if (!Property || !Property->HasAnyPropertyFlags(CPF_Parm) || Property->HasAnyPropertyFlags(CPF_ReturnParm))
						{
							continue;
						}
						const bool bOutput = Property->HasAnyPropertyFlags(CPF_OutParm) && !Property->HasAnyPropertyFlags(CPF_ReferenceParm);
						AddBlueprintPinSchema(Pins, Property->GetFName(), bOutput ? TEXT("output") : TEXT("input"), Property->GetClass()->GetName(), false);
					}
				}
			}
			return Pins;
		}

		TArray<FString> TailLines(const FString& Path, int32 LineLimit)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				return {};
			}
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			const int32 Start = FMath::Max(0, Lines.Num() - FMath::Clamp(LineLimit, 1, 500));
			TArray<FString> Tail;
			for (int32 Index = Start; Index < Lines.Num(); ++Index)
			{
				Tail.Add(Lines[Index]);
			}
			return Tail;
		}

		bool SaveJsonObject(const TSharedRef<FJsonObject>& Object, const FString& Path)
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString TemporaryPath = Path + FString::Printf(TEXT(".tmp.%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			if (!FFileHelper::SaveStringToFile(JsonObjectToString(Object), *TemporaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				return false;
			}
			if (!IFileManager::Get().Move(*Path, *TemporaryPath, true, true))
			{
				IFileManager::Get().Delete(*TemporaryPath, false, true);
				return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> ActorObject(AActor* Actor)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			if (!Actor)
			{
				return Object;
			}
			Object->SetStringField(TEXT("name"), Actor->GetName());
			Object->SetStringField(TEXT("path"), Actor->GetPathName());
			Object->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
			Object->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Object->SetStringField(TEXT("folder"), Actor->GetFolderPath().ToString());
			Object->SetStringField(TEXT("level"), Actor->GetLevel() ? Actor->GetLevel()->GetPathName() : FString());
			TArray<FString> Tags;
			for (const FName& Tag : Actor->Tags)
			{
				Tags.Add(Tag.ToString());
			}
			Object->SetArrayField(TEXT("tags"), StringArrayToJsonValues(Tags));
			const FTransform Transform = Actor->GetActorTransform();
			TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
			Location->SetNumberField(TEXT("x"), Transform.GetLocation().X);
			Location->SetNumberField(TEXT("y"), Transform.GetLocation().Y);
			Location->SetNumberField(TEXT("z"), Transform.GetLocation().Z);
			Object->SetObjectField(TEXT("location"), Location);
			TSharedRef<FJsonObject> Rotation = MakeShared<FJsonObject>();
			Rotation->SetNumberField(TEXT("pitch"), Transform.Rotator().Pitch);
			Rotation->SetNumberField(TEXT("yaw"), Transform.Rotator().Yaw);
			Rotation->SetNumberField(TEXT("roll"), Transform.Rotator().Roll);
			Object->SetObjectField(TEXT("rotation"), Rotation);
			TSharedRef<FJsonObject> Scale = MakeShared<FJsonObject>();
			Scale->SetNumberField(TEXT("x"), Transform.GetScale3D().X);
			Scale->SetNumberField(TEXT("y"), Transform.GetScale3D().Y);
			Scale->SetNumberField(TEXT("z"), Transform.GetScale3D().Z);
			Object->SetObjectField(TEXT("scale"), Scale);
			Object->SetNumberField(TEXT("component_count"), Actor->GetComponents().Num());
			return Object;
		}
	}

	bool FUEPIEditorCommandBridge::Start(const FString& InSessionId, int32 InRequestedPort, FString& OutError)
	{
		SessionId = InSessionId;
		SessionPath = UEPIBridgeSessionPath();
		TokenPath = UEPIBridgeTokenPath();
		Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		bActive = true;

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(TokenPath), true);
		if (!FFileHelper::SaveStringToFile(Token, *TokenPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			bActive = false;
			OutError = FString::Printf(TEXT("Failed to write UEPI bridge token file: %s"), *TokenPath);
			return false;
		}

		const int32 FirstPort = InRequestedPort > 0 ? FMath::Clamp(InRequestedPort, 1, 65535) : 48735;
		const int32 MaxAttempts = InRequestedPort > 0 ? 1 : 64;
		for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
		{
			const int32 CandidatePort = FirstPort + Attempt;
			if (CandidatePort > 65535)
			{
				break;
			}
			const FIPv4Endpoint Endpoint(FIPv4Address(127, 0, 0, 1), static_cast<uint16>(CandidatePort));
			// A bridge belongs to exactly one Editor session. Port reuse can route a
			// localhost client to another open project on Windows.
			Listener = MakeUnique<FTcpListener>(Endpoint, FTimespan::FromMilliseconds(100), false);
			if (Listener.IsValid() && Listener->IsActive())
			{
				Port = CandidatePort;
				break;
			}
			Listener.Reset();
		}
		if (!Listener.IsValid() || !Listener->IsActive())
		{
			bActive = false;
			OutError = InRequestedPort > 0
				? FString::Printf(TEXT("Failed to start UEPI bridge on requested port %d."), InRequestedPort)
				: TEXT("Failed to start UEPI bridge on any default localhost port.");
			return false;
		}
		Listener->OnConnectionAccepted().BindRaw(this, &FUEPIEditorCommandBridge::HandleConnectionAccepted);

		return WriteSessionObject(TEXT("active"), OutError);
	}

	void FUEPIEditorCommandBridge::Stop()
	{
		if (bOwnsPIESession && GEditor)
		{
			if (GEditor->PlayWorld) GEditor->RequestEndPlayMap(); else GEditor->CancelRequestPlaySession();
			bOwnsPIESession = false;
			OwnedRuntimeSessionId.Reset();
		}
		FSocket* PendingSocket = nullptr;
		while (PendingSockets.Dequeue(PendingSocket))
		{
			if (PendingSocket)
			{
				PendingSocket->Close();
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(PendingSocket);
			}
		}
		if (bActive)
		{
			FString Error;
			WriteSessionObject(TEXT("stopped"), Error);
		}
		bActive = false;
		Listener.Reset();
		Token.Reset();
	}

	void FUEPIEditorCommandBridge::TickHeartbeat()
	{
		if (!bActive)
		{
			return;
		}
		ProcessPendingSockets();
		FString Error;
		WriteSessionObject(TEXT("active"), Error);
	}

	bool FUEPIEditorCommandBridge::IsActive() const
	{
		return bActive;
	}

	const FString& FUEPIEditorCommandBridge::GetSessionPath() const
	{
		return SessionPath;
	}

	const FString& FUEPIEditorCommandBridge::GetTokenPath() const
	{
		return TokenPath;
	}

	int32 FUEPIEditorCommandBridge::GetPort() const
	{
		return Port;
	}

	bool FUEPIEditorCommandBridge::HandleConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& RemoteEndpoint)
	{
		if (!ClientSocket || RemoteEndpoint.Address != FIPv4Address(127, 0, 0, 1))
		{
			return false;
		}
		PendingSockets.Enqueue(ClientSocket);
		return true;
	}

	void FUEPIEditorCommandBridge::ProcessPendingSockets()
	{
		constexpr int32 MaxSocketsPerTick = 8;
		for (int32 Index = 0; Index < MaxSocketsPerTick; ++Index)
		{
			FSocket* ClientSocket = nullptr;
			if (!PendingSockets.Dequeue(ClientSocket))
			{
				return;
			}
			ProcessSocket(ClientSocket);
			if (ClientSocket)
			{
				ClientSocket->Close();
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
			}
		}
	}

	bool FUEPIEditorCommandBridge::ProcessSocket(FSocket* ClientSocket)
	{
		if (!ClientSocket)
		{
			return false;
		}
		FString JsonText;
		if (!ReadFrame(ClientSocket, JsonText))
		{
			WriteFrame(ClientSocket, ErrorResponse(TEXT(""), TEXT("UEPI_BRIDGE_READ_FAILED"), TEXT("Failed to read UEPI bridge request frame.")));
			return false;
		}

		TSharedPtr<FJsonObject> Request;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
		{
			WriteFrame(ClientSocket, ErrorResponse(TEXT(""), TEXT("UEPI_BRIDGE_BAD_JSON"), TEXT("UEPI bridge request was not a JSON object.")));
			return false;
		}

		return WriteFrame(ClientSocket, HandleRequest(Request));
	}

	bool FUEPIEditorCommandBridge::ReadFrame(FSocket* ClientSocket, FString& OutJsonText) const
	{
		uint8 Header[4] = { 0, 0, 0, 0 };
		int32 BytesRead = 0;
		for (int32 Offset = 0; Offset < 4;)
		{
			if (!ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(250)))
			{
				return false;
			}
			int32 ChunkRead = 0;
			if (!ClientSocket->Recv(Header + Offset, 4 - Offset, ChunkRead) || ChunkRead <= 0)
			{
				return false;
			}
			Offset += ChunkRead;
			BytesRead += ChunkRead;
		}

		const uint32 PayloadSize =
			(static_cast<uint32>(Header[0]) << 24) |
			(static_cast<uint32>(Header[1]) << 16) |
			(static_cast<uint32>(Header[2]) << 8) |
			static_cast<uint32>(Header[3]);
		if (PayloadSize == 0 || PayloadSize > 1024 * 1024)
		{
			return false;
		}

		TArray<uint8> Payload;
		Payload.SetNumUninitialized(static_cast<int32>(PayloadSize));
		for (int32 Offset = 0; Offset < static_cast<int32>(PayloadSize);)
		{
			if (!ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(250)))
			{
				return false;
			}
			int32 ChunkRead = 0;
			if (!ClientSocket->Recv(Payload.GetData() + Offset, static_cast<int32>(PayloadSize) - Offset, ChunkRead) || ChunkRead <= 0)
			{
				return false;
			}
			Offset += ChunkRead;
		}

		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Payload.GetData()), Payload.Num());
		OutJsonText = FString(Converter.Length(), Converter.Get());
		return true;
	}

	bool FUEPIEditorCommandBridge::WriteFrame(FSocket* ClientSocket, const TSharedRef<FJsonObject>& Response) const
	{
		const FString ResponseText = JsonObjectToString(Response);
		FTCHARToUTF8 Converter(*ResponseText);
		const uint32 PayloadSize = static_cast<uint32>(Converter.Length());
		uint8 Header[4] = {
			static_cast<uint8>((PayloadSize >> 24) & 0xff),
			static_cast<uint8>((PayloadSize >> 16) & 0xff),
			static_cast<uint8>((PayloadSize >> 8) & 0xff),
			static_cast<uint8>(PayloadSize & 0xff),
		};
		int32 BytesSent = 0;
		if (!ClientSocket->Send(Header, 4, BytesSent) || BytesSent != 4)
		{
			return false;
		}
		int32 PayloadSent = 0;
		const uint8* PayloadData = reinterpret_cast<const uint8*>(Converter.Get());
		while (PayloadSent < Converter.Length())
		{
			int32 ChunkSent = 0;
			if (!ClientSocket->Send(PayloadData + PayloadSent, Converter.Length() - PayloadSent, ChunkSent) || ChunkSent <= 0)
			{
				return false;
			}
			PayloadSent += ChunkSent;
		}
		return true;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::HandleRequest(const TSharedPtr<FJsonObject>& Request)
	{
		FString RequestId;
		Request->TryGetStringField(TEXT("id"), RequestId);
		FString RequestToken;
		if (!Request->TryGetStringField(TEXT("token"), RequestToken) || RequestToken != Token)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_BRIDGE_UNAUTHORIZED"), TEXT("UEPI bridge token is missing or invalid."));
		}

		FString Command;
		if (!Request->TryGetStringField(TEXT("command"), Command))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_BRIDGE_COMMAND_MISSING"), TEXT("UEPI bridge request did not include a command."));
		}

		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		TSharedPtr<FJsonObject> Params;
		if (Request->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
		{
			Params = *ParamsPtr;
		}
		const FString ExpectedProjectBindingId = JsonString(Params, TEXT("expected_project_binding_id"));
		if (!ExpectedProjectBindingId.IsEmpty() && ExpectedProjectBindingId != UEPIProjectBindingId())
		{
			return ErrorResponse(RequestId, TEXT("UEPI_PROJECT_BINDING_MISMATCH"), TEXT("Bridge request project binding does not match this Editor project."));
		}
		const FString ExpectedEditorSessionId = JsonString(Params, TEXT("expected_editor_session_id"));
		if (!ExpectedEditorSessionId.IsEmpty() && ExpectedEditorSessionId != SessionId)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDITOR_SESSION_MISMATCH"), TEXT("Bridge request session does not match this Editor session."));
		}

		if (Command == TEXT("editor.get_status"))
		{
			return StatusResult(RequestId);
		}
		if (Command == TEXT("editor.get_selection"))
		{
			return SelectionResult(RequestId);
		}
		if (Command == TEXT("editor.read_output_log"))
		{
			return OutputLogResult(RequestId, Params);
		}
		if (Command == TEXT("editor.read_world"))
		{
			return WorldReadResult(RequestId, Params);
		}
		if (Command == TEXT("schema.get"))
		{
			return SchemaResult(RequestId, Params);
		}
		if (Command == TEXT("runtime.control"))
		{
			return RuntimeResult(RequestId, Params);
		}
		if (Command == TEXT("asset.refresh_now"))
		{
			TArray<FString> Targets;
			FString DataMode = TEXT("live");
			if (Params.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* TargetValues = nullptr;
				if (Params->TryGetArrayField(TEXT("target_object_paths"), TargetValues) && TargetValues)
				{
					for (const TSharedPtr<FJsonValue>& Value : *TargetValues)
					{
						FString Target;
						if (Value.IsValid() && Value->TryGetString(Target) && !Target.IsEmpty())
						{
							Targets.AddUnique(Target);
						}
					}
				}
				Params->TryGetStringField(TEXT("data_mode"), DataMode);
			}
			return RefreshNowResult(RequestId, Targets, DataMode.IsEmpty() ? TEXT("live") : DataMode);
		}
		if (Command == TEXT("editor.capture_viewport"))
		{
			return ViewportCaptureUnsupported(RequestId);
		}
		if (Command == TEXT("edit.discover"))
		{
			return EditDiscoverResult(RequestId);
		}
		if (Command == TEXT("edit.apply"))
		{
			return EditApplyResult(RequestId, Params);
		}
		if (Command == TEXT("edit.validate"))
		{
			return EditValidateResult(RequestId, Params);
		}
		if (Command == TEXT("edit.rollback"))
		{
			return EditRollbackResult(RequestId, Params);
		}
		return ErrorResponse(RequestId, TEXT("UEPI_BRIDGE_UNKNOWN_COMMAND"), FString::Printf(TEXT("Unknown UEPI bridge command: %s"), *Command));
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::ErrorResponse(const FString& RequestId, const FString& Code, const FString& Message) const
	{
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), false);
		Response->SetArrayField(TEXT("diagnostics"), DiagnosticsArray(Code, TEXT("error"), Message));
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::StatusResult(const FString& RequestId) const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("session_id"), SessionId);
		Result->SetStringField(TEXT("editor_session_id"), SessionId);
		Result->SetStringField(TEXT("project_binding_id"), UEPIProjectBindingId());
		Result->SetStringField(TEXT("project_name"), FApp::GetProjectName());
		Result->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		Result->SetStringField(TEXT("session_path"), SessionPath);
		Result->SetNumberField(TEXT("port"), Port);
		Result->SetBoolField(TEXT("transport_ready"), true);
		TArray<FString> Capabilities = FUEPIBridgeProtocol::ReadCapabilities();
		Capabilities.Append(FUEPIBridgeProtocol::WriteCapabilities());
		Result->SetArrayField(TEXT("capabilities"), StringArrayToJsonValues(Capabilities));

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), true);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::SelectionResult(const FString& RequestId) const
	{
		TArray<TSharedPtr<FJsonValue>> ActorValues;
		if (GEditor && GEditor->GetSelectedActors())
		{
			TArray<AActor*> Actors;
			GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Actors);
			for (AActor* Actor : Actors)
			{
				ActorValues.Add(MakeShared<FJsonValueObject>(ActorObject(Actor)));
			}
		}

		TArray<TSharedPtr<FJsonValue>> ObjectValues;
		if (GEditor && GEditor->GetSelectedObjects())
		{
			TArray<UObject*> Objects;
			GEditor->GetSelectedObjects()->GetSelectedObjects(Objects);
			for (UObject* Object : Objects)
			{
				if (!Object)
				{
					continue;
				}
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Object->GetName());
				Item->SetStringField(TEXT("path"), Object->GetPathName());
				Item->SetStringField(TEXT("class"), Object->GetClass() ? Object->GetClass()->GetPathName() : FString());
				ObjectValues.Add(MakeShared<FJsonValueObject>(Item));
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("actor_count"), ActorValues.Num());
		Result->SetArrayField(TEXT("actors"), ActorValues);
		Result->SetNumberField(TEXT("object_count"), ObjectValues.Num());
		Result->SetArrayField(TEXT("objects"), ObjectValues);

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), true);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::OutputLogResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params) const
	{
		const FString OutputLogPath = AbsoluteLogFilename();
		const int32 LineLimit = FMath::Clamp(JsonInt(Params, TEXT("line_limit"), 100), 1, 2000);
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *OutputLogPath, FILEREAD_AllowWrite))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDITOR_LOG_READ_FAILED"), FString::Printf(TEXT("The active output log file could not be read: %s"), *OutputLogPath));
		}
		int64 ByteOffset = FMath::Max<int64>(0, Bytes.Num() - 1024 * 1024);
		if (const TSharedPtr<FJsonObject> Cursor = JsonObjectField(Params, TEXT("cursor")))
		{
			double RequestedOffset = 0.0;
			if (Cursor->TryGetNumberField(TEXT("byte_offset"), RequestedOffset))
			{
				ByteOffset = FMath::Clamp<int64>(static_cast<int64>(RequestedOffset), 0, Bytes.Num());
			}
		}
		const int32 RemainingBytes = Bytes.Num() - static_cast<int32>(ByteOffset);
		FString LogText;
		if (RemainingBytes > 0)
		{
			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + ByteOffset), RemainingBytes);
			LogText = FString(Converter.Length(), Converter.Get());
		}
		TArray<FString> ParsedLines;
		LogText.ParseIntoArrayLines(ParsedLines, false);
		TArray<TSharedPtr<FJsonValue>> Lines;
		const int32 FirstLine = FMath::Max(0, ParsedLines.Num() - LineLimit);
		for (int32 Index = FirstLine; Index < ParsedLines.Num(); ++Index)
		{
			Lines.Add(MakeShared<FJsonValueString>(ParsedLines[Index]));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("log_path"), OutputLogPath);
		Result->SetNumberField(TEXT("line_count"), Lines.Num());
		Result->SetArrayField(TEXT("lines"), Lines);
		TSharedRef<FJsonObject> Cursor = MakeShared<FJsonObject>();
		Cursor->SetStringField(TEXT("file_identity"), FMD5::HashAnsiString(*FString::Printf(TEXT("%s:%d"), *OutputLogPath, Bytes.Num())));
		Cursor->SetNumberField(TEXT("byte_offset"), Bytes.Num());
		Cursor->SetNumberField(TEXT("file_size"), Bytes.Num());
		Result->SetObjectField(TEXT("cursor"), Cursor);

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), true);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::WorldReadResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params) const
	{
		const FString WorldKind = JsonString(Params, TEXT("world"), TEXT("editor"));
		UWorld* World = nullptr;
		if (GEditor)
		{
			if (WorldKind.Equals(TEXT("pie"), ESearchCase::IgnoreCase))
			{
				for (const FWorldContext& Context : GEditor->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE)
					{
						World = Context.World();
						break;
					}
				}
			}
			else
			{
				World = GEditor->GetEditorWorldContext().World();
			}
		}
		if (!World)
		{
			return ErrorResponse(RequestId, WorldKind.Equals(TEXT("pie"), ESearchCase::IgnoreCase) ? TEXT("UEPI_RUNTIME_SESSION_REQUIRED") : TEXT("UEPI_EDITOR_WORLD_UNAVAILABLE"), TEXT("The requested Unreal world is not available."));
		}

		const TSharedPtr<FJsonObject> Filters = JsonObjectField(Params, TEXT("filters"));
		const TArray<FString> ClassPaths = Filters.IsValid() ? JsonStringArray(Filters, TEXT("class_paths")) : TArray<FString>();
		const TArray<FString> Labels = Filters.IsValid() ? JsonStringArray(Filters, TEXT("labels")) : TArray<FString>();
		const TArray<FString> ObjectPaths = Filters.IsValid() ? JsonStringArray(Filters, TEXT("object_paths")) : TArray<FString>();
		TArray<TSharedPtr<FJsonValue>> ActorValues;
		for (TActorIterator<AActor> It(World); It && ActorValues.Num() < 5000; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			const FString ClassPath = Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString();
			if (ClassPaths.Num() > 0 && !ClassPaths.Contains(ClassPath))
			{
				continue;
			}
			if (Labels.Num() > 0 && !Labels.Contains(Actor->GetActorLabel()))
			{
				continue;
			}
			if (ObjectPaths.Num() > 0 && !ObjectPaths.Contains(Actor->GetPathName()))
			{
				continue;
			}
			TSharedRef<FJsonObject> ActorJson = ActorObject(Actor);
			TArray<TSharedPtr<FJsonValue>> Components;
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (!Component)
				{
					continue;
				}
				TSharedRef<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
				ComponentJson->SetStringField(TEXT("name"), Component->GetName());
				ComponentJson->SetStringField(TEXT("path"), Component->GetPathName());
				ComponentJson->SetStringField(TEXT("class"), Component->GetClass() ? Component->GetClass()->GetPathName() : FString());
				Components.Add(MakeShared<FJsonValueObject>(ComponentJson));
			}
			ActorJson->SetArrayField(TEXT("components"), Components);
			ActorValues.Add(MakeShared<FJsonValueObject>(ActorJson));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("world"), WorldKind.ToLower());
		Result->SetStringField(TEXT("map"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
		Result->SetNumberField(TEXT("actor_count"), ActorValues.Num());
		Result->SetArrayField(TEXT("actors"), ActorValues);
		return SuccessResponse(RequestId, Result);
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::SchemaResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params) const
	{
		const FString Action = JsonString(Params, TEXT("action"), TEXT("class_property"));
		const int32 MaxDepth = FMath::Clamp(JsonInt(Params, TEXT("max_depth"), 8), 1, 16);
		if (Action == TEXT("class_property"))
		{
			const FString ClassPath = JsonString(Params, TEXT("class_path"));
			UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
			if (!Class)
			{
				return ErrorResponse(RequestId, TEXT("UEPI_SCHEMA_CLASS_NOT_FOUND"), FString::Printf(TEXT("Class was not found: %s"), *ClassPath));
			}
			return SuccessResponse(RequestId, FUEPIPropertyCodec::BuildSchema(Class, MaxDepth));
		}
		if (Action == TEXT("asset_property"))
		{
			const FString AssetPath = JsonString(Params, TEXT("asset"));
			UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
			if (!Object)
			{
				return ErrorResponse(RequestId, TEXT("UEPI_SCHEMA_ASSET_NOT_FOUND"), FString::Printf(TEXT("Asset was not found: %s"), *AssetPath));
			}
			TSharedRef<FJsonObject> Result = FUEPIPropertyCodec::BuildObjectSchema(Object, MaxDepth);
			Result->SetStringField(TEXT("asset"), Object->GetPathName());
			return SuccessResponse(RequestId, Result);
		}
		if (Action == TEXT("blueprint_node"))
		{
			const FString Kind = JsonString(Params, TEXT("kind"));
			const TSet<FString> SupportedKinds = { TEXT("custom_event"), TEXT("input_key"), TEXT("function_call"), TEXT("make_struct"), TEXT("variable_get"), TEXT("variable_set"), TEXT("branch"), TEXT("print_string"), TEXT("animgraph_slot") };
			if (!SupportedKinds.Contains(Kind))
			{
				return ErrorResponse(RequestId, TEXT("UEPI_SCHEMA_BLUEPRINT_NODE_KIND_UNSUPPORTED"), FString::Printf(TEXT("Blueprint node kind is not registered: %s"), *Kind));
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema_version"), TEXT("uepi.blueprint-node-schema.v1"));
			Result->SetStringField(TEXT("kind"), Kind);
			Result->SetStringField(TEXT("graph_schema"), JsonString(Params, TEXT("graph_schema"), Kind == TEXT("animgraph_slot") ? TEXT("AnimGraph") : TEXT("K2")));
			Result->SetBoolField(TEXT("returns_real_pins_after_create"), true);
			const TArray<TSharedPtr<FJsonValue>> Pins = BlueprintNodePinSchema(Kind);
			Result->SetArrayField(TEXT("pins"), Pins);
			Result->SetStringField(TEXT("pins_scope"), TEXT("stable_same_plan_connection_pins"));
			Result->SetBoolField(TEXT("same_plan_pin_names_available"), Pins.Num() > 0);
			Result->SetStringField(TEXT("connection_rule"), Pins.Num() > 0
				? TEXT("For same-plan nodes, use a pin_name returned in pins; after creation, prefer the returned real pin_id. Never guess pins.")
				: TEXT("Create the node first and use its returned real pin_id/name; this node kind does not expose a same-plan pin schema."));
			if (Kind == TEXT("make_struct"))
			{
				UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *JsonString(Params, TEXT("struct_path")));
				if (!Struct || !UK2Node_MakeStruct::CanBeMade(Struct))
				{
					return ErrorResponse(RequestId, TEXT("UEPI_SCHEMA_STRUCT_NOT_MAKEABLE"), TEXT("The requested struct is unavailable or cannot be exposed as a Make Struct node."));
				}
				Result->SetObjectField(TEXT("struct_schema"), FUEPIPropertyCodec::BuildSchema(Struct, MaxDepth));
			}
			return SuccessResponse(RequestId, Result);
		}
		if (Action == TEXT("runtime_function"))
		{
			const FString ClassPath = JsonString(Params, TEXT("class_path"));
			const FString FunctionName = JsonString(Params, TEXT("query"), JsonString(Params, TEXT("function")));
			UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
			UFunction* Function = Class ? Class->FindFunctionByName(FName(*FunctionName)) : nullptr;
			if (!Class)
			{
				return ErrorResponse(RequestId, TEXT("UEPI_SCHEMA_CLASS_NOT_FOUND"), FString::Printf(TEXT("Class was not found: %s"), *ClassPath));
			}
			if (!Function)
			{
				return ErrorResponse(RequestId, TEXT("UEPI_SCHEMA_RUNTIME_FUNCTION_NOT_FOUND"), FString::Printf(TEXT("Runtime function was not found: %s"), *FunctionName));
			}
			FString SemanticError;
			const bool bSemanticsAllowed = RuntimeFunctionSemanticsAllowed(Function, SemanticError);
			const FString FunctionKey = RuntimeFunctionKey(Function);
			const UUEPISettings* Settings = GetDefault<UUEPISettings>();
			const bool bAllowlisted = Settings && Settings->bAllowRuntimeInvoke && Settings->AllowedRuntimeFunctions.Contains(FunctionKey);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema_version"), TEXT("uepi.runtime-function-schema.v1"));
			Result->SetStringField(TEXT("class_path"), Class->GetPathName());
			Result->SetStringField(TEXT("function"), Function->GetName());
			Result->SetStringField(TEXT("function_key"), FunctionKey);
			Result->SetBoolField(TEXT("blueprint_callable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
			Result->SetBoolField(TEXT("blueprint_pure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
			Result->SetBoolField(TEXT("allowlisted"), bAllowlisted);
			Result->SetBoolField(TEXT("invocation_supported"), bSemanticsAllowed && bAllowlisted);
			Result->SetStringField(TEXT("blocked_reason"), bSemanticsAllowed ? (bAllowlisted ? FString() : TEXT("runtime_function_not_allowlisted")) : SemanticError);
			Result->SetObjectField(TEXT("parameter_schema"), FUEPIPropertyCodec::BuildSchema(Function, MaxDepth));
			return SuccessResponse(RequestId, Result);
		}
		return ErrorResponse(RequestId, TEXT("UEPI_SCHEMA_ACTION_UNSUPPORTED"), FString::Printf(TEXT("Schema action is not implemented by the Editor Bridge: %s"), *Action));
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::RuntimeResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params)
	{
		const UUEPISettings* Settings = GetDefault<UUEPISettings>();
		const FString Action = JsonString(Params, TEXT("action"), TEXT("status"));
		UWorld* PIEWorld = GEditor ? GEditor->PlayWorld : nullptr;
		if (Action == TEXT("status"))
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("runtime_session_id"), OwnedRuntimeSessionId);
			Result->SetStringField(TEXT("state"), PIEWorld ? TEXT("running") : (bOwnsPIESession ? TEXT("starting") : TEXT("stopped")));
			Result->SetBoolField(TEXT("owned_by_uepi"), bOwnsPIESession);
			Result->SetStringField(TEXT("map"), PIEWorld && PIEWorld->GetOutermost() ? PIEWorld->GetOutermost()->GetName() : FString());
			return SuccessResponse(RequestId, Result);
		}
		if (!Settings || !Settings->bAllowPIEControl)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_CAPABILITY_DISABLED"), TEXT("Controlled PIE is disabled in UEPI Project Settings."));
		}
		if (Action == TEXT("start"))
		{
			if (PIEWorld || bOwnsPIESession)
			{
				return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_ALREADY_RUNNING"), TEXT("A PIE session is already active or starting."));
			}
			FRequestPlaySessionParams SessionParams;
			SessionParams.SessionDestination = EPlaySessionDestinationType::InProcess;
			SessionParams.WorldType = EPlaySessionWorldType::PlayInEditor;
			SessionParams.bAllowOnlineSubsystem = false;
			SessionParams.GlobalMapOverride = JsonString(Params, TEXT("map"));
			OwnedRuntimeSessionId = FString::Printf(TEXT("uepi-runtime:%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			bOwnsPIESession = true;
			GEditor->RequestPlaySession(SessionParams);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("runtime_session_id"), OwnedRuntimeSessionId); Result->SetStringField(TEXT("state"), TEXT("starting")); return SuccessResponse(RequestId, Result);
		}
		if (Action == TEXT("stop"))
		{
			if (!bOwnsPIESession)
			{
				return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_NOT_OWNER"), TEXT("UEPI will not stop a PIE session it did not start."));
			}
			if (PIEWorld) GEditor->RequestEndPlayMap(); else GEditor->CancelRequestPlaySession();
			const FString StoppedSession = OwnedRuntimeSessionId; OwnedRuntimeSessionId.Reset(); bOwnsPIESession = false;
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("runtime_session_id"), StoppedSession); Result->SetStringField(TEXT("state"), TEXT("stopping")); return SuccessResponse(RequestId, Result);
		}
		if (!PIEWorld || !bOwnsPIESession)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_SESSION_REQUIRED"), TEXT("A UEPI-owned PIE session is required."));
		}
		if (Action == TEXT("input"))
		{
			APlayerController* Controller = PIEWorld->GetFirstPlayerController();
			const FKey Key(FName(*JsonString(Params, TEXT("key"))));
			const FString Event = JsonString(Params, TEXT("event"), TEXT("pressed"));
			if (!Controller || !Key.IsValid()) return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_INPUT_INVALID"), TEXT("PIE PlayerController or input key is invalid."));
			const bool bHandled = Controller->InputKey(FInputKeyParams(Key, Event == TEXT("released") ? IE_Released : IE_Pressed, 1.0, false));
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetBoolField(TEXT("handled"), bHandled); Result->SetStringField(TEXT("key"), Key.ToString()); return SuccessResponse(RequestId, Result);
		}
		FString ObjectError;
		UObject* Object = ResolveRuntimeObject(PIEWorld, Params, ObjectError);
		if (!Object)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_OBJECT_NOT_FOUND"), ObjectError);
		}
		if (Action == TEXT("read"))
		{
			const FString PropertyName = JsonString(Params, TEXT("property"));
			FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *PropertyName);
			if (!Property) return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_PROPERTY_NOT_FOUND"), TEXT("Runtime property was not found."));
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("object_path"), Object->GetPathName()); Result->SetStringField(TEXT("property"), PropertyName); Result->SetField(TEXT("value"), FUEPIPropertyCodec::ReadValue(Property, Property->ContainerPtrToValuePtr<void>(Object))); return SuccessResponse(RequestId, Result);
		}
		if (Action == TEXT("invoke"))
		{
			if (!Settings->bAllowRuntimeInvoke) return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_INVOKE_DISABLED"), TEXT("Runtime invoke is disabled."));
			const FString FunctionName = JsonString(Params, TEXT("function"));
			UFunction* Function = Object->FindFunction(FName(*FunctionName));
			FString SemanticError;
			if (!RuntimeFunctionSemanticsAllowed(Function, SemanticError))
			{
				return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_FUNCTION_NOT_ALLOWED"), SemanticError);
			}
			const FString FunctionKey = RuntimeFunctionKey(Function);
			if (!Settings->AllowedRuntimeFunctions.Contains(FunctionKey)) return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_FUNCTION_NOT_ALLOWLISTED"), FString::Printf(TEXT("Runtime function is not in Project Settings allowlist: %s"), *FunctionKey));
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("object_path"), Object->GetPathName());
			Result->SetStringField(TEXT("function"), Function->GetName());
			if (!InvokeRuntimeFunction(Object, Function, JsonObjectField(Params, TEXT("arguments")), Result, SemanticError))
			{
				return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_ARGUMENT_INVALID"), SemanticError);
			}
			Result->SetBoolField(TEXT("invoked"), true);
			return SuccessResponse(RequestId, Result);
		}
		return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_ACTION_UNSUPPORTED"), FString::Printf(TEXT("Runtime action is unsupported: %s"), *Action));
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::RefreshNowResult(const FString& RequestId, const TArray<FString>& Targets, const FString& DataMode) const
	{
		FString RequestPath;
		FString Error;
		if (!WriteRefreshRequest(Targets, DataMode, RequestPath, Error))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_BRIDGE_REFRESH_REQUEST_FAILED"), Error);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("request_path"), RequestPath);
		Result->SetStringField(TEXT("data_mode"), DataMode);
		Result->SetArrayField(TEXT("target_object_paths"), StringArrayToJsonValues(Targets));
		return SuccessResponse(RequestId, Result);
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::ViewportCaptureUnsupported(const FString& RequestId) const
	{
		if (!GEditor || !GEditor->GetActiveViewport())
		{
			return ErrorResponse(RequestId, TEXT("UEPI_VIEWPORT_CAPTURE_NO_ACTIVE_VIEWPORT"), TEXT("No active editor viewport is available for capture."));
		}

		FViewport* Viewport = GEditor->GetActiveViewport();
		const FIntPoint Size = Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_VIEWPORT_CAPTURE_BAD_SIZE"), TEXT("Active viewport has no readable size."));
		}

		TArray<FColor> Pixels;
		if (!Viewport->ReadPixels(Pixels) || Pixels.Num() != Size.X * Size.Y)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_VIEWPORT_CAPTURE_READ_FAILED"), TEXT("Failed to read pixels from the active editor viewport."));
		}
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}

		TArray64<uint8> PngData;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, MakeArrayView(Pixels), PngData);
		if (PngData.Num() <= 0)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_VIEWPORT_CAPTURE_ENCODE_FAILED"), TEXT("Failed to encode active viewport pixels as PNG."));
		}

		const FString ArtifactDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("artifacts"), TEXT("screenshots"));
		IFileManager::Get().MakeDirectory(*ArtifactDirectory, true);
		const FString CreatedAt = FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ"));
		const FString ArtifactPath = FPaths::Combine(ArtifactDirectory, FString::Printf(TEXT("viewport-%s.png"), *CreatedAt));
		if (!FFileHelper::SaveArrayToFile(PngData, *ArtifactPath))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_VIEWPORT_CAPTURE_SAVE_FAILED"), FString::Printf(TEXT("Failed to save viewport artifact: %s"), *ArtifactPath));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.viewport-capture.v1"));
		Result->SetStringField(TEXT("artifact_uri"), FString::Printf(TEXT("uepi://artifact/screenshots/%s"), *FPaths::GetCleanFilename(ArtifactPath)));
		Result->SetStringField(TEXT("artifact_path"), ArtifactPath);
		Result->SetStringField(TEXT("artifact_directory"), ArtifactDirectory);
		Result->SetStringField(TEXT("format"), TEXT("png"));
		Result->SetNumberField(TEXT("width"), Size.X);
		Result->SetNumberField(TEXT("height"), Size.Y);
		Result->SetNumberField(TEXT("byte_count"), static_cast<double>(PngData.Num()));
		return SuccessResponse(RequestId, Result);
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::EditDiscoverResult(const FString& RequestId) const
	{
		const UUEPISettings* Settings = GetDefault<UUEPISettings>();
		const bool bWriteEnabled = Settings && Settings->bEnableWriteTools;
		const bool bBlueprintApplyEnabled = bWriteEnabled && Settings->bAllowBlueprintEdits;
		const bool bActorApplyEnabled = bWriteEnabled && Settings->bAllowActorEdits;
		const bool bContentApplyEnabled = bWriteEnabled && Settings->bAllowContentEdits;
		const bool bMaterialApplyEnabled = bWriteEnabled && Settings->bAllowMaterialEdits;
		const bool bMaterialBlueprintApplyEnabled = bMaterialApplyEnabled && Settings->bAllowBlueprintEdits;
		const bool bUMGApplyEnabled = bWriteEnabled && Settings->bAllowUMGEdits;
#if UEPI_WITH_ENHANCED_INPUT
		const bool bInputApplyEnabled = bWriteEnabled && Settings->bAllowInputEdits;
#else
		const bool bInputApplyEnabled = false;
#endif

		FUEPIEditOperationRegistry& Registry = FUEPIEditOperationRegistry::Get();
		Registry.EnsureBuiltinsRegistered();
		TArray<TSharedPtr<FJsonValue>> Operations;
		for (const FUEPIEditOperationDescriptor& Descriptor : Registry.GetDescriptors())
		{
			const bool bDomainEnabled =
				(Descriptor.Domain == TEXT("blueprint") && bBlueprintApplyEnabled) ||
				(Descriptor.Domain == TEXT("actor") && bActorApplyEnabled) ||
				(Descriptor.Domain == TEXT("content") && bContentApplyEnabled) ||
				(Descriptor.Domain == TEXT("asset") && bContentApplyEnabled) ||
				(Descriptor.Domain == TEXT("material") && (Descriptor.Name == TEXT("material.apply_to_blueprint_component") ? bMaterialBlueprintApplyEnabled : bMaterialApplyEnabled)) ||
				(Descriptor.Domain == TEXT("umg") && bUMGApplyEnabled) ||
				(Descriptor.Domain == TEXT("input") && bInputApplyEnabled);
			const bool bApplySupported = bDomainEnabled || ((Descriptor.Domain == TEXT("animgraph") || Descriptor.Domain == TEXT("animation")) && bBlueprintApplyEnabled);
			TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
			Operation->SetStringField(TEXT("name"), Descriptor.Name);
			Operation->SetNumberField(TEXT("version"), Descriptor.Version);
			Operation->SetStringField(TEXT("domain"), Descriptor.Domain);
			Operation->SetStringField(TEXT("summary"), Descriptor.Summary);
			Operation->SetStringField(TEXT("risk"), Descriptor.Risk);
			Operation->SetStringField(TEXT("rollback_mode"), Descriptor.RollbackMode);
			Operation->SetStringField(TEXT("validation_mode"), Descriptor.ValidationMode);
			Operation->SetStringField(TEXT("validation_behavior"), Descriptor.ValidationMode);
			Operation->SetStringField(TEXT("rollback_behavior"), Descriptor.RollbackMode);
			Operation->SetStringField(TEXT("save_behavior"), Descriptor.SaveBehavior);
			Operation->SetStringField(TEXT("idempotency_behavior"), Descriptor.IdempotencyBehavior);
			Operation->SetStringField(TEXT("required_plugin"), Descriptor.RequiredPlugin);
			Operation->SetBoolField(TEXT("requires_save"), Descriptor.bRequiresSave);
			Operation->SetBoolField(TEXT("atomic_supported"), Descriptor.bAtomicSupported);
			Operation->SetBoolField(TEXT("preview_supported"), true);
			Operation->SetBoolField(TEXT("apply_supported"), bApplySupported);
			Operation->SetArrayField(TEXT("target_fields"), StringArrayToJsonValues(Descriptor.TargetFields));
			Operation->SetArrayField(TEXT("dependency_fields"), StringArrayToJsonValues(Descriptor.DependencyFields));
			Operation->SetArrayField(TEXT("required_capabilities"), StringArrayToJsonValues(Descriptor.RequiredCapabilities));
			Operation->SetArrayField(TEXT("supported_engine_versions"), StringArrayToJsonValues(Descriptor.SupportedEngineVersions));
			Operation->SetArrayField(TEXT("supported_asset_classes"), StringArrayToJsonValues(Descriptor.SupportedAssetClasses));
			Operation->SetArrayField(TEXT("supported_graph_schemas"), StringArrayToJsonValues(Descriptor.SupportedGraphSchemas));
			TSharedRef<FJsonObject> InputSchema = MakeShared<FJsonObject>();
			InputSchema->SetStringField(TEXT("type"), TEXT("object"));
			TSharedRef<FJsonObject> InputProperties = MakeShared<FJsonObject>();
			for (const FString& TargetField : Descriptor.TargetFields)
			{
				TSharedRef<FJsonObject> TargetSchema = MakeShared<FJsonObject>();
				TargetSchema->SetArrayField(TEXT("type"), { MakeShared<FJsonValueString>(TEXT("string")), MakeShared<FJsonValueString>(TEXT("object")), MakeShared<FJsonValueString>(TEXT("array")) });
				InputProperties->SetObjectField(TargetField, TargetSchema);
			}
			for (const FString& DependencyField : Descriptor.DependencyFields)
			{
				TSharedRef<FJsonObject> DependencySchema = MakeShared<FJsonObject>();
				DependencySchema->SetArrayField(TEXT("type"), { MakeShared<FJsonValueString>(TEXT("string")), MakeShared<FJsonValueString>(TEXT("object")), MakeShared<FJsonValueString>(TEXT("array")) });
				InputProperties->SetObjectField(DependencyField, DependencySchema);
			}
			InputSchema->SetObjectField(TEXT("properties"), InputProperties);
			InputSchema->SetBoolField(TEXT("additionalProperties"), true);
			Operation->SetObjectField(TEXT("input_schema"), InputSchema);
			TSharedRef<FJsonObject> OutputSchema = MakeShared<FJsonObject>();
			OutputSchema->SetStringField(TEXT("type"), TEXT("object"));
			Operation->SetObjectField(TEXT("output_schema"), OutputSchema);
			Operation->SetArrayField(TEXT("examples"), EmptyJsonArray());
			TSharedRef<FJsonObject> Availability = MakeShared<FJsonObject>();
			Availability->SetBoolField(TEXT("preview"), true);
			Availability->SetBoolField(TEXT("apply"), bApplySupported);
			Availability->SetStringField(TEXT("reason"), bApplySupported ? TEXT("available") : TEXT("disabled_by_project_capability"));
			Operation->SetObjectField(TEXT("availability"), Availability);
			Operations.Add(MakeShared<FJsonValueObject>(Operation));
		}

		TSharedRef<FJsonObject> SettingsObject = MakeShared<FJsonObject>();
		SettingsObject->SetBoolField(TEXT("write_tools_enabled"), bWriteEnabled);
		SettingsObject->SetBoolField(TEXT("blueprint_edits_enabled"), Settings && Settings->bAllowBlueprintEdits);
		SettingsObject->SetBoolField(TEXT("actor_edits_enabled"), Settings && Settings->bAllowActorEdits);
		SettingsObject->SetBoolField(TEXT("content_edits_enabled"), Settings && Settings->bAllowContentEdits);
		SettingsObject->SetBoolField(TEXT("material_edits_enabled"), Settings && Settings->bAllowMaterialEdits);
		SettingsObject->SetBoolField(TEXT("umg_edits_enabled"), Settings && Settings->bAllowUMGEdits);
		SettingsObject->SetBoolField(TEXT("input_edits_enabled"), Settings && Settings->bAllowInputEdits);
		SettingsObject->SetBoolField(TEXT("enhanced_input_compiled"), UEPI_WITH_ENHANCED_INPUT != 0);
		SettingsObject->SetBoolField(TEXT("saving_enabled"), Settings && Settings->bAllowSavingPackages);
		SettingsObject->SetNumberField(TEXT("max_operations_per_transaction"), Settings ? Settings->MaxWriteOperationsPerTransaction : 0);
		SettingsObject->SetNumberField(TEXT("max_assets_per_transaction"), Settings ? Settings->MaxWriteAssetsPerTransaction : 0);
		SettingsObject->SetNumberField(TEXT("high_risk_operation_threshold"), 64);
		SettingsObject->SetNumberField(TEXT("high_risk_asset_threshold"), 12);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.bridge-edit-discover.v2"));
		Result->SetStringField(TEXT("catalog_version"), TEXT("2.0.0"));
		Result->SetStringField(TEXT("catalog_hash"), Registry.GetCatalogHash());
		Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Result->SetStringField(TEXT("plugin_build_id"), TEXT("uepi-vnext"));
		Result->SetObjectField(TEXT("settings"), SettingsObject);
		Result->SetArrayField(TEXT("operations"), Operations);
		Result->SetArrayField(TEXT("capabilities"), StringArrayToJsonValues(FUEPIBridgeProtocol::WriteCapabilities()));
		return SuccessResponse(RequestId, Result);
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::EditApplyResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params)
	{
		const UUEPISettings* Settings = GetDefault<UUEPISettings>();
		if (!Settings || !Settings->bEnableWriteTools)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_WRITE_DISABLED"), TEXT("UEPI write tools are disabled in Project Settings."));
		}
		if (!GEditor)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_EDITOR_UNAVAILABLE"), TEXT("GEditor is not available."));
		}
		if (GEditor->PlayWorld)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_BLOCKED_DURING_PIE"), TEXT("UEPI write tools do not run while PIE is active."));
		}
		if (!JsonBool(Params, TEXT("approved"), false))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_APPROVAL_REQUIRED"), TEXT("edit.apply requires approved=true after user review."));
		}
		if (JsonBool(Params, TEXT("save"), false) && !Settings->bAllowSavingPackages)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_SAVE_DISABLED"), TEXT("Saving packages is disabled for write alpha."));
		}

		const FString TransactionId = JsonString(Params, TEXT("transaction_id"));
		const TSharedPtr<FJsonObject> Plan = JsonObjectField(Params, TEXT("plan"));
		if (!Plan.IsValid() || JsonString(Plan, TEXT("schema_version")) != TEXT("uepi.edit_plan.v2"))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_PLAN_VERSION_UNSUPPORTED"), TEXT("Apply requires an immutable uepi.edit_plan.v2 generated by the current Preview."));
		}
		if (JsonString(Params, TEXT("plan_hash")) != JsonString(Plan, TEXT("plan_hash")) || JsonString(Params, TEXT("approval_nonce")) != JsonString(Plan, TEXT("approval_nonce")))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_APPROVAL_MISMATCH"), TEXT("Apply approval identity does not match the immutable Preview plan."));
		}
		if (JsonString(Plan, TEXT("project_binding_id")) != UEPIProjectBindingId())
		{
			return ErrorResponse(RequestId, TEXT("UEPI_PROJECT_BINDING_MISMATCH"), TEXT("Edit plan belongs to a different project binding."));
		}
		if (JsonString(Plan, TEXT("editor_session_id")) != SessionId)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDITOR_SESSION_MISMATCH"), TEXT("Edit plan belongs to a different Editor session."));
		}
		FDateTime ExpiresAt;
		if (!FDateTime::ParseIso8601(*JsonString(Plan, TEXT("expires_at")), ExpiresAt) || ExpiresAt < FDateTime::UtcNow())
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_PLAN_EXPIRED"), TEXT("Edit plan approval window has expired; run Preview again."));
		}
		FUEPIEditOperationRegistry& Registry = FUEPIEditOperationRegistry::Get();
		Registry.EnsureBuiltinsRegistered();
		if (JsonString(Plan, TEXT("catalog_hash")) != Registry.GetCatalogHash())
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_CATALOG_STALE"), TEXT("Operation catalog changed after Preview; run Preview again."));
		}
		const TArray<TSharedPtr<FJsonValue>>* Operations = Plan.IsValid() ? JsonArray(Plan, TEXT("operations")) : JsonArray(Params, TEXT("operations"));
		if (!Operations)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_PLAN_MISSING"), TEXT("edit.apply requires a plan with operations."));
		}
		if (Operations->Num() <= 0)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_PLAN_EMPTY"), TEXT("edit.apply received an empty operation plan."));
		}
		if (Operations->Num() > Settings->MaxWriteOperationsPerTransaction)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_TOO_MANY_OPERATIONS"), FString::Printf(TEXT("Operation count %d exceeds MaxWriteOperationsPerTransaction=%d."), Operations->Num(), Settings->MaxWriteOperationsPerTransaction));
		}

		TArray<FString> AffectedAssets;
		if (Plan.IsValid())
		{
			if (const TArray<TSharedPtr<FJsonValue>>* AssetValues = JsonArray(Plan, TEXT("affected_assets")))
			{
				for (const TSharedPtr<FJsonValue>& Value : *AssetValues)
				{
					FString AssetPath;
					if (Value.IsValid() && Value->TryGetString(AssetPath) && !AssetPath.IsEmpty())
					{
						AffectedAssets.AddUnique(NormalizeBlueprintObjectPath(AssetPath));
					}
				}
			}
		}
		if (AffectedAssets.Num() > Settings->MaxWriteAssetsPerTransaction)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_TOO_MANY_ASSETS"), FString::Printf(TEXT("Affected asset count %d exceeds MaxWriteAssetsPerTransaction=%d before modification."), AffectedAssets.Num(), Settings->MaxWriteAssetsPerTransaction));
		}
		TMap<FString, UClass*> PlannedAssetClasses;
		TMap<FString, FString> PlannedAssetPaths;
		TMap<FString, USkeleton*> PlannedMontageSkeletons;
		TMap<USkeleton*, TSet<FName>> PlannedSkeletonSlots;
		FUEPIEditExecutionState PreflightExecutionState;
		for (int32 Index = 0; Index < Operations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Operation = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
			const FString Type = JsonString(Operation, TEXT("type"), JsonString(Operation, TEXT("operation")));
			if (!Operation.IsValid() || Type.IsEmpty() || !Registry.FindOperation(Type).IsValid())
			{
				return ErrorResponse(RequestId, TEXT("UEPI_EDIT_OPERATION_UNSUPPORTED"), FString::Printf(TEXT("Operation %d is not present in the active Operation Registry: %s"), Index, *Type));
			}
			TSharedPtr<FJsonObject> OpParams = JsonObjectField(Operation, TEXT("params"));
			if (!OpParams.IsValid()) OpParams = Operation;
			if (Type.StartsWith(TEXT("actor.")) || Type.StartsWith(TEXT("input.")) || Type.StartsWith(TEXT("material.")) || Type.StartsWith(TEXT("content.")) || Type.StartsWith(TEXT("widget.")) || Type.StartsWith(TEXT("animation.")) || Type.StartsWith(TEXT("blueprint.")) || Type.StartsWith(TEXT("animgraph.")) || Type == TEXT("asset.set_properties"))
			{
				FUEPIEditContext Context;
				Context.TransactionId = TransactionId;
				Context.ProjectId = UEPIProjectBindingId();
				Context.AssetAllowList = AffectedAssets;
				Context.ResolvedAssets = PlannedAssetPaths;
				Context.ResolvedAssetClasses = PlannedAssetClasses;
				for (const TPair<USkeleton*, TSet<FName>>& Pair : PlannedSkeletonSlots) if (Pair.Key) Context.PlannedSkeletonSlots.Add(Pair.Key->GetPathName(), Pair.Value);
				for (const TPair<FString, USkeleton*>& Pair : PlannedMontageSkeletons) if (Pair.Value) Context.PlannedAssetSkeletons.Add(Pair.Key, Pair.Value->GetPathName());
				Context.ExecutionState = &PreflightExecutionState;
				Context.OperationReferences = OperationReferences(Operation, OpParams);
				Context.OperationIndex = Index;
				Context.bAllowBlueprintEdits = Settings->bAllowBlueprintEdits;
				Context.bDryRun = true;
				const FUEPIEditResult Preflight = Registry.FindOperation(Type)->Preview(Context, *OpParams);
				if (!Preflight.bOk)
				{
					return ErrorResponse(RequestId, Preflight.ErrorCode.IsEmpty() ? TEXT("UEPI_EDIT_PRECHECK_FAILED") : Preflight.ErrorCode, Preflight.Message);
				}
				if (Type == TEXT("input.create_action") || Type == TEXT("input.create_mapping_context"))
				{
					FString PackagePath; FString AssetName; FString DestinationError;
					const FString DefaultName = Type == TEXT("input.create_action") ? TEXT("IA_UEPIAction") : TEXT("IMC_UEPIContext");
					if (!SplitDestinationPath(OpParams, DefaultName, PackagePath, AssetName, DestinationError))
					{
						return ErrorResponse(RequestId, TEXT("UEPI_EDIT_DESTINATION_INVALID"), DestinationError);
					}
					const FString OpId = OperationId(Operation);
					if (!OpId.IsEmpty()) PlannedAssetPaths.Add(OpId, FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName));
				}
				else if (Type == TEXT("material.create_instance") && Preflight.Result.IsValid())
				{
					const FString PlannedPath = JsonString(Preflight.Result, TEXT("asset_path"));
					const FString OpId = OperationId(Operation);
					if (!OpId.IsEmpty() && !PlannedPath.IsEmpty()) PlannedAssetPaths.Add(OpId, PlannedPath);
				}
				else if ((Type == TEXT("content.create_asset") || Type == TEXT("content.duplicate_asset") || Type == TEXT("content.rename_asset")) && Preflight.Result.IsValid())
				{
					const FString PlannedPath = JsonString(Preflight.Result, TEXT("asset_path")); const FString OpId = OperationId(Operation);
					if (!OpId.IsEmpty() && !PlannedPath.IsEmpty()) PlannedAssetPaths.Add(OpId, PlannedPath);
					if (Type == TEXT("content.create_asset")) { UClass* PlannedClass = LoadObject<UClass>(nullptr, *JsonString(Preflight.Result, TEXT("asset_class"))); if (!OpId.IsEmpty() && PlannedClass) PlannedAssetClasses.Add(OpId, PlannedClass); }
					else if (!OpId.IsEmpty())
					{
						const FString SourcePath = NormalizeBlueprintObjectPath(JsonString(OpParams, TEXT("source"), JsonString(OpParams, TEXT("asset"))));
						if (UObject* SourceAsset = LoadObject<UObject>(nullptr, *SourcePath))
						{
							PlannedAssetClasses.Add(OpId, SourceAsset->GetClass());
							UObject* Probe = Type == TEXT("content.duplicate_asset") ? DuplicateObject(SourceAsset, GetTransientPackage()) : SourceAsset;
							if (Probe) PreflightExecutionState.ObjectRefs.Add(OpId, Probe);
						}
					}
				}
				else if (Type == TEXT("widget.create") && Preflight.Result.IsValid())
				{
					const FString PlannedPath = JsonString(Preflight.Result, TEXT("asset_path")); const FString OpId = OperationId(Operation); if (!OpId.IsEmpty() && !PlannedPath.IsEmpty()) { PlannedAssetPaths.Add(OpId, PlannedPath); PlannedAssetClasses.Add(OpId, UWidgetBlueprint::StaticClass()); }
				}
				else if ((Type == TEXT("animation.register_slot") || Type == TEXT("animation.create_slot_group")) && Preflight.Result.IsValid())
				{
					USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *JsonString(Preflight.Result, TEXT("skeleton"))); const FName Slot(*JsonString(Preflight.Result, TEXT("slot_name"))); if (Skeleton && !Slot.IsNone()) PlannedSkeletonSlots.FindOrAdd(Skeleton).Add(Slot);
				}
				else if (Type == TEXT("animation.create_montage_from_sequence") && Preflight.Result.IsValid())
				{
					const FString OpId = OperationId(Operation); const FString PlannedPath = JsonString(Preflight.Result, TEXT("asset_path")); USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *JsonString(Preflight.Result, TEXT("skeleton"))); if (!OpId.IsEmpty() && !PlannedPath.IsEmpty() && Skeleton) { PlannedAssetPaths.Add(OpId, PlannedPath); PlannedAssetClasses.Add(OpId, UAnimMontage::StaticClass()); PlannedMontageSkeletons.Add(OpId, Skeleton); }
				}
				continue;
			}
		}
		for (const FString& AssetPath : AffectedAssets)
		{
			if (!AssetPath.StartsWith(TEXT("/")))
			{
				continue;
			}
			const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
			if (UPackage* ExistingPackage = FindPackage(nullptr, *PackageName))
			{
				if (ExistingPackage->IsDirty())
				{
					return ErrorResponse(RequestId, TEXT("UEPI_EDIT_TARGET_DIRTY"), FString::Printf(TEXT("Target package already has user changes: %s"), *PackageName));
				}
			}
			const FString PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
			if (IFileManager::Get().FileExists(*PackageFile) && IFileManager::Get().IsReadOnly(*PackageFile))
			{
				return ErrorResponse(RequestId, TEXT("UEPI_EDIT_TARGET_READ_ONLY"), FString::Printf(TEXT("Target package file is read-only: %s"), *PackageFile));
			}
		}
		if (const TArray<TSharedPtr<FJsonValue>>* BeforeFingerprints = JsonArray(Plan, TEXT("before_fingerprints")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *BeforeFingerprints)
			{
				const TSharedPtr<FJsonObject> Fingerprint = Value.IsValid() ? Value->AsObject() : nullptr;
				const FString AssetPath = JsonString(Fingerprint, TEXT("asset"));
				if (!Fingerprint.IsValid() || AssetPath.IsEmpty() || !AssetPath.StartsWith(TEXT("/")))
				{
					continue;
				}
				const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
				const FString PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
				const bool bExpectedExists = JsonBool(Fingerprint, TEXT("exists"), false);
				const bool bExists = IFileManager::Get().FileExists(*PackageFile);
				const FString ExpectedSha256 = JsonString(Fingerprint, TEXT("sha256"));
				if (bExists != bExpectedExists || (bExists && !ExpectedSha256.IsEmpty() && UEPIFileSha256(PackageFile) != ExpectedSha256))
				{
					return ErrorResponse(RequestId, TEXT("UEPI_EDIT_BEFORE_FINGERPRINT_CHANGED"), FString::Printf(TEXT("Target package changed after Preview: %s"), *AssetPath));
				}
			}
		}
		TMap<FString, FString> TransactionBackupFiles;
		FString BackupDirectory;
		FString ServiceError;
		if (!FUEPIBackupService::Create(TransactionId, AffectedAssets, TransactionBackupFiles, BackupDirectory, ServiceError))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_BACKUP_FAILED"), ServiceError);
		}
		FString JournalPath;
		if (!FUEPITransactionJournal::Write(TransactionId, TEXT("prepared"), AffectedAssets, TransactionBackupFiles, false, TEXT("Preflight passed and package backups were created."), JournalPath, ServiceError))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_JOURNAL_FAILED"), ServiceError);
		}

		TArray<TSharedPtr<FJsonValue>> OperationResults;
		TArray<TSharedPtr<FJsonValue>> CompileResults;
		bool bAllOk = true;
		bool bValidationOk = true;
		bool bMutated = false;
		FString FailureMessage;
		TSet<UBlueprint*> TouchedBlueprints;
		FUEPIEditExecutionState HandlerExecutionState;
		TMap<FString, FString> OperationAssets;
		TArray<TWeakObjectPtr<UObject>> PendingAssetRegistryCreates;

		const FText TransactionText = FText::FromString(TransactionId.IsEmpty() ? FString(TEXT("UEPI edit transaction")) : FString::Printf(TEXT("UEPI edit %s"), *TransactionId));
		FScopedTransaction Transaction(TEXT("UEProjectIntelligence"), TransactionText, nullptr, true);
		for (int32 Index = 0; Index < Operations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Operation = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
			FString Type = JsonString(Operation, TEXT("type"), JsonString(Operation, TEXT("operation")));
			TSharedPtr<FJsonObject> OpParams = JsonObjectField(Operation, TEXT("params"));
			if (!OpParams.IsValid())
			{
				OpParams = Operation;
			}
			if (Type.StartsWith(TEXT("actor.")) || Type.StartsWith(TEXT("input.")) || Type.StartsWith(TEXT("material.")) || Type.StartsWith(TEXT("content.")) || Type.StartsWith(TEXT("widget.")) || Type.StartsWith(TEXT("animation.")) || Type.StartsWith(TEXT("blueprint.")) || Type.StartsWith(TEXT("animgraph.")) || Type == TEXT("asset.set_properties"))
			{
				FUEPIEditContext Context;
				Context.TransactionId = TransactionId;
				Context.ProjectId = UEPIProjectBindingId();
				Context.AssetAllowList = AffectedAssets;
				Context.ResolvedAssets = OperationAssets;
				Context.ResolvedAssetClasses = PlannedAssetClasses;
				for (const TPair<USkeleton*, TSet<FName>>& Pair : PlannedSkeletonSlots) if (Pair.Key) Context.PlannedSkeletonSlots.Add(Pair.Key->GetPathName(), Pair.Value);
				for (const TPair<FString, USkeleton*>& Pair : PlannedMontageSkeletons) if (Pair.Value) Context.PlannedAssetSkeletons.Add(Pair.Key, Pair.Value->GetPathName());
				Context.ExecutionState = &HandlerExecutionState;
				Context.OperationReferences = OperationReferences(Operation, OpParams);
				Context.OperationIndex = Index;
				Context.bAllowBlueprintEdits = Settings->bAllowBlueprintEdits;
				Context.bDryRun = false;
				Context.bAllowSave = JsonBool(Params, TEXT("save"), false);
				const FUEPIEditResult OperationResult = Registry.FindOperation(Type)->Apply(Context, *OpParams);
				AddOperationResult(OperationResults, Index, Type, OperationResult.bOk, OperationResult.Message, OperationResult.Result);
				if (!OperationResult.bOk)
				{
					bAllOk = false;
					FailureMessage = OperationResult.Message;
					break;
				}
				if (Type == TEXT("input.create_action") || Type == TEXT("input.create_mapping_context"))
				{
					const TSharedPtr<FJsonObject> Asset = OperationResult.Result.IsValid() ? JsonObjectField(OperationResult.Result, TEXT("asset")) : nullptr;
					const FString AssetPath = JsonString(Asset, TEXT("path"));
					if (!AssetPath.IsEmpty())
					{
						AffectedAssets.AddUnique(AssetPath);
						const FString OpId = OperationId(Operation);
						if (!OpId.IsEmpty()) OperationAssets.Add(OpId, AssetPath);
					}
				}
				else if (Type == TEXT("material.create_instance"))
				{
					const TSharedPtr<FJsonObject> Asset = OperationResult.Result.IsValid() ? JsonObjectField(OperationResult.Result, TEXT("asset")) : nullptr;
					const FString AssetPath = JsonString(Asset, TEXT("path"));
					if (!AssetPath.IsEmpty()) { AffectedAssets.AddUnique(AssetPath); const FString OpId = OperationId(Operation); if (!OpId.IsEmpty()) OperationAssets.Add(OpId, AssetPath); }
				}
				else if (Type == TEXT("material.apply_to_blueprint_component"))
				{
					const FString BlueprintPath = JsonString(OperationResult.Result, TEXT("blueprint"));
					if (!BlueprintPath.IsEmpty()) { AffectedAssets.AddUnique(BlueprintPath); if (UBlueprint* Touched = LoadObject<UBlueprint>(nullptr, *BlueprintPath)) TouchedBlueprints.Add(Touched); }
				}
				else if (Type.StartsWith(TEXT("material.set_")))
				{
					const FString MaterialPath = JsonString(OperationResult.Result, TEXT("asset")); if (!MaterialPath.IsEmpty()) AffectedAssets.AddUnique(MaterialPath);
				}
				else if (Type == TEXT("content.create_asset") || Type == TEXT("content.duplicate_asset"))
				{
					const TSharedPtr<FJsonObject> Asset = JsonObjectField(OperationResult.Result, TEXT("asset")); const FString AssetPath = JsonString(Asset, TEXT("path")); if (!AssetPath.IsEmpty()) { AffectedAssets.AddUnique(AssetPath); const FString OpId = OperationId(Operation); if (!OpId.IsEmpty()) OperationAssets.Add(OpId, AssetPath); if (Type == TEXT("content.create_asset")) PendingAssetRegistryCreates.Add(FindObject<UObject>(nullptr, *AssetPath)); }
				}
				else if (Type == TEXT("content.rename_asset"))
				{
					const FString AssetPath = JsonString(OperationResult.Result, TEXT("new_asset")); if (!AssetPath.IsEmpty()) { AffectedAssets.AddUnique(AssetPath); const FString OpId = OperationId(Operation); if (!OpId.IsEmpty()) OperationAssets.Add(OpId, AssetPath); }
				}
				else if (Type == TEXT("content.import"))
				{
					if (const TArray<TSharedPtr<FJsonValue>>* Assets = JsonArray(OperationResult.Result, TEXT("assets"))) for (const TSharedPtr<FJsonValue>& Value : *Assets) { const FString AssetPath = JsonString(Value.IsValid() ? Value->AsObject() : nullptr, TEXT("path")); if (!AssetPath.IsEmpty()) AffectedAssets.AddUnique(AssetPath); }
				}
				else if (Type == TEXT("content.save_assets"))
				{
					for (const FString& AssetPath : JsonStringArray(OperationResult.Result, TEXT("assets"))) AffectedAssets.AddUnique(AssetPath);
				}
				else if (Type == TEXT("asset.set_properties"))
				{
					const FString AssetPath = JsonString(OperationResult.Result, TEXT("asset")); if (!AssetPath.IsEmpty()) AffectedAssets.AddUnique(AssetPath);
				}
				else if (Type.StartsWith(TEXT("widget.")))
				{
					const TSharedPtr<FJsonObject> CreatedAsset = JsonObjectField(OperationResult.Result, TEXT("asset")); FString AssetPath = JsonString(CreatedAsset, TEXT("path")); if (AssetPath.IsEmpty()) AssetPath = JsonString(OperationResult.Result, TEXT("asset"));
					if (!AssetPath.IsEmpty()) { AffectedAssets.AddUnique(AssetPath); if (UWidgetBlueprint* Touched = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath)) TouchedBlueprints.Add(Touched); if (Type == TEXT("widget.create")) { const FString OpId = OperationId(Operation); if (!OpId.IsEmpty()) OperationAssets.Add(OpId, AssetPath); } }
				}
				else if (Type == TEXT("animation.register_slot") || Type == TEXT("animation.create_slot_group"))
				{
					const FString SkeletonPath = JsonString(OperationResult.Result, TEXT("skeleton")); if (!SkeletonPath.IsEmpty()) AffectedAssets.AddUnique(SkeletonPath);
				}
				else if (Type == TEXT("animation.create_montage_from_sequence"))
				{
					const FString AssetPath = JsonString(OperationResult.Result, TEXT("asset_path")); if (!AssetPath.IsEmpty()) { AffectedAssets.AddUnique(AssetPath); const FString OpId = OperationId(Operation); if (!OpId.IsEmpty()) OperationAssets.Add(OpId, AssetPath); }
				}
				else if (Type.StartsWith(TEXT("animation.")))
				{
					const FString AssetPath = JsonString(OperationResult.Result, TEXT("asset")); if (!AssetPath.IsEmpty()) AffectedAssets.AddUnique(AssetPath);
				}
				else if (Type.StartsWith(TEXT("blueprint.")) || Type.StartsWith(TEXT("animgraph.")))
				{
					const FString BlueprintPath = JsonString(OperationResult.Result, TEXT("asset"));
					if (!BlueprintPath.IsEmpty()) { AffectedAssets.AddUnique(BlueprintPath); if (UBlueprint* Touched = LoadObject<UBlueprint>(nullptr, *BlueprintPath)) TouchedBlueprints.Add(Touched); }
					if (Type == TEXT("blueprint.compile") || Type == TEXT("animgraph.compile")) { if (OperationResult.Result.IsValid()) CompileResults.Add(MakeShared<FJsonValueObject>(OperationResult.Result.ToSharedRef())); bValidationOk &= OperationResult.bOk; }
				}
				if (Type != TEXT("content.save_assets") && Type != TEXT("blueprint.compile") && Type != TEXT("animgraph.compile")) bMutated = true;
				continue;
			}
		}

		if (!bAllOk)
		{
			Transaction.Cancel();
		}

		if (bAllOk)
		{
			for (const TWeakObjectPtr<UObject>& PendingAsset : PendingAssetRegistryCreates)
			{
				if (UObject* Asset = PendingAsset.Get()) FAssetRegistryModule::AssetCreated(Asset);
			}
			for (UBlueprint* Blueprint : TouchedBlueprints)
			{
				TSharedRef<FJsonObject> CompileResult = CompileBlueprintToJson(Blueprint);
				CompileResults.Add(MakeShared<FJsonValueObject>(CompileResult));
				if (!CompileResult->GetBoolField(TEXT("ok")))
				{
					bValidationOk = false;
					FailureMessage = FString::Printf(TEXT("Blueprint compile returned errors for %s."), *Blueprint->GetPathName());
				}
			}
		}
		if (!bValidationOk)
		{
			bAllOk = false;
			Transaction.Cancel();
		}

		bool bSaved = false;
		TArray<TSharedPtr<FJsonValue>> SavedFileHashes;
		if (bAllOk && JsonBool(Params, TEXT("save"), false))
		{
			TArray<FUEPISavedFileHash> FileHashes;
			bSaved = FUEPIPackageSaveService::SaveTouched(AffectedAssets, FileHashes, ServiceError);
			if (!bSaved)
			{
				bAllOk = false;
				FailureMessage = ServiceError;
				Transaction.Cancel();
				FString RestoreError;
				FUEPIBackupService::Restore(TransactionBackupFiles, AffectedAssets, RestoreError);
			}
			else
			{
				for (const FUEPISavedFileHash& Hash : FileHashes)
				{
					TSharedRef<FJsonObject> FileHash = MakeShared<FJsonObject>();
					FileHash->SetStringField(TEXT("file"), Hash.File);
					FileHash->SetStringField(TEXT("md5"), Hash.Md5);
					SavedFileHashes.Add(MakeShared<FJsonValueObject>(FileHash));
				}
			}
		}
		FString JournalError;
		FUEPITransactionJournal::Write(TransactionId, bAllOk ? TEXT("applied") : TEXT("failed"), AffectedAssets, TransactionBackupFiles, bSaved, FailureMessage, JournalPath, JournalError);

		FString RefreshRequestPath;
		FString RefreshError;
		if (AffectedAssets.Num() > 0)
		{
			WriteRefreshRequest(AffectedAssets, TEXT("live"), RefreshRequestPath, RefreshError);
		}

		if (bMutated)
		{
			LastAppliedTransactionId = TransactionId;
			LastAppliedSummary = FString::Printf(TEXT("%d operation(s), %d asset(s)"), Operations->Num(), AffectedAssets.Num());
			LastAppliedBackupFiles = TransactionBackupFiles;
			LastAppliedAffectedAssets = AffectedAssets;
			bLastAppliedSaved = bSaved;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.bridge-edit-apply.v1"));
		Result->SetStringField(TEXT("transaction_id"), TransactionId);
		Result->SetBoolField(TEXT("applied"), bAllOk);
		Result->SetBoolField(TEXT("validation_ok"), bValidationOk);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetArrayField(TEXT("saved_file_hashes"), SavedFileHashes);
		Result->SetStringField(TEXT("backup_directory"), BackupDirectory);
		Result->SetStringField(TEXT("journal_path"), JournalPath);
		Result->SetStringField(TEXT("failure_message"), FailureMessage);
		Result->SetArrayField(TEXT("affected_assets"), StringArrayToJsonValues(AffectedAssets));
		Result->SetArrayField(TEXT("operations"), OperationResults);
		Result->SetArrayField(TEXT("compile"), CompileResults);
		Result->SetStringField(TEXT("refresh_request_path"), RefreshRequestPath);
		Result->SetStringField(TEXT("refresh_error"), RefreshError);
		Result->SetStringField(TEXT("rollback_strategy"), bSaved ? TEXT("file_backup_restore_and_package_reload") : TEXT("editor_transaction_undo"));

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), bAllOk);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), bAllOk ? (bValidationOk ? EmptyJsonArray() : DiagnosticsArray(TEXT("UEPI_EDIT_VALIDATE_FAILED"), TEXT("warning"), FailureMessage)) : DiagnosticsArray(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("error"), FailureMessage));
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::EditValidateResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params) const
	{
		const TSharedPtr<FJsonObject> Plan = JsonObjectField(Params, TEXT("plan"));
		TArray<FString> Assets;
		if (Plan.IsValid())
		{
			if (const TArray<TSharedPtr<FJsonValue>>* AssetValues = JsonArray(Plan, TEXT("affected_assets")))
			{
				for (const TSharedPtr<FJsonValue>& Value : *AssetValues)
				{
					FString AssetPath;
					if (Value.IsValid() && Value->TryGetString(AssetPath))
					{
						Assets.AddUnique(NormalizeBlueprintObjectPath(AssetPath));
					}
				}
			}
		}
		if (Assets.Num() == 0)
		{
			const FString AssetPath = NormalizeBlueprintObjectPath(JsonString(Params, TEXT("asset")));
			if (!AssetPath.IsEmpty())
			{
				Assets.Add(AssetPath);
			}
		}

		TArray<TSharedPtr<FJsonValue>> ValidationResults;
		bool bAllOk = true;
		for (const FString& AssetPath : Assets)
		{
			UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
			FUEPIValidationResult Validation = FUEPIValidatorRegistry::Get().Validate(Object);
			if (Validation.Asset.IsEmpty()) Validation.Asset = AssetPath;
			TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetBoolField(TEXT("ok"), Validation.bOk);
			Value->SetStringField(TEXT("asset"), Validation.Asset);
			Value->SetStringField(TEXT("class"), Validation.ClassPath);
			Value->SetStringField(TEXT("validator"), Validation.Validator);
			Value->SetStringField(TEXT("message"), Validation.Message);
			ValidationResults.Add(MakeShared<FJsonValueObject>(Value));
			bAllOk &= Validation.bOk;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.bridge-edit-validate.v2"));
		Result->SetStringField(TEXT("transaction_id"), JsonString(Params, TEXT("transaction_id")));
		Result->SetBoolField(TEXT("validated"), bAllOk);
		Result->SetArrayField(TEXT("assets"), StringArrayToJsonValues(Assets));
		Result->SetArrayField(TEXT("validations"), ValidationResults);

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), bAllOk);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), bAllOk ? EmptyJsonArray() : DiagnosticsArray(TEXT("UEPI_EDIT_VALIDATE_FAILED"), TEXT("error"), TEXT("One or more typed target validations failed.")));
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::EditRollbackResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params)
	{
		const FString TransactionId = JsonString(Params, TEXT("transaction_id"));
		if (TransactionId.IsEmpty() || TransactionId != LastAppliedTransactionId)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_ROLLBACK_NOT_MATCHING_LAST_TRANSACTION"), TEXT("Rollback is only allowed for the last UEPI-applied transaction in this editor session."));
		}
		if (!GEditor)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_EDITOR_UNAVAILABLE"), TEXT("GEditor is not available."));
		}
		const bool bUndone = GEditor->UndoTransaction(false);
		bool bFilesRestored = true;
		FString RestoreError;
		if (bLastAppliedSaved)
		{
			bFilesRestored = FUEPIBackupService::Restore(LastAppliedBackupFiles, LastAppliedAffectedAssets, RestoreError);
		}
		FString RefreshRequestPath;
		FString RefreshError;
		if (bUndone && bFilesRestored && LastAppliedAffectedAssets.Num() > 0)
		{
			WriteRefreshRequest(LastAppliedAffectedAssets, TEXT("live"), RefreshRequestPath, RefreshError);
		}
		FString JournalPath;
		FString JournalError;
		FUEPITransactionJournal::Write(TransactionId, (bUndone && bFilesRestored) ? TEXT("rolled_back") : TEXT("rollback_failed"), LastAppliedAffectedAssets, LastAppliedBackupFiles, false, RestoreError, JournalPath, JournalError);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.bridge-edit-rollback.v1"));
		Result->SetStringField(TEXT("transaction_id"), TransactionId);
		Result->SetStringField(TEXT("summary"), LastAppliedSummary);
		Result->SetBoolField(TEXT("undone"), bUndone);
		Result->SetBoolField(TEXT("files_restored"), bFilesRestored);
		Result->SetStringField(TEXT("journal_path"), JournalPath);
		Result->SetStringField(TEXT("refresh_request_path"), RefreshRequestPath);
		Result->SetStringField(TEXT("refresh_error"), RefreshError);
		if (bUndone && bFilesRestored)
		{
			LastAppliedTransactionId.Reset();
			LastAppliedSummary.Reset();
			LastAppliedBackupFiles.Reset();
			LastAppliedAffectedAssets.Reset();
			bLastAppliedSaved = false;
		}
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), bUndone && bFilesRestored);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), (bUndone && bFilesRestored) ? EmptyJsonArray() : DiagnosticsArray(TEXT("UEPI_EDIT_ROLLBACK_FAILED"), TEXT("error"), TEXT("Editor undo or persisted file restoration failed.")));
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::MakeSessionObject(const FString& State) const
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema_version"), FUEPIBridgeProtocol::SessionSchemaVersion());
		Root->SetBoolField(TEXT("active"), State.Equals(TEXT("active"), ESearchCase::IgnoreCase));
		Root->SetStringField(TEXT("state"), State);
		Root->SetStringField(TEXT("session_id"), SessionId);
		Root->SetStringField(TEXT("editor_session_id"), SessionId);
		Root->SetStringField(TEXT("host"), TEXT("127.0.0.1"));
		Root->SetNumberField(TEXT("port"), Port);
		Root->SetStringField(TEXT("protocol"), FUEPIBridgeProtocol::ProtocolName());
		Root->SetBoolField(TEXT("transport_ready"), Listener.IsValid() && Listener->IsActive());
		Root->SetStringField(TEXT("implementation"), TEXT("tcp_length_prefixed_json"));
		Root->SetStringField(TEXT("project_name"), FApp::GetProjectName());
		Root->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		Root->SetStringField(TEXT("canonical_project_file"), UEPICanonicalProjectFile());
		Root->SetStringField(TEXT("project_binding_id"), UEPIProjectBindingId());
		Root->SetStringField(TEXT("project_root"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		Root->SetStringField(TEXT("store_root"), UEPIStoreRoot());
		Root->SetStringField(TEXT("session_path"), FPaths::ConvertRelativePathToFull(SessionPath));
		Root->SetStringField(TEXT("token_path"), FPaths::ConvertRelativePathToFull(TokenPath));
		Root->SetStringField(TEXT("token_hash"), FMD5::HashAnsiString(*Token));
		Root->SetNumberField(TEXT("pid"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
		Root->SetStringField(TEXT("started_at"), FDateTime::UtcNow().ToIso8601());
		Root->SetStringField(TEXT("last_heartbeat"), FDateTime::UtcNow().ToIso8601());
		Root->SetStringField(TEXT("heartbeat_at"), FDateTime::UtcNow().ToIso8601());
		const UUEPISettings* Settings = GetDefault<UUEPISettings>();
		Root->SetBoolField(TEXT("allow_save"), Settings && Settings->bAllowSavingPackages);
		Root->SetBoolField(TEXT("allow_pie"), Settings && Settings->bAllowPIEControl);
		Root->SetBoolField(TEXT("allow_runtime_invoke"), Settings && Settings->bAllowRuntimeInvoke);
		TSharedRef<FJsonObject> Bridge = MakeShared<FJsonObject>();
		Bridge->SetStringField(TEXT("host"), TEXT("127.0.0.1"));
		Bridge->SetNumberField(TEXT("port"), Port);
		Bridge->SetStringField(TEXT("protocol"), FUEPIBridgeProtocol::ProtocolName());
		Root->SetObjectField(TEXT("bridge"), Bridge);
		TArray<FString> Capabilities = FUEPIBridgeProtocol::ReadCapabilities();
		Capabilities.Append(FUEPIBridgeProtocol::WriteCapabilities());
		Root->SetArrayField(TEXT("capabilities"), StringArrayToJsonValues(Capabilities));
		return Root;
	}

	bool FUEPIEditorCommandBridge::WriteSessionObject(const FString& State, FString& OutError) const
	{
		if (SessionPath.IsEmpty())
		{
			OutError = TEXT("UEPI bridge session path is empty.");
			return false;
		}

		const TSharedRef<FJsonObject> SessionObject = MakeSessionObject(State);
		const FString SessionText = JsonObjectToString(SessionObject);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SessionPath), true);
		if (!FFileHelper::SaveStringToFile(SessionText, *SessionPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write UEPI bridge session file: %s"), *SessionPath);
			return false;
		}
		const FString GlobalSessionPath = UEPIGlobalBridgeSessionPath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(GlobalSessionPath), true);
		FFileHelper::SaveStringToFile(SessionText, *GlobalSessionPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		OutError.Reset();
		return true;
	}
}
