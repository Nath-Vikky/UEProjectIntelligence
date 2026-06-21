#include "UEPINiagaraReader.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/UnrealType.h"
#include "VectorField/VectorFieldStatic.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString NiagaraBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString NiagaraGuidString(const FGuid& Guid)
{
	return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString();
}

FString NiagaraEnumValue(const UEnum* Enum, int64 Value)
{
	if (Enum)
	{
		return Enum->GetNameStringByValue(Value);
	}
	return FString::FromInt(static_cast<int32>(Value));
}

FString NiagaraScriptUsageString(ENiagaraScriptUsage Usage)
{
	return NiagaraEnumValue(StaticEnum<ENiagaraScriptUsage>(), static_cast<int64>(Usage));
}

FString NiagaraVariableTypeName(const FNiagaraVariableBase& Variable)
{
	return Variable.GetType().GetName();
}

FString NiagaraParameterValueString(const FNiagaraVariable& Variable)
{
	if (!Variable.IsDataAllocated())
	{
		return FString();
	}
	return Variable.GetType().ToString(Variable.GetData());
}

FString NiagaraParameterNamespace(const FNiagaraVariableBase& Variable)
{
	const FString Name = Variable.GetName().ToString();
	int32 DotIndex = INDEX_NONE;
	if (Name.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
	{
		return Name.Left(DotIndex);
	}
	return FString();
}

void AddNiagaraEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

void AddNiagaraRelation(
	const FString& ProjectId,
	const FString& Type,
	const FString& FromId,
	const FString& ToId,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FRelationRecord>& OutRelations,
	const TMap<FString, FString>& Attributes = TMap<FString, FString>())
{
	FRelationRecord Relation;
	Relation.Id = MakeRelationId(ProjectId, Type, FromId, ToId, Attributes.Num() > 0 ? &Attributes : nullptr);
	Relation.Type = Type;
	Relation.FromId = FromId;
	Relation.ToId = ToId;
	Relation.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Relation.bDerived = false;
	Relation.Confidence = 1.0f;
	Relation.Attributes = Attributes;
	Relation.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		EvidencePath,
		EvidenceDetail
	});
	OutRelations.Add(MoveTemp(Relation));
}

FEntityRecord* FindNiagaraEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

TSharedRef<FJsonObject> NiagaraVectorObject(const FVector& Vector)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Vector.X);
	Object->SetNumberField(TEXT("y"), Vector.Y);
	Object->SetNumberField(TEXT("z"), Vector.Z);
	return Object;
}

int64 VectorFieldVoxelCount(const UVectorFieldStatic& VectorField)
{
	if (VectorField.SizeX <= 0 || VectorField.SizeY <= 0 || VectorField.SizeZ <= 0)
	{
		return 0;
	}
	return static_cast<int64>(VectorField.SizeX) * static_cast<int64>(VectorField.SizeY) * static_cast<int64>(VectorField.SizeZ);
}

FString AddVectorFieldEntity(
	const FString& ProjectId,
	const UVectorFieldStatic& VectorField,
	TArray<FEntityRecord>& OutEntities)
{
	const FString VectorFieldPath = VectorField.GetPathName();
	const FString VectorFieldId = MakeStableId(ProjectId, TEXT("vector_field_static"), VectorFieldPath);
	if (FindNiagaraEntity(OutEntities, VectorFieldId))
	{
		return VectorFieldId;
	}

	FEntityRecord Entity;
	Entity.Id = VectorFieldId;
	Entity.Kind = TEXT("vector_field_static");
	Entity.CanonicalKey = VectorFieldPath;
	Entity.DisplayName = VectorField.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("vector_field_path"), VectorFieldPath);
	Entity.Attributes.Add(TEXT("vector_field_class"), VectorField.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("size_x"), FString::FromInt(VectorField.SizeX));
	Entity.Attributes.Add(TEXT("size_y"), FString::FromInt(VectorField.SizeY));
	Entity.Attributes.Add(TEXT("size_z"), FString::FromInt(VectorField.SizeZ));
	Entity.Attributes.Add(TEXT("voxel_count"), FString::Printf(TEXT("%lld"), static_cast<long long>(VectorFieldVoxelCount(VectorField))));
	Entity.Attributes.Add(TEXT("intensity"), FString::SanitizeFloat(VectorField.Intensity));
	Entity.Attributes.Add(TEXT("allow_cpu_access"), NiagaraBool(VectorField.bAllowCPUAccess));
	Entity.Attributes.Add(TEXT("source_data_size_bytes"), FString::Printf(TEXT("%lld"), static_cast<long long>(VectorField.SourceData.GetBulkDataSize())));
	Entity.Attributes.Add(TEXT("has_cpu_data"), NiagaraBool(VectorField.HasCPUData()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("vector_field_dimensions"), TEXT("bounds"), TEXT("source_bulk_data_summary") };
	Entity.Completeness.Omitted = { TEXT("vector_samples"), TEXT("gpu_volume_texture"), TEXT("runtime_vector_field_instance") };
	AddNiagaraEvidence(Entity, VectorFieldPath, TEXT("VectorFieldStatic metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return VectorFieldId;
}

TSharedRef<FJsonObject> VectorFieldSnapshot(const UVectorFieldStatic& VectorField)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.vector_field_static.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("vector_field_path"), VectorField.GetPathName());
	Object->SetStringField(TEXT("vector_field_class"), VectorField.GetClass()->GetPathName());
	Object->SetNumberField(TEXT("size_x"), VectorField.SizeX);
	Object->SetNumberField(TEXT("size_y"), VectorField.SizeY);
	Object->SetNumberField(TEXT("size_z"), VectorField.SizeZ);
	Object->SetNumberField(TEXT("voxel_count"), static_cast<double>(VectorFieldVoxelCount(VectorField)));
	Object->SetNumberField(TEXT("intensity"), VectorField.Intensity);
	Object->SetObjectField(TEXT("bounds_min"), NiagaraVectorObject(VectorField.Bounds.Min));
	Object->SetObjectField(TEXT("bounds_max"), NiagaraVectorObject(VectorField.Bounds.Max));
	Object->SetObjectField(TEXT("bounds_extent"), NiagaraVectorObject(VectorField.Bounds.GetExtent()));
	Object->SetBoolField(TEXT("allow_cpu_access"), VectorField.bAllowCPUAccess);
	Object->SetNumberField(TEXT("source_data_size_bytes"), static_cast<double>(VectorField.SourceData.GetBulkDataSize()));
	Object->SetBoolField(TEXT("has_cpu_data"), VectorField.HasCPUData());
	Object->SetNumberField(TEXT("cpu_value_count"), VectorField.HasCPUData() ? VectorField.ReadCPUData().Num() : 0);
	return Object;
}

bool IsNiagaraParameterDefinitionsAsset(const UObject& Asset)
{
	return Asset.GetClass() && Asset.GetClass()->GetPathName() == TEXT("/Script/NiagaraEditor.NiagaraParameterDefinitions");
}

FString NiagaraPropertyText(const UObject& Object, const TCHAR* PropertyName)
{
	const FProperty* Property = FindFProperty<FProperty>(Object.GetClass(), PropertyName);
	if (!Property)
	{
		return FString();
	}

	FString Value;
	const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(&Object);
	Property->ExportTextItem_Direct(Value, ValuePtr, nullptr, const_cast<UObject*>(&Object), PPF_None);
	return Value;
}

bool NiagaraBoolProperty(const UObject& Object, const TCHAR* PropertyName, bool bDefault = false)
{
	if (const FBoolProperty* Property = FindFProperty<FBoolProperty>(Object.GetClass(), PropertyName))
	{
		return Property->GetPropertyValue_InContainer(&Object);
	}
	return bDefault;
}

int32 NiagaraIntegerProperty(const UObject& Object, const TCHAR* PropertyName, int32 DefaultValue = 0)
{
	if (const FIntProperty* Property = FindFProperty<FIntProperty>(Object.GetClass(), PropertyName))
	{
		return Property->GetPropertyValue_InContainer(&Object);
	}

	if (const FNumericProperty* Property = FindFProperty<FNumericProperty>(Object.GetClass(), PropertyName))
	{
		if (Property->IsInteger())
		{
			return FCString::Atoi(*NiagaraPropertyText(Object, PropertyName));
		}
	}
	return DefaultValue;
}

