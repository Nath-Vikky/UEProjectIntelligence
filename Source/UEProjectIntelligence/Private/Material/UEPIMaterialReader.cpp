#include "UEPIMaterialReader.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString MaterialBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString MaterialEnumValue(const UEnum* Enum, int64 Value)
{
	if (Enum)
	{
		return Enum->GetNameStringByValue(Value);
	}
	return FString::FromInt(static_cast<int32>(Value));
}

FString MaterialGuidString(const FGuid& Guid)
{
	return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString();
}

FGuid MaterialExpressionGuid(const UMaterialExpression& Expression)
{
#if WITH_EDITORONLY_DATA
	return Expression.MaterialExpressionGuid;
#else
	return FGuid();
#endif
}

FString MaterialExpressionKey(const FString& OwnerPath, const UMaterialExpression& Expression, int32 ExpressionIndex)
{
	const FGuid Guid = MaterialExpressionGuid(Expression);
	if (Guid.IsValid())
	{
		return FString::Printf(TEXT("%s:expression:%d:%s"), *OwnerPath, ExpressionIndex, *Guid.ToString(EGuidFormats::Digits));
	}
	return FString::Printf(TEXT("%s:expression:%d:%s"), *OwnerPath, ExpressionIndex, *Expression.GetName());
}

FString MaterialExpressionId(const FString& ProjectId, const FString& OwnerPath, const UMaterialExpression& Expression, int32 ExpressionIndex)
{
	return MakeStableId(ProjectId, TEXT("material_expression"), MaterialExpressionKey(OwnerPath, Expression, ExpressionIndex));
}

void AddMaterialEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

void AddMaterialRelation(
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
	Relation.Id = MakeRelationId(ProjectId, Type, FromId, ToId);
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

FEntityRecord* FindMaterialEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

FString AddTextureReferenceEntity(
	const FString& ProjectId,
	const UTexture* Texture,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	if (!Texture)
	{
		return FString();
	}

	const FString TexturePath = Texture->GetPathName();
	const FString TextureId = MakeStableId(ProjectId, TEXT("texture"), TexturePath);
	if (FindMaterialEntity(OutEntities, TextureId))
	{
		return TextureId;
	}

	FEntityRecord Entity;
	Entity.Id = TextureId;
	Entity.Kind = TEXT("texture");
	Entity.CanonicalKey = TexturePath;
	Entity.DisplayName = Texture->GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("texture_path"), TexturePath);
	Entity.Attributes.Add(TEXT("texture_class"), Texture->GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Registry));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("texture_reference") };
	Entity.Completeness.Omitted = { TEXT("texture_dimensions"), TEXT("source_pixels") };
	AddMaterialEvidence(Entity, EvidencePath, TEXT("Texture reference read from a material expression or parameter."));
	OutEntities.Add(MoveTemp(Entity));
	return TextureId;
}

FString AddMaterialFunctionEntity(
	const FString& ProjectId,
	const UMaterialFunctionInterface* Function,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	if (!Function)
	{
		return FString();
	}

	const FString FunctionPath = Function->GetPathName();
	const FString FunctionId = MakeStableId(ProjectId, TEXT("material_function"), FunctionPath);
	if (FindMaterialEntity(OutEntities, FunctionId))
	{
		return FunctionId;
	}

	FEntityRecord Entity;
	Entity.Id = FunctionId;
	Entity.Kind = TEXT("material_function");
	Entity.CanonicalKey = FunctionPath;
	Entity.DisplayName = Function->GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("function_path"), FunctionPath);
	Entity.Attributes.Add(TEXT("function_class"), Function->GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("material_function_metadata") };
	Entity.Completeness.Omitted = { TEXT("compiled_hlsl"), TEXT("runtime_shader_state") };
	AddMaterialEvidence(Entity, EvidencePath, TEXT("Material function reference read from a material expression."));
	OutEntities.Add(MoveTemp(Entity));
	return FunctionId;
}

FString MaterialInterfaceKind(const UMaterialInterface& MaterialInterface)
{
	return MaterialInterface.IsA<UMaterialInstance>() ? TEXT("material_instance") : TEXT("material");
}

