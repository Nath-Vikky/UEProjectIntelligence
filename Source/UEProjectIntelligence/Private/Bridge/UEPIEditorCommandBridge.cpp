#include "Bridge/UEPIEditorCommandBridge.h"

#include "Bridge/UEPIBridgeProtocol.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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
	}

	bool FUEPIEditorCommandBridge::Start(const FString& InSessionId, int32 InRequestedPort, FString& OutError)
	{
		SessionId = InSessionId;
		SessionPath = UEPIBridgeSessionPath();
		TokenPath = UEPIBridgeTokenPath();
		Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Port = FMath::Clamp(InRequestedPort, 0, 65535);
		bActive = true;

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(TokenPath), true);
		if (!FFileHelper::SaveStringToFile(Token, *TokenPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			bActive = false;
			OutError = FString::Printf(TEXT("Failed to write UEPI bridge token file: %s"), *TokenPath);
			return false;
		}

		return WriteSessionObject(TEXT("active"), OutError);
	}

	void FUEPIEditorCommandBridge::Stop()
	{
		if (bActive)
		{
			FString Error;
			WriteSessionObject(TEXT("stopped"), Error);
		}
		bActive = false;
		Token.Reset();
	}

	void FUEPIEditorCommandBridge::TickHeartbeat()
	{
		if (!bActive)
		{
			return;
		}
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
		Root->SetBoolField(TEXT("transport_ready"), false);
		Root->SetStringField(TEXT("implementation"), TEXT("session_file_skeleton"));
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
