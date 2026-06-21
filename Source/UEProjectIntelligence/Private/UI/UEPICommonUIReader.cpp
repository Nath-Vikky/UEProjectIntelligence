#include "UEPICommonUIReader.h"

#include "CommonInputBaseTypes.h"
#include "CommonInputTypeEnum.h"
#include "CommonUITypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Input/CommonGenericInputActionDataTable.h"
#include "InputAction.h"
#include "Styling/SlateBrush.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString CommonUIBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString CommonInputTypeString(ECommonInputType InputType)
{
	switch (InputType)
	{
	case ECommonInputType::MouseAndKeyboard:
		return TEXT("mouse_and_keyboard");
	case ECommonInputType::Gamepad:
		return TEXT("gamepad");
	case ECommonInputType::Touch:
		return TEXT("touch");
	default:
		return TEXT("unknown");
	}
}

FString InputActionStateString(EInputActionState State)
{
	switch (State)
	{
	case EInputActionState::Enabled:
		return TEXT("enabled");
	case EInputActionState::Disabled:
		return TEXT("disabled");
	case EInputActionState::Hidden:
		return TEXT("hidden");
	case EInputActionState::HiddenAndDisabled:
		return TEXT("hidden_and_disabled");
	default:
		return TEXT("unknown");
	}
}

FString BrushResourcePath(const FSlateBrush& Brush)
{
	if (UObject* ResourceObject = Brush.GetResourceObject())
	{
		return ResourceObject->GetPathName();
	}
	return Brush.GetResourceName().ToString();
}

FString RowHandleTablePath(const FDataTableRowHandle& RowHandle)
{
	return RowHandle.DataTable ? RowHandle.DataTable->GetPathName() : FString();
}

FString SoftClassPathString(const TSoftClassPtr<UCommonUIHoldData>& ClassPtr)
{
	return ClassPtr.ToSoftObjectPath().ToString();
}

void AddCommonUIEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

void AddCommonUIRelation(
	const FString& ProjectId,
	const FString& Type,
	const FString& FromId,
	const FString& ToId,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FRelationRecord>& OutRelations,
	const TMap<FString, FString>* Attributes = nullptr)
{
	FRelationRecord Relation;
	Relation.Id = MakeRelationId(ProjectId, Type, FromId, ToId, Attributes);
	Relation.Type = Type;
	Relation.FromId = FromId;
	Relation.ToId = ToId;
	Relation.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Relation.bDerived = false;
	Relation.Confidence = 1.0f;
	if (Attributes)
	{
		Relation.Attributes = *Attributes;
	}
	Relation.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		EvidencePath,
		EvidenceDetail
	});
	OutRelations.Add(MoveTemp(Relation));
}

FEntityRecord* FindCommonUIEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

template <typename AssetType>
const AssetType* AssetOrBlueprintDefaultObject(const UObject& Asset)
{
	if (const AssetType* DirectAsset = Cast<AssetType>(&Asset))
	{
		return DirectAsset;
	}

	if (const UClass* ClassAsset = Cast<UClass>(&Asset))
	{
		if (ClassAsset->IsChildOf(AssetType::StaticClass()))
		{
			return Cast<AssetType>(ClassAsset->GetDefaultObject());
		}
	}

	if (const UBlueprint* Blueprint = Cast<UBlueprint>(&Asset))
	{
		if (const UClass* GeneratedClass = Blueprint->GeneratedClass)
		{
			if (GeneratedClass->IsChildOf(AssetType::StaticClass()))
			{
				return Cast<AssetType>(GeneratedClass->GetDefaultObject());
			}
		}
	}

	return nullptr;
}

FString DataClassPath(const UObject& Asset, const UObject& DataObject)
{
	if (const UBlueprint* Blueprint = Cast<UBlueprint>(&Asset))
	{
		return Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString();
	}
	return DataObject.GetClass() ? DataObject.GetClass()->GetPathName() : FString();
}