FGuid NiagaraGuidProperty(const UObject& Object, const TCHAR* PropertyName)
{
	if (const FStructProperty* Property = FindFProperty<FStructProperty>(Object.GetClass(), PropertyName))
	{
		if (Property->Struct && Property->Struct->GetFName() == TEXT("Guid"))
		{
			return *static_cast<const FGuid*>(Property->ContainerPtrToValuePtr<void>(&Object));
		}
	}
	return FGuid();
}

const FNiagaraVariable* NiagaraScriptVariableValue(const UObject& ScriptVariable)
{
	if (const FStructProperty* Property = FindFProperty<FStructProperty>(ScriptVariable.GetClass(), TEXT("Variable")))
	{
		if (Property->Struct == FNiagaraVariable::StaticStruct())
		{
			return static_cast<const FNiagaraVariable*>(Property->ContainerPtrToValuePtr<void>(&ScriptVariable));
		}
	}
	return nullptr;
}

const FNiagaraVariableMetaData* NiagaraScriptVariableMetadata(const UObject& ScriptVariable)
{
	if (const FStructProperty* Property = FindFProperty<FStructProperty>(ScriptVariable.GetClass(), TEXT("Metadata")))
	{
		if (Property->Struct == FNiagaraVariableMetaData::StaticStruct())
		{
			return static_cast<const FNiagaraVariableMetaData*>(Property->ContainerPtrToValuePtr<void>(&ScriptVariable));
		}
	}
	return nullptr;
}

TArray<UObject*> NiagaraDefinitionScriptVariables(const UObject& Definitions)
{
	TArray<UObject*> ScriptVariables;
	const FArrayProperty* ScriptVariablesProperty = FindFProperty<FArrayProperty>(Definitions.GetClass(), TEXT("ScriptVariables"));
	const FObjectPropertyBase* InnerObjectProperty = ScriptVariablesProperty ? CastField<FObjectPropertyBase>(ScriptVariablesProperty->Inner) : nullptr;
	if (!ScriptVariablesProperty || !InnerObjectProperty)
	{
		return ScriptVariables;
	}

	const void* ArrayPtr = ScriptVariablesProperty->ContainerPtrToValuePtr<void>(&Definitions);
	FScriptArrayHelper Helper(ScriptVariablesProperty, ArrayPtr);
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
	{
		if (UObject* ScriptVariable = InnerObjectProperty->GetObjectPropertyValue(Helper.GetRawPtr(Index)))
		{
			ScriptVariables.Add(ScriptVariable);
		}
	}
	return ScriptVariables;
}

FString NiagaraParameterDefinitionsKey(const UObject& Definitions)
{
	return Definitions.GetPathName();
}

FString NiagaraParameterDefinitionKey(const UObject& Definitions, const UObject& ScriptVariable, int32 Index)
{
	const FNiagaraVariable* Variable = NiagaraScriptVariableValue(ScriptVariable);
	const FNiagaraVariableMetaData* Metadata = NiagaraScriptVariableMetadata(ScriptVariable);
	const FString VariableGuid = Metadata && Metadata->GetVariableGuid().IsValid()
		? Metadata->GetVariableGuid().ToString(EGuidFormats::DigitsWithHyphens)
		: FString::FromInt(Index);
	const FString Name = Variable ? Variable->GetName().ToString() : ScriptVariable.GetName();
	const FString Type = Variable ? NiagaraVariableTypeName(*Variable) : FString();
	return FString::Printf(
		TEXT("%s:parameter_definition:%s:%s:%s"),
		*NiagaraParameterDefinitionsKey(Definitions),
		*VariableGuid,
		*Name,
		*Type);
}

FString AddNiagaraParameterDefinitionsEntity(
	const FString& ProjectId,
	const UObject& Definitions,
	int32 ParameterCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString DefinitionsPath = NiagaraParameterDefinitionsKey(Definitions);
	const FString DefinitionsId = MakeStableId(ProjectId, TEXT("niagara_parameter_definitions"), DefinitionsPath);
	if (FindNiagaraEntity(OutEntities, DefinitionsId))
	{
		return DefinitionsId;
	}

	const FGuid UniqueId = NiagaraGuidProperty(Definitions, TEXT("UniqueId"));

	FEntityRecord Entity;
	Entity.Id = DefinitionsId;
	Entity.Kind = TEXT("niagara_parameter_definitions");
	Entity.CanonicalKey = DefinitionsPath;
	Entity.DisplayName = Definitions.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("definitions_path"), DefinitionsPath);
	Entity.Attributes.Add(TEXT("definitions_class"), Definitions.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("definitions_unique_id"), UniqueId.IsValid() ? UniqueId.ToString(EGuidFormats::DigitsWithHyphens) : FString());
	Entity.Attributes.Add(TEXT("parameter_count"), FString::FromInt(ParameterCount));
	Entity.Attributes.Add(TEXT("promote_to_top_in_add_menus"), NiagaraBool(NiagaraBoolProperty(Definitions, TEXT("bPromoteToTopInAddMenus"))));
	Entity.Attributes.Add(TEXT("menu_sort_order"), FString::FromInt(NiagaraIntegerProperty(Definitions, TEXT("MenuSortOrder"))));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("definition_identity"), TEXT("definition_preferences"), TEXT("script_variable_metadata") };
	Entity.Completeness.Omitted = { TEXT("parameter_default_bytes"), TEXT("subscription_synchronization"), TEXT("parameter_map_usage") };
	AddNiagaraEvidence(Entity, DefinitionsPath, TEXT("Niagara Parameter Definitions metadata read through reflected serialized properties."));
	OutEntities.Add(MoveTemp(Entity));
	return DefinitionsId;
}

FString AddNiagaraParameterDefinitionEntity(
	const FString& ProjectId,
	const UObject& Definitions,
	const UObject& ScriptVariable,
	int32 Index,
	TArray<FEntityRecord>& OutEntities)
{
	const FString DefinitionKey = NiagaraParameterDefinitionKey(Definitions, ScriptVariable, Index);
	const FString DefinitionId = MakeStableId(ProjectId, TEXT("niagara_parameter_definition"), DefinitionKey);
	if (FindNiagaraEntity(OutEntities, DefinitionId))
	{
		return DefinitionId;
	}

	const FNiagaraVariable* Variable = NiagaraScriptVariableValue(ScriptVariable);
	const FNiagaraVariableMetaData* Metadata = NiagaraScriptVariableMetadata(ScriptVariable);
	const FString Name = Variable ? Variable->GetName().ToString() : ScriptVariable.GetName();
	const FString Type = Variable ? NiagaraVariableTypeName(*Variable) : FString();
	const FGuid VariableGuid = Metadata ? Metadata->GetVariableGuid() : FGuid();
	const FGuid ChangeId = NiagaraGuidProperty(ScriptVariable, TEXT("ChangeId"));

	FEntityRecord Entity;
	Entity.Id = DefinitionId;
	Entity.Kind = TEXT("niagara_parameter_definition");
	Entity.CanonicalKey = DefinitionKey;
	Entity.DisplayName = Name;
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("definitions_path"), NiagaraParameterDefinitionsKey(Definitions));
	Entity.Attributes.Add(TEXT("parameter_index"), FString::FromInt(Index));
	Entity.Attributes.Add(TEXT("name"), Name);
	Entity.Attributes.Add(TEXT("namespace"), Variable ? NiagaraParameterNamespace(*Variable) : FString());
	Entity.Attributes.Add(TEXT("type"), Type);
	Entity.Attributes.Add(TEXT("size_bytes"), Variable ? FString::FromInt(Variable->GetSizeInBytes()) : TEXT("0"));
	Entity.Attributes.Add(TEXT("default_mode"), NiagaraPropertyText(ScriptVariable, TEXT("DefaultMode")));
	Entity.Attributes.Add(TEXT("variable_guid"), VariableGuid.IsValid() ? VariableGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
	Entity.Attributes.Add(TEXT("change_id"), ChangeId.IsValid() ? ChangeId.ToString(EGuidFormats::DigitsWithHyphens) : FString());
	Entity.Attributes.Add(TEXT("static_switch"), NiagaraBool(NiagaraBoolProperty(ScriptVariable, TEXT("bIsStaticSwitch"))));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("parameter_identity"), TEXT("parameter_type"), TEXT("editor_metadata_summary") };
	Entity.Completeness.Omitted = { TEXT("default_value_bytes"), TEXT("graph_usage"), TEXT("runtime_parameter_store_values") };
	AddNiagaraEvidence(Entity, ScriptVariable.GetPathName(), TEXT("Niagara parameter definition read from serialized script variable properties."));
	OutEntities.Add(MoveTemp(Entity));
	return DefinitionId;
}