FString AddMaterialInterfaceEntity(
	const FString& ProjectId,
	const UMaterialInterface& MaterialInterface,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString MaterialPath = MaterialInterface.GetPathName();
	const FString Kind = MaterialInterfaceKind(MaterialInterface);
	const FString MaterialId = MakeStableId(ProjectId, Kind, MaterialPath);
	if (FindMaterialEntity(OutEntities, MaterialId))
	{
		return MaterialId;
	}

	FEntityRecord Entity;
	Entity.Id = MaterialId;
	Entity.Kind = Kind;
	Entity.CanonicalKey = MaterialPath;
	Entity.DisplayName = MaterialInterface.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("material_path"), MaterialPath);
	Entity.Attributes.Add(TEXT("material_class"), MaterialInterface.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("blend_mode"), MaterialEnumValue(StaticEnum<EBlendMode>(), static_cast<int64>(MaterialInterface.GetBlendMode())));
	Entity.Attributes.Add(TEXT("two_sided"), MaterialBool(MaterialInterface.IsTwoSided()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("material_metadata") };
	Entity.Completeness.Omitted = { TEXT("compiled_hlsl"), TEXT("runtime_shader_state") };
	AddMaterialEvidence(Entity, EvidencePath, TEXT("Material interface metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return MaterialId;
}

TSharedRef<FJsonObject> LinearColorObject(const FLinearColor& Color)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("r"), Color.R);
	Object->SetNumberField(TEXT("g"), Color.G);
	Object->SetNumberField(TEXT("b"), Color.B);
	Object->SetNumberField(TEXT("a"), Color.A);
	return Object;
}

TSharedRef<FJsonObject> Vector4Object(const FVector4d& Vector)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Vector.X);
	Object->SetNumberField(TEXT("y"), Vector.Y);
	Object->SetNumberField(TEXT("z"), Vector.Z);
	Object->SetNumberField(TEXT("w"), Vector.W);
	return Object;
}

FString AddMaterialParameterCollectionEntity(
	const FString& ProjectId,
	const UMaterialParameterCollection& Collection,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CollectionPath = Collection.GetPathName();
	const FString CollectionId = MakeStableId(ProjectId, TEXT("material_parameter_collection"), CollectionPath);
	if (FindMaterialEntity(OutEntities, CollectionId))
	{
		return CollectionId;
	}

	FEntityRecord Entity;
	Entity.Id = CollectionId;
	Entity.Kind = TEXT("material_parameter_collection");
	Entity.CanonicalKey = CollectionPath;
	Entity.DisplayName = Collection.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("collection_path"), CollectionPath);
	Entity.Attributes.Add(TEXT("collection_class"), Collection.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("state_id"), MaterialGuidString(Collection.StateId));
	Entity.Attributes.Add(TEXT("scalar_parameter_count"), FString::FromInt(Collection.ScalarParameters.Num()));
	Entity.Attributes.Add(TEXT("vector_parameter_count"), FString::FromInt(Collection.VectorParameters.Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("collection_parameters"), TEXT("parameter_default_values") };
	Entity.Completeness.Omitted = { TEXT("runtime_collection_instances"), TEXT("uniform_buffer_layout") };
	AddMaterialEvidence(Entity, CollectionPath, TEXT("MaterialParameterCollection metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return CollectionId;
}

FString AddMaterialCollectionParameterEntity(
	const FString& ProjectId,
	const UMaterialParameterCollection& Collection,
	const FString& ParameterType,
	const FName& ParameterName,
	const FGuid& ParameterId,
	int32 ParameterIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = FString::Printf(TEXT("%s:%s:%d:%s"), *Collection.GetPathName(), *ParameterType, ParameterIndex, *ParameterName.ToString());
	const FString ParameterEntityId = MakeStableId(ProjectId, TEXT("material_collection_parameter"), CanonicalKey);
	if (FindMaterialEntity(OutEntities, ParameterEntityId))
	{
		return ParameterEntityId;
	}

	FEntityRecord Entity;
	Entity.Id = ParameterEntityId;
	Entity.Kind = TEXT("material_collection_parameter");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = ParameterName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("collection_path"), Collection.GetPathName());
	Entity.Attributes.Add(TEXT("parameter_type"), ParameterType);
	Entity.Attributes.Add(TEXT("parameter_name"), ParameterName.ToString());
	Entity.Attributes.Add(TEXT("parameter_id"), MaterialGuidString(ParameterId));
	Entity.Attributes.Add(TEXT("parameter_index"), FString::FromInt(ParameterIndex));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("parameter_metadata"), TEXT("default_value") };
	Entity.Completeness.Omitted = { TEXT("runtime_override_value") };
	AddMaterialEvidence(Entity, Collection.GetPathName(), TEXT("MaterialParameterCollection parameter read from serialized collection data."));
	OutEntities.Add(MoveTemp(Entity));
	return ParameterEntityId;
}

