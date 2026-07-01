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
			TEXT("blueprint.create_function"),
			TEXT("blueprint.add_event_node"),
			TEXT("blueprint.add_function_call_node"),
			TEXT("blueprint.add_variable_get_node"),
			TEXT("blueprint.add_variable_set_node"),
			TEXT("blueprint.add_branch_node"),
			TEXT("blueprint.add_print_string_node"),
			TEXT("blueprint.connect_pins"),
			TEXT("blueprint.compile"),
			TEXT("actor.set_transform"),
			TEXT("actor.set_property"),
			TEXT("material.set_scalar_parameter"),
			TEXT("material.set_vector_parameter"),
			TEXT("material.set_texture_parameter"),
			TEXT("content.import"),
			TEXT("content.create_folder"),
			TEXT("content.duplicate_asset"),
			TEXT("content.rename_asset"),
			TEXT("widget.create"),
			TEXT("widget.add_text"),
			TEXT("widget.add_button"),
			TEXT("input.create_action"),
			TEXT("input.create_mapping_context"),
			TEXT("input.add_key_mapping")
		};
	}
}
