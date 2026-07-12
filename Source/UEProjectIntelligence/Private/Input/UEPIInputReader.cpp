#include "UEPIInputReader.h"

#if UEPI_WITH_ENHANCED_INPUT

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString InputValueTypeString(EInputActionValueType ValueType)
{
	switch (ValueType)
	{
	case EInputActionValueType::Boolean:
		return TEXT("boolean");
	case EInputActionValueType::Axis1D:
		return TEXT("axis1d");
	case EInputActionValueType::Axis2D:
		return TEXT("axis2d");
	case EInputActionValueType::Axis3D:
		return TEXT("axis3d");
	default:
		return TEXT("unknown");
	}
}

void AddInputRelation(
	const FString& ProjectId,
	const FString& Type,
	const FString& FromId,
	const FString& ToId,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FRelationRecord>& OutRelations)
{
	FRelationRecord Relation;
	Relation.Id = MakeRelationId(ProjectId, Type, FromId, ToId);
	Relation.Type = Type;
	Relation.FromId = FromId;
	Relation.ToId = ToId;
	Relation.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Relation.bDerived = false;
	Relation.Confidence = 1.0f;
	Relation.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		EvidencePath,
		EvidenceDetail
	});
	OutRelations.Add(MoveTemp(Relation));
}

void AddInputEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

template <typename ObjectType>
TArray<TSharedPtr<FJsonValue>> ObjectClassArrayToJson(const TArray<TObjectPtr<ObjectType>>& Objects)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const TObjectPtr<ObjectType>& Object : Objects)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("class"), Object && Object->GetClass() ? Object->GetClass()->GetPathName() : FString());
		Item->SetStringField(TEXT("path"), Object ? Object->GetPathName() : FString());
		Values.Add(MakeShared<FJsonValueObject>(Item));
	}
	return Values;
}

FString AddInputActionEntity(
	const FString& ProjectId,
	const UInputAction* Action,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString ActionPath = Action ? Action->GetPathName() : FString();
	const FString ActionId = MakeStableId(ProjectId, TEXT("input_action"), ActionPath);
	for (const FEntityRecord& Entity : OutEntities)
	{
		if (Entity.Id == ActionId)
		{
			return ActionId;
		}
	}

	FEntityRecord Entity;
	Entity.Id = ActionId;
	Entity.Kind = TEXT("input_action");
	Entity.CanonicalKey = ActionPath;
	Entity.DisplayName = Action ? Action->GetName() : FString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("action_path"), ActionPath);
	Entity.Attributes.Add(TEXT("value_type"), Action ? InputValueTypeString(Action->ValueType) : FString());
	Entity.Attributes.Add(TEXT("trigger_count"), Action ? FString::FromInt(Action->Triggers.Num()) : TEXT("0"));
	Entity.Attributes.Add(TEXT("modifier_count"), Action ? FString::FromInt(Action->Modifiers.Num()) : TEXT("0"));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("input_action_metadata") };
	Entity.Completeness.Omitted = { TEXT("runtime_input_state") };
	AddInputEvidence(Entity, EvidencePath, TEXT("Input action reference read from Enhanced Input assets."));
	OutEntities.Add(MoveTemp(Entity));
	return ActionId;
}

FEntityRecord MakeInputMappingEntity(
	const FString& ProjectId,
	const UInputMappingContext& Context,
	const FEnhancedActionKeyMapping& Mapping,
	int32 MappingIndex)
{
	const FString ActionPath = Mapping.Action ? Mapping.Action->GetPathName() : FString();
	const FString KeyName = Mapping.Key.ToString();
	const FString CanonicalKey = Context.GetPathName() + TEXT(":mapping:") + FString::FromInt(MappingIndex) + TEXT(":") + ActionPath + TEXT(":") + KeyName;

	FEntityRecord Entity;
	Entity.Id = MakeStableId(ProjectId, TEXT("input_mapping"), CanonicalKey);
	Entity.Kind = TEXT("input_mapping");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = FString::Printf(TEXT("%s -> %s"), *KeyName, Mapping.Action ? *Mapping.Action->GetName() : TEXT("<none>"));
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("context_path"), Context.GetPathName());
	Entity.Attributes.Add(TEXT("mapping_index"), FString::FromInt(MappingIndex));
	Entity.Attributes.Add(TEXT("key"), KeyName);
	Entity.Attributes.Add(TEXT("action_path"), ActionPath);
	Entity.Attributes.Add(TEXT("trigger_count"), FString::FromInt(Mapping.Triggers.Num()));
	Entity.Attributes.Add(TEXT("modifier_count"), FString::FromInt(Mapping.Modifiers.Num()));
	Entity.Attributes.Add(TEXT("player_mappable"), Mapping.IsPlayerMappable() ? TEXT("true") : TEXT("false"));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("input_mapping_metadata"), TEXT("input_action_reference") };
	Entity.Completeness.Omitted = { TEXT("runtime_mapping_resolution") };
	AddInputEvidence(Entity, Context.GetPathName(), TEXT("Enhanced Input mapping entry read from UInputMappingContext."));
	return Entity;
}

