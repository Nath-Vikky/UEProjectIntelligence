#include "Edit/UEPIEditOperationRegistry.h"

#include "Common/UEPIHash.h"
#include "Operations/UEPIActorOperations.h"
#include "Operations/UEPIAnimationOperations.h"
#include "Operations/UEPIBlueprintOperations.h"
#include "Operations/UEPIContentOperations.h"
#include "Operations/UEPIInputOperations.h"
#include "Operations/UEPIMaterialOperations.h"
#include "Operations/UEPIWidgetOperations.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::ProjectIntelligence
{
	namespace
	{
		TSharedPtr<FJsonObject> OperationContracts()
		{
			static TSharedPtr<FJsonObject> Contracts;
			static bool bAttempted = false;
			if (bAttempted)
			{
				return Contracts;
			}
			bAttempted = true;
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEProjectIntelligence"));
			if (!Plugin.IsValid())
			{
				return nullptr;
			}
			FString Json;
			const FString Path = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Schemas"), TEXT("edit-operation-contracts.json"));
			if (!FFileHelper::LoadFileToString(Json, *Path))
			{
				return nullptr;
			}
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				return nullptr;
			}
			const TSharedPtr<FJsonObject>* Operations = nullptr;
			if (!Root->TryGetObjectField(TEXT("operations"), Operations) || !Operations || !Operations->IsValid())
			{
				return nullptr;
			}
			Contracts = *Operations;
			return Contracts;
		}

		void AttachMachineContract(FUEPIEditOperationDescriptor& Descriptor)
		{
			const TSharedPtr<FJsonObject> Contracts = OperationContracts();
			const TSharedPtr<FJsonObject>* Contract = nullptr;
			if (!Contracts.IsValid() || !Contracts->TryGetObjectField(Descriptor.Name, Contract) || !Contract || !Contract->IsValid())
			{
				return;
			}
			const TSharedPtr<FJsonObject>* InputSchema = nullptr;
			if ((*Contract)->TryGetObjectField(TEXT("input_schema"), InputSchema) && InputSchema && InputSchema->IsValid())
			{
				Descriptor.InputSchema = *InputSchema;
			}
			const TArray<TSharedPtr<FJsonValue>>* Examples = nullptr;
			if ((*Contract)->TryGetArrayField(TEXT("examples"), Examples) && Examples)
			{
				Descriptor.Examples = *Examples;
			}
			(*Contract)->TryGetStringField(TEXT("contract_hash"), Descriptor.ContractHash);
		}

		FUEPIEditOperationDescriptor Descriptor(
			const TCHAR* Name,
			const TCHAR* Domain,
			const TCHAR* Risk,
			std::initializer_list<const TCHAR*> TargetFields,
			const TCHAR* Validation = TEXT("generic_uobject"),
			std::initializer_list<const TCHAR*> DependencyFields = {})
		{
			FUEPIEditOperationDescriptor Value;
			Value.Name = Name;
			Value.Domain = Domain;
			Value.Summary = FString::Printf(TEXT("Guarded %s operation executed by the exact-project Unreal Editor."), Name);
			Value.Risk = Risk;
			Value.ValidationMode = Validation;
			Value.RequiredCapabilities = { TEXT("exact_project_editor"), TEXT("immutable_plan_v2"), TEXT("explicit_user_approval") };
			if (Value.Domain == TEXT("blueprint")) Value.SupportedGraphSchemas = { TEXT("K2") };
			if (Value.Domain == TEXT("animgraph")) Value.SupportedGraphSchemas = { TEXT("AnimationGraph") };
			if (Value.Domain == TEXT("input")) Value.RequiredPlugin = TEXT("EnhancedInput");
			for (const TCHAR* Field : TargetFields)
			{
				Value.TargetFields.Add(Field);
			}
			for (const TCHAR* Field : DependencyFields)
			{
				Value.DependencyFields.Add(Field);
			}
			AttachMachineContract(Value);
			return Value;
		}
	}
	FUEPIEditOperationRegistry& FUEPIEditOperationRegistry::Get()
	{
		static FUEPIEditOperationRegistry Registry;
		return Registry;
	}

	bool FUEPIEditOperationRegistry::RegisterOperation(TSharedRef<IUEPIEditOperation> Operation)
	{
		const FString OperationType = Operation->GetOperationType();
		if (OperationType.IsEmpty() || OperationsByType.Contains(OperationType))
		{
			return false;
		}
		OperationsByType.Add(OperationType, Operation);
		return true;
	}

	TSharedPtr<IUEPIEditOperation> FUEPIEditOperationRegistry::FindOperation(const FString& OperationType) const
	{
		if (const TSharedPtr<IUEPIEditOperation>* Operation = OperationsByType.Find(OperationType))
		{
			return *Operation;
		}
		return nullptr;
	}

	TArray<FString> FUEPIEditOperationRegistry::GetOperationTypes() const
	{
		TArray<FString> Types;
		OperationsByType.GetKeys(Types);
		Types.Sort();
		return Types;
	}

	TArray<FUEPIEditOperationDescriptor> FUEPIEditOperationRegistry::GetDescriptors() const
	{
		TArray<FUEPIEditOperationDescriptor> Descriptors;
		for (const TPair<FString, TSharedPtr<IUEPIEditOperation>>& Pair : OperationsByType)
		{
			Descriptors.Add(Pair.Value->GetDescriptor());
		}
		Descriptors.Sort([](const FUEPIEditOperationDescriptor& A, const FUEPIEditOperationDescriptor& B) { return A.Name < B.Name; });
		return Descriptors;
	}

	FString FUEPIEditOperationRegistry::GetCatalogHash() const
	{
		FString Canonical;
		for (const FUEPIEditOperationDescriptor& Item : GetDescriptors())
		{
			Canonical += FString::Printf(
				TEXT("%s|%d|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%d|%d\n"),
				*Item.Name,
				Item.Version,
				*Item.Domain,
				*Item.Summary,
				*Item.Risk,
				*FString::Join(Item.TargetFields, TEXT(",")),
				*FString::Join(Item.DependencyFields, TEXT(",")),
				*FString::Join(Item.RequiredCapabilities, TEXT(",")),
				*FString::Join(Item.SupportedEngineVersions, TEXT(",")),
				*FString::Join(Item.SupportedAssetClasses, TEXT(",")),
				*FString::Join(Item.SupportedGraphSchemas, TEXT(",")),
				*Item.RequiredPlugin,
				*Item.SaveBehavior,
				*Item.RollbackMode,
				Item.bRequiresSave ? 1 : 0,
				Item.bAtomicSupported ? 1 : 0);
			Canonical += FString::Printf(TEXT("validation=%s|idempotency=%s|contract=%s\n"), *Item.ValidationMode, *Item.IdempotencyBehavior, *Item.ContractHash);
		}
		FTCHARToUTF8 Utf8(*Canonical);
		const FString Digest = Sha256Hex(Utf8.Get(), static_cast<uint64>(Utf8.Length()));
		return Digest.IsEmpty() ? FString() : TEXT("sha256:") + Digest;
	}

	void FUEPIEditOperationRegistry::EnsureBuiltinsRegistered()
	{
		if (OperationsByType.Num() > 0)
		{
			return;
		}
		const TArray<FUEPIEditOperationDescriptor> Builtins = {
			Descriptor(TEXT("blueprint.add_variable"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.set_variable_default"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.add_component"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.set_component_property"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.set_component_properties"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.create_function"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.add_event_node"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.add_function_call_node"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.add_variable_get_node"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.add_variable_set_node"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.add_branch_node"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.add_print_string_node"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.connect_pins"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.compile"), TEXT("blueprint"), TEXT("low"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.add_node"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.set_pin_default"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.disconnect_pins"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.break_all_links"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.remove_node"), TEXT("blueprint"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.move_node"), TEXT("blueprint"), TEXT("low"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("blueprint.set_node_comment"), TEXT("blueprint"), TEXT("low"), { TEXT("asset") }, TEXT("blueprint")),
			Descriptor(TEXT("animgraph.add_slot_node"), TEXT("animgraph"), TEXT("medium"), { TEXT("asset") }, TEXT("anim_blueprint")),
			Descriptor(TEXT("animgraph.add_slot"), TEXT("animgraph"), TEXT("medium"), { TEXT("asset") }, TEXT("anim_blueprint")),
			Descriptor(TEXT("animgraph.set_node_property"), TEXT("animgraph"), TEXT("medium"), { TEXT("asset") }, TEXT("anim_blueprint")),
			Descriptor(TEXT("animgraph.connect_pose"), TEXT("animgraph"), TEXT("medium"), { TEXT("asset") }, TEXT("anim_blueprint")),
			Descriptor(TEXT("animgraph.connect_pose_pins"), TEXT("animgraph"), TEXT("medium"), { TEXT("asset") }, TEXT("anim_blueprint")),
			Descriptor(TEXT("animgraph.disconnect_pose_pins"), TEXT("animgraph"), TEXT("medium"), { TEXT("asset") }, TEXT("anim_blueprint")),
			Descriptor(TEXT("animgraph.remove_node"), TEXT("animgraph"), TEXT("medium"), { TEXT("asset") }, TEXT("anim_blueprint")),
			Descriptor(TEXT("animgraph.compile"), TEXT("animgraph"), TEXT("low"), { TEXT("asset") }, TEXT("anim_blueprint")),
			Descriptor(TEXT("animation.register_slot"), TEXT("animation"), TEXT("medium"), { TEXT("skeleton") }, TEXT("animation")),
			Descriptor(TEXT("animation.create_slot_group"), TEXT("animation"), TEXT("medium"), { TEXT("skeleton") }, TEXT("animation")),
			Descriptor(TEXT("animation.create_montage_from_sequence"), TEXT("animation"), TEXT("medium"), { TEXT("destination_asset") }, TEXT("animation"), { TEXT("sequence") }),
			Descriptor(TEXT("animation.add_montage_slot_track"), TEXT("animation"), TEXT("medium"), { TEXT("asset") }, TEXT("animation")),
			Descriptor(TEXT("animation.add_montage_segment"), TEXT("animation"), TEXT("medium"), { TEXT("asset") }, TEXT("animation"), { TEXT("sequence") }),
			Descriptor(TEXT("animation.add_montage_section"), TEXT("animation"), TEXT("medium"), { TEXT("asset") }, TEXT("animation")),
			Descriptor(TEXT("animation.set_montage_blend"), TEXT("animation"), TEXT("low"), { TEXT("asset") }, TEXT("animation")),
			Descriptor(TEXT("animation.set_preview_mesh"), TEXT("animation"), TEXT("low"), { TEXT("asset") }, TEXT("animation"), { TEXT("preview_mesh") }),
			Descriptor(TEXT("content.save_assets"), TEXT("content"), TEXT("low"), { TEXT("assets") }),
			Descriptor(TEXT("actor.spawn"), TEXT("actor"), TEXT("medium"), { TEXT("level") }, TEXT("world")),
			Descriptor(TEXT("actor.set_transform"), TEXT("actor"), TEXT("medium"), { TEXT("actor") }, TEXT("world")),
			Descriptor(TEXT("actor.set_property"), TEXT("actor"), TEXT("medium"), { TEXT("actor") }, TEXT("world")),
			Descriptor(TEXT("actor.set_properties"), TEXT("actor"), TEXT("medium"), { TEXT("actor") }, TEXT("world")),
			Descriptor(TEXT("material.create_instance"), TEXT("material"), TEXT("medium"), { TEXT("destination_asset") }, TEXT("material"), { TEXT("parent") }),
			Descriptor(TEXT("material.set_scalar_parameter"), TEXT("material"), TEXT("medium"), { TEXT("asset") }, TEXT("material")),
			Descriptor(TEXT("material.set_vector_parameter"), TEXT("material"), TEXT("medium"), { TEXT("asset") }, TEXT("material")),
			Descriptor(TEXT("material.set_texture_parameter"), TEXT("material"), TEXT("medium"), { TEXT("asset") }, TEXT("material"), { TEXT("texture") }),
			Descriptor(TEXT("material.apply_to_actor"), TEXT("material"), TEXT("medium"), { TEXT("actor") }, TEXT("world"), { TEXT("material") }),
			Descriptor(TEXT("material.apply_to_blueprint_component"), TEXT("material"), TEXT("medium"), { TEXT("asset") }, TEXT("blueprint"), { TEXT("material") }),
			Descriptor(TEXT("content.import"), TEXT("content"), TEXT("medium"), { TEXT("destination_asset") }),
			Descriptor(TEXT("content.create_folder"), TEXT("content"), TEXT("low"), { TEXT("folder") }),
			Descriptor(TEXT("content.duplicate_asset"), TEXT("content"), TEXT("medium"), { TEXT("destination_asset") }, TEXT("generic_uobject"), { TEXT("source") }),
			Descriptor(TEXT("content.rename_asset"), TEXT("content"), TEXT("high"), { TEXT("asset"), TEXT("destination_asset") }),
			Descriptor(TEXT("content.create_asset"), TEXT("content"), TEXT("medium"), { TEXT("destination_asset") }, TEXT("data_asset")),
			Descriptor(TEXT("asset.set_properties"), TEXT("asset"), TEXT("medium"), { TEXT("asset") }, TEXT("generic_uobject")),
			Descriptor(TEXT("widget.create"), TEXT("umg"), TEXT("medium"), { TEXT("destination_asset") }, TEXT("widget")),
			Descriptor(TEXT("widget.add_text"), TEXT("umg"), TEXT("medium"), { TEXT("asset") }, TEXT("widget")),
			Descriptor(TEXT("widget.add_button"), TEXT("umg"), TEXT("medium"), { TEXT("asset") }, TEXT("widget")),
			Descriptor(TEXT("widget.add_widget"), TEXT("umg"), TEXT("medium"), { TEXT("asset") }, TEXT("widget")),
			Descriptor(TEXT("widget.set_slot"), TEXT("umg"), TEXT("medium"), { TEXT("asset") }, TEXT("widget")),
			Descriptor(TEXT("widget.bind_button_to_custom_event"), TEXT("umg"), TEXT("medium"), { TEXT("asset") }, TEXT("widget")),
			Descriptor(TEXT("input.create_action"), TEXT("input"), TEXT("medium"), { TEXT("destination_asset") }, TEXT("input")),
			Descriptor(TEXT("input.create_mapping_context"), TEXT("input"), TEXT("medium"), { TEXT("destination_asset") }, TEXT("input")),
			Descriptor(TEXT("input.add_key_mapping"), TEXT("input"), TEXT("medium"), { TEXT("context") }, TEXT("input"), { TEXT("action") }),
			Descriptor(TEXT("input.remove_key_mapping"), TEXT("input"), TEXT("medium"), { TEXT("context") }, TEXT("input"), { TEXT("action") })
		};
		for (const FUEPIEditOperationDescriptor& Item : Builtins)
		{
			if (Item.Domain == TEXT("actor")) RegisterOperation(MakeUEPIActorOperation(Item));
			else if (Item.Domain == TEXT("animation")) RegisterOperation(MakeUEPIAnimationOperation(Item));
			else if (Item.Domain == TEXT("blueprint") || Item.Domain == TEXT("animgraph")) RegisterOperation(MakeUEPIBlueprintOperation(Item));
			else if (Item.Domain == TEXT("input")) RegisterOperation(MakeUEPIInputOperation(Item));
			else if (Item.Domain == TEXT("material")) RegisterOperation(MakeUEPIMaterialOperation(Item));
			else if (Item.Domain == TEXT("content") || Item.Name == TEXT("asset.set_properties")) RegisterOperation(MakeUEPIContentOperation(Item));
			else if (Item.Domain == TEXT("umg")) RegisterOperation(MakeUEPIWidgetOperation(Item));
		}
	}

	void FUEPIEditOperationRegistry::Reset()
	{
		OperationsByType.Reset();
	}
}