FString AddCommonUIInputActionEntity(
	const FString& ProjectId,
	const UInputAction* Action,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString ActionPath = Action ? Action->GetPathName() : FString();
	const FString EntityId = MakeStableId(ProjectId, TEXT("input_action"), ActionPath);
	if (FindCommonUIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("input_action");
	Entity.CanonicalKey = ActionPath;
	Entity.DisplayName = Action ? Action->GetName() : FString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("action_path"), ActionPath);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("input_action_reference") };
	Entity.Completeness.Omitted = { TEXT("runtime_input_state") };
	AddCommonUIEvidence(Entity, EvidencePath, TEXT("Input Action reference read from CommonUI input data."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddCommonUIInputActionRowEntity(
	const FString& ProjectId,
	const FString& TablePath,
	const FName RowName,
	int32 RowIndex,
	const FCommonInputActionDataBase* RowData,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString RowCanonicalKey = TablePath + TEXT(":row:") + RowName.ToString();
	const FString RowId = MakeStableId(ProjectId, TEXT("common_ui_input_action_row"), RowCanonicalKey);
	if (FindCommonUIEntity(OutEntities, RowId))
	{
		return RowId;
	}

	FEntityRecord Entity;
	Entity.Id = RowId;
	Entity.Kind = TEXT("common_ui_input_action_row");
	Entity.CanonicalKey = RowCanonicalKey;
	Entity.DisplayName = RowName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("table_path"), TablePath);
	Entity.Attributes.Add(TEXT("row_name"), RowName.ToString());
	Entity.Attributes.Add(TEXT("row_index"), FString::FromInt(RowIndex));
	if (RowData)
	{
		Entity.Attributes.Add(TEXT("display_name"), RowData->DisplayName.ToString());
		Entity.Attributes.Add(TEXT("hold_display_name"), RowData->HoldDisplayName.ToString());
		Entity.Attributes.Add(TEXT("nav_bar_priority"), FString::FromInt(RowData->NavBarPriority));
		Entity.Attributes.Add(TEXT("has_hold_bindings"), CommonUIBool(RowData->HasHoldBindings()));
	}
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("common_ui_input_action_row_metadata") };
	Entity.Completeness.Omitted = { TEXT("runtime_input_reflector_state") };
	AddCommonUIEvidence(Entity, EvidencePath, TEXT("CommonUI input action row read from a CommonUI input action data table."));
	OutEntities.Add(MoveTemp(Entity));
	return RowId;
}