TSharedRef<FJsonObject> MaterialCollectionScalarParameterObject(
	const FString& ParameterEntityId,
	const FCollectionScalarParameter& Parameter,
	int32 ParameterIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), ParameterEntityId);
	Object->SetNumberField(TEXT("index"), ParameterIndex);
	Object->SetStringField(TEXT("name"), Parameter.ParameterName.ToString());
	Object->SetStringField(TEXT("type"), TEXT("scalar"));
	Object->SetStringField(TEXT("guid"), MaterialGuidString(Parameter.Id));
	Object->SetNumberField(TEXT("default_scalar"), Parameter.DefaultValue);
	Object->SetObjectField(TEXT("default_vector"), LinearColorObject(FLinearColor::Transparent));
	return Object;
}

TSharedRef<FJsonObject> MaterialCollectionVectorParameterObject(
	const FString& ParameterEntityId,
	const FCollectionVectorParameter& Parameter,
	int32 ParameterIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), ParameterEntityId);
	Object->SetNumberField(TEXT("index"), ParameterIndex);
	Object->SetStringField(TEXT("name"), Parameter.ParameterName.ToString());
	Object->SetStringField(TEXT("type"), TEXT("vector"));
	Object->SetStringField(TEXT("guid"), MaterialGuidString(Parameter.Id));
	Object->SetNumberField(TEXT("default_scalar"), 0.0f);
	Object->SetObjectField(TEXT("default_vector"), LinearColorObject(Parameter.DefaultValue));
	return Object;
}

TSharedRef<FJsonObject> MaterialParameterCollectionSnapshot(
	const FString& ProjectId,
	const UMaterialParameterCollection& Collection,
	const FString& CollectionId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> ParameterValues;
	for (int32 ParameterIndex = 0; ParameterIndex < Collection.ScalarParameters.Num(); ++ParameterIndex)
	{
		const FCollectionScalarParameter& Parameter = Collection.ScalarParameters[ParameterIndex];
		const FString ParameterEntityId = AddMaterialCollectionParameterEntity(ProjectId, Collection, TEXT("scalar"), Parameter.ParameterName, Parameter.Id, ParameterIndex, OutEntities);
		AddMaterialRelation(ProjectId, TEXT("contains_material_collection_parameter"), CollectionId, ParameterEntityId, Collection.GetPathName(), TEXT("MaterialParameterCollection contains this scalar parameter."), OutRelations);
		ParameterValues.Add(MakeShared<FJsonValueObject>(MaterialCollectionScalarParameterObject(ParameterEntityId, Parameter, ParameterValues.Num())));
	}
	for (int32 ParameterIndex = 0; ParameterIndex < Collection.VectorParameters.Num(); ++ParameterIndex)
	{
		const FCollectionVectorParameter& Parameter = Collection.VectorParameters[ParameterIndex];
		const FString ParameterEntityId = AddMaterialCollectionParameterEntity(ProjectId, Collection, TEXT("vector"), Parameter.ParameterName, Parameter.Id, ParameterIndex, OutEntities);
		AddMaterialRelation(ProjectId, TEXT("contains_material_collection_parameter"), CollectionId, ParameterEntityId, Collection.GetPathName(), TEXT("MaterialParameterCollection contains this vector parameter."), OutRelations);
		ParameterValues.Add(MakeShared<FJsonValueObject>(MaterialCollectionVectorParameterObject(ParameterEntityId, Parameter, ParameterValues.Num())));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.material_parameter_collection.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("collection_path"), Collection.GetPathName());
	Object->SetStringField(TEXT("collection_class"), Collection.GetClass()->GetPathName());
	Object->SetStringField(TEXT("state_id"), MaterialGuidString(Collection.StateId));
	Object->SetNumberField(TEXT("scalar_parameter_count"), Collection.ScalarParameters.Num());
	Object->SetNumberField(TEXT("vector_parameter_count"), Collection.VectorParameters.Num());
	Object->SetNumberField(TEXT("parameter_count"), ParameterValues.Num());
	Object->SetArrayField(TEXT("parameters"), ParameterValues);
	return Object;
}

