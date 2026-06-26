#include "UEPIGASReader.h"

#if UEPI_WITH_GAMEPLAY_ABILITIES

#include "Abilities/GameplayAbility.h"
#include "Abilities/GameplayAbilityTypes.h"
#include "AttributeSet.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "GameplayAbilitySpec.h"
#include "GameplayCueNotify_Actor.h"
#include "GameplayCueNotify_Static.h"
#include "GameplayEffect.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayEffectTypes.h"
#include "GameplayModMagnitudeCalculation.h"
#include "Misc/PackageName.h"
#include "ScalableFloat.h"
#include "UObject/UnrealType.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString GASBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString GASObjectPath(const UObject* Object)
{
	return Object ? Object->GetPathName() : FString();
}

FString GASClassPath(const UObject* Object)
{
	return Object && Object->GetClass() ? Object->GetClass()->GetPathName() : FString();
}

FString GASClassPath(const UClass* Class)
{
	return Class ? Class->GetPathName() : FString();
}

template <typename ClassType>
FString GASSubclassPath(TSubclassOf<ClassType> Class)
{
	return GASClassPath(Class.Get());
}

FString GASEnumValueString(const UEnum* Enum, int64 Value)
{
	if (!Enum)
	{
		return FString::FromInt(static_cast<int32>(Value));
	}
	return Enum->GetNameStringByValue(Value);
}

template <typename EnumType>
FString GASEnumString(EnumType Value)
{
	return GASEnumValueString(StaticEnum<EnumType>(), static_cast<int64>(Value));
}

FString AbilityInstancingPolicyString(EGameplayAbilityInstancingPolicy::Type Policy)
{
	return GASEnumValueString(StaticEnum<EGameplayAbilityInstancingPolicy::Type>(), static_cast<int64>(Policy));
}

FString AbilityReplicationPolicyString(EGameplayAbilityReplicationPolicy::Type Policy)
{
	return GASEnumValueString(StaticEnum<EGameplayAbilityReplicationPolicy::Type>(), static_cast<int64>(Policy));
}

FString AbilityNetExecutionPolicyString(EGameplayAbilityNetExecutionPolicy::Type Policy)
{
	return GASEnumValueString(StaticEnum<EGameplayAbilityNetExecutionPolicy::Type>(), static_cast<int64>(Policy));
}

FString AbilityNetSecurityPolicyString(EGameplayAbilityNetSecurityPolicy::Type Policy)
{
	return GASEnumValueString(StaticEnum<EGameplayAbilityNetSecurityPolicy::Type>(), static_cast<int64>(Policy));
}

FString AbilityTriggerSourceString(EGameplayAbilityTriggerSource::Type Source)
{
	return GASEnumValueString(StaticEnum<EGameplayAbilityTriggerSource::Type>(), static_cast<int64>(Source));
}

void AddGASEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

void AddGASRelation(
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

FEntityRecord* FindGASEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

template <typename AssetType>
const AssetType* GASAssetOrBlueprintDefaultObject(const UObject& Asset)
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

FString AssetOrClassPath(const UObject& Asset, const UObject& DefaultObject)
{
	if (const UBlueprint* Blueprint = Cast<UBlueprint>(&Asset))
	{
		return Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : Asset.GetPathName();
	}
	if (const UClass* ClassAsset = Cast<UClass>(&Asset))
	{
		return ClassAsset->GetPathName();
	}
	return DefaultObject.GetClass() ? DefaultObject.GetClass()->GetPathName() : Asset.GetPathName();
}

TArray<TSharedPtr<FJsonValue>> GameplayTagsToJson(const FGameplayTagContainer& Tags)
{
	TArray<FGameplayTag> TagArray;
	Tags.GetGameplayTagArray(TagArray);
	TagArray.Sort([](const FGameplayTag& Left, const FGameplayTag& Right)
	{
		return Left.ToString() < Right.ToString();
	});

	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(TagArray.Num());
	for (const FGameplayTag& Tag : TagArray)
	{
		Values.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	return Values;
}

int32 GameplayTagCount(const FGameplayTagContainer& Tags)
{
	TArray<FGameplayTag> TagArray;
	Tags.GetGameplayTagArray(TagArray);
	return TagArray.Num();
}

const FGameplayTagContainer* FindTagContainerProperty(const UObject& Object, const TCHAR* PropertyName)
{
	const FStructProperty* Property = FindFProperty<FStructProperty>(Object.GetClass(), PropertyName);
	if (!Property || Property->Struct != FGameplayTagContainer::StaticStruct())
	{
		return nullptr;
	}
	return Property->ContainerPtrToValuePtr<FGameplayTagContainer>(&Object);
}

FGameplayTagContainer CopyTagContainerProperty(const UObject& Object, const TCHAR* PropertyName)
{
	if (const FGameplayTagContainer* Tags = FindTagContainerProperty(Object, PropertyName))
	{
		return *Tags;
	}
	return FGameplayTagContainer();
}

TArray<FAbilityTriggerData> CopyAbilityTriggers(const UGameplayAbility& Ability)
{
	TArray<FAbilityTriggerData> Triggers;
	const FArrayProperty* Property = FindFProperty<FArrayProperty>(Ability.GetClass(), TEXT("AbilityTriggers"));
	if (!Property)
	{
		return Triggers;
	}

	const FStructProperty* InnerStruct = CastField<FStructProperty>(Property->Inner);
	if (!InnerStruct || InnerStruct->Struct != FAbilityTriggerData::StaticStruct())
	{
		return Triggers;
	}

	const void* ArrayPtr = Property->ContainerPtrToValuePtr<void>(&Ability);
	FScriptArrayHelper Helper(Property, ArrayPtr);
	Triggers.Reserve(Helper.Num());
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
	{
		const FAbilityTriggerData* Trigger = reinterpret_cast<const FAbilityTriggerData*>(Helper.GetRawPtr(Index));
		if (Trigger)
		{
			Triggers.Add(*Trigger);
		}
	}
	return Triggers;
}

TSharedRef<FJsonObject> ScalableFloatSnapshot(const FScalableFloat& Value)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("value"), Value.Value);
	Object->SetBoolField(TEXT("is_static"), Value.IsStatic());
	Object->SetStringField(TEXT("curve_table_path"), Value.Curve.CurveTable ? Value.Curve.CurveTable->GetPathName() : FString());
	Object->SetStringField(TEXT("curve_row_name"), Value.Curve.RowName.ToString());
	return Object;
}