FString AddCommonUIInputDataEntity(
	const FString& ProjectId,
	const UObject& Asset,
	const UCommonUIInputData& InputData,
	TArray<FEntityRecord>& OutEntities)
{
	const FString AssetPath = Asset.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("common_ui_input_data"), AssetPath);
	if (FindCommonUIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("common_ui_input_data");
	Entity.CanonicalKey = AssetPath;
	Entity.DisplayName = Asset.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("asset_path"), AssetPath);
	Entity.Attributes.Add(TEXT("data_object_path"), InputData.GetPathName());
	Entity.Attributes.Add(TEXT("data_class_path"), DataClassPath(Asset, InputData));
	Entity.Attributes.Add(TEXT("default_click_table"), RowHandleTablePath(InputData.DefaultClickAction));
	Entity.Attributes.Add(TEXT("default_click_row"), InputData.DefaultClickAction.RowName.ToString());
	Entity.Attributes.Add(TEXT("default_back_table"), RowHandleTablePath(InputData.DefaultBackAction));
	Entity.Attributes.Add(TEXT("default_back_row"), InputData.DefaultBackAction.RowName.ToString());
	Entity.Attributes.Add(TEXT("default_hold_data_class"), SoftClassPathString(InputData.DefaultHoldData));
	Entity.Attributes.Add(TEXT("enhanced_click_action"), InputData.EnhancedInputClickAction ? InputData.EnhancedInputClickAction->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("enhanced_back_action"), InputData.EnhancedInputBackAction ? InputData.EnhancedInputBackAction->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("common_ui_input_data_defaults"), TEXT("common_ui_action_row_handles"), TEXT("enhanced_input_action_references") };
	Entity.Completeness.Omitted = { TEXT("runtime_common_input_subsystem_state") };
	AddCommonUIEvidence(Entity, AssetPath, TEXT("CommonUI input data read from the asset or generated class default object."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddCommonUIHoldDataClassEntity(
	const FString& ProjectId,
	const FString& ClassPath,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString EntityId = MakeStableId(ProjectId, TEXT("common_ui_hold_data_class"), ClassPath);
	if (ClassPath.IsEmpty() || FindCommonUIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("common_ui_hold_data_class");
	Entity.CanonicalKey = ClassPath;
	Entity.DisplayName = FPackageName::ObjectPathToObjectName(ClassPath);
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("class_path"), ClassPath);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("soft_class_reference") };
	Entity.Completeness.Omitted = { TEXT("hold_timing_values") };
	AddCommonUIEvidence(Entity, EvidencePath, TEXT("CommonUI hold data soft class reference read from input data."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddCommonUIHoldDataEntity(
	const FString& ProjectId,
	const UObject& Asset,
	const UCommonUIHoldData& HoldData,
	TArray<FEntityRecord>& OutEntities)
{
	const FString AssetPath = Asset.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("common_ui_hold_data"), AssetPath);
	if (FindCommonUIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("common_ui_hold_data");
	Entity.CanonicalKey = AssetPath;
	Entity.DisplayName = Asset.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("asset_path"), AssetPath);
	Entity.Attributes.Add(TEXT("data_object_path"), HoldData.GetPathName());
	Entity.Attributes.Add(TEXT("data_class_path"), DataClassPath(Asset, HoldData));
	Entity.Attributes.Add(TEXT("keyboard_hold_time"), FString::SanitizeFloat(HoldData.KeyboardAndMouse.HoldTime));
	Entity.Attributes.Add(TEXT("gamepad_hold_time"), FString::SanitizeFloat(HoldData.Gamepad.HoldTime));
	Entity.Attributes.Add(TEXT("touch_hold_time"), FString::SanitizeFloat(HoldData.Touch.HoldTime));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("common_ui_hold_data_defaults") };
	Entity.Completeness.Omitted = { TEXT("runtime_hold_progress") };
	AddCommonUIEvidence(Entity, AssetPath, TEXT("CommonUI hold timing defaults read from the asset or generated class default object."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddCommonUIInputActionTableEntity(
	const FString& ProjectId,
	const UCommonGenericInputActionDataTable& Table,
	int32 RowCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString TablePath = Table.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("common_ui_input_action_table"), TablePath);
	if (FindCommonUIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("common_ui_input_action_table");
	Entity.CanonicalKey = TablePath;
	Entity.DisplayName = Table.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("table_path"), TablePath);
	Entity.Attributes.Add(TEXT("row_struct_path"), Table.GetRowStruct() ? Table.GetRowStruct()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("row_count"), FString::FromInt(RowCount));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("common_ui_input_action_rows") };
	Entity.Completeness.Omitted = { TEXT("runtime_input_reflector_resolution") };
	AddCommonUIEvidence(Entity, TablePath, TEXT("CommonUI input action data table rows read through DataTable row map."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

TSharedRef<FJsonObject> InputHoldDataJson(const FInputHoldData& HoldData)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("hold_time"), HoldData.HoldTime);
	Object->SetNumberField(TEXT("hold_rollback_time"), HoldData.HoldRollbackTime);
	return Object;
}

TSharedRef<FJsonObject> CommonInputTypeInfoJson(const FCommonInputTypeInfo& TypeInfo)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("key"), TypeInfo.GetKey().ToString());
	Object->SetStringField(TEXT("override_state"), InputActionStateString(TypeInfo.OverrrideState));
	Object->SetBoolField(TEXT("action_requires_hold"), TypeInfo.bActionRequiresHold);
	Object->SetNumberField(TEXT("hold_time"), TypeInfo.HoldTime);
	Object->SetNumberField(TEXT("hold_rollback_time"), TypeInfo.HoldRollbackTime);
	Object->SetStringField(TEXT("override_brush_resource"), BrushResourcePath(TypeInfo.OverrideBrush));
	return Object;
}

TSharedRef<FJsonObject> RowHandleJson(const FDataTableRowHandle& RowHandle)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("table_path"), RowHandleTablePath(RowHandle));
	Object->SetStringField(TEXT("row_name"), RowHandle.RowName.ToString());
	return Object;
}