TSharedRef<FJsonObject> ParameterInfoObject(const FMaterialParameterInfo& Info)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("name"), Info.Name.ToString());
	Object->SetNumberField(TEXT("association"), static_cast<int32>(Info.Association));
	Object->SetNumberField(TEXT("index"), Info.Index);
	return Object;
}

void AddParameterCommonFields(TSharedRef<FJsonObject> Object, const FString& Type, const FMaterialParameterInfo& Info, const FGuid& ExpressionGuid)
{
	Object->SetStringField(TEXT("type"), Type);
	Object->SetObjectField(TEXT("parameter_info"), ParameterInfoObject(Info));
	Object->SetStringField(TEXT("name"), Info.Name.ToString());
	Object->SetStringField(TEXT("expression_guid"), MaterialGuidString(ExpressionGuid));
}

TArray<TSharedPtr<FJsonValue>> MaterialInstanceParameters(const UMaterialInstance& Instance)
{
	TArray<TSharedPtr<FJsonValue>> Values;

	for (const FScalarParameterValue& Parameter : Instance.ScalarParameterValues)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		AddParameterCommonFields(Object, TEXT("scalar"), Parameter.ParameterInfo, Parameter.ExpressionGUID);
		Object->SetNumberField(TEXT("value"), Parameter.ParameterValue);
		Values.Add(MakeShared<FJsonValueObject>(Object));
	}

	for (const FVectorParameterValue& Parameter : Instance.VectorParameterValues)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		AddParameterCommonFields(Object, TEXT("vector"), Parameter.ParameterInfo, Parameter.ExpressionGUID);
		Object->SetObjectField(TEXT("value"), LinearColorObject(Parameter.ParameterValue));
		Values.Add(MakeShared<FJsonValueObject>(Object));
	}

	for (const FDoubleVectorParameterValue& Parameter : Instance.DoubleVectorParameterValues)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		AddParameterCommonFields(Object, TEXT("double_vector"), Parameter.ParameterInfo, Parameter.ExpressionGUID);
		Object->SetObjectField(TEXT("value"), Vector4Object(Parameter.ParameterValue));
		Values.Add(MakeShared<FJsonValueObject>(Object));
	}

	for (const FTextureParameterValue& Parameter : Instance.TextureParameterValues)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		AddParameterCommonFields(Object, TEXT("texture"), Parameter.ParameterInfo, Parameter.ExpressionGUID);
		Object->SetStringField(TEXT("value"), Parameter.ParameterValue ? Parameter.ParameterValue->GetPathName() : FString());
		Values.Add(MakeShared<FJsonValueObject>(Object));
	}

	return Values;
}

FString MaterialExpressionCaption(UMaterialExpression& Expression)
{
#if WITH_EDITOR
	TArray<FString> Captions;
	Expression.GetCaption(Captions);
	return FString::Join(Captions, TEXT(" | "));
#else
	return FString();
#endif
}

void IndexMaterialExpressions(
	const FString& ProjectId,
	const FString& OwnerPath,
	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions,
	TMap<const UMaterialExpression*, FString>& OutExpressionIds)
{
	for (int32 ExpressionIndex = 0; ExpressionIndex < Expressions.Num(); ++ExpressionIndex)
	{
		const UMaterialExpression* Expression = Expressions[ExpressionIndex];
		if (!Expression)
		{
			continue;
		}
		OutExpressionIds.Add(Expression, MaterialExpressionId(ProjectId, OwnerPath, *Expression, ExpressionIndex));
	}
}