TSharedRef<FJsonObject> InputActionSnapshot(const UInputAction& Action)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.input_action.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("action_path"), Action.GetPathName());
	Object->SetStringField(TEXT("description"), Action.ActionDescription.ToString());
	Object->SetStringField(TEXT("value_type"), InputValueTypeString(Action.ValueType));
	Object->SetBoolField(TEXT("trigger_when_paused"), Action.bTriggerWhenPaused);
	Object->SetBoolField(TEXT("consume_input"), Action.bConsumeInput);
	Object->SetBoolField(TEXT("reserve_all_mappings"), Action.bReserveAllMappings);
	Object->SetNumberField(TEXT("trigger_count"), Action.Triggers.Num());
	Object->SetNumberField(TEXT("modifier_count"), Action.Modifiers.Num());
	Object->SetArrayField(TEXT("triggers"), ObjectClassArrayToJson(Action.Triggers));
	Object->SetArrayField(TEXT("modifiers"), ObjectClassArrayToJson(Action.Modifiers));
	return Object;
}

TSharedRef<FJsonObject> MappingContextSnapshot(
	const UInputMappingContext& Context,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.input_mapping_context.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("context_path"), Context.GetPathName());
	Object->SetStringField(TEXT("description"), Context.ContextDescription.ToString());

	const TArray<FEnhancedActionKeyMapping>& Mappings = Context.GetMappings();
	Object->SetNumberField(TEXT("mapping_count"), Mappings.Num());

	TArray<TSharedPtr<FJsonValue>> MappingValues;
	for (int32 MappingIndex = 0; MappingIndex < Mappings.Num(); ++MappingIndex)
	{
		const FEnhancedActionKeyMapping& Mapping = Mappings[MappingIndex];
		FEntityRecord MappingEntity = MakeInputMappingEntity(ProjectId, Context, Mapping, MappingIndex);
		const FString MappingId = MappingEntity.Id;
		OutEntities.Add(MoveTemp(MappingEntity));
		AddInputRelation(ProjectId, TEXT("contains_input_mapping"), AssetEntity.Id, MappingId, Context.GetPathName(), TEXT("Input mapping context contains an action-key mapping."), OutRelations);

		if (Mapping.Action)
		{
			const FString ActionId = AddInputActionEntity(ProjectId, Mapping.Action, Context.GetPathName(), OutEntities);
			AddInputRelation(ProjectId, TEXT("maps_input_action"), MappingId, ActionId, Context.GetPathName(), TEXT("Input mapping points to an Input Action asset."), OutRelations);
		}

		TSharedRef<FJsonObject> MappingObject = MakeShared<FJsonObject>();
		MappingObject->SetStringField(TEXT("id"), MappingId);
		MappingObject->SetNumberField(TEXT("index"), MappingIndex);
		MappingObject->SetStringField(TEXT("key"), Mapping.Key.ToString());
		MappingObject->SetStringField(TEXT("action_path"), Mapping.Action ? Mapping.Action->GetPathName() : FString());
		MappingObject->SetStringField(TEXT("action_name"), Mapping.Action ? Mapping.Action->GetName() : FString());
		MappingObject->SetBoolField(TEXT("player_mappable"), Mapping.IsPlayerMappable());
		MappingObject->SetNumberField(TEXT("trigger_count"), Mapping.Triggers.Num());
		MappingObject->SetNumberField(TEXT("modifier_count"), Mapping.Modifiers.Num());
		MappingValues.Add(MakeShared<FJsonValueObject>(MappingObject));
	}

	Object->SetArrayField(TEXT("mappings"), MappingValues);
	return Object;
}
}

bool FInputReader::AppendInputAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (UInputAction* Action = Cast<UInputAction>(&Asset))
	{
		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("input_action"), InputActionSnapshot(*Action));
		AssetEntity.Attributes.Add(TEXT("input_value_type"), InputValueTypeString(Action->ValueType));
		AssetEntity.Attributes.Add(TEXT("input_trigger_count"), FString::FromInt(Action->Triggers.Num()));
		AssetEntity.Attributes.Add(TEXT("input_modifier_count"), FString::FromInt(Action->Modifiers.Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("input_action_metadata"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_input_state"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Action->GetPathName(),
			TEXT("Enhanced Input Action structure extracted from a loaded UInputAction asset.")
		});
		const FString ActionId = AddInputActionEntity(ProjectId, Action, Action->GetPathName(), OutEntities);
		AddInputRelation(
			ProjectId,
			TEXT("contains_input_action"),
			AssetEntity.Id,
			ActionId,
			Action->GetPathName(),
			TEXT("Input Action asset contains the extracted Enhanced Input action record."),
			OutRelations);
		return true;
	}

	if (UInputMappingContext* Context = Cast<UInputMappingContext>(&Asset))
	{
		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("input_mapping_context"), MappingContextSnapshot(*Context, ProjectId, AssetEntity, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("input_mapping_count"), FString::FromInt(Context->GetMappings().Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("input_mapping_context"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("input_action_mappings"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_mapping_resolution"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Context->GetPathName(),
			TEXT("Enhanced Input Mapping Context structure extracted from a loaded UInputMappingContext asset.")
		});
		return true;
	}

	return false;
}
}

#else

namespace UE::ProjectIntelligence
{
bool FInputReader::AppendInputAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	return false;
}
}

#endif