TSharedRef<FJsonObject> InputActionReferenceJson(const UInputAction* Action)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("path"), Action ? Action->GetPathName() : FString());
	Object->SetStringField(TEXT("name"), Action ? Action->GetName() : FString());
	return Object;
}

TSharedRef<FJsonObject> CommonUIInputDataSnapshot(
	const UObject& Asset,
	const UCommonUIInputData& InputData,
	const FString& InputDataId)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.common_ui_input_data.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), InputDataId);
	Object->SetStringField(TEXT("asset_path"), Asset.GetPathName());
	Object->SetStringField(TEXT("data_object_path"), InputData.GetPathName());
	Object->SetStringField(TEXT("data_class_path"), DataClassPath(Asset, InputData));
	Object->SetObjectField(TEXT("default_click_action"), RowHandleJson(InputData.DefaultClickAction));
	Object->SetObjectField(TEXT("default_back_action"), RowHandleJson(InputData.DefaultBackAction));
	Object->SetStringField(TEXT("default_hold_data_class"), SoftClassPathString(InputData.DefaultHoldData));
	Object->SetObjectField(TEXT("enhanced_input_click_action"), InputActionReferenceJson(InputData.EnhancedInputClickAction));
	Object->SetObjectField(TEXT("enhanced_input_back_action"), InputActionReferenceJson(InputData.EnhancedInputBackAction));
	return Object;
}

TSharedRef<FJsonObject> CommonUIHoldDataSnapshot(
	const UObject& Asset,
	const UCommonUIHoldData& HoldData,
	const FString& HoldDataId)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.common_ui_hold_data.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), HoldDataId);
	Object->SetStringField(TEXT("asset_path"), Asset.GetPathName());
	Object->SetStringField(TEXT("data_object_path"), HoldData.GetPathName());
	Object->SetStringField(TEXT("data_class_path"), DataClassPath(Asset, HoldData));
	Object->SetObjectField(TEXT("keyboard_and_mouse"), InputHoldDataJson(HoldData.KeyboardAndMouse));
	Object->SetObjectField(TEXT("gamepad"), InputHoldDataJson(HoldData.Gamepad));
	Object->SetObjectField(TEXT("touch"), InputHoldDataJson(HoldData.Touch));
	return Object;
}

TSharedRef<FJsonObject> CommonUIInputActionRowSnapshot(
	const FString& RowId,
	const FName RowName,
	int32 RowIndex,
	const FCommonInputActionDataBase& RowData)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), RowId);
	Object->SetNumberField(TEXT("index"), RowIndex);
	Object->SetStringField(TEXT("row_name"), RowName.ToString());
	Object->SetStringField(TEXT("display_name"), RowData.DisplayName.ToString());
	Object->SetStringField(TEXT("hold_display_name"), RowData.HoldDisplayName.ToString());
	Object->SetNumberField(TEXT("nav_bar_priority"), RowData.NavBarPriority);
	Object->SetBoolField(TEXT("has_hold_bindings"), RowData.HasHoldBindings());
	Object->SetObjectField(TEXT("keyboard"), CommonInputTypeInfoJson(RowData.GetInputTypeInfo(ECommonInputType::MouseAndKeyboard, NAME_None)));
	Object->SetObjectField(TEXT("gamepad"), CommonInputTypeInfoJson(RowData.GetDefaultGamepadInputTypeInfo()));
	Object->SetObjectField(TEXT("touch"), CommonInputTypeInfoJson(RowData.GetInputTypeInfo(ECommonInputType::Touch, NAME_None)));
	return Object;
}