void AddMaterialExpressionEntity(
	const FString& ProjectId,
	const FString& OwnerPath,
	const FString& OwnerId,
	UMaterialExpression& Expression,
	int32 ExpressionIndex,
	const FString& ExpressionId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (!FindMaterialEntity(OutEntities, ExpressionId))
	{
		FEntityRecord Entity;
		Entity.Id = ExpressionId;
		Entity.Kind = TEXT("material_expression");
		Entity.CanonicalKey = MaterialExpressionKey(OwnerPath, Expression, ExpressionIndex);
		Entity.DisplayName = Expression.GetName();
		Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
		Entity.Attributes.Add(TEXT("owner_path"), OwnerPath);
		Entity.Attributes.Add(TEXT("expression_index"), FString::FromInt(ExpressionIndex));
		Entity.Attributes.Add(TEXT("expression_class"), Expression.GetClass()->GetPathName());
		Entity.Attributes.Add(TEXT("expression_guid"), MaterialGuidString(MaterialExpressionGuid(Expression)));
		Entity.Attributes.Add(TEXT("caption"), MaterialExpressionCaption(Expression));
		Entity.Attributes.Add(TEXT("editor_x"), FString::FromInt(Expression.MaterialExpressionEditorX));
		Entity.Attributes.Add(TEXT("editor_y"), FString::FromInt(Expression.MaterialExpressionEditorY));
		Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
		Entity.Completeness.State = ECompletenessState::Partial;
		Entity.Completeness.Covered = { TEXT("expression_metadata"), TEXT("input_links") };
		Entity.Completeness.Omitted = { TEXT("compiled_hlsl"), TEXT("preview_value") };
		AddMaterialEvidence(Entity, OwnerPath, TEXT("Material expression read from editor-only material graph data."));
		OutEntities.Add(MoveTemp(Entity));
	}

	AddMaterialRelation(ProjectId, TEXT("contains_material_expression"), OwnerId, ExpressionId, OwnerPath, TEXT("Material graph owner contains this expression."), OutRelations);
}