TSharedRef<FJsonObject> MagnitudeSnapshot(const FGameplayEffectModifierMagnitude& Magnitude)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("calculation_type"), GASEnumString(Magnitude.GetMagnitudeCalculationType()));

	float StaticMagnitude = 0.0f;
	const FString Context = TEXT("UEPI GAS static magnitude snapshot");
	const bool bHasStaticMagnitude = Magnitude.GetStaticMagnitudeIfPossible(1.0f, StaticMagnitude, &Context);
	Object->SetBoolField(TEXT("has_static_level_1_value"), bHasStaticMagnitude);
	Object->SetNumberField(TEXT("static_level_1_value"), bHasStaticMagnitude ? StaticMagnitude : 0.0f);

	const FSetByCallerFloat& SetByCaller = Magnitude.GetSetByCallerFloat();
	Object->SetStringField(TEXT("set_by_caller_name"), SetByCaller.DataName.ToString());
	Object->SetStringField(TEXT("set_by_caller_tag"), SetByCaller.DataTag.ToString());
	Object->SetStringField(TEXT("custom_calculation_class"), GASSubclassPath(Magnitude.GetCustomMagnitudeCalculationClass()));
	return Object;
}

FString GameplayAttributeName(const FGameplayAttribute& Attribute)
{
	return Attribute.IsValid() ? Attribute.GetName() : FString();
}

FString GameplayAttributePropertyPath(const FGameplayAttribute& Attribute)
{
	const FProperty* Property = Attribute.GetUProperty();
	return Property ? Property->GetPathName() : FString();
}

