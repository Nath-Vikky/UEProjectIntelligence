#include "Bridge/UEPIEditorCommandBridge.h"

#include "Bridge/UEPIBridgeProtocol.h"
#include "Common/TcpListener.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UEPISnapshotStore.h"

namespace UE::ProjectIntelligence
{
	namespace
	{
		FString UEPISessionsDirectory()
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("sessions"));
		}

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
			const int32 LineLimit = Params.IsValid() ? static_cast<int32>(Params->GetNumberField(TEXT("line_limit"))) : 100;
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
		Result->SetArrayField(TEXT("capabilities"), StringArrayToJsonValues(FUEPIBridgeProtocol::ReadCapabilities()));

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
		Request->SetStringField(TEXT("reason"), TEXT("bridge_asset_refresh_now"));
		Request->SetStringField(TEXT("tool_name"), TEXT("uepi_bridge"));

		if (!SaveJsonObject(Request, RequestPath))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_BRIDGE_REFRESH_REQUEST_FAILED"), FString::Printf(TEXT("Failed to write refresh request: %s"), *RequestPath));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("request_path"), RequestPath);
		Result->SetStringField(TEXT("data_mode"), DataMode);
		Result->SetArrayField(TEXT("target_object_paths"), StringArrayToJsonValues(Targets));

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), true);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
		return Response;
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
		Root->SetArrayField(TEXT("capabilities"), StringArrayToJsonValues(FUEPIBridgeProtocol::ReadCapabilities()));
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