TSharedRef<FJsonObject> MaterialExpressionObject(
	const FString& ProjectId,
	const FString& OwnerPath,
	UMaterialExpression& Expression,
	int32 ExpressionIndex,
	const FString& ExpressionId,
	const TMap<const UMaterialExpression*, FString>& ExpressionIds,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> InputValues;
	TArray<TSharedPtr<FJsonValue>> OutputValues;

#if WITH_EDITOR
	TArrayView<FExpressionInput*> Inputs = Expression.GetInputsView();
	for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
	{
		const FExpressionInput* Input = Inputs[InputIndex];
		TSharedRef<FJsonObject> InputObject = MakeShared<FJsonObject>();
		InputObject->SetNumberField(TEXT("index"), InputIndex);
		InputObject->SetStringField(TEXT("name"), Expression.GetInputName(InputIndex).ToString());
		InputObject->SetStringField(TEXT("connected_expression_id"), FString());
		InputObject->SetNumberField(TEXT("connected_output_index"), Input ? Input->OutputIndex : INDEX_NONE);
		InputObject->SetNumberField(TEXT("mask"), Input ? Input->Mask : 0);
		InputObject->SetNumberField(TEXT("mask_r"), Input ? Input->MaskR : 0);
		InputObject->SetNumberField(TEXT("mask_g"), Input ? Input->MaskG : 0);
		InputObject->SetNumberField(TEXT("mask_b"), Input ? Input->MaskB : 0);
		InputObject->SetNumberField(TEXT("mask_a"), Input ? Input->MaskA : 0);

		if (Input && Input->Expression)
		{
			if (const FString* ConnectedId = ExpressionIds.Find(Input->Expression))
			{
				InputObject->SetStringField(TEXT("connected_expression_id"), *ConnectedId);
				TMap<FString, FString> RelationAttributes;
				RelationAttributes.Add(TEXT("input_index"), FString::FromInt(InputIndex));
				RelationAttributes.Add(TEXT("input_name"), Expression.GetInputName(InputIndex).ToString());
				RelationAttributes.Add(TEXT("connected_output_index"), FString::FromInt(Input->OutputIndex));
				AddMaterialRelation(ProjectId, TEXT("material_expression_depends_on"), ExpressionId, *ConnectedId, OwnerPath, TEXT("Material expression input is connected to another expression."), OutRelations, RelationAttributes);
			}
		}

		InputValues.Add(MakeShared<FJsonValueObject>(InputObject));
	}

	TArray<FExpressionOutput>& Outputs = Expression.GetOutputs();
	for (int32 OutputIndex = 0; OutputIndex < Outputs.Num(); ++OutputIndex)
	{
		const FExpressionOutput& Output = Outputs[OutputIndex];
		TSharedRef<FJsonObject> OutputObject = MakeShared<FJsonObject>();
		OutputObject->SetNumberField(TEXT("index"), OutputIndex);
		OutputObject->SetStringField(TEXT("name"), Output.OutputName.ToString());
		OutputObject->SetNumberField(TEXT("mask"), Output.Mask);
		OutputObject->SetNumberField(TEXT("mask_r"), Output.MaskR);
		OutputObject->SetNumberField(TEXT("mask_g"), Output.MaskG);
		OutputObject->SetNumberField(TEXT("mask_b"), Output.MaskB);
		OutputObject->SetNumberField(TEXT("mask_a"), Output.MaskA);
		OutputValues.Add(MakeShared<FJsonValueObject>(OutputObject));
	}
#endif

	FString ParameterName;
#if WITH_EDITOR
	if (Expression.HasAParameterName())
	{
		ParameterName = Expression.GetParameterName().ToString();
	}
#endif

	FString TexturePath;
	if (const UMaterialExpressionTextureBase* TextureExpression = Cast<UMaterialExpressionTextureBase>(&Expression))
	{
		if (const UTexture* Texture = TextureExpression->Texture)
		{
			TexturePath = Texture->GetPathName();
			const FString TextureId = AddTextureReferenceEntity(ProjectId, Texture, OwnerPath, OutEntities);
			AddMaterialRelation(ProjectId, TEXT("uses_texture"), ExpressionId, TextureId, OwnerPath, TEXT("Material expression references this texture."), OutRelations);
		}
	}

	FString FunctionPath;
	if (const UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(&Expression))
	{
		if (const UMaterialFunctionInterface* Function = FunctionCall->MaterialFunction)
		{
			FunctionPath = Function->GetPathName();
			const FString FunctionId = AddMaterialFunctionEntity(ProjectId, Function, OwnerPath, OutEntities);
			AddMaterialRelation(ProjectId, TEXT("uses_material_function"), ExpressionId, FunctionId, OwnerPath, TEXT("Material expression calls this material function."), OutRelations);
		}
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), ExpressionId);
	Object->SetNumberField(TEXT("index"), ExpressionIndex);
	Object->SetStringField(TEXT("name"), Expression.GetName());
	Object->SetStringField(TEXT("class"), Expression.GetClass()->GetPathName());
	Object->SetStringField(TEXT("guid"), MaterialGuidString(MaterialExpressionGuid(Expression)));
	Object->SetStringField(TEXT("caption"), MaterialExpressionCaption(Expression));
	Object->SetStringField(TEXT("description"), Expression.Desc);
	Object->SetNumberField(TEXT("editor_x"), Expression.MaterialExpressionEditorX);
	Object->SetNumberField(TEXT("editor_y"), Expression.MaterialExpressionEditorY);
	Object->SetStringField(TEXT("parameter_name"), ParameterName);
	Object->SetStringField(TEXT("referenced_texture_path"), TexturePath);
	Object->SetStringField(TEXT("material_function_path"), FunctionPath);
	Object->SetNumberField(TEXT("input_count"), InputValues.Num());
	Object->SetNumberField(TEXT("output_count"), OutputValues.Num());
	Object->SetArrayField(TEXT("inputs"), InputValues);
	Object->SetArrayField(TEXT("outputs"), OutputValues);
	return Object;
}

