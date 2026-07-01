#include "Bridge/UEPIBridgeProtocol.h"

namespace UE::ProjectIntelligence
{
	const TCHAR* FUEPIBridgeProtocol::ProtocolName()
	{
		return TEXT("uepi-bridge-v1");
	}

	const TCHAR* FUEPIBridgeProtocol::SessionSchemaVersion()
	{
		return TEXT("uepi.editor-bridge-session.v1");
	}

	TArray<FString> FUEPIBridgeProtocol::ReadCapabilities()
	{
		return {
			TEXT("editor.get_status"),
			TEXT("editor.get_selection"),
			TEXT("editor.capture_viewport"),
			TEXT("editor.read_output_log"),
			TEXT("asset.refresh_now")
		};
	}
}
