#pragma once

#include "CoreMinimal.h"

namespace UE::ProjectIntelligence
{
	struct FUEPIBridgeProtocol
	{
		static const TCHAR* ProtocolName();
		static const TCHAR* SessionSchemaVersion();
		static TArray<FString> ReadCapabilities();
		static TArray<FString> WriteCapabilities();
	};
}