TSharedRef<FJsonObject> NiagaraParameterDefinitionObject(
	const FString& Id,
	const UObject& ScriptVariable,
	int32 Index)
{
	const FNiagaraVariable* Variable = NiagaraScriptVariableValue(ScriptVariable);
	const FNiagaraVariableMetaData* Metadata = NiagaraScriptVariableMetadata(ScriptVariable);
	const FGuid ChangeId = NiagaraGuidProperty(ScriptVariable, TEXT("ChangeId"));

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), Id);
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("script_variable_path"), ScriptVariable.GetPathName());
	Object->SetStringField(TEXT("name"), Variable ? Variable->GetName().ToString() : ScriptVariable.GetName());
	Object->SetStringField(TEXT("namespace"), Variable ? NiagaraParameterNamespace(*Variable) : FString());
	Object->SetStringField(TEXT("type"), Variable ? NiagaraVariableTypeName(*Variable) : FString());
	Object->SetNumberField(TEXT("size_bytes"), Variable ? Variable->GetSizeInBytes() : 0);
	Object->SetStringField(TEXT("default_mode"), NiagaraPropertyText(ScriptVariable, TEXT("DefaultMode")));
	Object->SetBoolField(TEXT("static_switch"), NiagaraBoolProperty(ScriptVariable, TEXT("bIsStaticSwitch")));
	Object->SetNumberField(TEXT("static_switch_default_value"), NiagaraIntegerProperty(ScriptVariable, TEXT("StaticSwitchDefaultValue")));
	Object->SetBoolField(TEXT("subscribed_to_parameter_definitions"), NiagaraBoolProperty(ScriptVariable, TEXT("bSubscribedToParameterDefinitions")));
	Object->SetBoolField(TEXT("overrides_parameter_definitions_default_value"), NiagaraBoolProperty(ScriptVariable, TEXT("bOverrideParameterDefinitionsDefaultValue")));
	Object->SetStringField(TEXT("change_id"), ChangeId.IsValid() ? ChangeId.ToString(EGuidFormats::DigitsWithHyphens) : FString());

	if (Metadata)
	{
		const FGuid VariableGuid = Metadata->GetVariableGuid();
		const UEnum* UnitEnum = StaticEnum<EUnit>();
		Object->SetStringField(TEXT("variable_guid"), VariableGuid.IsValid() ? VariableGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		Object->SetStringField(TEXT("description"), Metadata->Description.ToString());
		Object->SetStringField(TEXT("category_name"), Metadata->CategoryName.ToString());
		Object->SetStringField(TEXT("display_unit"), UnitEnum ? UnitEnum->GetNameStringByValue(static_cast<int64>(Metadata->DisplayUnit)) : FString::FromInt(static_cast<int32>(Metadata->DisplayUnit)));
		Object->SetBoolField(TEXT("advanced_display"), Metadata->bAdvancedDisplay);
		Object->SetBoolField(TEXT("display_in_overview_stack"), Metadata->bDisplayInOverviewStack);
		Object->SetNumberField(TEXT("inline_parameter_sort_priority"), Metadata->InlineParameterSortPriority);
		Object->SetNumberField(TEXT("editor_sort_priority"), Metadata->EditorSortPriority);
		Object->SetBoolField(TEXT("inline_edit_condition_toggle"), Metadata->bInlineEditConditionToggle);
		Object->SetStringField(TEXT("parent_attribute"), Metadata->ParentAttribute.ToString());
		Object->SetNumberField(TEXT("alternate_alias_count"), Metadata->AlternateAliases.Num());
		Object->SetNumberField(TEXT("property_metadata_count"), Metadata->PropertyMetaData.Num());
	}
	else
	{
		Object->SetStringField(TEXT("variable_guid"), FString());
		Object->SetStringField(TEXT("description"), FString());
		Object->SetStringField(TEXT("category_name"), FString());
		Object->SetStringField(TEXT("display_unit"), FString());
		Object->SetBoolField(TEXT("advanced_display"), false);
		Object->SetBoolField(TEXT("display_in_overview_stack"), false);
		Object->SetNumberField(TEXT("inline_parameter_sort_priority"), 0);
		Object->SetNumberField(TEXT("editor_sort_priority"), 0);
		Object->SetBoolField(TEXT("inline_edit_condition_toggle"), false);
		Object->SetStringField(TEXT("parent_attribute"), FString());
		Object->SetNumberField(TEXT("alternate_alias_count"), 0);
		Object->SetNumberField(TEXT("property_metadata_count"), 0);
	}

	return Object;
}

TSharedRef<FJsonObject> NiagaraParameterDefinitionsSnapshot(
	const FString& ProjectId,
	const UObject& Definitions,
	const FString& DefinitionsId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const TArray<UObject*> ScriptVariables = NiagaraDefinitionScriptVariables(Definitions);
	const FGuid UniqueId = NiagaraGuidProperty(Definitions, TEXT("UniqueId"));

	TArray<TSharedPtr<FJsonValue>> ParameterValues;
	for (int32 Index = 0; Index < ScriptVariables.Num(); ++Index)
	{
		const UObject* ScriptVariable = ScriptVariables[Index];
		if (!ScriptVariable)
		{
			continue;
		}

		const FString ParameterId = AddNiagaraParameterDefinitionEntity(ProjectId, Definitions, *ScriptVariable, Index, OutEntities);
		TMap<FString, FString> RelationAttributes;
		RelationAttributes.Add(TEXT("parameter_index"), FString::FromInt(Index));
		AddNiagaraRelation(
			ProjectId,
			TEXT("contains_niagara_parameter_definition"),
			DefinitionsId,
			ParameterId,
			Definitions.GetPathName(),
			TEXT("Niagara Parameter Definitions asset contains this script variable definition."),
			OutRelations,
			RelationAttributes);
		ParameterValues.Add(MakeShared<FJsonValueObject>(NiagaraParameterDefinitionObject(ParameterId, *ScriptVariable, Index)));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.niagara_parameter_definitions.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("definitions_path"), NiagaraParameterDefinitionsKey(Definitions));
	Object->SetStringField(TEXT("definitions_class"), Definitions.GetClass()->GetPathName());
	Object->SetStringField(TEXT("definitions_unique_id"), UniqueId.IsValid() ? UniqueId.ToString(EGuidFormats::DigitsWithHyphens) : FString());
	Object->SetBoolField(TEXT("promote_to_top_in_add_menus"), NiagaraBoolProperty(Definitions, TEXT("bPromoteToTopInAddMenus")));
	Object->SetNumberField(TEXT("menu_sort_order"), NiagaraIntegerProperty(Definitions, TEXT("MenuSortOrder")));
	Object->SetNumberField(TEXT("parameter_count"), ParameterValues.Num());
	Object->SetArrayField(TEXT("parameters"), ParameterValues);
	return Object;
}

TSharedRef<FJsonObject> NiagaraParameterObject(const FNiagaraVariable& Variable, int32 Index, const FString& Scope)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("scope"), Scope);
	Object->SetStringField(TEXT("name"), Variable.GetName().ToString());
	Object->SetStringField(TEXT("namespace"), NiagaraParameterNamespace(Variable));
	Object->SetStringField(TEXT("type"), NiagaraVariableTypeName(Variable));
	Object->SetNumberField(TEXT("size_bytes"), Variable.GetSizeInBytes());
	Object->SetBoolField(TEXT("has_default_value"), Variable.IsDataAllocated());
	Object->SetStringField(TEXT("default_value"), NiagaraParameterValueString(Variable));
	return Object;
}

FString AddNiagaraParameterEntity(
	const FString& ProjectId,
	const FString& OwnerKey,
	const FString& OwnerPath,
	const FNiagaraVariable& Variable,
	int32 Index,
	const FString& Scope,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = FString::Printf(
		TEXT("%s:parameter:%s:%s:%s"),
		*OwnerKey,
		*Scope,
		*Variable.GetName().ToString(),
		*NiagaraVariableTypeName(Variable));
	const FString ParameterId = MakeStableId(ProjectId, TEXT("niagara_parameter"), CanonicalKey);
	if (FindNiagaraEntity(OutEntities, ParameterId))
	{
		return ParameterId;
	}

	FEntityRecord Entity;
	Entity.Id = ParameterId;
	Entity.Kind = TEXT("niagara_parameter");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Variable.GetName().ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("owner_path"), OwnerPath);
	Entity.Attributes.Add(TEXT("scope"), Scope);
	Entity.Attributes.Add(TEXT("parameter_index"), FString::FromInt(Index));
	Entity.Attributes.Add(TEXT("name"), Variable.GetName().ToString());
	Entity.Attributes.Add(TEXT("namespace"), NiagaraParameterNamespace(Variable));
	Entity.Attributes.Add(TEXT("type"), NiagaraVariableTypeName(Variable));
	Entity.Attributes.Add(TEXT("size_bytes"), FString::FromInt(Variable.GetSizeInBytes()));
	Entity.Attributes.Add(TEXT("has_default_value"), NiagaraBool(Variable.IsDataAllocated()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("parameter_name"), TEXT("parameter_type"), TEXT("default_value_summary") };
	Entity.Completeness.Omitted = { TEXT("runtime_parameter_binding") };
	AddNiagaraEvidence(Entity, OwnerPath, TEXT("Niagara parameter read from a static parameter store."));
	OutEntities.Add(MoveTemp(Entity));
	return ParameterId;
}