FString AddGameplayAbilityEntity(
	const FString& ProjectId,
	const UGameplayAbility& Ability,
	const FString& AssetPath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString AbilityClassPath = GASClassPath(Ability.GetClass());
	const FString CanonicalKey = AbilityClassPath.IsEmpty() ? Ability.GetPathName() : AbilityClassPath;
	const FString EntityId = MakeStableId(ProjectId, TEXT("gameplay_ability"), CanonicalKey);
	if (FindGASEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	const TArray<FAbilityTriggerData> Triggers = CopyAbilityTriggers(Ability);
	const FGameplayTagContainer CancelTags = CopyTagContainerProperty(Ability, TEXT("CancelAbilitiesWithTag"));
	const FGameplayTagContainer BlockTags = CopyTagContainerProperty(Ability, TEXT("BlockAbilitiesWithTag"));
	const FGameplayTagContainer OwnedTags = CopyTagContainerProperty(Ability, TEXT("ActivationOwnedTags"));
	const FGameplayTagContainer RequiredTags = CopyTagContainerProperty(Ability, TEXT("ActivationRequiredTags"));
	const FGameplayTagContainer BlockedTags = CopyTagContainerProperty(Ability, TEXT("ActivationBlockedTags"));

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("gameplay_ability");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Ability.GetClass() ? Ability.GetClass()->GetName() : Ability.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("asset_path"), AssetPath);
	Entity.Attributes.Add(TEXT("ability_object_path"), Ability.GetPathName());
	Entity.Attributes.Add(TEXT("ability_class_path"), AbilityClassPath);
	Entity.Attributes.Add(TEXT("ability_tag_count"), FString::FromInt(GameplayTagCount(Ability.AbilityTags)));
	Entity.Attributes.Add(TEXT("trigger_count"), FString::FromInt(Triggers.Num()));
	Entity.Attributes.Add(TEXT("instancing_policy"), AbilityInstancingPolicyString(Ability.GetInstancingPolicy()));
	Entity.Attributes.Add(TEXT("replication_policy"), AbilityReplicationPolicyString(Ability.GetReplicationPolicy()));
	Entity.Attributes.Add(TEXT("net_execution_policy"), AbilityNetExecutionPolicyString(Ability.GetNetExecutionPolicy()));
	Entity.Attributes.Add(TEXT("net_security_policy"), AbilityNetSecurityPolicyString(Ability.GetNetSecurityPolicy()));
	Entity.Attributes.Add(TEXT("cost_gameplay_effect_class"), GASClassPath(Ability.GetCostGameplayEffect() ? Ability.GetCostGameplayEffect()->GetClass() : nullptr));
	Entity.Attributes.Add(TEXT("cooldown_gameplay_effect_class"), GASClassPath(Ability.GetCooldownGameplayEffect() ? Ability.GetCooldownGameplayEffect()->GetClass() : nullptr));
	Entity.Attributes.Add(TEXT("cancel_tag_count"), FString::FromInt(GameplayTagCount(CancelTags)));
	Entity.Attributes.Add(TEXT("block_tag_count"), FString::FromInt(GameplayTagCount(BlockTags)));
	Entity.Attributes.Add(TEXT("activation_owned_tag_count"), FString::FromInt(GameplayTagCount(OwnedTags)));
	Entity.Attributes.Add(TEXT("activation_required_tag_count"), FString::FromInt(GameplayTagCount(RequiredTags)));
	Entity.Attributes.Add(TEXT("activation_blocked_tag_count"), FString::FromInt(GameplayTagCount(BlockedTags)));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("ability_tags"), TEXT("activation_tag_rules"), TEXT("cost_cooldown_effect_references"), TEXT("ability_triggers"), TEXT("network_policies") };
	Entity.Completeness.Omitted = { TEXT("runtime_activation_state"), TEXT("ability_task_graph_semantics"), TEXT("runtime_commit_result") };
	AddGASEvidence(Entity, AssetPath, TEXT("GameplayAbility default object metadata read from UObject reflection."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddGameplayEffectEntity(
	const FString& ProjectId,
	const UGameplayEffect& Effect,
	const FString& AssetPath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString EffectClassPath = GASClassPath(Effect.GetClass());
	const FString CanonicalKey = EffectClassPath.IsEmpty() ? Effect.GetPathName() : EffectClassPath;
	const FString EntityId = MakeStableId(ProjectId, TEXT("gameplay_effect"), CanonicalKey);
	if (FindGASEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("gameplay_effect");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Effect.GetClass() ? Effect.GetClass()->GetName() : Effect.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("asset_path"), AssetPath);
	Entity.Attributes.Add(TEXT("effect_object_path"), Effect.GetPathName());
	Entity.Attributes.Add(TEXT("effect_class_path"), EffectClassPath);
	Entity.Attributes.Add(TEXT("duration_policy"), GASEnumString(Effect.DurationPolicy));
	Entity.Attributes.Add(TEXT("modifier_count"), FString::FromInt(Effect.Modifiers.Num()));
	Entity.Attributes.Add(TEXT("execution_count"), FString::FromInt(Effect.Executions.Num()));
	Entity.Attributes.Add(TEXT("gameplay_cue_count"), FString::FromInt(Effect.GameplayCues.Num()));
	Entity.Attributes.Add(TEXT("overflow_effect_count"), FString::FromInt(Effect.OverflowEffects.Num()));
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	Entity.Attributes.Add(TEXT("granted_ability_count"), FString::FromInt(Effect.GrantedAbilities.Num()));
PRAGMA_ENABLE_DEPRECATION_WARNINGS
	Entity.Attributes.Add(TEXT("stacking_type"), GASEnumString(Effect.StackingType));
	Entity.Attributes.Add(TEXT("stack_limit_count"), FString::FromInt(Effect.StackLimitCount));
	Entity.Attributes.Add(TEXT("asset_tag_count"), FString::FromInt(GameplayTagCount(Effect.GetAssetTags())));
	Entity.Attributes.Add(TEXT("granted_tag_count"), FString::FromInt(GameplayTagCount(Effect.GetGrantedTags())));
	Entity.Attributes.Add(TEXT("blocked_ability_tag_count"), FString::FromInt(GameplayTagCount(Effect.GetBlockedAbilityTags())));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("duration_policy"), TEXT("modifiers"), TEXT("executions"), TEXT("gameplay_cues"), TEXT("stacking"), TEXT("tag_caches") };
	Entity.Completeness.Omitted = { TEXT("runtime_effect_specs"), TEXT("active_effect_handles"), TEXT("aggregator_runtime_values") };
	AddGASEvidence(Entity, AssetPath, TEXT("GameplayEffect default object metadata read from UObject reflection."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddGameplayEffectClassEntity(
	const FString& ProjectId,
	const UGameplayEffect* Effect,
	const FString& EffectClassPath,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	if (Effect)
	{
		return AddGameplayEffectEntity(ProjectId, *Effect, EvidencePath, OutEntities);
	}

	if (EffectClassPath.IsEmpty())
	{
		return FString();
	}

	const FString EntityId = MakeStableId(ProjectId, TEXT("gameplay_effect"), EffectClassPath);
	if (FindGASEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("gameplay_effect");
	Entity.CanonicalKey = EffectClassPath;
	Entity.DisplayName = FPackageName::ObjectPathToObjectName(EffectClassPath);
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("effect_class_path"), EffectClassPath);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("gameplay_effect_class_reference") };
	Entity.Completeness.Omitted = { TEXT("gameplay_effect_default_object") };
	AddGASEvidence(Entity, EvidencePath, TEXT("GameplayEffect class reference discovered from another GAS asset."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddGameplayCueNotifyEntity(
	const FString& ProjectId,
	const UObject& CueObject,
	const FString& CueType,
	const FString& GameplayCueTag,
	const FString& AssetPath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CueClassPath = GASClassPath(CueObject.GetClass());
	const FString CanonicalKey = CueClassPath.IsEmpty() ? CueObject.GetPathName() : CueClassPath;
	const FString EntityId = MakeStableId(ProjectId, TEXT("gameplay_cue_notify"), CanonicalKey);
	if (FindGASEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("gameplay_cue_notify");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = CueObject.GetClass() ? CueObject.GetClass()->GetName() : CueObject.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("asset_path"), AssetPath);
	Entity.Attributes.Add(TEXT("cue_object_path"), CueObject.GetPathName());
	Entity.Attributes.Add(TEXT("cue_class_path"), CueClassPath);
	Entity.Attributes.Add(TEXT("cue_type"), CueType);
	Entity.Attributes.Add(TEXT("gameplay_cue_tag"), GameplayCueTag);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("gameplay_cue_tag"), TEXT("notify_class_defaults") };
	Entity.Completeness.Omitted = { TEXT("runtime_cue_dispatch"), TEXT("spawned_notify_instances") };
	AddGASEvidence(Entity, AssetPath, TEXT("GameplayCueNotify default metadata read from UObject reflection."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddGameplayAbilityTriggerEntity(
	const FString& ProjectId,
	const FString& AbilityId,
	const FString& AbilityClassPath,
	const FAbilityTriggerData& Trigger,
	int32 TriggerIndex,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString TriggerTag = Trigger.TriggerTag.ToString();
	const FString CanonicalKey = AbilityClassPath + TEXT(":trigger:") + FString::FromInt(TriggerIndex) + TEXT(":") + TriggerTag + TEXT(":") + AbilityTriggerSourceString(Trigger.TriggerSource);
	const FString EntityId = MakeStableId(ProjectId, TEXT("gameplay_ability_trigger"), CanonicalKey);
	if (FindGASEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("gameplay_ability_trigger");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = TriggerTag;
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("ability_id"), AbilityId);
	Entity.Attributes.Add(TEXT("ability_class_path"), AbilityClassPath);
	Entity.Attributes.Add(TEXT("trigger_index"), FString::FromInt(TriggerIndex));
	Entity.Attributes.Add(TEXT("trigger_tag"), TriggerTag);
	Entity.Attributes.Add(TEXT("trigger_source"), AbilityTriggerSourceString(Trigger.TriggerSource));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Complete;
	Entity.Completeness.Covered = { TEXT("trigger_tag"), TEXT("trigger_source") };
	AddGASEvidence(Entity, EvidencePath, TEXT("Ability trigger read from GameplayAbility default object."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddGameplayEffectModifierEntity(
	const FString& ProjectId,
	const FString& EffectId,
	const FString& EffectClassPath,
	const FGameplayModifierInfo& Modifier,
	int32 ModifierIndex,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString AttributeName = GameplayAttributeName(Modifier.Attribute);
	const FString CanonicalKey = EffectClassPath + TEXT(":modifier:") + FString::FromInt(ModifierIndex) + TEXT(":") + AttributeName;
	const FString EntityId = MakeStableId(ProjectId, TEXT("gameplay_effect_modifier"), CanonicalKey);
	if (FindGASEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("gameplay_effect_modifier");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = AttributeName;
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("effect_id"), EffectId);
	Entity.Attributes.Add(TEXT("effect_class_path"), EffectClassPath);
	Entity.Attributes.Add(TEXT("modifier_index"), FString::FromInt(ModifierIndex));
	Entity.Attributes.Add(TEXT("attribute"), AttributeName);
	Entity.Attributes.Add(TEXT("attribute_property_path"), GameplayAttributePropertyPath(Modifier.Attribute));
	Entity.Attributes.Add(TEXT("modifier_op"), EGameplayModOpToString(Modifier.ModifierOp));
	Entity.Attributes.Add(TEXT("magnitude_calculation_type"), GASEnumString(Modifier.ModifierMagnitude.GetMagnitudeCalculationType()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("attribute"), TEXT("operation"), TEXT("magnitude_calculation_type") };
	Entity.Completeness.Omitted = { TEXT("runtime_evaluated_magnitude") };
	AddGASEvidence(Entity, EvidencePath, TEXT("GameplayEffect modifier metadata read from default object."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

TSharedRef<FJsonObject> GameplayAbilityTriggerSnapshot(const FString& TriggerId, const FAbilityTriggerData& Trigger, int32 TriggerIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), TriggerId);
	Object->SetNumberField(TEXT("index"), TriggerIndex);
	Object->SetStringField(TEXT("tag"), Trigger.TriggerTag.ToString());
	Object->SetStringField(TEXT("source"), AbilityTriggerSourceString(Trigger.TriggerSource));
	return Object;
}

TSharedRef<FJsonObject> GameplayAbilitySnapshot(
	const UObject& Asset,
	const UGameplayAbility& Ability,
	const FString& AbilityId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations,
	const FString& ProjectId)
{
	const FString AssetPath = Asset.GetPathName();
	const FString AbilityClassPath = GASClassPath(Ability.GetClass());
	const FGameplayTagContainer CancelTags = CopyTagContainerProperty(Ability, TEXT("CancelAbilitiesWithTag"));
	const FGameplayTagContainer BlockTags = CopyTagContainerProperty(Ability, TEXT("BlockAbilitiesWithTag"));
	const FGameplayTagContainer OwnedTags = CopyTagContainerProperty(Ability, TEXT("ActivationOwnedTags"));
	const FGameplayTagContainer RequiredTags = CopyTagContainerProperty(Ability, TEXT("ActivationRequiredTags"));
	const FGameplayTagContainer BlockedTags = CopyTagContainerProperty(Ability, TEXT("ActivationBlockedTags"));
	const FGameplayTagContainer SourceRequiredTags = CopyTagContainerProperty(Ability, TEXT("SourceRequiredTags"));
	const FGameplayTagContainer SourceBlockedTags = CopyTagContainerProperty(Ability, TEXT("SourceBlockedTags"));
	const FGameplayTagContainer TargetRequiredTags = CopyTagContainerProperty(Ability, TEXT("TargetRequiredTags"));
	const FGameplayTagContainer TargetBlockedTags = CopyTagContainerProperty(Ability, TEXT("TargetBlockedTags"));
	const TArray<FAbilityTriggerData> Triggers = CopyAbilityTriggers(Ability);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.gameplay_ability.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), AbilityId);
	Object->SetStringField(TEXT("asset_path"), AssetPath);
	Object->SetStringField(TEXT("ability_object_path"), Ability.GetPathName());
	Object->SetStringField(TEXT("ability_class_path"), AbilityClassPath);
	Object->SetStringField(TEXT("instancing_policy"), AbilityInstancingPolicyString(Ability.GetInstancingPolicy()));
	Object->SetStringField(TEXT("replication_policy"), AbilityReplicationPolicyString(Ability.GetReplicationPolicy()));
	Object->SetStringField(TEXT("net_execution_policy"), AbilityNetExecutionPolicyString(Ability.GetNetExecutionPolicy()));
	Object->SetStringField(TEXT("net_security_policy"), AbilityNetSecurityPolicyString(Ability.GetNetSecurityPolicy()));
	Object->SetBoolField(TEXT("replicate_input_directly"), Ability.bReplicateInputDirectly);
	Object->SetArrayField(TEXT("ability_tags"), GameplayTagsToJson(Ability.AbilityTags));
	Object->SetArrayField(TEXT("cancel_abilities_with_tags"), GameplayTagsToJson(CancelTags));
	Object->SetArrayField(TEXT("block_abilities_with_tags"), GameplayTagsToJson(BlockTags));
	Object->SetArrayField(TEXT("activation_owned_tags"), GameplayTagsToJson(OwnedTags));
	Object->SetArrayField(TEXT("activation_required_tags"), GameplayTagsToJson(RequiredTags));
	Object->SetArrayField(TEXT("activation_blocked_tags"), GameplayTagsToJson(BlockedTags));
	Object->SetArrayField(TEXT("source_required_tags"), GameplayTagsToJson(SourceRequiredTags));
	Object->SetArrayField(TEXT("source_blocked_tags"), GameplayTagsToJson(SourceBlockedTags));
	Object->SetArrayField(TEXT("target_required_tags"), GameplayTagsToJson(TargetRequiredTags));
	Object->SetArrayField(TEXT("target_blocked_tags"), GameplayTagsToJson(TargetBlockedTags));

	const UGameplayEffect* CostEffect = Ability.GetCostGameplayEffect();
	const UGameplayEffect* CooldownEffect = Ability.GetCooldownGameplayEffect();
	const FString CostEffectClassPath = GASClassPath(CostEffect ? CostEffect->GetClass() : nullptr);
	const FString CooldownEffectClassPath = GASClassPath(CooldownEffect ? CooldownEffect->GetClass() : nullptr);
	Object->SetStringField(TEXT("cost_gameplay_effect_class"), CostEffectClassPath);
	Object->SetStringField(TEXT("cooldown_gameplay_effect_class"), CooldownEffectClassPath);

	if (!CostEffectClassPath.IsEmpty())
	{
		const FString CostEffectId = AddGameplayEffectClassEntity(ProjectId, CostEffect, CostEffectClassPath, AssetPath, OutEntities);
		AddGASRelation(ProjectId, TEXT("gameplay_ability_cost_effect"), AbilityId, CostEffectId, AssetPath, TEXT("GameplayAbility cost effect class reference."), OutRelations);
	}
	if (!CooldownEffectClassPath.IsEmpty())
	{
		const FString CooldownEffectId = AddGameplayEffectClassEntity(ProjectId, CooldownEffect, CooldownEffectClassPath, AssetPath, OutEntities);
		AddGASRelation(ProjectId, TEXT("gameplay_ability_cooldown_effect"), AbilityId, CooldownEffectId, AssetPath, TEXT("GameplayAbility cooldown effect class reference."), OutRelations);
	}

	TArray<TSharedPtr<FJsonValue>> TriggerValues;
	for (int32 TriggerIndex = 0; TriggerIndex < Triggers.Num(); ++TriggerIndex)
	{
		const FString TriggerId = AddGameplayAbilityTriggerEntity(ProjectId, AbilityId, AbilityClassPath, Triggers[TriggerIndex], TriggerIndex, AssetPath, OutEntities);
		AddGASRelation(ProjectId, TEXT("contains_gameplay_ability_trigger"), AbilityId, TriggerId, AssetPath, TEXT("GameplayAbility contains an activation trigger."), OutRelations);
		TriggerValues.Add(MakeShared<FJsonValueObject>(GameplayAbilityTriggerSnapshot(TriggerId, Triggers[TriggerIndex], TriggerIndex)));
	}
	Object->SetNumberField(TEXT("trigger_count"), TriggerValues.Num());
	Object->SetArrayField(TEXT("triggers"), TriggerValues);
	return Object;
}

TSharedRef<FJsonObject> GameplayEffectModifierSnapshot(const FString& ModifierId, const FGameplayModifierInfo& Modifier, int32 ModifierIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), ModifierId);
	Object->SetNumberField(TEXT("index"), ModifierIndex);
	Object->SetStringField(TEXT("attribute"), GameplayAttributeName(Modifier.Attribute));
	Object->SetStringField(TEXT("attribute_property_path"), GameplayAttributePropertyPath(Modifier.Attribute));
	Object->SetStringField(TEXT("modifier_op"), EGameplayModOpToString(Modifier.ModifierOp));
	Object->SetObjectField(TEXT("magnitude"), MagnitudeSnapshot(Modifier.ModifierMagnitude));
	Object->SetArrayField(TEXT("source_required_tags"), GameplayTagsToJson(Modifier.SourceTags.RequireTags));
	Object->SetArrayField(TEXT("source_ignored_tags"), GameplayTagsToJson(Modifier.SourceTags.IgnoreTags));
	Object->SetArrayField(TEXT("target_required_tags"), GameplayTagsToJson(Modifier.TargetTags.RequireTags));
	Object->SetArrayField(TEXT("target_ignored_tags"), GameplayTagsToJson(Modifier.TargetTags.IgnoreTags));
	return Object;
}

TSharedRef<FJsonObject> GameplayEffectExecutionSnapshot(const FGameplayEffectExecutionDefinition& Execution, int32 ExecutionIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), ExecutionIndex);
	Object->SetStringField(TEXT("calculation_class"), GASSubclassPath(Execution.CalculationClass));
	Object->SetNumberField(TEXT("passed_in_tag_requirement_count"), Execution.PassedInTags.Num());
	Object->SetNumberField(TEXT("calculation_modifier_count"), Execution.CalculationModifiers.Num());
	return Object;
}

TSharedRef<FJsonObject> GameplayEffectCueSnapshot(const FGameplayEffectCue& Cue, int32 CueIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), CueIndex);
	Object->SetStringField(TEXT("magnitude_attribute"), GameplayAttributeName(Cue.MagnitudeAttribute));
	Object->SetStringField(TEXT("magnitude_attribute_property_path"), GameplayAttributePropertyPath(Cue.MagnitudeAttribute));
	Object->SetNumberField(TEXT("min_level"), Cue.MinLevel);
	Object->SetNumberField(TEXT("max_level"), Cue.MaxLevel);
	Object->SetArrayField(TEXT("gameplay_cue_tags"), GameplayTagsToJson(Cue.GameplayCueTags));
	return Object;
}

TSharedRef<FJsonObject> GrantedAbilitySnapshot(const FGameplayAbilitySpecDef& GrantedAbility, int32 AbilityIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), AbilityIndex);
	Object->SetStringField(TEXT("ability_class"), GASSubclassPath(GrantedAbility.Ability));
	Object->SetObjectField(TEXT("level"), ScalableFloatSnapshot(GrantedAbility.LevelScalableFloat));
	Object->SetNumberField(TEXT("input_id"), GrantedAbility.InputID);
	Object->SetStringField(TEXT("removal_policy"), GASEnumString(GrantedAbility.RemovalPolicy));
	return Object;
}

TSharedRef<FJsonObject> GameplayEffectSnapshot(
	const UObject& Asset,
	const UGameplayEffect& Effect,
	const FString& EffectId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations,
	const FString& ProjectId)
{
	const FString AssetPath = Asset.GetPathName();
	const FString EffectClassPath = GASClassPath(Effect.GetClass());
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.gameplay_effect.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), EffectId);
	Object->SetStringField(TEXT("asset_path"), AssetPath);
	Object->SetStringField(TEXT("effect_object_path"), Effect.GetPathName());
	Object->SetStringField(TEXT("effect_class_path"), EffectClassPath);
	Object->SetStringField(TEXT("duration_policy"), GASEnumString(Effect.DurationPolicy));
	Object->SetObjectField(TEXT("duration_magnitude"), MagnitudeSnapshot(Effect.DurationMagnitude));
	Object->SetObjectField(TEXT("period"), ScalableFloatSnapshot(Effect.Period));
	Object->SetBoolField(TEXT("execute_periodic_effect_on_application"), Effect.bExecutePeriodicEffectOnApplication);
	Object->SetStringField(TEXT("periodic_inhibition_policy"), GASEnumString(Effect.PeriodicInhibitionPolicy));
	Object->SetBoolField(TEXT("deny_overflow_application"), Effect.bDenyOverflowApplication);
	Object->SetBoolField(TEXT("clear_stack_on_overflow"), Effect.bClearStackOnOverflow);
	Object->SetBoolField(TEXT("require_modifier_success_to_trigger_cues"), Effect.bRequireModifierSuccessToTriggerCues);
	Object->SetBoolField(TEXT("suppress_stacking_cues"), Effect.bSuppressStackingCues);
	Object->SetStringField(TEXT("stacking_type"), GASEnumString(Effect.StackingType));
	Object->SetNumberField(TEXT("stack_limit_count"), Effect.StackLimitCount);
	Object->SetStringField(TEXT("stack_duration_refresh_policy"), GASEnumString(Effect.StackDurationRefreshPolicy));
	Object->SetStringField(TEXT("stack_period_reset_policy"), GASEnumString(Effect.StackPeriodResetPolicy));
	Object->SetStringField(TEXT("stack_expiration_policy"), GASEnumString(Effect.StackExpirationPolicy));
	Object->SetArrayField(TEXT("asset_tags"), GameplayTagsToJson(Effect.GetAssetTags()));
	Object->SetArrayField(TEXT("granted_tags"), GameplayTagsToJson(Effect.GetGrantedTags()));
	Object->SetArrayField(TEXT("blocked_ability_tags"), GameplayTagsToJson(Effect.GetBlockedAbilityTags()));

	TArray<TSharedPtr<FJsonValue>> ModifierValues;
	for (int32 ModifierIndex = 0; ModifierIndex < Effect.Modifiers.Num(); ++ModifierIndex)
	{
		const FString ModifierId = AddGameplayEffectModifierEntity(ProjectId, EffectId, EffectClassPath, Effect.Modifiers[ModifierIndex], ModifierIndex, AssetPath, OutEntities);
		AddGASRelation(ProjectId, TEXT("contains_gameplay_effect_modifier"), EffectId, ModifierId, AssetPath, TEXT("GameplayEffect contains a modifier definition."), OutRelations);
		ModifierValues.Add(MakeShared<FJsonValueObject>(GameplayEffectModifierSnapshot(ModifierId, Effect.Modifiers[ModifierIndex], ModifierIndex)));
	}
	Object->SetNumberField(TEXT("modifier_count"), ModifierValues.Num());
	Object->SetArrayField(TEXT("modifiers"), ModifierValues);

	TArray<TSharedPtr<FJsonValue>> ExecutionValues;
	for (int32 ExecutionIndex = 0; ExecutionIndex < Effect.Executions.Num(); ++ExecutionIndex)
	{
		ExecutionValues.Add(MakeShared<FJsonValueObject>(GameplayEffectExecutionSnapshot(Effect.Executions[ExecutionIndex], ExecutionIndex)));
	}
	Object->SetNumberField(TEXT("execution_count"), ExecutionValues.Num());
	Object->SetArrayField(TEXT("executions"), ExecutionValues);

	TArray<TSharedPtr<FJsonValue>> CueValues;
	for (int32 CueIndex = 0; CueIndex < Effect.GameplayCues.Num(); ++CueIndex)
	{
		CueValues.Add(MakeShared<FJsonValueObject>(GameplayEffectCueSnapshot(Effect.GameplayCues[CueIndex], CueIndex)));
		TArray<FGameplayTag> CueTags;
		Effect.GameplayCues[CueIndex].GameplayCueTags.GetGameplayTagArray(CueTags);
		for (const FGameplayTag& CueTag : CueTags)
		{
			if (!CueTag.IsValid())
			{
				continue;
			}
			const FString CueEntityId = MakeStableId(ProjectId, TEXT("gameplay_cue_tag"), CueTag.ToString());
			if (!FindGASEntity(OutEntities, CueEntityId))
			{
				FEntityRecord CueTagEntity;
				CueTagEntity.Id = CueEntityId;
				CueTagEntity.Kind = TEXT("gameplay_cue_tag");
				CueTagEntity.CanonicalKey = CueTag.ToString();
				CueTagEntity.DisplayName = CueTag.ToString();
				CueTagEntity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
				CueTagEntity.Attributes.Add(TEXT("gameplay_cue_tag"), CueTag.ToString());
				CueTagEntity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
				CueTagEntity.Completeness.State = ECompletenessState::Complete;
				CueTagEntity.Completeness.Covered = { TEXT("gameplay_cue_tag_reference") };
				AddGASEvidence(CueTagEntity, AssetPath, TEXT("GameplayCue tag referenced by GameplayEffect cue definition."));
				OutEntities.Add(MoveTemp(CueTagEntity));
			}
			TMap<FString, FString> Attributes;
			Attributes.Add(TEXT("cue_index"), FString::FromInt(CueIndex));
			Attributes.Add(TEXT("gameplay_cue_tag"), CueTag.ToString());
			AddGASRelation(ProjectId, TEXT("triggers_gameplay_cue"), EffectId, CueEntityId, AssetPath, TEXT("GameplayEffect cue emits this GameplayCue tag."), OutRelations, &Attributes);
		}
	}
	Object->SetNumberField(TEXT("gameplay_cue_count"), CueValues.Num());
	Object->SetArrayField(TEXT("gameplay_cues"), CueValues);

	TArray<TSharedPtr<FJsonValue>> OverflowValues;
	for (int32 OverflowIndex = 0; OverflowIndex < Effect.OverflowEffects.Num(); ++OverflowIndex)
	{
		const TSubclassOf<UGameplayEffect> OverflowClass = Effect.OverflowEffects[OverflowIndex];
		TSharedRef<FJsonObject> OverflowObject = MakeShared<FJsonObject>();
		const FString OverflowClassPath = GASSubclassPath(OverflowClass);
		OverflowObject->SetNumberField(TEXT("index"), OverflowIndex);
		OverflowObject->SetStringField(TEXT("effect_class"), OverflowClassPath);
		OverflowValues.Add(MakeShared<FJsonValueObject>(OverflowObject));
		const FString OverflowId = AddGameplayEffectClassEntity(ProjectId, OverflowClass ? Cast<UGameplayEffect>(OverflowClass->GetDefaultObject()) : nullptr, OverflowClassPath, AssetPath, OutEntities);
		if (!OverflowId.IsEmpty())
		{
			TMap<FString, FString> Attributes;
			Attributes.Add(TEXT("overflow_index"), FString::FromInt(OverflowIndex));
			AddGASRelation(ProjectId, TEXT("overflows_to_gameplay_effect"), EffectId, OverflowId, AssetPath, TEXT("GameplayEffect overflow applies another GameplayEffect."), OutRelations, &Attributes);
		}
	}
	Object->SetNumberField(TEXT("overflow_effect_count"), OverflowValues.Num());
	Object->SetArrayField(TEXT("overflow_effects"), OverflowValues);

	TArray<TSharedPtr<FJsonValue>> GrantedAbilityValues;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	const TArray<FGameplayAbilitySpecDef>& GrantedAbilities = Effect.GrantedAbilities;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
	for (int32 AbilityIndex = 0; AbilityIndex < GrantedAbilities.Num(); ++AbilityIndex)
	{
		const FGameplayAbilitySpecDef& GrantedAbility = GrantedAbilities[AbilityIndex];
		GrantedAbilityValues.Add(MakeShared<FJsonValueObject>(GrantedAbilitySnapshot(GrantedAbility, AbilityIndex)));
		const FString AbilityClassPath = GASSubclassPath(GrantedAbility.Ability);
		if (!AbilityClassPath.IsEmpty())
		{
			const FString AbilityId = MakeStableId(ProjectId, TEXT("gameplay_ability"), AbilityClassPath);
			if (!FindGASEntity(OutEntities, AbilityId))
			{
				FEntityRecord AbilityEntity;
				AbilityEntity.Id = AbilityId;
				AbilityEntity.Kind = TEXT("gameplay_ability");
				AbilityEntity.CanonicalKey = AbilityClassPath;
				AbilityEntity.DisplayName = FPackageName::ObjectPathToObjectName(AbilityClassPath);
				AbilityEntity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
				AbilityEntity.Attributes.Add(TEXT("ability_class_path"), AbilityClassPath);
				AbilityEntity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
				AbilityEntity.Completeness.State = ECompletenessState::MetadataOnly;
				AbilityEntity.Completeness.Covered = { TEXT("gameplay_ability_class_reference") };
				AbilityEntity.Completeness.Omitted = { TEXT("gameplay_ability_default_object") };
				AddGASEvidence(AbilityEntity, AssetPath, TEXT("GameplayAbility class referenced by GameplayEffect granted ability definition."));
				OutEntities.Add(MoveTemp(AbilityEntity));
			}
			TMap<FString, FString> Attributes;
			Attributes.Add(TEXT("granted_ability_index"), FString::FromInt(AbilityIndex));
			AddGASRelation(ProjectId, TEXT("grants_gameplay_ability"), EffectId, AbilityId, AssetPath, TEXT("GameplayEffect grants this GameplayAbility class."), OutRelations, &Attributes);
		}
	}
	Object->SetNumberField(TEXT("granted_ability_count"), GrantedAbilityValues.Num());
	Object->SetArrayField(TEXT("granted_abilities"), GrantedAbilityValues);
	return Object;
}

TSharedRef<FJsonObject> StaticCueSnapshot(const UObject& Asset, const UGameplayCueNotify_Static& Cue, const FString& CueId)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.gameplay_cue_notify.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), CueId);
	Object->SetStringField(TEXT("asset_path"), Asset.GetPathName());
	Object->SetStringField(TEXT("cue_object_path"), Cue.GetPathName());
	Object->SetStringField(TEXT("cue_class_path"), GASClassPath(Cue.GetClass()));
	Object->SetStringField(TEXT("cue_type"), TEXT("static"));
	Object->SetStringField(TEXT("gameplay_cue_tag"), Cue.GameplayCueTag.ToString());
	Object->SetStringField(TEXT("gameplay_cue_name"), Cue.GameplayCueName.ToString());
	Object->SetBoolField(TEXT("is_override"), Cue.IsOverride);
	return Object;
}

