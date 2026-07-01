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

	TArray<FString> FUEPIBridgeProtocol::WriteCapabilities()
	{
		return {
			TEXT("edit.discover"),
			TEXT("edit.apply"),
			TEXT("edit.validate"),
			TEXT("edit.rollback"),
			TEXT("blueprint.add_variable"),
			TEXT("blueprint.set_variable_default"),
			TEXT("blueprint.add_component"),
			TEXT("blueprint.set_component_property"),
			TEXT("blueprint.compile")
		};
	}
}
