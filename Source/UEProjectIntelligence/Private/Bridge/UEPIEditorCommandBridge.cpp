#include "Bridge/UEPIEditorCommandBridge.h"

#include "Bridge/UEPIBridgeProtocol.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Common/TcpListener.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UEPISettings.h"
#include "UEPISnapshotStore.h"
#include "UObject/UnrealType.h"

namespace UE::ProjectIntelligence
{
	namespace
	{
		FString UEPISessionsDirectory()
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("sessions"));
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

		FString UEPIRequestsDirectory()
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("requests"));
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

		TSharedPtr<FJsonObject> JsonObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
		{
			if (!Object.IsValid())
			{
				return nullptr;
			}
			const TSharedPtr<FJsonObject>* Value = nullptr;
			return Object->TryGetObjectField(Field, Value) && Value ? *Value : nullptr;
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

		bool BuildPinType(const FString& RequestedType, FEdGraphPinType& OutPinType, FString& OutError)
		{
			const FString Type = RequestedType.TrimStartAndEnd().ToLower();
			OutPinType = FEdGraphPinType();
			if (Type == TEXT("bool") || Type == TEXT("boolean"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			}
			else if (Type == TEXT("byte"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			}
			else if (Type == TEXT("int") || Type == TEXT("integer"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			}
			else if (Type == TEXT("int64") || Type == TEXT("long"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
			}
			else if (Type == TEXT("float") || Type == TEXT("real"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Float;
			}
			else if (Type == TEXT("double"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Double;
			}
			else if (Type == TEXT("string"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
			}
			else if (Type == TEXT("name"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
			}
			else if (Type == TEXT("text"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
			}
			else
			{
				OutError = FString::Printf(TEXT("Unsupported simple Blueprint variable type: %s"), *RequestedType);
				return false;
			}
			return true;
		}

		FString NormalizeBlueprintObjectPath(const FString& RawPath)
		{
			FString Path = RawPath.TrimStartAndEnd();
			if (Path.IsEmpty() || Path.Contains(TEXT(".")))
			{
				return Path;
			}
			FString Package;
			FString AssetName;
			if (Path.Split(TEXT("/"), &Package, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !AssetName.IsEmpty())
			{
				return Path + TEXT(".") + AssetName;
			}
			return Path;
		}

		UBlueprint* LoadBlueprintForEdit(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
		{
			OutAssetPath = NormalizeBlueprintObjectPath(JsonString(Params, TEXT("asset")));
			if (OutAssetPath.IsEmpty())
			{
				OutAssetPath = NormalizeBlueprintObjectPath(JsonString(Params, TEXT("blueprint")));
			}
			if (OutAssetPath.IsEmpty())
			{
				OutError = TEXT("Blueprint operation requires params.asset or params.blueprint.");
				return nullptr;
			}
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *OutAssetPath);
			if (!Blueprint)
			{
				OutError = FString::Printf(TEXT("Failed to load Blueprint asset: %s"), *OutAssetPath);
				return nullptr;
			}
			return Blueprint;
		}

		UClass* ResolveComponentClass(const FString& RequestedClass)
		{
			const FString ClassName = RequestedClass.TrimStartAndEnd();
			if (ClassName.IsEmpty() || ClassName == TEXT("SceneComponent") || ClassName == TEXT("USceneComponent"))
			{
				return USceneComponent::StaticClass();
			}
			if (ClassName == TEXT("StaticMeshComponent") || ClassName == TEXT("UStaticMeshComponent"))
			{
				return UStaticMeshComponent::StaticClass();
			}
			if (ClassName == TEXT("ActorComponent") || ClassName == TEXT("UActorComponent"))
			{
				return UActorComponent::StaticClass();
			}
			return StaticLoadClass(UActorComponent::StaticClass(), nullptr, *ClassName);
		}

		bool SetSimplePropertyValue(UObject* Object, const FString& PropertyName, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			if (!Object)
			{
				OutError = TEXT("Property target object is null.");
				return false;
			}
			FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
			if (!Property)
			{
				OutError = FString::Printf(TEXT("Property was not found on %s: %s"), *Object->GetClass()->GetName(), *PropertyName);
				return false;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
			{
				BoolProperty->SetPropertyValue(ValuePtr, JsonBool(Params, TEXT("value")));
				return true;
			}
			if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
			{
				IntProperty->SetPropertyValue(ValuePtr, JsonInt(Params, TEXT("value")));
				return true;
			}
			double NumberValue = 0.0;
			const bool bHasNumber = Params.IsValid() && Params->TryGetNumberField(TEXT("value"), NumberValue);
			if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
			{
				if (!bHasNumber)
				{
					OutError = TEXT("Float property requires a numeric value.");
					return false;
				}
				FloatProperty->SetPropertyValue(ValuePtr, static_cast<float>(NumberValue));
				return true;
			}
			if (FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
			{
				if (!bHasNumber)
				{
					OutError = TEXT("Double property requires a numeric value.");
					return false;
				}
				DoubleProperty->SetPropertyValue(ValuePtr, NumberValue);
				return true;
			}
			FString StringValue = JsonString(Params, TEXT("value"));
			if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
			{
				StringProperty->SetPropertyValue(ValuePtr, StringValue);
				return true;
			}
			if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
			{
				NameProperty->SetPropertyValue(ValuePtr, FName(*StringValue));
				return true;
			}
			OutError = FString::Printf(TEXT("Property type is not supported by write alpha: %s"), *Property->GetClass()->GetName());
			return false;
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
			const FString RequestIdPart = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString CreatedAt = FDateTime::UtcNow().ToIso8601();
			const FString RequestPath = FPaths::Combine(UEPIRequestsDirectory(), FString::Printf(TEXT("refresh-%s-%s.json"), *CreatedAt.Replace(TEXT(":"), TEXT("")).Replace(TEXT("-"), TEXT("")), *RequestIdPart.Left(12)));

			TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
			Request->SetStringField(TEXT("schema_version"), TEXT("uepi.refresh-request.v1"));
			Request->SetStringField(TEXT("request_id"), RequestIdPart);
			Request->SetStringField(TEXT("status"), TEXT("pending"));
			Request->SetStringField(TEXT("created_at_utc"), CreatedAt);
			Request->SetStringField(TEXT("data_mode"), DataMode);
			Request->SetArrayField(TEXT("target_object_paths"), StringArrayToJsonValues(Targets));
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
			return FFileHelper::SaveStringToFile(JsonObjectToString(Object), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
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
			const FTransform Transform = Actor->GetActorTransform();
			TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
			Location->SetNumberField(TEXT("x"), Transform.GetLocation().X);
			Location->SetNumberField(TEXT("y"), Transform.GetLocation().Y);
			Location->SetNumberField(TEXT("z"), Transform.GetLocation().Z);
			Object->SetObjectField(TEXT("location"), Location);
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
		Port = InRequestedPort > 0 ? FMath::Clamp(InRequestedPort, 1, 65535) : 48735;
		bActive = true;

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(TokenPath), true);
		if (!FFileHelper::SaveStringToFile(Token, *TokenPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			bActive = false;
			OutError = FString::Printf(TEXT("Failed to write UEPI bridge token file: %s"), *TokenPath);
			return false;
		}

		const FIPv4Endpoint Endpoint(FIPv4Address(127, 0, 0, 1), static_cast<uint16>(Port));
		Listener = MakeUnique<FTcpListener>(Endpoint, FTimespan::FromMilliseconds(100), true);
		Listener->OnConnectionAccepted().BindRaw(this, &FUEPIEditorCommandBridge::HandleConnectionAccepted);

		return WriteSessionObject(TEXT("active"), OutError);
	}

	void FUEPIEditorCommandBridge::Stop()
	{
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
			const int32 LineLimit = JsonInt(Params, TEXT("line_limit"), 100);
			return OutputLogResult(RequestId, LineLimit <= 0 ? 100 : LineLimit);
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

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::OutputLogResult(const FString& RequestId, int32 LineLimit) const
	{
		const FString OutputLogPath = AbsoluteLogFilename();
		TArray<TSharedPtr<FJsonValue>> Lines;
		for (const FString& Line : TailLines(OutputLogPath, LineLimit))
		{
			Lines.Add(MakeShared<FJsonValueString>(Line));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("log_path"), OutputLogPath);
		Result->SetNumberField(TEXT("line_count"), Lines.Num());
		Result->SetArrayField(TEXT("lines"), Lines);

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), true);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
		return Response;
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
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("artifact_directory"), FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("artifacts"), TEXT("screenshots")));
		Result->SetStringField(TEXT("status"), TEXT("unsupported_in_current_build"));

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), false);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), DiagnosticsArray(TEXT("UEPI_VIEWPORT_CAPTURE_NOT_IMPLEMENTED"), TEXT("warning"), TEXT("Viewport capture artifact creation is reserved for a later bridge build.")));
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::EditDiscoverResult(const FString& RequestId) const
	{
		const UUEPISettings* Settings = GetDefault<UUEPISettings>();
		const bool bWriteEnabled = Settings && Settings->bEnableWriteTools;
		const bool bBlueprintApplyEnabled = bWriteEnabled && Settings->bAllowBlueprintEdits;

		auto MakeOperation = [bBlueprintApplyEnabled](const FString& Name, const FString& Domain, const FString& Status, bool bApplySupported)
		{
			TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
			Operation->SetStringField(TEXT("name"), Name);
			Operation->SetStringField(TEXT("domain"), Domain);
			Operation->SetStringField(TEXT("status"), Status);
			Operation->SetBoolField(TEXT("preview_supported"), true);
			Operation->SetBoolField(TEXT("apply_supported"), bApplySupported && bBlueprintApplyEnabled);
			return MakeShared<FJsonValueObject>(Operation);
		};

		TArray<TSharedPtr<FJsonValue>> Operations;
		Operations.Add(MakeOperation(TEXT("blueprint.add_variable"), TEXT("blueprint"), TEXT("alpha_apply"), true));
		Operations.Add(MakeOperation(TEXT("blueprint.set_variable_default"), TEXT("blueprint"), TEXT("alpha_apply"), true));
		Operations.Add(MakeOperation(TEXT("blueprint.add_component"), TEXT("blueprint"), TEXT("alpha_apply"), true));
		Operations.Add(MakeOperation(TEXT("blueprint.set_component_property"), TEXT("blueprint"), TEXT("alpha_apply"), true));
		Operations.Add(MakeOperation(TEXT("blueprint.compile"), TEXT("blueprint"), TEXT("alpha_apply"), true));

		const TArray<FString> UnsupportedBlueprintOps = {
			TEXT("blueprint.create_function"),
			TEXT("blueprint.add_event_node"),
			TEXT("blueprint.add_function_call_node"),
			TEXT("blueprint.add_variable_get_node"),
			TEXT("blueprint.add_variable_set_node"),
			TEXT("blueprint.add_branch_node"),
			TEXT("blueprint.add_print_string_node"),
			TEXT("blueprint.connect_pins")
		};
		for (const FString& Name : UnsupportedBlueprintOps)
		{
			Operations.Add(MakeOperation(Name, TEXT("blueprint"), TEXT("unsupported_alpha"), false));
		}

		TSharedRef<FJsonObject> SettingsObject = MakeShared<FJsonObject>();
		SettingsObject->SetBoolField(TEXT("write_tools_enabled"), bWriteEnabled);
		SettingsObject->SetBoolField(TEXT("blueprint_edits_enabled"), Settings && Settings->bAllowBlueprintEdits);
		SettingsObject->SetBoolField(TEXT("actor_edits_enabled"), Settings && Settings->bAllowActorEdits);
		SettingsObject->SetBoolField(TEXT("content_edits_enabled"), Settings && Settings->bAllowContentEdits);
		SettingsObject->SetBoolField(TEXT("material_edits_enabled"), Settings && Settings->bAllowMaterialEdits);
		SettingsObject->SetBoolField(TEXT("umg_edits_enabled"), Settings && Settings->bAllowUMGEdits);
		SettingsObject->SetBoolField(TEXT("input_edits_enabled"), Settings && Settings->bAllowInputEdits);
		SettingsObject->SetBoolField(TEXT("saving_enabled"), Settings && Settings->bAllowSavingPackages);
		SettingsObject->SetNumberField(TEXT("max_operations_per_transaction"), Settings ? Settings->MaxWriteOperationsPerTransaction : 0);
		SettingsObject->SetNumberField(TEXT("max_assets_per_transaction"), Settings ? Settings->MaxWriteAssetsPerTransaction : 0);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.bridge-edit-discover.v1"));
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
		if (!Settings->bAllowBlueprintEdits)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_BLUEPRINT_DISABLED"), TEXT("Blueprint write alpha is disabled in UEPI Project Settings."));
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

		TArray<TSharedPtr<FJsonValue>> OperationResults;
		TArray<TSharedPtr<FJsonValue>> CompileResults;
		bool bAllOk = true;
		bool bValidationOk = true;
		bool bMutated = false;
		FString FailureMessage;
		TSet<UBlueprint*> TouchedBlueprints;

		const FText TransactionText = FText::FromString(TransactionId.IsEmpty() ? FString(TEXT("UEPI edit transaction")) : FString::Printf(TEXT("UEPI edit %s"), *TransactionId));
		FScopedTransaction Transaction(TEXT("UEProjectIntelligence"), TransactionText, nullptr, true);
		for (int32 Index = 0; Index < Operations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Operation = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
			const FString Type = JsonString(Operation, TEXT("type"), JsonString(Operation, TEXT("operation")));
			TSharedPtr<FJsonObject> OpParams = JsonObjectField(Operation, TEXT("params"));
			if (!OpParams.IsValid())
			{
				OpParams = Operation;
			}

			if (!Operation.IsValid() || Type.IsEmpty())
			{
				bAllOk = false;
				FailureMessage = TEXT("Operation is not a JSON object with a type.");
				AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
				break;
			}

			FString AssetPath;
			FString Error;
			UBlueprint* Blueprint = nullptr;
			if (Type.StartsWith(TEXT("blueprint.")))
			{
				Blueprint = LoadBlueprintForEdit(OpParams, AssetPath, Error);
				if (!Blueprint)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				AffectedAssets.AddUnique(AssetPath);
			}

			if (Type == TEXT("blueprint.add_variable"))
			{
				const FString VariableNameText = JsonString(OpParams, TEXT("name"));
				if (VariableNameText.IsEmpty())
				{
					bAllOk = false;
					FailureMessage = TEXT("blueprint.add_variable requires params.name.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				FEdGraphPinType PinType;
				if (!BuildPinType(JsonString(OpParams, TEXT("pin_type"), JsonString(OpParams, TEXT("type_name"), TEXT("float"))), PinType, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				const FName VariableName(*VariableNameText);
				if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName) != INDEX_NONE)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Blueprint variable already exists: %s"), *VariableNameText);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString DefaultValue = JsonString(OpParams, TEXT("default_value"), JsonString(OpParams, TEXT("default")));
				const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, PinType, DefaultValue);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("variable"), VariableNameText);
				Detail->SetStringField(TEXT("pin_category"), PinType.PinCategory.ToString());
				Detail->SetStringField(TEXT("default_value"), DefaultValue);
				AddOperationResult(OperationResults, Index, Type, bAdded, bAdded ? TEXT("Variable added.") : TEXT("FBlueprintEditorUtils::AddMemberVariable failed."), Detail);
				bAllOk &= bAdded;
				if (!bAdded)
				{
					FailureMessage = TEXT("Failed to add Blueprint variable.");
					break;
				}
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.set_variable_default"))
			{
				const FString VariableNameText = JsonString(OpParams, TEXT("name"), JsonString(OpParams, TEXT("variable")));
				const FName VariableName(*VariableNameText);
				const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName);
				if (VariableIndex == INDEX_NONE)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Blueprint variable not found: %s"), *VariableNameText);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString DefaultValue = JsonString(OpParams, TEXT("default_value"), JsonString(OpParams, TEXT("value")));
				Blueprint->Modify();
				Blueprint->NewVariables[VariableIndex].DefaultValue = DefaultValue;
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("variable"), VariableNameText);
				Detail->SetStringField(TEXT("default_value"), DefaultValue);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Variable default updated."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.add_component"))
			{
				const FString ComponentNameText = JsonString(OpParams, TEXT("name"), TEXT("UEPIComponent"));
				UClass* ComponentClass = ResolveComponentClass(JsonString(OpParams, TEXT("component_class")));
				if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
				{
					bAllOk = false;
					FailureMessage = TEXT("blueprint.add_component requires a component_class derived from UActorComponent.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				if (!Blueprint->SimpleConstructionScript)
				{
					bAllOk = false;
					FailureMessage = TEXT("Blueprint has no SimpleConstructionScript.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FName ComponentName(*ComponentNameText);
				if (Blueprint->SimpleConstructionScript->FindSCSNode(ComponentName))
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Component already exists: %s"), *ComponentNameText);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				Blueprint->Modify();
				Blueprint->SimpleConstructionScript->Modify();
				USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(ComponentClass, ComponentName);
				Blueprint->SimpleConstructionScript->AddNode(Node);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("component"), ComponentNameText);
				Detail->SetStringField(TEXT("component_class"), ComponentClass->GetPathName());
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Component added."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.set_component_property"))
			{
				const FString ComponentNameText = JsonString(OpParams, TEXT("component"), JsonString(OpParams, TEXT("component_name")));
				const FString PropertyName = JsonString(OpParams, TEXT("property"));
				if (!Blueprint->SimpleConstructionScript || ComponentNameText.IsEmpty() || PropertyName.IsEmpty())
				{
					bAllOk = false;
					FailureMessage = TEXT("blueprint.set_component_property requires component and property.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(FName(*ComponentNameText));
				UObject* Template = Node ? Node->ComponentTemplate : nullptr;
				if (!Template)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Component template not found: %s"), *ComponentNameText);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				Template->Modify();
				if (!SetSimplePropertyValue(Template, PropertyName, OpParams, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("component"), ComponentNameText);
				Detail->SetStringField(TEXT("property"), PropertyName);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Component property updated."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.compile"))
			{
				TSharedRef<FJsonObject> CompileResult = CompileBlueprintToJson(Blueprint);
				CompileResults.Add(MakeShared<FJsonValueObject>(CompileResult));
				const bool bCompileOk = CompileResult->GetBoolField(TEXT("ok"));
				AddOperationResult(OperationResults, Index, Type, bCompileOk, bCompileOk ? TEXT("Blueprint compiled.") : TEXT("Blueprint compile returned errors."), CompileResult);
				bValidationOk &= bCompileOk;
				if (!bCompileOk)
				{
					FailureMessage = TEXT("Blueprint compile returned errors.");
					break;
				}
			}
			else
			{
				bAllOk = false;
				FailureMessage = FString::Printf(TEXT("Operation is not implemented in Blueprint write alpha: %s"), *Type);
				AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
				break;
			}
		}

		if (!bAllOk)
		{
			Transaction.Cancel();
		}

		if (bAllOk)
		{
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

		FString RefreshRequestPath;
		FString RefreshError;
		if (AffectedAssets.Num() > Settings->MaxWriteAssetsPerTransaction)
		{
			bValidationOk = false;
			FailureMessage = FString::Printf(TEXT("Affected asset count %d exceeds MaxWriteAssetsPerTransaction=%d."), AffectedAssets.Num(), Settings->MaxWriteAssetsPerTransaction);
		}
		else if (AffectedAssets.Num() > 0)
		{
			WriteRefreshRequest(AffectedAssets, TEXT("live"), RefreshRequestPath, RefreshError);
		}

		if (bMutated)
		{
			LastAppliedTransactionId = TransactionId;
			LastAppliedSummary = FString::Printf(TEXT("%d operation(s), %d asset(s)"), Operations->Num(), AffectedAssets.Num());
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.bridge-edit-apply.v1"));
		Result->SetStringField(TEXT("transaction_id"), TransactionId);
		Result->SetBoolField(TEXT("applied"), bAllOk);
		Result->SetBoolField(TEXT("validation_ok"), bValidationOk);
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetStringField(TEXT("failure_message"), FailureMessage);
		Result->SetArrayField(TEXT("affected_assets"), StringArrayToJsonValues(AffectedAssets));
		Result->SetArrayField(TEXT("operations"), OperationResults);
		Result->SetArrayField(TEXT("compile"), CompileResults);
		Result->SetStringField(TEXT("refresh_request_path"), RefreshRequestPath);
		Result->SetStringField(TEXT("refresh_error"), RefreshError);
		Result->SetStringField(TEXT("rollback_strategy"), TEXT("editor_transaction_undo"));

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

		TArray<TSharedPtr<FJsonValue>> CompileResults;
		bool bAllOk = true;
		for (const FString& AssetPath : Assets)
		{
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
			if (!Blueprint)
			{
				TSharedRef<FJsonObject> Missing = MakeShared<FJsonObject>();
				Missing->SetBoolField(TEXT("ok"), false);
				Missing->SetStringField(TEXT("asset"), AssetPath);
				Missing->SetStringField(TEXT("error"), TEXT("Failed to load Blueprint."));
				CompileResults.Add(MakeShared<FJsonValueObject>(Missing));
				bAllOk = false;
				continue;
			}
			TSharedRef<FJsonObject> CompileResult = CompileBlueprintToJson(Blueprint);
			CompileResults.Add(MakeShared<FJsonValueObject>(CompileResult));
			bAllOk &= CompileResult->GetBoolField(TEXT("ok"));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.bridge-edit-validate.v1"));
		Result->SetStringField(TEXT("transaction_id"), JsonString(Params, TEXT("transaction_id")));
		Result->SetBoolField(TEXT("validated"), bAllOk);
		Result->SetArrayField(TEXT("assets"), StringArrayToJsonValues(Assets));
		Result->SetArrayField(TEXT("compile"), CompileResults);

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), bAllOk);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), bAllOk ? EmptyJsonArray() : DiagnosticsArray(TEXT("UEPI_EDIT_VALIDATE_FAILED"), TEXT("error"), TEXT("One or more Blueprint validations failed.")));
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
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.bridge-edit-rollback.v1"));
		Result->SetStringField(TEXT("transaction_id"), TransactionId);
		Result->SetStringField(TEXT("summary"), LastAppliedSummary);
		Result->SetBoolField(TEXT("undone"), bUndone);
		if (bUndone)
		{
			LastAppliedTransactionId.Reset();
			LastAppliedSummary.Reset();
		}
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), bUndone);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), bUndone ? EmptyJsonArray() : DiagnosticsArray(TEXT("UEPI_EDIT_ROLLBACK_FAILED"), TEXT("error"), TEXT("GEditor undo transaction failed.")));
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::MakeSessionObject(const FString& State) const
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema_version"), FUEPIBridgeProtocol::SessionSchemaVersion());
		Root->SetBoolField(TEXT("active"), State.Equals(TEXT("active"), ESearchCase::IgnoreCase));
		Root->SetStringField(TEXT("state"), State);
		Root->SetStringField(TEXT("session_id"), SessionId);
		Root->SetStringField(TEXT("host"), TEXT("127.0.0.1"));
		Root->SetNumberField(TEXT("port"), Port);
		Root->SetStringField(TEXT("protocol"), FUEPIBridgeProtocol::ProtocolName());
		Root->SetBoolField(TEXT("transport_ready"), Listener.IsValid() && Listener->IsActive());
		Root->SetStringField(TEXT("implementation"), TEXT("tcp_length_prefixed_json"));
		Root->SetStringField(TEXT("project_name"), FApp::GetProjectName());
		Root->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		Root->SetStringField(TEXT("token_path"), TokenPath);
		Root->SetStringField(TEXT("token_hash"), FMD5::HashAnsiString(*Token));
		Root->SetStringField(TEXT("started_at"), FDateTime::UtcNow().ToIso8601());
		Root->SetStringField(TEXT("last_heartbeat"), FDateTime::UtcNow().ToIso8601());
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

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SessionPath), true);
		if (!FFileHelper::SaveStringToFile(JsonObjectToString(MakeSessionObject(State)), *SessionPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write UEPI bridge session file: %s"), *SessionPath);
			return false;
		}
		OutError.Reset();
		return true;
	}
}
