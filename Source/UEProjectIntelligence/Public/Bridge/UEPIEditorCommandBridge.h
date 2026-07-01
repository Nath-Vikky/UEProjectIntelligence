#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FJsonObject;

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
		TSharedRef<FJsonObject> MakeSessionObject(const FString& State) const;
		bool WriteSessionObject(const FString& State, FString& OutError) const;

		FString SessionId;
		FString SessionPath;
		FString TokenPath;
		FString Token;
		int32 Port = 0;
		bool bActive = false;
	};
}
