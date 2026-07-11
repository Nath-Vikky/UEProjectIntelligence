#include "Bridge/UEPIBridgeProtocol.h"
#include "Edit/UEPIEditOperationRegistry.h"

namespace UE::ProjectIntelligence
{
	const TCHAR* FUEPIBridgeProtocol::ProtocolName()
	{
		return TEXT("uepi-bridge-v2");
	}

	const TCHAR* FUEPIBridgeProtocol::SessionSchemaVersion()
	{
		return TEXT("uepi.editor-bridge-session.v2");
	}

	TArray<FString> FUEPIBridgeProtocol::ReadCapabilities()
	{
		return {
			TEXT("editor.get_status"),
			TEXT("editor.get_selection"),
			TEXT("editor.capture_viewport"),
			TEXT("editor.read_output_log"),
			TEXT("editor.read_world"),
			TEXT("schema.get"),
			TEXT("runtime.control"),
			TEXT("asset.refresh_now")
		};
	}

	TArray<FString> FUEPIBridgeProtocol::WriteCapabilities()
	{
		TArray<FString> Capabilities = {
			TEXT("edit.discover"),
			TEXT("edit.apply"),
			TEXT("edit.validate"),
			TEXT("edit.rollback")
		};
		FUEPIEditOperationRegistry& Registry = FUEPIEditOperationRegistry::Get();
		Registry.EnsureBuiltinsRegistered();
		Capabilities.Append(Registry.GetOperationTypes());
		return Capabilities;
	}
}