TArray<TSharedPtr<FJsonValue>> MaterialExpressionsSnapshot(
	const FString& ProjectId,
	const FString& OwnerPath,
	const FString& OwnerId,
	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TMap<const UMaterialExpression*, FString> ExpressionIds;
	IndexMaterialExpressions(ProjectId, OwnerPath, Expressions, ExpressionIds);

	TArray<TSharedPtr<FJsonValue>> ExpressionValues;
	for (int32 ExpressionIndex = 0; ExpressionIndex < Expressions.Num(); ++ExpressionIndex)
	{
		UMaterialExpression* Expression = Expressions[ExpressionIndex];
		if (!Expression)
		{
			continue;
		}

		const FString* ExpressionId = ExpressionIds.Find(Expression);
		if (!ExpressionId)
		{
			continue;
		}

		AddMaterialExpressionEntity(ProjectId, OwnerPath, OwnerId, *Expression, ExpressionIndex, *ExpressionId, OutEntities, OutRelations);
		ExpressionValues.Add(MakeShared<FJsonValueObject>(
			MaterialExpressionObject(ProjectId, OwnerPath, *Expression, ExpressionIndex, *ExpressionId, ExpressionIds, OutEntities, OutRelations)));
	}

	return ExpressionValues;
}

TSharedRef<FJsonObject> MaterialSnapshot(
	const FString& ProjectId,
	UMaterial& Material,
	const FString& MaterialId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString MaterialPath = Material.GetPathName();
	TArray<TSharedPtr<FJsonValue>> Expressions = MaterialExpressionsSnapshot(ProjectId, MaterialPath, MaterialId, Material.GetExpressions(), OutEntities, OutRelations);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.material.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("material_path"), MaterialPath);
	Object->SetStringField(TEXT("material_class"), Material.GetClass()->GetPathName());
	Object->SetStringField(TEXT("material_domain"), MaterialEnumValue(StaticEnum<EMaterialDomain>(), static_cast<int64>(Material.MaterialDomain.GetValue())));
	Object->SetStringField(TEXT("blend_mode"), MaterialEnumValue(StaticEnum<EBlendMode>(), static_cast<int64>(Material.GetBlendMode())));
	Object->SetStringField(TEXT("shading_model_field"), FString::FromInt(Material.GetShadingModels().GetShadingModelField()));
	Object->SetBoolField(TEXT("two_sided"), Material.IsTwoSided());
	Object->SetNumberField(TEXT("expression_count"), Expressions.Num());
	Object->SetNumberField(TEXT("comment_count"), Material.GetEditorComments().Num());
	Object->SetArrayField(TEXT("expressions"), Expressions);
	return Object;
}

TSharedRef<FJsonObject> MaterialFunctionSnapshot(
	const FString& ProjectId,
	UMaterialFunctionInterface& Function,
	const FString& FunctionId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString FunctionPath = Function.GetPathName();
	TArray<TSharedPtr<FJsonValue>> Expressions;
#if WITH_EDITORONLY_DATA
	Expressions = MaterialExpressionsSnapshot(ProjectId, FunctionPath, FunctionId, Function.GetExpressions(), OutEntities, OutRelations);
#endif

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.material_function.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("function_path"), FunctionPath);
	Object->SetStringField(TEXT("function_class"), Function.GetClass()->GetPathName());
	Object->SetStringField(TEXT("usage"), MaterialEnumValue(StaticEnum<EMaterialFunctionUsage>(), static_cast<int64>(Function.GetMaterialFunctionUsage())));
	Object->SetNumberField(TEXT("expression_count"), Expressions.Num());
	Object->SetArrayField(TEXT("expressions"), Expressions);
	return Object;
}

TSharedRef<FJsonObject> MaterialInstanceSnapshot(const UMaterialInstance& Instance)
{
	const TArray<TSharedPtr<FJsonValue>> Parameters = MaterialInstanceParameters(Instance);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.material_instance.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("instance_path"), Instance.GetPathName());
	Object->SetStringField(TEXT("instance_class"), Instance.GetClass()->GetPathName());
	Object->SetStringField(TEXT("parent_path"), Instance.Parent ? Instance.Parent->GetPathName() : FString());
	Object->SetStringField(TEXT("blend_mode"), MaterialEnumValue(StaticEnum<EBlendMode>(), static_cast<int64>(Instance.GetBlendMode())));
	Object->SetStringField(TEXT("shading_model_field"), FString::FromInt(Instance.GetShadingModels().GetShadingModelField()));
	Object->SetBoolField(TEXT("two_sided"), Instance.IsTwoSided());
	Object->SetNumberField(TEXT("parameter_override_count"), Parameters.Num());
	Object->SetArrayField(TEXT("parameters"), Parameters);
	return Object;
}
}