TArray<TSharedPtr<FJsonValue>> AppendNiagaraParameters(
	const FString& ProjectId,
	const FString& OwnerId,
	const FString& OwnerKey,
	const FString& OwnerPath,
	const FString& Scope,
	const FNiagaraParameterStore& Store,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<FNiagaraVariable> Parameters;
	Store.GetParameters(Parameters);
	Parameters.Sort([](const FNiagaraVariable& Left, const FNiagaraVariable& Right)
	{
		const FString LeftKey = Left.GetName().ToString() + TEXT("|") + NiagaraVariableTypeName(Left);
		const FString RightKey = Right.GetName().ToString() + TEXT("|") + NiagaraVariableTypeName(Right);
		return LeftKey < RightKey;
	});

	TArray<TSharedPtr<FJsonValue>> Values;
	for (int32 ParameterIndex = 0; ParameterIndex < Parameters.Num(); ++ParameterIndex)
	{
		const FNiagaraVariable& Parameter = Parameters[ParameterIndex];
		const FString ParameterId = AddNiagaraParameterEntity(ProjectId, OwnerKey, OwnerPath, Parameter, ParameterIndex, Scope, OutEntities);
		TMap<FString, FString> RelationAttributes;
		RelationAttributes.Add(TEXT("scope"), Scope);
		RelationAttributes.Add(TEXT("parameter_index"), FString::FromInt(ParameterIndex));
		AddNiagaraRelation(
			ProjectId,
			TEXT("contains_niagara_parameter"),
			OwnerId,
			ParameterId,
			OwnerPath,
			TEXT("Niagara owner contains this parameter."),
			OutRelations,
			RelationAttributes);
		Values.Add(MakeShared<FJsonValueObject>(NiagaraParameterObject(Parameter, ParameterIndex, Scope)));
	}
	return Values;
}

FString NiagaraScriptKey(const UNiagaraScript& Script)
{
	return FString::Printf(
		TEXT("%s:usage:%s:usage_id:%s"),
		*Script.GetPathName(),
		*NiagaraScriptUsageString(Script.GetUsage()),
		*NiagaraGuidString(Script.GetUsageId()));
}

FString AddNiagaraScriptEntity(
	const FString& ProjectId,
	UNiagaraScript* Script,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	if (!Script)
	{
		return FString();
	}

	const FString CanonicalKey = NiagaraScriptKey(*Script);
	const FString ScriptId = MakeStableId(ProjectId, TEXT("niagara_script"), CanonicalKey);
	if (FindNiagaraEntity(OutEntities, ScriptId))
	{
		return ScriptId;
	}

	const FNiagaraVMExecutableData& VmData = Script->GetVMExecutableData();

	TArray<FNiagaraVariable> RapidIterationParameters;
	Script->RapidIterationParameters.GetParameters(RapidIterationParameters);

	FEntityRecord Entity;
	Entity.Id = ScriptId;
	Entity.Kind = TEXT("niagara_script");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Script->GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("script_path"), Script->GetPathName());
	Entity.Attributes.Add(TEXT("script_class"), Script->GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("usage"), NiagaraScriptUsageString(Script->GetUsage()));
	Entity.Attributes.Add(TEXT("usage_id"), NiagaraGuidString(Script->GetUsageId()));
	Entity.Attributes.Add(TEXT("compile_status"), NiagaraEnumValue(StaticEnum<ENiagaraScriptCompileStatus>(), static_cast<int64>(Script->GetLastCompileStatus())));
	Entity.Attributes.Add(TEXT("vm_valid"), NiagaraBool(VmData.IsValid()));
	Entity.Attributes.Add(TEXT("has_bytecode"), NiagaraBool(VmData.HasByteCode()));
	Entity.Attributes.Add(TEXT("bytecode_length"), FString::FromInt(VmData.ByteCode.GetLength()));
	Entity.Attributes.Add(TEXT("attribute_count"), FString::FromInt(VmData.Attributes.Num()));
	Entity.Attributes.Add(TEXT("data_interface_count"), FString::FromInt(VmData.DataInterfaceInfo.Num()));
	Entity.Attributes.Add(TEXT("uobject_reference_count"), FString::FromInt(VmData.UObjectInfos.Num()));
	Entity.Attributes.Add(TEXT("rapid_iteration_parameter_count"), FString::FromInt(RapidIterationParameters.Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("script_usage"), TEXT("vm_executable_summary"), TEXT("rapid_iteration_parameters") };
	Entity.Completeness.Omitted = { TEXT("vm_bytecode"), TEXT("hlsl_translation"), TEXT("niagara_graph_nodes") };
	AddNiagaraEvidence(Entity, EvidencePath, TEXT("UNiagaraScript static metadata and VM executable summary read through public Niagara API."));
	OutEntities.Add(MoveTemp(Entity));
	return ScriptId;
}

TSharedRef<FJsonObject> NiagaraScriptObject(
	const FString& ProjectId,
	UNiagaraScript& Script,
	const FString& ScriptId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FNiagaraVMExecutableData& VmData = Script.GetVMExecutableData();

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), ScriptId);
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.niagara_script.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("script_path"), Script.GetPathName());
	Object->SetStringField(TEXT("script_class"), Script.GetClass()->GetPathName());
	Object->SetStringField(TEXT("usage"), NiagaraScriptUsageString(Script.GetUsage()));
	Object->SetStringField(TEXT("usage_id"), NiagaraGuidString(Script.GetUsageId()));
	Object->SetStringField(TEXT("compile_status"), NiagaraEnumValue(StaticEnum<ENiagaraScriptCompileStatus>(), static_cast<int64>(Script.GetLastCompileStatus())));
	Object->SetBoolField(TEXT("vm_valid"), VmData.IsValid());
	Object->SetBoolField(TEXT("has_bytecode"), VmData.HasByteCode());
	Object->SetNumberField(TEXT("bytecode_length"), VmData.ByteCode.GetLength());
	Object->SetNumberField(TEXT("num_temp_registers"), VmData.NumTempRegisters);
	Object->SetNumberField(TEXT("num_user_pointers"), VmData.NumUserPtrs);
	Object->SetNumberField(TEXT("attribute_count"), VmData.Attributes.Num());
	Object->SetNumberField(TEXT("data_interface_count"), VmData.DataInterfaceInfo.Num());
	Object->SetNumberField(TEXT("uobject_reference_count"), VmData.UObjectInfos.Num());
	Object->SetNumberField(TEXT("called_vm_external_function_count"), VmData.CalledVMExternalFunctions.Num());
	Object->SetNumberField(TEXT("read_dataset_count"), VmData.ReadDataSets.Num());
	Object->SetNumberField(TEXT("write_dataset_count"), VmData.WriteDataSets.Num());
	Object->SetNumberField(TEXT("simulation_stage_metadata_count"), VmData.SimulationStageMetaData.Num());
#if WITH_EDITORONLY_DATA
	Object->SetNumberField(TEXT("last_op_count"), VmData.LastOpCount);
	Object->SetStringField(TEXT("error_message"), VmData.ErrorMsg);
#else
	Object->SetNumberField(TEXT("last_op_count"), 0);
	Object->SetStringField(TEXT("error_message"), FString());
#endif
	Object->SetArrayField(
		TEXT("rapid_iteration_parameters"),
		AppendNiagaraParameters(
			ProjectId,
			ScriptId,
			NiagaraScriptKey(Script),
			Script.GetPathName(),
			TEXT("rapid_iteration"),
			Script.RapidIterationParameters,
			OutEntities,
			OutRelations));
	Object->SetNumberField(TEXT("rapid_iteration_parameter_count"), Object->GetArrayField(TEXT("rapid_iteration_parameters")).Num());
	return Object;
}

