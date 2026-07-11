#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Common/TcpListener.h"
#include "Templates/SharedPointer.h"

class FJsonObject;
class FSocket;
struct FIPv4Endpoint;

namespace UE::ProjectIntelligence
{
	class FUEPIEditorCommandBridge
	{
	public:
		bool Start(const FString& InSessionId, int32 InRequestedPort, FString& OutError);
		void Stop();
		void TickHeartbeat();

		bool IsActive() const;
		const FString& GetSessionPath() const;
		const FString& GetTokenPath() const;
		int32 GetPort() const;

	private:
		bool HandleConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& RemoteEndpoint);
		void ProcessPendingSockets();
		bool ProcessSocket(FSocket* ClientSocket);
		bool ReadFrame(FSocket* ClientSocket, FString& OutJsonText) const;
		bool WriteFrame(FSocket* ClientSocket, const TSharedRef<FJsonObject>& Response) const;
		TSharedRef<FJsonObject> HandleRequest(const TSharedPtr<FJsonObject>& Request);
		TSharedRef<FJsonObject> ErrorResponse(const FString& RequestId, const FString& Code, const FString& Message) const;
		TSharedRef<FJsonObject> StatusResult(const FString& RequestId) const;
		TSharedRef<FJsonObject> SelectionResult(const FString& RequestId) const;
		TSharedRef<FJsonObject> OutputLogResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params) const;
		TSharedRef<FJsonObject> WorldReadResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params) const;
		TSharedRef<FJsonObject> SchemaResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params) const;
		TSharedRef<FJsonObject> RuntimeResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params);
		TSharedRef<FJsonObject> RefreshNowResult(const FString& RequestId, const TArray<FString>& Targets, const FString& DataMode) const;
		TSharedRef<FJsonObject> ViewportCaptureUnsupported(const FString& RequestId) const;
		TSharedRef<FJsonObject> EditDiscoverResult(const FString& RequestId) const;
		TSharedRef<FJsonObject> EditApplyResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params);
		TSharedRef<FJsonObject> EditValidateResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params) const;
		TSharedRef<FJsonObject> EditRollbackResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params);
		TSharedRef<FJsonObject> MakeSessionObject(const FString& State) const;
		bool WriteSessionObject(const FString& State, FString& OutError) const;

		FString SessionId;
		FString SessionPath;
		FString TokenPath;
		FString Token;
		FString LastAppliedTransactionId;
		FString LastAppliedSummary;
		TMap<FString, FString> LastAppliedBackupFiles;
		TArray<FString> LastAppliedAffectedAssets;
		bool bLastAppliedSaved = false;
		FString OwnedRuntimeSessionId;
		bool bOwnsPIESession = false;
		int32 Port = 0;
		bool bActive = false;
		TUniquePtr<FTcpListener> Listener;
		TQueue<FSocket*, EQueueMode::Mpsc> PendingSockets;
	};
}