bool FMaterialReader::AppendMaterialAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(&Asset))
	{
		const FString CollectionId = AddMaterialParameterCollectionEntity(ProjectId, *Collection, OutEntities);
		AddMaterialRelation(ProjectId, TEXT("contains_material_parameter_collection"), AssetEntity.Id, CollectionId, Collection->GetPathName(), TEXT("Asset contains the extracted MaterialParameterCollection record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("material_parameter_collection"), MaterialParameterCollectionSnapshot(ProjectId, *Collection, CollectionId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("material_collection_scalar_parameter_count"), FString::FromInt(Collection->ScalarParameters.Num()));
		AssetEntity.Attributes.Add(TEXT("material_collection_vector_parameter_count"), FString::FromInt(Collection->VectorParameters.Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("material_collection_parameters"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("material_collection_default_values"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("material_collection_runtime_instances"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("material_collection_uniform_buffer_layout"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Collection->GetPathName(),
			TEXT("UMaterialParameterCollection defaults extracted through public Engine API.")
		});
		return true;
	}

	if (UMaterial* Material = Cast<UMaterial>(&Asset))
	{
		const FString MaterialId = AddMaterialInterfaceEntity(ProjectId, *Material, Material->GetPathName(), OutEntities);
		AddMaterialRelation(ProjectId, TEXT("contains_material"), AssetEntity.Id, MaterialId, Material->GetPathName(), TEXT("Material asset contains the extracted material graph record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("material"), MaterialSnapshot(ProjectId, *Material, MaterialId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("material_expression_count"), FString::FromInt(Material->GetExpressions().Num()));
		AssetEntity.Attributes.Add(TEXT("material_blend_mode"), MaterialEnumValue(StaticEnum<EBlendMode>(), static_cast<int64>(Material->GetBlendMode())));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("material_expression_graph"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("compiled_hlsl"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_shader_state"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Material->GetPathName(),
			TEXT("UMaterial editor expression graph extracted through public Engine API.")
		});
		return true;
	}

	if (UMaterialInstance* Instance = Cast<UMaterialInstance>(&Asset))
	{
		const FString InstanceId = AddMaterialInterfaceEntity(ProjectId, *Instance, Instance->GetPathName(), OutEntities);
		AddMaterialRelation(ProjectId, TEXT("contains_material_instance"), AssetEntity.Id, InstanceId, Instance->GetPathName(), TEXT("Material instance asset contains the extracted instance record."), OutRelations);

		if (Instance->Parent)
		{
			const FString ParentId = AddMaterialInterfaceEntity(ProjectId, *Instance->Parent, Instance->GetPathName(), OutEntities);
			AddMaterialRelation(ProjectId, TEXT("uses_material"), InstanceId, ParentId, Instance->GetPathName(), TEXT("Material instance inherits from this parent material."), OutRelations);
		}

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("material_instance"), MaterialInstanceSnapshot(*Instance));
		AssetEntity.Attributes.Add(TEXT("material_parent_path"), Instance->Parent ? Instance->Parent->GetPathName() : FString());
		AssetEntity.Attributes.Add(TEXT("material_parameter_override_count"), FString::FromInt(MaterialInstanceParameters(*Instance).Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("material_instance_parameters"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("compiled_hlsl"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_shader_state"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Instance->GetPathName(),
			TEXT("UMaterialInstance parent and parameter overrides extracted through public Engine API.")
		});
		return true;
	}

	if (UMaterialFunctionInterface* Function = Cast<UMaterialFunctionInterface>(&Asset))
	{
		const FString FunctionId = AddMaterialFunctionEntity(ProjectId, Function, Function->GetPathName(), OutEntities);
		AddMaterialRelation(ProjectId, TEXT("contains_material_function"), AssetEntity.Id, FunctionId, Function->GetPathName(), TEXT("Material function asset contains the extracted function graph record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("material_function"), MaterialFunctionSnapshot(ProjectId, *Function, FunctionId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("material_function_expression_count"), FString::FromInt(Function->GetExpressions().Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("material_function_expression_graph"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("compiled_hlsl"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_shader_state"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Function->GetPathName(),
			TEXT("UMaterialFunction expression graph extracted through public Engine API.")
		});
		return true;
	}

	return false;
}
}