TSharedRef<FJsonObject> NiagaraScriptRefObject(const FString& ScriptId, const UNiagaraScript& Script, const FString& Role, int32 Index)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), ScriptId);
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("role"), Role);
	Object->SetStringField(TEXT("script_path"), Script.GetPathName());
	Object->SetStringField(TEXT("usage"), NiagaraScriptUsageString(Script.GetUsage()));
	Object->SetStringField(TEXT("usage_id"), NiagaraGuidString(Script.GetUsageId()));
	return Object;
}

void AppendNiagaraScriptRef(
	const FString& ProjectId,
	const FString& OwnerId,
	const FString& OwnerPath,
	const FString& Role,
	int32 ScriptIndex,
	UNiagaraScript* Script,
	TArray<TSharedPtr<FJsonValue>>& OutScriptValues,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (!Script)
	{
		return;
	}

	const FString ScriptId = AddNiagaraScriptEntity(ProjectId, Script, OwnerPath, OutEntities);
	TMap<FString, FString> RelationAttributes;
	RelationAttributes.Add(TEXT("role"), Role);
	RelationAttributes.Add(TEXT("script_index"), FString::FromInt(ScriptIndex));
	AddNiagaraRelation(
		ProjectId,
		TEXT("contains_niagara_script"),
		OwnerId,
		ScriptId,
		OwnerPath,
		TEXT("Niagara owner contains or references this compiled script."),
		OutRelations,
		RelationAttributes);
	OutScriptValues.Add(MakeShared<FJsonValueObject>(NiagaraScriptRefObject(ScriptId, *Script, Role, ScriptIndex)));
}

FString AddNiagaraSystemEntity(const FString& ProjectId, UNiagaraSystem& System, TArray<FEntityRecord>& OutEntities)
{
	const FString SystemPath = System.GetPathName();
	const FString SystemId = MakeStableId(ProjectId, TEXT("niagara_system"), SystemPath);
	if (FindNiagaraEntity(OutEntities, SystemId))
	{
		return SystemId;
	}

	FEntityRecord Entity;
	Entity.Id = SystemId;
	Entity.Kind = TEXT("niagara_system");
	Entity.CanonicalKey = SystemPath;
	Entity.DisplayName = System.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("system_path"), SystemPath);
	Entity.Attributes.Add(TEXT("system_class"), System.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("emitter_count"), FString::FromInt(System.GetEmitterHandles().Num()));
	Entity.Attributes.Add(TEXT("warmup_tick_count"), FString::FromInt(System.GetWarmupTickCount()));
	Entity.Attributes.Add(TEXT("needs_warmup"), NiagaraBool(System.NeedsWarmup()));
	Entity.Attributes.Add(TEXT("deterministic"), NiagaraBool(System.NeedsDeterminism()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("system_metadata"), TEXT("emitter_handles"), TEXT("user_parameters"), TEXT("system_scripts") };
	Entity.Completeness.Omitted = { TEXT("runtime_simulation"), TEXT("system_readiness_check"), TEXT("niagara_graph_nodes"), TEXT("compiled_shader_data") };
	AddNiagaraEvidence(Entity, SystemPath, TEXT("UNiagaraSystem static metadata read through public Niagara API."));
	OutEntities.Add(MoveTemp(Entity));
	return SystemId;
}

FString NiagaraEmitterKey(const FString& OwnerPath, const FNiagaraEmitterHandle* Handle, const UNiagaraEmitter* Emitter, const FVersionedNiagaraEmitterData* EmitterData)
{
	if (Handle)
	{
		return FString::Printf(TEXT("%s:emitter_handle:%s"), *OwnerPath, *NiagaraGuidString(Handle->GetId()));
	}
	const FString EmitterPath = Emitter ? Emitter->GetPathName() : OwnerPath;
	const FString VersionGuid = EmitterData ? NiagaraGuidString(EmitterData->Version.VersionGuid) : FString();
	return FString::Printf(TEXT("%s:version:%s"), *EmitterPath, *VersionGuid);
}

FString AddNiagaraEmitterEntity(
	const FString& ProjectId,
	const FString& OwnerPath,
	const FNiagaraEmitterHandle* Handle,
	UNiagaraEmitter* Emitter,
	FVersionedNiagaraEmitterData* EmitterData,
	int32 EmitterIndex,
	TArray<FEntityRecord>& OutEntities)
{
	if (!EmitterData)
	{
		return FString();
	}

	const FString CanonicalKey = NiagaraEmitterKey(OwnerPath, Handle, Emitter, EmitterData);
	const FString EmitterId = MakeStableId(ProjectId, TEXT("niagara_emitter"), CanonicalKey);
	if (FindNiagaraEntity(OutEntities, EmitterId))
	{
		return EmitterId;
	}

	const FString EmitterPath = Emitter ? Emitter->GetPathName() : OwnerPath;
	const FString DisplayName = Handle ? Handle->GetName().ToString() : (Emitter ? Emitter->GetName() : CanonicalKey);

	FEntityRecord Entity;
	Entity.Id = EmitterId;
	Entity.Kind = TEXT("niagara_emitter");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = DisplayName;
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("owner_path"), OwnerPath);
	Entity.Attributes.Add(TEXT("emitter_path"), EmitterPath);
	Entity.Attributes.Add(TEXT("emitter_index"), FString::FromInt(EmitterIndex));
	Entity.Attributes.Add(TEXT("handle_id"), Handle ? NiagaraGuidString(Handle->GetId()) : FString());
	Entity.Attributes.Add(TEXT("handle_name"), Handle ? Handle->GetName().ToString() : FString());
	Entity.Attributes.Add(TEXT("is_enabled"), Handle ? NiagaraBool(Handle->GetIsEnabled()) : TEXT("true"));
	Entity.Attributes.Add(TEXT("version_guid"), NiagaraGuidString(EmitterData->Version.VersionGuid));
	Entity.Attributes.Add(TEXT("version_major"), FString::FromInt(EmitterData->Version.MajorVersion));
	Entity.Attributes.Add(TEXT("version_minor"), FString::FromInt(EmitterData->Version.MinorVersion));
	Entity.Attributes.Add(TEXT("sim_target"), NiagaraEnumValue(StaticEnum<ENiagaraSimTarget>(), static_cast<int64>(EmitterData->SimTarget)));
	Entity.Attributes.Add(TEXT("renderer_count"), FString::FromInt(EmitterData->GetRenderers().Num()));
	Entity.Attributes.Add(TEXT("event_handler_count"), FString::FromInt(EmitterData->GetEventHandlers().Num()));
	Entity.Attributes.Add(TEXT("simulation_stage_count"), FString::FromInt(EmitterData->GetSimulationStages().Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("emitter_metadata"), TEXT("scripts"), TEXT("renderers"), TEXT("event_handlers"), TEXT("simulation_stages") };
	Entity.Completeness.Omitted = { TEXT("runtime_particles"), TEXT("niagara_graph_nodes"), TEXT("gpu_runtime_state") };
	AddNiagaraEvidence(Entity, OwnerPath, TEXT("Niagara emitter data read from static versioned emitter data."));
	OutEntities.Add(MoveTemp(Entity));
	return EmitterId;
}

FString AddNiagaraRendererEntity(
	const FString& ProjectId,
	const FString& EmitterKey,
	const FString& EvidencePath,
	UNiagaraRendererProperties& Renderer,
	int32 RendererIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = FString::Printf(TEXT("%s:renderer:%d:%s"), *EmitterKey, RendererIndex, *Renderer.GetClass()->GetPathName());
	const FString RendererId = MakeStableId(ProjectId, TEXT("niagara_renderer"), CanonicalKey);
	if (FindNiagaraEntity(OutEntities, RendererId))
	{
		return RendererId;
	}

	FEntityRecord Entity;
	Entity.Id = RendererId;
	Entity.Kind = TEXT("niagara_renderer");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Renderer.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("renderer_index"), FString::FromInt(RendererIndex));
	Entity.Attributes.Add(TEXT("renderer_class"), Renderer.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("is_enabled"), NiagaraBool(Renderer.GetIsEnabled()));
	Entity.Attributes.Add(TEXT("is_active"), NiagaraBool(Renderer.GetIsActive()));
	Entity.Attributes.Add(TEXT("sort_order_hint"), FString::FromInt(Renderer.SortOrderHint));
	Entity.Attributes.Add(TEXT("source_mode"), FString::FromInt(static_cast<int32>(Renderer.GetCurrentSourceMode())));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("renderer_metadata") };
	Entity.Completeness.Omitted = { TEXT("renderer_runtime_resources"), TEXT("renderer_material_resolution") };
	AddNiagaraEvidence(Entity, EvidencePath, TEXT("Niagara renderer properties read from static emitter data."));
	OutEntities.Add(MoveTemp(Entity));
	return RendererId;
}