TSharedRef<FJsonObject> CommonUIInputActionTableSnapshot(
	const FString& ProjectId,
	const UCommonGenericInputActionDataTable& Table,
	const FString& TableId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.common_ui_input_action_table.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), TableId);
	Object->SetStringField(TEXT("table_path"), Table.GetPathName());
	Object->SetStringField(TEXT("row_struct_path"), Table.GetRowStruct() ? Table.GetRowStruct()->GetPathName() : FString());
	Object->SetStringField(TEXT("row_struct_name"), Table.GetRowStruct() ? Table.GetRowStruct()->GetName() : FString());

	TArray<FName> RowNames = Table.GetRowNames();
	RowNames.Sort(FNameLexicalLess());
	Object->SetNumberField(TEXT("row_count"), RowNames.Num());

	TArray<TSharedPtr<FJsonValue>> RowValues;
	int32 RowIndex = 0;
	for (const FName RowName : RowNames)
	{
		const FCommonInputActionDataBase* RowData = Table.FindRow<FCommonInputActionDataBase>(RowName, TEXT("UEPICommonUIReader"), false);
		const FString RowId = AddCommonUIInputActionRowEntity(ProjectId, Table.GetPathName(), RowName, RowIndex, RowData, Table.GetPathName(), OutEntities);
		AddCommonUIRelation(
			ProjectId,
			TEXT("contains_common_ui_input_action_row"),
			TableId,
			RowId,
			Table.GetPathName(),
			TEXT("CommonUI input action table contains this row."),
			OutRelations);
		if (RowData)
		{
			RowValues.Add(MakeShared<FJsonValueObject>(CommonUIInputActionRowSnapshot(RowId, RowName, RowIndex, *RowData)));
		}
		++RowIndex;
	}

	Object->SetArrayField(TEXT("rows"), RowValues);
	return Object;
}

void AppendRowHandleRelation(
	const FString& ProjectId,
	const FString& InputDataId,
	const FDataTableRowHandle& RowHandle,
	const FString& Role,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString TablePath = RowHandleTablePath(RowHandle);
	if (TablePath.IsEmpty() || RowHandle.RowName.IsNone())
	{
		return;
	}

	const FString RowId = AddCommonUIInputActionRowEntity(
		ProjectId,
		TablePath,
		RowHandle.RowName,
		INDEX_NONE,
		nullptr,
		EvidencePath,
		OutEntities);
	TMap<FString, FString> Attributes;
	Attributes.Add(TEXT("role"), Role);
	AddCommonUIRelation(
		ProjectId,
		TEXT("uses_common_ui_action_row"),
		InputDataId,
		RowId,
		EvidencePath,
		TEXT("CommonUI input data references this default input action row."),
		OutRelations,
		&Attributes);
}

void AppendInputActionRelation(
	const FString& ProjectId,
	const FString& InputDataId,
	const UInputAction* Action,
	const FString& Role,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (!Action)
	{
		return;
	}

	const FString ActionId = AddCommonUIInputActionEntity(ProjectId, Action, EvidencePath, OutEntities);
	TMap<FString, FString> Attributes;
	Attributes.Add(TEXT("role"), Role);
	AddCommonUIRelation(
		ProjectId,
		TEXT("uses_common_ui_input_action"),
		InputDataId,
		ActionId,
		EvidencePath,
		TEXT("CommonUI input data references this Enhanced Input action."),
		OutRelations,
		&Attributes);
}
}