TSharedRef<FJsonObject> ActorCueSnapshot(const UObject& Asset, const AGameplayCueNotify_Actor& Cue, const FString& CueId)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.gameplay_cue_notify.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), CueId);
	Object->SetStringField(TEXT("asset_path"), Asset.GetPathName());
	Object->SetStringField(TEXT("cue_object_path"), Cue.GetPathName());
	Object->SetStringField(TEXT("cue_class_path"), GASClassPath(Cue.GetClass()));
	Object->SetStringField(TEXT("cue_type"), TEXT("actor"));
	Object->SetStringField(TEXT("gameplay_cue_tag"), Cue.GameplayCueTag.ToString());
	Object->SetStringField(TEXT("gameplay_cue_name"), Cue.GameplayCueName.ToString());
	Object->SetBoolField(TEXT("is_override"), Cue.IsOverride);
	Object->SetBoolField(TEXT("auto_destroy_on_remove"), Cue.bAutoDestroyOnRemove);
	Object->SetNumberField(TEXT("auto_destroy_delay"), Cue.AutoDestroyDelay);
	Object->SetBoolField(TEXT("warn_if_timeline_is_still_running"), Cue.WarnIfTimelineIsStillRunning);
	Object->SetBoolField(TEXT("warn_if_latent_action_is_still_running"), Cue.WarnIfLatentActionIsStillRunning);
	Object->SetBoolField(TEXT("auto_attach_to_owner"), Cue.bAutoAttachToOwner);
	Object->SetBoolField(TEXT("unique_instance_per_instigator"), Cue.bUniqueInstancePerInstigator);
	Object->SetBoolField(TEXT("unique_instance_per_source_object"), Cue.bUniqueInstancePerSourceObject);
	Object->SetBoolField(TEXT("allow_multiple_on_active_events"), Cue.bAllowMultipleOnActiveEvents);
	Object->SetBoolField(TEXT("allow_multiple_while_active_events"), Cue.bAllowMultipleWhileActiveEvents);
	Object->SetNumberField(TEXT("num_preallocated_instances"), Cue.NumPreallocatedInstances);
	return Object;
}
bool FGASReader::AppendGASAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (const UGameplayAbility* Ability = GASAssetOrBlueprintDefaultObject<UGameplayAbility>(Asset))
	{
		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}

		const FString AbilityId = AddGameplayAbilityEntity(ProjectId, *Ability, Asset.GetPathName(), OutEntities);
		AddGASRelation(ProjectId, TEXT("contains_gameplay_ability"), AssetEntity.Id, AbilityId, Asset.GetPathName(), TEXT("Asset contains the extracted GameplayAbility record."), OutRelations);
		AssetEntity.Snapshot->SetObjectField(TEXT("gameplay_ability"), GameplayAbilitySnapshot(Asset, *Ability, AbilityId, OutEntities, OutRelations, ProjectId));
		AssetEntity.Attributes.Add(TEXT("gameplay_ability_class"), GASClassPath(Ability->GetClass()));
		AssetEntity.Attributes.Add(TEXT("gameplay_ability_trigger_count"), FString::FromInt(CopyAbilityTriggers(*Ability).Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("gameplay_ability_defaults"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("gameplay_ability_triggers"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_activation_state"));
		AddGASEvidence(AssetEntity, Asset.GetPathName(), TEXT("GameplayAbility structure extracted from default object."));
		return true;
	}

	if (const UGameplayEffect* Effect = GASAssetOrBlueprintDefaultObject<UGameplayEffect>(Asset))
	{
		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}

		const FString EffectId = AddGameplayEffectEntity(ProjectId, *Effect, Asset.GetPathName(), OutEntities);
		AddGASRelation(ProjectId, TEXT("contains_gameplay_effect"), AssetEntity.Id, EffectId, Asset.GetPathName(), TEXT("Asset contains the extracted GameplayEffect record."), OutRelations);
		AssetEntity.Snapshot->SetObjectField(TEXT("gameplay_effect"), GameplayEffectSnapshot(Asset, *Effect, EffectId, OutEntities, OutRelations, ProjectId));
		AssetEntity.Attributes.Add(TEXT("gameplay_effect_class"), GASClassPath(Effect->GetClass()));
		AssetEntity.Attributes.Add(TEXT("gameplay_effect_modifier_count"), FString::FromInt(Effect->Modifiers.Num()));
		AssetEntity.Attributes.Add(TEXT("gameplay_effect_cue_count"), FString::FromInt(Effect->GameplayCues.Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("gameplay_effect_defaults"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("gameplay_effect_modifiers"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("gameplay_effect_cues"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_effect_specs"));
		AddGASEvidence(AssetEntity, Asset.GetPathName(), TEXT("GameplayEffect structure extracted from default object."));
		return true;
	}

	if (const UGameplayCueNotify_Static* StaticCue = GASAssetOrBlueprintDefaultObject<UGameplayCueNotify_Static>(Asset))
	{
		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}

		const FString CueId = AddGameplayCueNotifyEntity(ProjectId, *StaticCue, TEXT("static"), StaticCue->GameplayCueTag.ToString(), Asset.GetPathName(), OutEntities);
		AddGASRelation(ProjectId, TEXT("contains_gameplay_cue_notify"), AssetEntity.Id, CueId, Asset.GetPathName(), TEXT("Asset contains the extracted static GameplayCueNotify record."), OutRelations);
		AssetEntity.Snapshot->SetObjectField(TEXT("gameplay_cue_notify"), StaticCueSnapshot(Asset, *StaticCue, CueId));
		AssetEntity.Attributes.Add(TEXT("gameplay_cue_notify_class"), GASClassPath(StaticCue->GetClass()));
		AssetEntity.Attributes.Add(TEXT("gameplay_cue_type"), TEXT("static"));
		AssetEntity.Attributes.Add(TEXT("gameplay_cue_tag"), StaticCue->GameplayCueTag.ToString());
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("gameplay_cue_notify_defaults"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_cue_dispatch"));
		AddGASEvidence(AssetEntity, Asset.GetPathName(), TEXT("Static GameplayCueNotify structure extracted from default object."));
		return true;
	}

	if (const AGameplayCueNotify_Actor* ActorCue = GASAssetOrBlueprintDefaultObject<AGameplayCueNotify_Actor>(Asset))
	{
		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}

		const FString CueId = AddGameplayCueNotifyEntity(ProjectId, *ActorCue, TEXT("actor"), ActorCue->GameplayCueTag.ToString(), Asset.GetPathName(), OutEntities);
		AddGASRelation(ProjectId, TEXT("contains_gameplay_cue_notify"), AssetEntity.Id, CueId, Asset.GetPathName(), TEXT("Asset contains the extracted actor GameplayCueNotify record."), OutRelations);
		AssetEntity.Snapshot->SetObjectField(TEXT("gameplay_cue_notify"), ActorCueSnapshot(Asset, *ActorCue, CueId));
		AssetEntity.Attributes.Add(TEXT("gameplay_cue_notify_class"), GASClassPath(ActorCue->GetClass()));
		AssetEntity.Attributes.Add(TEXT("gameplay_cue_type"), TEXT("actor"));
		AssetEntity.Attributes.Add(TEXT("gameplay_cue_tag"), ActorCue->GameplayCueTag.ToString());
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("gameplay_cue_notify_defaults"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_cue_dispatch"));
		AddGASEvidence(AssetEntity, Asset.GetPathName(), TEXT("Actor GameplayCueNotify structure extracted from default object."));
		return true;
	}

	return false;
}
}

#else

namespace UE::ProjectIntelligence
{
bool FGASReader::AppendGASAsset(
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