TSharedRef<FJsonObject> NiagaraRendererObject(UNiagaraRendererProperties& Renderer, const FString& RendererId, int32 RendererIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), RendererId);
	Object->SetNumberField(TEXT("index"), RendererIndex);
	Object->SetStringField(TEXT("name"), Renderer.GetName());
	Object->SetStringField(TEXT("class"), Renderer.GetClass()->GetPathName());
	Object->SetBoolField(TEXT("is_enabled"), Renderer.GetIsEnabled());
	Object->SetBoolField(TEXT("is_active"), Renderer.GetIsActive());
	Object->SetNumberField(TEXT("sort_order_hint"), Renderer.SortOrderHint);
	Object->SetNumberField(TEXT("source_mode"), static_cast<int32>(Renderer.GetCurrentSourceMode()));
	return Object;
}

FString AddNiagaraEventHandlerEntity(
	const FString& ProjectId,
	const FString& EmitterKey,
	const FString& EvidencePath,
	const FNiagaraEventScriptProperties& EventHandler,
	int32 EventIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = FString::Printf(
		TEXT("%s:event_handler:%d:%s"),
		*EmitterKey,
		EventIndex,
		EventHandler.Script ? *NiagaraGuidString(EventHandler.Script->GetUsageId()) : TEXT(""));
	const FString EventId = MakeStableId(ProjectId, TEXT("niagara_event_handler"), CanonicalKey);
	if (FindNiagaraEntity(OutEntities, EventId))
	{
		return EventId;
	}

	FEntityRecord Entity;
	Entity.Id = EventId;
	Entity.Kind = TEXT("niagara_event_handler");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = EventHandler.SourceEventName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("event_index"), FString::FromInt(EventIndex));
	Entity.Attributes.Add(TEXT("source_event_name"), EventHandler.SourceEventName.ToString());
	Entity.Attributes.Add(TEXT("source_emitter_id"), NiagaraGuidString(EventHandler.SourceEmitterID));
	Entity.Attributes.Add(TEXT("execution_mode"), NiagaraEnumValue(StaticEnum<EScriptExecutionMode>(), static_cast<int64>(EventHandler.ExecutionMode)));
	Entity.Attributes.Add(TEXT("spawn_number"), FString::FromInt(static_cast<int32>(EventHandler.SpawnNumber)));
	Entity.Attributes.Add(TEXT("max_events_per_frame"), FString::FromInt(static_cast<int32>(EventHandler.MaxEventsPerFrame)));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("event_handler_metadata") };
	Entity.Completeness.Omitted = { TEXT("runtime_event_payloads") };
	AddNiagaraEvidence(Entity, EvidencePath, TEXT("Niagara event handler read from static emitter data."));
	OutEntities.Add(MoveTemp(Entity));
	return EventId;
}

TSharedRef<FJsonObject> NiagaraEventHandlerObject(const FNiagaraEventScriptProperties& EventHandler, const FString& EventId, int32 EventIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), EventId);
	Object->SetNumberField(TEXT("index"), EventIndex);
	Object->SetStringField(TEXT("source_event_name"), EventHandler.SourceEventName.ToString());
	Object->SetStringField(TEXT("source_emitter_id"), NiagaraGuidString(EventHandler.SourceEmitterID));
	Object->SetStringField(TEXT("execution_mode"), NiagaraEnumValue(StaticEnum<EScriptExecutionMode>(), static_cast<int64>(EventHandler.ExecutionMode)));
	Object->SetNumberField(TEXT("spawn_number"), static_cast<int32>(EventHandler.SpawnNumber));
	Object->SetNumberField(TEXT("max_events_per_frame"), static_cast<int32>(EventHandler.MaxEventsPerFrame));
	Object->SetBoolField(TEXT("random_spawn_number"), EventHandler.bRandomSpawnNumber);
	Object->SetNumberField(TEXT("min_spawn_number"), static_cast<int32>(EventHandler.MinSpawnNumber));
	Object->SetBoolField(TEXT("update_attribute_initial_values"), EventHandler.UpdateAttributeInitialValues);
	Object->SetStringField(TEXT("script_usage_id"), EventHandler.Script ? NiagaraGuidString(EventHandler.Script->GetUsageId()) : FString());
	return Object;
}

FString AddNiagaraSimulationStageEntity(
	const FString& ProjectId,
	const FString& EmitterKey,
	const FString& EvidencePath,
	UNiagaraSimulationStageBase& Stage,
	int32 StageIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString ScriptUsageId = Stage.Script ? NiagaraGuidString(Stage.Script->GetUsageId()) : FString();
	const FString CanonicalKey = FString::Printf(TEXT("%s:simulation_stage:%d:%s"), *EmitterKey, StageIndex, *ScriptUsageId);
	const FString StageId = MakeStableId(ProjectId, TEXT("niagara_simulation_stage"), CanonicalKey);
	if (FindNiagaraEntity(OutEntities, StageId))
	{
		return StageId;
	}

	FEntityRecord Entity;
	Entity.Id = StageId;
	Entity.Kind = TEXT("niagara_simulation_stage");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Stage.SimulationStageName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("stage_index"), FString::FromInt(StageIndex));
	Entity.Attributes.Add(TEXT("stage_name"), Stage.SimulationStageName.ToString());
	Entity.Attributes.Add(TEXT("stage_class"), Stage.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("is_enabled"), NiagaraBool(Stage.bEnabled));
	Entity.Attributes.Add(TEXT("script_usage_id"), ScriptUsageId);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("simulation_stage_metadata") };
	Entity.Completeness.Omitted = { TEXT("runtime_dispatch_state") };
	AddNiagaraEvidence(Entity, EvidencePath, TEXT("Niagara simulation stage read from static emitter data."));
	OutEntities.Add(MoveTemp(Entity));
	return StageId;
}

TSharedRef<FJsonObject> NiagaraSimulationStageObject(UNiagaraSimulationStageBase& Stage, const FString& StageId, int32 StageIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), StageId);
	Object->SetNumberField(TEXT("index"), StageIndex);
	Object->SetStringField(TEXT("name"), Stage.SimulationStageName.ToString());
	Object->SetStringField(TEXT("class"), Stage.GetClass()->GetPathName());
	Object->SetBoolField(TEXT("is_enabled"), Stage.bEnabled);
	Object->SetStringField(TEXT("script_usage_id"), Stage.Script ? NiagaraGuidString(Stage.Script->GetUsageId()) : FString());
	Object->SetStringField(TEXT("script_path"), Stage.Script ? Stage.Script->GetPathName() : FString());
	return Object;
}