bool FCommonUIReader::AppendCommonUIAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (const UCommonUIInputData* InputData = AssetOrBlueprintDefaultObject<UCommonUIInputData>(Asset))
	{
		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}

		const FString InputDataId = AddCommonUIInputDataEntity(ProjectId, Asset, *InputData, OutEntities);
		AddCommonUIRelation(ProjectId, TEXT("contains_common_ui_input_data"), AssetEntity.Id, InputDataId, Asset.GetPathName(), TEXT("Asset contains the extracted CommonUI input data record."), OutRelations);
		AppendRowHandleRelation(ProjectId, InputDataId, InputData->DefaultClickAction, TEXT("default_click_action"), Asset.GetPathName(), OutEntities, OutRelations);
		AppendRowHandleRelation(ProjectId, InputDataId, InputData->DefaultBackAction, TEXT("default_back_action"), Asset.GetPathName(), OutEntities, OutRelations);

		const FString HoldDataClassPath = SoftClassPathString(InputData->DefaultHoldData);
		if (!HoldDataClassPath.IsEmpty())
		{
			const FString HoldDataClassId = AddCommonUIHoldDataClassEntity(ProjectId, HoldDataClassPath, Asset.GetPathName(), OutEntities);
			AddCommonUIRelation(ProjectId, TEXT("uses_common_ui_hold_data"), InputDataId, HoldDataClassId, Asset.GetPathName(), TEXT("CommonUI input data references this default hold data class."), OutRelations);
		}

		AppendInputActionRelation(ProjectId, InputDataId, InputData->EnhancedInputClickAction, TEXT("enhanced_input_click_action"), Asset.GetPathName(), OutEntities, OutRelations);
		AppendInputActionRelation(ProjectId, InputDataId, InputData->EnhancedInputBackAction, TEXT("enhanced_input_back_action"), Asset.GetPathName(), OutEntities, OutRelations);

		AssetEntity.Snapshot->SetObjectField(TEXT("common_ui_input_data"), CommonUIInputDataSnapshot(Asset, *InputData, InputDataId));
		AssetEntity.Attributes.Add(TEXT("common_ui_input_data_class"), DataClassPath(Asset, *InputData));
		AssetEntity.Attributes.Add(TEXT("common_ui_default_click_row"), InputData->DefaultClickAction.RowName.ToString());
		AssetEntity.Attributes.Add(TEXT("common_ui_default_back_row"), InputData->DefaultBackAction.RowName.ToString());
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("common_ui_input_data"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_common_input_subsystem_state"));
		AddCommonUIEvidence(AssetEntity, Asset.GetPathName(), TEXT("CommonUI input data extracted from the asset or Blueprint generated class default object."));
		return true;
	}

	if (const UCommonUIHoldData* HoldData = AssetOrBlueprintDefaultObject<UCommonUIHoldData>(Asset))
	{
		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}

		const FString HoldDataId = AddCommonUIHoldDataEntity(ProjectId, Asset, *HoldData, OutEntities);
		AddCommonUIRelation(ProjectId, TEXT("contains_common_ui_hold_data"), AssetEntity.Id, HoldDataId, Asset.GetPathName(), TEXT("Asset contains the extracted CommonUI hold data record."), OutRelations);
		AssetEntity.Snapshot->SetObjectField(TEXT("common_ui_hold_data"), CommonUIHoldDataSnapshot(Asset, *HoldData, HoldDataId));
		AssetEntity.Attributes.Add(TEXT("common_ui_hold_data_class"), DataClassPath(Asset, *HoldData));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("common_ui_hold_data"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_hold_progress"));
		AddCommonUIEvidence(AssetEntity, Asset.GetPathName(), TEXT("CommonUI hold data extracted from the asset or Blueprint generated class default object."));
		return true;
	}

	if (UCommonGenericInputActionDataTable* InputActionTable = Cast<UCommonGenericInputActionDataTable>(&Asset))
	{
		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}

		const TArray<FName> RowNames = InputActionTable->GetRowNames();
		const FString TableId = AddCommonUIInputActionTableEntity(ProjectId, *InputActionTable, RowNames.Num(), OutEntities);
		AddCommonUIRelation(ProjectId, TEXT("contains_common_ui_input_action_table"), AssetEntity.Id, TableId, InputActionTable->GetPathName(), TEXT("Asset contains the extracted CommonUI input action table record."), OutRelations);
		AssetEntity.Snapshot->SetObjectField(TEXT("common_ui_input_action_table"), CommonUIInputActionTableSnapshot(ProjectId, *InputActionTable, TableId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("common_ui_input_action_row_count"), FString::FromInt(RowNames.Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("common_ui_input_action_rows"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_input_reflector_resolution"));
		AddCommonUIEvidence(AssetEntity, InputActionTable->GetPathName(), TEXT("CommonUI input action table extracted through public DataTable API."));
		return true;
	}

	return false;
}
}