TSharedRef<FJsonObject> NiagaraEmitterSnapshot(
	const FString& ProjectId,
	const FString& OwnerPath,
	const FNiagaraEmitterHandle* Handle,
	UNiagaraEmitter* Emitter,
	FVersionedNiagaraEmitterData& EmitterData,
	const FString& EmitterId,
	int32 EmitterIndex,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString EmitterKey = NiagaraEmitterKey(OwnerPath, Handle, Emitter, &EmitterData);
	TArray<TSharedPtr<FJsonValue>> ScriptValues;
	int32 ScriptIndex = 0;

#if WITH_EDITORONLY_DATA
	AppendNiagaraScriptRef(ProjectId, EmitterId, OwnerPath, TEXT("emitter_spawn"), ScriptIndex++, EmitterData.EmitterSpawnScriptProps.Script, ScriptValues, OutEntities, OutRelations);
	AppendNiagaraScriptRef(ProjectId, EmitterId, OwnerPath, TEXT("emitter_update"), ScriptIndex++, EmitterData.EmitterUpdateScriptProps.Script, ScriptValues, OutEntities, OutRelations);
#endif
	AppendNiagaraScriptRef(ProjectId, EmitterId, OwnerPath, TEXT("particle_spawn"), ScriptIndex++, EmitterData.SpawnScriptProps.Script, ScriptValues, OutEntities, OutRelations);
	AppendNiagaraScriptRef(ProjectId, EmitterId, OwnerPath, TEXT("particle_update"), ScriptIndex++, EmitterData.UpdateScriptProps.Script, ScriptValues, OutEntities, OutRelations);
	AppendNiagaraScriptRef(ProjectId, EmitterId, OwnerPath, TEXT("gpu_compute"), ScriptIndex++, EmitterData.GetGPUComputeScript(), ScriptValues, OutEntities, OutRelations);

	TArray<TSharedPtr<FJsonValue>> RendererValues;
	const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData.GetRenderers();
	for (int32 RendererIndex = 0; RendererIndex < Renderers.Num(); ++RendererIndex)
	{
		UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];
		if (!Renderer)
		{
			continue;
		}
		const FString RendererId = AddNiagaraRendererEntity(ProjectId, EmitterKey, OwnerPath, *Renderer, RendererIndex, OutEntities);
		TMap<FString, FString> RelationAttributes;
		RelationAttributes.Add(TEXT("renderer_index"), FString::FromInt(RendererIndex));
		AddNiagaraRelation(ProjectId, TEXT("contains_niagara_renderer"), EmitterId, RendererId, OwnerPath, TEXT("Niagara emitter contains this renderer."), OutRelations, RelationAttributes);
		RendererValues.Add(MakeShared<FJsonValueObject>(NiagaraRendererObject(*Renderer, RendererId, RendererIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> EventValues;
	const TArray<FNiagaraEventScriptProperties>& EventHandlers = EmitterData.GetEventHandlers();
	for (int32 EventIndex = 0; EventIndex < EventHandlers.Num(); ++EventIndex)
	{
		const FNiagaraEventScriptProperties& EventHandler = EventHandlers[EventIndex];
		const FString EventId = AddNiagaraEventHandlerEntity(ProjectId, EmitterKey, OwnerPath, EventHandler, EventIndex, OutEntities);
		TMap<FString, FString> RelationAttributes;
		RelationAttributes.Add(TEXT("event_index"), FString::FromInt(EventIndex));
		AddNiagaraRelation(ProjectId, TEXT("contains_niagara_event_handler"), EmitterId, EventId, OwnerPath, TEXT("Niagara emitter contains this event handler."), OutRelations, RelationAttributes);
		AppendNiagaraScriptRef(ProjectId, EventId, OwnerPath, TEXT("event_handler"), ScriptIndex++, EventHandler.Script, ScriptValues, OutEntities, OutRelations);
		EventValues.Add(MakeShared<FJsonValueObject>(NiagaraEventHandlerObject(EventHandler, EventId, EventIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> SimulationStageValues;
	const TArray<UNiagaraSimulationStageBase*>& SimulationStages = EmitterData.GetSimulationStages();
	for (int32 StageIndex = 0; StageIndex < SimulationStages.Num(); ++StageIndex)
	{
		UNiagaraSimulationStageBase* Stage = SimulationStages[StageIndex];
		if (!Stage)
		{
			continue;
		}
		const FString StageId = AddNiagaraSimulationStageEntity(ProjectId, EmitterKey, OwnerPath, *Stage, StageIndex, OutEntities);
		TMap<FString, FString> RelationAttributes;
		RelationAttributes.Add(TEXT("stage_index"), FString::FromInt(StageIndex));
		AddNiagaraRelation(ProjectId, TEXT("contains_niagara_simulation_stage"), EmitterId, StageId, OwnerPath, TEXT("Niagara emitter contains this simulation stage."), OutRelations, RelationAttributes);
		AppendNiagaraScriptRef(ProjectId, StageId, OwnerPath, TEXT("simulation_stage"), ScriptIndex++, Stage->Script, ScriptValues, OutEntities, OutRelations);
		SimulationStageValues.Add(MakeShared<FJsonValueObject>(NiagaraSimulationStageObject(*Stage, StageId, StageIndex)));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), EmitterId);
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.niagara_emitter.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("emitter_path"), Emitter ? Emitter->GetPathName() : FString());
	Object->SetNumberField(TEXT("index"), EmitterIndex);
	Object->SetStringField(TEXT("name"), Handle ? Handle->GetName().ToString() : (Emitter ? Emitter->GetName() : FString()));
	Object->SetStringField(TEXT("handle_id"), Handle ? NiagaraGuidString(Handle->GetId()) : FString());
	Object->SetStringField(TEXT("unique_instance_name"), Handle ? Handle->GetUniqueInstanceName() : FString());
	Object->SetBoolField(TEXT("is_enabled"), Handle ? Handle->GetIsEnabled() : true);
	Object->SetStringField(TEXT("version_guid"), NiagaraGuidString(EmitterData.Version.VersionGuid));
	Object->SetNumberField(TEXT("version_major"), EmitterData.Version.MajorVersion);
	Object->SetNumberField(TEXT("version_minor"), EmitterData.Version.MinorVersion);
	Object->SetBoolField(TEXT("deprecated"), EmitterData.bDeprecated);
	Object->SetBoolField(TEXT("local_space"), EmitterData.bLocalSpace);
	Object->SetBoolField(TEXT("deterministic"), EmitterData.bDeterminism);
	Object->SetNumberField(TEXT("random_seed"), EmitterData.RandomSeed);
	Object->SetBoolField(TEXT("interpolated_spawning"), EmitterData.bInterpolatedSpawning != 0);
	Object->SetBoolField(TEXT("requires_persistent_ids"), EmitterData.RequiresPersistentIDs());
	Object->SetStringField(TEXT("sim_target"), NiagaraEnumValue(StaticEnum<ENiagaraSimTarget>(), static_cast<int64>(EmitterData.SimTarget)));
	Object->SetStringField(TEXT("bounds_mode"), NiagaraEnumValue(StaticEnum<ENiagaraEmitterCalculateBoundMode>(), static_cast<int64>(EmitterData.CalculateBoundsMode)));
	Object->SetNumberField(TEXT("preallocation_count"), EmitterData.PreAllocationCount);
	Object->SetStringField(TEXT("allocation_mode"), NiagaraEnumValue(StaticEnum<EParticleAllocationMode>(), static_cast<int64>(EmitterData.AllocationMode)));
	Object->SetNumberField(TEXT("renderer_count"), RendererValues.Num());
	Object->SetNumberField(TEXT("event_handler_count"), EventValues.Num());
	Object->SetNumberField(TEXT("simulation_stage_count"), SimulationStageValues.Num());
	Object->SetNumberField(TEXT("script_count"), ScriptValues.Num());
	Object->SetArrayField(TEXT("renderers"), RendererValues);
	Object->SetArrayField(TEXT("event_handlers"), EventValues);
	Object->SetArrayField(TEXT("simulation_stages"), SimulationStageValues);
	Object->SetArrayField(TEXT("scripts"), ScriptValues);
	return Object;
}

TSharedRef<FJsonObject> NiagaraSystemSnapshot(
	const FString& ProjectId,
	UNiagaraSystem& System,
	const FString& SystemId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString SystemPath = System.GetPathName();

	TArray<TSharedPtr<FJsonValue>> ScriptValues;
	AppendNiagaraScriptRef(ProjectId, SystemId, SystemPath, TEXT("system_spawn"), 0, System.GetSystemSpawnScript(), ScriptValues, OutEntities, OutRelations);
	AppendNiagaraScriptRef(ProjectId, SystemId, SystemPath, TEXT("system_update"), 1, System.GetSystemUpdateScript(), ScriptValues, OutEntities, OutRelations);

	TArray<TSharedPtr<FJsonValue>> EmitterValues;
	const TArray<FNiagaraEmitterHandle>& Handles = System.GetEmitterHandles();
	for (int32 EmitterIndex = 0; EmitterIndex < Handles.Num(); ++EmitterIndex)
	{
		const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
		FVersionedNiagaraEmitter Instance = Handle.GetInstance();
		UNiagaraEmitter* Emitter = Instance.Emitter;
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (!EmitterData)
		{
			continue;
		}

		const FString EmitterId = AddNiagaraEmitterEntity(ProjectId, SystemPath, &Handle, Emitter, EmitterData, EmitterIndex, OutEntities);
		TMap<FString, FString> RelationAttributes;
		RelationAttributes.Add(TEXT("emitter_index"), FString::FromInt(EmitterIndex));
		RelationAttributes.Add(TEXT("handle_id"), NiagaraGuidString(Handle.GetId()));
		AddNiagaraRelation(ProjectId, TEXT("contains_niagara_emitter"), SystemId, EmitterId, SystemPath, TEXT("Niagara system contains this emitter handle."), OutRelations, RelationAttributes);
		EmitterValues.Add(MakeShared<FJsonValueObject>(NiagaraEmitterSnapshot(ProjectId, SystemPath, &Handle, Emitter, *EmitterData, EmitterId, EmitterIndex, OutEntities, OutRelations)));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.niagara_system.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("system_path"), SystemPath);
	Object->SetStringField(TEXT("system_class"), System.GetClass()->GetPathName());
	Object->SetBoolField(TEXT("is_valid"), true);
	Object->SetBoolField(TEXT("is_ready_to_run"), false);
	Object->SetBoolField(TEXT("needs_warmup"), System.NeedsWarmup());
	Object->SetNumberField(TEXT("warmup_time"), System.GetWarmupTime());
	Object->SetNumberField(TEXT("warmup_tick_count"), System.GetWarmupTickCount());
	Object->SetNumberField(TEXT("warmup_tick_delta"), System.GetWarmupTickDelta());
	Object->SetBoolField(TEXT("fixed_tick_delta"), System.HasFixedTickDelta());
	Object->SetNumberField(TEXT("fixed_tick_delta_time"), System.GetFixedTickDeltaTime());
	Object->SetBoolField(TEXT("deterministic"), System.NeedsDeterminism());
	Object->SetNumberField(TEXT("random_seed"), System.GetRandomSeed());
	Object->SetBoolField(TEXT("fixed_bounds"), System.bFixedBounds != 0);
	Object->SetStringField(TEXT("effect_type_path"), System.GetEffectType() ? System.GetEffectType()->GetPathName() : FString());
	Object->SetNumberField(TEXT("emitter_count"), EmitterValues.Num());
	Object->SetNumberField(TEXT("system_script_count"), ScriptValues.Num());
	Object->SetArrayField(TEXT("scripts"), ScriptValues);
	Object->SetArrayField(
		TEXT("user_parameters"),
		AppendNiagaraParameters(ProjectId, SystemId, SystemPath, SystemPath, TEXT("user"), System.GetExposedParameters(), OutEntities, OutRelations));
	Object->SetNumberField(TEXT("user_parameter_count"), Object->GetArrayField(TEXT("user_parameters")).Num());
	Object->SetArrayField(TEXT("emitters"), EmitterValues);
	return Object;
}
}

bool FNiagaraReader::AppendNiagaraAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (IsNiagaraParameterDefinitionsAsset(Asset))
	{
		const TArray<UObject*> ScriptVariables = NiagaraDefinitionScriptVariables(Asset);
		const FString DefinitionsId = AddNiagaraParameterDefinitionsEntity(ProjectId, Asset, ScriptVariables.Num(), OutEntities);
		AddNiagaraRelation(ProjectId, TEXT("contains_niagara_parameter_definitions"), AssetEntity.Id, DefinitionsId, Asset.GetPathName(), TEXT("Asset contains the extracted Niagara Parameter Definitions record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("niagara_parameter_definitions"), NiagaraParameterDefinitionsSnapshot(ProjectId, Asset, DefinitionsId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("niagara_parameter_definition_count"), FString::FromInt(ScriptVariables.Num()));
		AssetEntity.Attributes.Add(TEXT("niagara_parameter_definitions_unique_id"), NiagaraGuidProperty(Asset, TEXT("UniqueId")).IsValid() ? NiagaraGuidProperty(Asset, TEXT("UniqueId")).ToString(EGuidFormats::DigitsWithHyphens) : FString());
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("niagara_parameter_definitions_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("niagara_parameter_definition_variables"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("niagara_parameter_definition_editor_metadata"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("niagara_parameter_definition_default_value_bytes"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("niagara_parameter_map_usage"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Asset.GetPathName(),
			TEXT("UNiagaraParameterDefinitions serialized properties extracted through reflection without NiagaraEditor compile dependency.")
		});
		return true;
	}

	if (UVectorFieldStatic* VectorField = Cast<UVectorFieldStatic>(&Asset))
	{
		const FString VectorFieldId = AddVectorFieldEntity(ProjectId, *VectorField, OutEntities);
		AddNiagaraRelation(ProjectId, TEXT("contains_vector_field"), AssetEntity.Id, VectorFieldId, VectorField->GetPathName(), TEXT("Asset contains the extracted VectorFieldStatic record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("vector_field_static"), VectorFieldSnapshot(*VectorField));
		AssetEntity.Attributes.Add(TEXT("vector_field_size_x"), FString::FromInt(VectorField->SizeX));
		AssetEntity.Attributes.Add(TEXT("vector_field_size_y"), FString::FromInt(VectorField->SizeY));
		AssetEntity.Attributes.Add(TEXT("vector_field_size_z"), FString::FromInt(VectorField->SizeZ));
		AssetEntity.Attributes.Add(TEXT("vector_field_voxel_count"), FString::Printf(TEXT("%lld"), static_cast<long long>(VectorFieldVoxelCount(*VectorField))));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("vector_field_dimensions"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("vector_field_bounds"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("vector_field_source_bulk_data_summary"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("vector_field_samples"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("vector_field_gpu_texture"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			VectorField->GetPathName(),
			TEXT("UVectorFieldStatic metadata extracted through public Engine API.")
		});
		return true;
	}

	if (UNiagaraSystem* System = Cast<UNiagaraSystem>(&Asset))
	{
		const FString SystemId = AddNiagaraSystemEntity(ProjectId, *System, OutEntities);
		AddNiagaraRelation(ProjectId, TEXT("contains_niagara_system"), AssetEntity.Id, SystemId, System->GetPathName(), TEXT("Niagara system asset contains the extracted system record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("niagara_system"), NiagaraSystemSnapshot(ProjectId, *System, SystemId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("niagara_emitter_count"), FString::FromInt(System->GetEmitterHandles().Num()));
		TArray<FNiagaraVariable> ExposedParameters;
		System->GetExposedParameters().GetParameters(ExposedParameters);
		AssetEntity.Attributes.Add(TEXT("niagara_user_parameter_count"), FString::FromInt(ExposedParameters.Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("niagara_system_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("niagara_emitters"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("niagara_scripts"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("niagara_runtime_simulation"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("niagara_system_readiness_check"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("niagara_graph_nodes"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			System->GetPathName(),
			TEXT("UNiagaraSystem static structure extracted through public Niagara API.")
		});
		return true;
	}

	if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&Asset))
	{
		FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
		if (!EmitterData)
		{
			return false;
		}

		const FString EmitterId = AddNiagaraEmitterEntity(ProjectId, Emitter->GetPathName(), nullptr, Emitter, EmitterData, 0, OutEntities);
		AddNiagaraRelation(ProjectId, TEXT("contains_niagara_emitter"), AssetEntity.Id, EmitterId, Emitter->GetPathName(), TEXT("Niagara emitter asset contains the extracted emitter record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("niagara_emitter"), NiagaraEmitterSnapshot(ProjectId, Emitter->GetPathName(), nullptr, Emitter, *EmitterData, EmitterId, 0, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("niagara_renderer_count"), FString::FromInt(EmitterData->GetRenderers().Num()));
		AssetEntity.Attributes.Add(TEXT("niagara_event_handler_count"), FString::FromInt(EmitterData->GetEventHandlers().Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("niagara_emitter_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("niagara_scripts"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("niagara_renderers"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("niagara_runtime_particles"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("niagara_graph_nodes"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Emitter->GetPathName(),
			TEXT("UNiagaraEmitter static structure extracted through public Niagara API.")
		});
		return true;
	}

	if (UNiagaraScript* Script = Cast<UNiagaraScript>(&Asset))
	{
		const FString ScriptId = AddNiagaraScriptEntity(ProjectId, Script, Script->GetPathName(), OutEntities);
		AddNiagaraRelation(ProjectId, TEXT("contains_niagara_script"), AssetEntity.Id, ScriptId, Script->GetPathName(), TEXT("Niagara script asset contains the extracted script record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("niagara_script"), NiagaraScriptObject(ProjectId, *Script, ScriptId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("niagara_script_usage"), NiagaraScriptUsageString(Script->GetUsage()));
		AssetEntity.Attributes.Add(TEXT("niagara_script_compile_status"), NiagaraEnumValue(StaticEnum<ENiagaraScriptCompileStatus>(), static_cast<int64>(Script->GetLastCompileStatus())));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("niagara_script_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("niagara_vm_summary"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("niagara_vm_bytecode"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("niagara_graph_nodes"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Script->GetPathName(),
			TEXT("UNiagaraScript static metadata extracted through public Niagara API.")
		});
		return true;
	}

	return false;
}
}
