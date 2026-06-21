#include "UEPIUObjectReflectionReader.h"

#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/FrameRate.h"
#include "Misc/FrameTime.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/PrimaryAssetId.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UnrealType.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString PropertyKind(const FProperty* Property)
{
	if (Property->IsA<FNumericProperty>())
	{
		return TEXT("numeric");
	}
	if (Property->IsA<FBoolProperty>())
	{
		return TEXT("bool");
	}
	if (Property->IsA<FStrProperty>())
	{
		return TEXT("string");
	}
	if (Property->IsA<FNameProperty>())
	{
		return TEXT("name");
	}
	if (Property->IsA<FTextProperty>())
	{
		return TEXT("text");
	}
	if (Property->IsA<FEnumProperty>() || Property->IsA<FByteProperty>())
	{
		return TEXT("enum");
	}
	if (Property->IsA<FObjectPropertyBase>())
	{
		return TEXT("object_reference");
	}
	if (Property->IsA<FSoftObjectProperty>() || Property->IsA<FSoftClassProperty>())
	{
		return TEXT("soft_object_reference");
	}
	if (Property->IsA<FStructProperty>())
	{
		return TEXT("struct");
	}
	if (Property->IsA<FArrayProperty>())
	{
		return TEXT("array");
	}
	if (Property->IsA<FMapProperty>())
	{
		return TEXT("map");
	}
	if (Property->IsA<FSetProperty>())
	{
		return TEXT("set");
	}

	return TEXT("unsupported_text_export");
}

FString ExportPropertyText(const FProperty* Property, const void* ValuePtr, UObject* Owner)
{
	FString ValueText;
	Property->ExportText_Direct(ValueText, ValuePtr, ValuePtr, Owner, PPF_None);
	return ValueText;
}

FString TruncatedPropertyText(const FProperty* Property, const void* ValuePtr, UObject* Owner, int32 MaxLength, bool& bOutTruncated)
{
	FString ValueText = ExportPropertyText(Property, ValuePtr, Owner);
	if (MaxLength > 0 && ValueText.Len() > MaxLength)
	{
		ValueText.LeftInline(MaxLength);
		bOutTruncated = true;
	}
	else
	{
		bOutTruncated = false;
	}
	return ValueText;
}

TSharedRef<FJsonObject> VectorObject(double X, double Y, double Z)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), X);
	Object->SetNumberField(TEXT("y"), Y);
	Object->SetNumberField(TEXT("z"), Z);
	return Object;
}

TSharedRef<FJsonObject> Vector4Object(double X, double Y, double Z, double W)
{
	TSharedRef<FJsonObject> Object = VectorObject(X, Y, Z);
	Object->SetNumberField(TEXT("w"), W);
	return Object;
}

TSharedRef<FJsonObject> ColorObject(double R, double G, double B, double A)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("r"), R);
	Object->SetNumberField(TEXT("g"), G);
	Object->SetNumberField(TEXT("b"), B);
	Object->SetNumberField(TEXT("a"), A);
	return Object;
}

FString ReflectionArtifactRoot(const FScanOptions& Options)
{
	const FString BaseDir = Options.OutputPath.IsEmpty()
		? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"))
		: FPaths::GetPath(FPaths::ConvertRelativePathToFull(Options.OutputPath));
	return FPaths::Combine(BaseDir, TEXT("Artifacts"), TEXT("uobject_collections"));
}

TSharedRef<FJsonObject> WriteCollectionArtifact(
	const FScanOptions& Options,
	const FEntityRecord& Entity,
	const UObject* AssetObject,
	const FProperty* Property,
	int32 CollectionCount,
	const FString& FullValueText,
	FString& OutError)
{
	const FString ObjectPath = AssetObject ? AssetObject->GetPathName() : FString();
	const FString ArtifactSeed = ObjectPath + TEXT(":") + Property->GetName();
	const FString ArtifactId = MakeStableId(Entity.Id, TEXT("uobject_collection_artifact"), ArtifactSeed);
	const FString ArtifactDir = ReflectionArtifactRoot(Options);
	const FString ArtifactPath = FPaths::Combine(ArtifactDir, ArtifactId + TEXT(".json"));

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("schema_version"), TEXT("uepi.uobject_collection_artifact.v1"));
	Payload->SetStringField(TEXT("artifact_id"), ArtifactId);
	Payload->SetStringField(TEXT("object_path"), ObjectPath);
	Payload->SetStringField(TEXT("property_name"), Property->GetName());
	Payload->SetStringField(TEXT("kind"), PropertyKind(Property));
	Payload->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
	Payload->SetNumberField(TEXT("item_count"), CollectionCount);
	Payload->SetStringField(TEXT("encoding"), TEXT("export_text"));
	Payload->SetStringField(TEXT("value_text"), FullValueText);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Payload, Writer);

	TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("schema_version"), TEXT("uepi.uobject_collection_artifact_manifest.v1"));
	Manifest->SetStringField(TEXT("artifact_id"), ArtifactId);
	Manifest->SetStringField(TEXT("artifact_uri"), TEXT("uepi://uobject-collection-artifact/") + ArtifactId);
	Manifest->SetStringField(TEXT("storage"), TEXT("scan_sidecar_json"));
	Manifest->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(ArtifactPath));
	Manifest->SetNumberField(TEXT("item_count"), CollectionCount);
	Manifest->SetNumberField(TEXT("byte_count"), FTCHARToUTF8(*Json).Length());
	Manifest->SetStringField(TEXT("encoding"), TEXT("json"));

	IFileManager::Get().MakeDirectory(*ArtifactDir, true);
	if (!FFileHelper::SaveStringToFile(Json, *ArtifactPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write UObject collection artifact: %s"), *ArtifactPath);
		Manifest->SetStringField(TEXT("storage"), TEXT("scan_sidecar_json_write_failed"));
		Manifest->SetStringField(TEXT("write_error"), OutError);
	}

	return Manifest;
}

bool IsInOuterChain(const UObject* Candidate, const UObject* ExpectedOuter)
{
	for (const UObject* Current = Candidate; Current; Current = Current->GetOuter())
	{
		if (Current == ExpectedOuter)
		{
			return true;
		}
	}
	return false;
}

int32 CollectionElementCount(const FProperty* Property, const void* ValuePtr)
{
	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		const FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
		return Helper.Num();
	}

	if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		const FScriptMapHelper Helper(MapProperty, ValuePtr);
		return Helper.Num();
	}

	if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
	{
		const FScriptSetHelper Helper(SetProperty, ValuePtr);
		return Helper.Num();
	}

	return INDEX_NONE;
}

TSharedRef<FJsonObject> StructValueSnapshot(
	const FStructProperty* StructProperty,
	const void* ValuePtr,
	UObject* Owner,
	int32 Depth,
	int32 MaxDepth,
	int32 MaxInlineItems);

void AddReferenceFields(const FProperty* Property, const void* ValuePtr, UObject* Owner, TSharedRef<FJsonObject> PropertyObject);

TSharedPtr<FJsonObject> NormalizedStructValue(const FStructProperty* StructProperty, const void* ValuePtr)
{
	if (!StructProperty || !StructProperty->Struct || !ValuePtr)
	{
		return nullptr;
	}

	const FName StructName = StructProperty->Struct->GetFName();
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();

	if (StructName == FName(TEXT("Vector2D")))
	{
		const FVector2D& Value = *reinterpret_cast<const FVector2D*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FVector2D"));
		Object->SetNumberField(TEXT("x"), Value.X);
		Object->SetNumberField(TEXT("y"), Value.Y);
		return Object;
	}
	if (StructName == FName(TEXT("Vector")))
	{
		const FVector& Value = *reinterpret_cast<const FVector*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FVector"));
		Object->SetObjectField(TEXT("value"), VectorObject(Value.X, Value.Y, Value.Z));
		return Object;
	}
	if (StructName == FName(TEXT("Vector4")))
	{
		const FVector4& Value = *reinterpret_cast<const FVector4*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FVector4"));
		Object->SetObjectField(TEXT("value"), Vector4Object(Value.X, Value.Y, Value.Z, Value.W));
		return Object;
	}
	if (StructName == FName(TEXT("Rotator")))
	{
		const FRotator& Value = *reinterpret_cast<const FRotator*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FRotator"));
		Object->SetNumberField(TEXT("pitch"), Value.Pitch);
		Object->SetNumberField(TEXT("yaw"), Value.Yaw);
		Object->SetNumberField(TEXT("roll"), Value.Roll);
		return Object;
	}
	if (StructName == FName(TEXT("Quat")))
	{
		const FQuat& Value = *reinterpret_cast<const FQuat*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FQuat"));
		Object->SetObjectField(TEXT("value"), Vector4Object(Value.X, Value.Y, Value.Z, Value.W));
		return Object;
	}
	if (StructName == FName(TEXT("Transform")))
	{
		const FTransform& Value = *reinterpret_cast<const FTransform*>(ValuePtr);
		const FVector Translation = Value.GetTranslation();
		const FQuat Rotation = Value.GetRotation();
		const FVector Scale = Value.GetScale3D();
		Object->SetStringField(TEXT("type"), TEXT("FTransform"));
		Object->SetObjectField(TEXT("translation"), VectorObject(Translation.X, Translation.Y, Translation.Z));
		Object->SetObjectField(TEXT("rotation"), Vector4Object(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W));
		Object->SetObjectField(TEXT("scale"), VectorObject(Scale.X, Scale.Y, Scale.Z));
		return Object;
	}
	if (StructName == FName(TEXT("LinearColor")))
	{
		const FLinearColor& Value = *reinterpret_cast<const FLinearColor*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FLinearColor"));
		Object->SetObjectField(TEXT("value"), ColorObject(Value.R, Value.G, Value.B, Value.A));
		return Object;
	}
	if (StructName == FName(TEXT("Color")))
	{
		const FColor& Value = *reinterpret_cast<const FColor*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FColor"));
		Object->SetObjectField(TEXT("value"), ColorObject(Value.R, Value.G, Value.B, Value.A));
		return Object;
	}
	if (StructName == FName(TEXT("Guid")))
	{
		const FGuid& Value = *reinterpret_cast<const FGuid*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FGuid"));
		Object->SetStringField(TEXT("value"), Value.ToString(EGuidFormats::DigitsWithHyphens));
		return Object;
	}
	if (StructName == FName(TEXT("DateTime")))
	{
		const FDateTime& Value = *reinterpret_cast<const FDateTime*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FDateTime"));
		Object->SetStringField(TEXT("iso8601"), Value.ToIso8601());
		Object->SetStringField(TEXT("ticks"), FString::Printf(TEXT("%lld"), Value.GetTicks()));
		return Object;
	}
	if (StructName == FName(TEXT("Timespan")))
	{
		const FTimespan& Value = *reinterpret_cast<const FTimespan*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FTimespan"));
		Object->SetStringField(TEXT("text"), Value.ToString());
		Object->SetStringField(TEXT("ticks"), FString::Printf(TEXT("%lld"), Value.GetTicks()));
		return Object;
	}
	if (StructName == FName(TEXT("SoftObjectPath")))
	{
		const FSoftObjectPath& Value = *reinterpret_cast<const FSoftObjectPath*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FSoftObjectPath"));
		Object->SetStringField(TEXT("path"), Value.ToString());
		return Object;
	}
	if (StructName == FName(TEXT("TopLevelAssetPath")))
	{
		const FTopLevelAssetPath& Value = *reinterpret_cast<const FTopLevelAssetPath*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FTopLevelAssetPath"));
		Object->SetStringField(TEXT("path"), Value.ToString());
		Object->SetStringField(TEXT("package_name"), Value.GetPackageName().ToString());
		Object->SetStringField(TEXT("asset_name"), Value.GetAssetName().ToString());
		return Object;
	}
	if (StructName == FName(TEXT("PrimaryAssetId")))
	{
		const FPrimaryAssetId& Value = *reinterpret_cast<const FPrimaryAssetId*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FPrimaryAssetId"));
		Object->SetStringField(TEXT("value"), Value.ToString());
		Object->SetStringField(TEXT("primary_asset_type"), Value.PrimaryAssetType.ToString());
		Object->SetStringField(TEXT("primary_asset_name"), Value.PrimaryAssetName.ToString());
		return Object;
	}
	if (StructName == FName(TEXT("PrimaryAssetType")))
	{
		const FPrimaryAssetType& Value = *reinterpret_cast<const FPrimaryAssetType*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FPrimaryAssetType"));
		Object->SetStringField(TEXT("value"), Value.ToString());
		return Object;
	}
	if (StructName == FName(TEXT("FrameRate")))
	{
		const FFrameRate& Value = *reinterpret_cast<const FFrameRate*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FFrameRate"));
		Object->SetNumberField(TEXT("numerator"), Value.Numerator);
		Object->SetNumberField(TEXT("denominator"), Value.Denominator);
		Object->SetNumberField(TEXT("decimal"), Value.AsDecimal());
		return Object;
	}
	if (StructName == FName(TEXT("FrameNumber")))
	{
		const FFrameNumber& Value = *reinterpret_cast<const FFrameNumber*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FFrameNumber"));
		Object->SetNumberField(TEXT("value"), Value.Value);
		return Object;
	}
	if (StructName == FName(TEXT("FrameTime")))
	{
		const FFrameTime& Value = *reinterpret_cast<const FFrameTime*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FFrameTime"));
		Object->SetNumberField(TEXT("frame_number"), Value.GetFrame().Value);
		Object->SetNumberField(TEXT("sub_frame"), Value.GetSubFrame());
		Object->SetNumberField(TEXT("decimal"), Value.AsDecimal());
		return Object;
	}
	if (StructName == FName(TEXT("Box")))
	{
		const FBox& Value = *reinterpret_cast<const FBox*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FBox"));
		Object->SetBoolField(TEXT("is_valid"), Value.IsValid != 0);
		Object->SetObjectField(TEXT("min"), VectorObject(Value.Min.X, Value.Min.Y, Value.Min.Z));
		Object->SetObjectField(TEXT("max"), VectorObject(Value.Max.X, Value.Max.Y, Value.Max.Z));
		return Object;
	}
	if (StructName == FName(TEXT("BoxSphereBounds")))
	{
		const FBoxSphereBounds& Value = *reinterpret_cast<const FBoxSphereBounds*>(ValuePtr);
		Object->SetStringField(TEXT("type"), TEXT("FBoxSphereBounds"));
		Object->SetObjectField(TEXT("origin"), VectorObject(Value.Origin.X, Value.Origin.Y, Value.Origin.Z));
		Object->SetObjectField(TEXT("box_extent"), VectorObject(Value.BoxExtent.X, Value.BoxExtent.Y, Value.BoxExtent.Z));
		Object->SetNumberField(TEXT("sphere_radius"), Value.SphereRadius);
		return Object;
	}

	return nullptr;
}

TSharedRef<FJsonObject> PropertyFieldSnapshot(
	const FProperty* Property,
	const void* ValuePtr,
	UObject* Owner,
	int32 Depth,
	int32 MaxDepth,
	int32 MaxInlineItems)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("name"), Property->GetName());
	Object->SetStringField(TEXT("kind"), PropertyKind(Property));
	Object->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));

	bool bTruncated = false;
	Object->SetStringField(TEXT("value_text"), TruncatedPropertyText(Property, ValuePtr, Owner, MaxInlineItems, bTruncated));
	Object->SetBoolField(TEXT("truncated"), bTruncated);

	const int32 ElementCount = CollectionElementCount(Property, ValuePtr);
	if (ElementCount != INDEX_NONE)
	{
		Object->SetNumberField(TEXT("collection_count"), ElementCount);
		Object->SetNumberField(TEXT("inline_limit"), MaxInlineItems);
		Object->SetBoolField(TEXT("artifact_required"), ElementCount > MaxInlineItems);
	}
	AddReferenceFields(Property, ValuePtr, Owner, Object);

	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		if (Depth < MaxDepth)
		{
			Object->SetObjectField(TEXT("struct_value"), StructValueSnapshot(StructProperty, ValuePtr, Owner, Depth + 1, MaxDepth, MaxInlineItems));
		}
		else
		{
			Object->SetBoolField(TEXT("struct_depth_limited"), true);
		}
	}
	return Object;
}

TSharedRef<FJsonObject> StructValueSnapshot(
	const FStructProperty* StructProperty,
	const void* ValuePtr,
	UObject* Owner,
	int32 Depth,
	int32 MaxDepth,
	int32 MaxInlineItems)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.property_struct_value.v1"));
	Object->SetStringField(TEXT("struct_path"), StructProperty->Struct ? StructProperty->Struct->GetPathName() : FString());
	Object->SetStringField(TEXT("struct_name"), StructProperty->Struct ? StructProperty->Struct->GetName() : FString());
	Object->SetNumberField(TEXT("depth"), Depth);
	Object->SetNumberField(TEXT("max_depth"), MaxDepth);

	const TSharedPtr<FJsonObject> Normalized = NormalizedStructValue(StructProperty, ValuePtr);
	Object->SetStringField(TEXT("serializer"), Normalized.IsValid() ? TEXT("specialized_field_walk") : TEXT("field_walk"));
	if (Normalized.IsValid())
	{
		Object->SetObjectField(TEXT("normalized"), Normalized.ToSharedRef());
	}

	TArray<FProperty*> Fields;
	if (StructProperty->Struct)
	{
		for (TFieldIterator<FProperty> FieldIt(StructProperty->Struct, EFieldIteratorFlags::IncludeSuper); FieldIt; ++FieldIt)
		{
			Fields.Add(*FieldIt);
		}
	}
	Fields.Sort([](const FProperty& Left, const FProperty& Right)
	{
		return Left.GetName() < Right.GetName();
	});

	const int32 MaxFieldCount = FMath::Clamp(MaxInlineItems, 0, 256);
	TArray<TSharedPtr<FJsonValue>> FieldValues;
	for (int32 FieldIndex = 0; FieldIndex < Fields.Num() && FieldIndex < MaxFieldCount; ++FieldIndex)
	{
		const FProperty* Field = Fields[FieldIndex];
		const void* FieldValuePtr = Field->ContainerPtrToValuePtr<void>(ValuePtr);
		TSharedRef<FJsonObject> FieldObject = PropertyFieldSnapshot(Field, FieldValuePtr, Owner, Depth, MaxDepth, MaxInlineItems);
		FieldObject->SetNumberField(TEXT("index"), FieldIndex);
		FieldValues.Add(MakeShared<FJsonValueObject>(FieldObject));
	}

	Object->SetNumberField(TEXT("field_count"), Fields.Num());
	Object->SetNumberField(TEXT("inline_field_count"), FieldValues.Num());
	Object->SetBoolField(TEXT("truncated"), FieldValues.Num() < Fields.Num());
	Object->SetArrayField(TEXT("fields"), FieldValues);
	return Object;
}

void AddReferenceFields(const FProperty* Property, const void* ValuePtr, UObject* Owner, TSharedRef<FJsonObject> PropertyObject)
{
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		UObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue(ValuePtr);
		PropertyObject->SetStringField(
			TEXT("object_path"),
			ReferencedObject ? ReferencedObject->GetPathName() : FString());
		PropertyObject->SetStringField(
			TEXT("object_class_path"),
			ReferencedObject && ReferencedObject->GetClass() ? ReferencedObject->GetClass()->GetPathName() : FString());
		PropertyObject->SetBoolField(TEXT("is_subobject_reference"), ReferencedObject && IsInOuterChain(ReferencedObject, Owner));
		PropertyObject->SetBoolField(TEXT("cycle_detected"), ReferencedObject == Owner);
		PropertyObject->SetNumberField(TEXT("reference_depth"), ReferencedObject ? 1 : 0);
		return;
	}

	if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
	{
		const FSoftObjectPtr SoftObjectPtr = SoftObjectProperty->GetPropertyValue(ValuePtr);
		PropertyObject->SetStringField(TEXT("object_path"), SoftObjectPtr.ToSoftObjectPath().ToString());
		PropertyObject->SetBoolField(TEXT("is_subobject_reference"), false);
		PropertyObject->SetBoolField(TEXT("cycle_detected"), false);
		PropertyObject->SetNumberField(TEXT("reference_depth"), SoftObjectPtr.IsNull() ? 0 : 1);
		return;
	}

	if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
	{
		const FSoftObjectPtr SoftObjectPtr = SoftClassProperty->GetPropertyValue(ValuePtr);
		PropertyObject->SetStringField(TEXT("object_path"), SoftObjectPtr.ToSoftObjectPath().ToString());
		PropertyObject->SetBoolField(TEXT("is_subobject_reference"), false);
		PropertyObject->SetBoolField(TEXT("cycle_detected"), false);
		PropertyObject->SetNumberField(TEXT("reference_depth"), SoftObjectPtr.IsNull() ? 0 : 1);
	}
}

void AddDefaultComparisonFields(
	const FProperty* Property,
	const void* ValuePtr,
	UObject* AssetObject,
	TSharedRef<FJsonObject> PropertyObject)
{
	UObject* ClassDefaultObject = AssetObject && AssetObject->GetClass()
		? AssetObject->GetClass()->GetDefaultObject(false)
		: nullptr;

	if (ClassDefaultObject)
	{
		const void* CdoValuePtr = Property->ContainerPtrToValuePtr<void>(ClassDefaultObject);
		const FString CdoValueText = ExportPropertyText(Property, CdoValuePtr, ClassDefaultObject);
		const FString CurrentValueText = PropertyObject->GetStringField(TEXT("value_text"));
		PropertyObject->SetStringField(TEXT("cdo_value_text"), CdoValueText);
		PropertyObject->SetBoolField(TEXT("differs_from_cdo"), !CurrentValueText.Equals(CdoValueText, ESearchCase::CaseSensitive));
	}

	const UStruct* OwnerStruct = Property->GetOwnerStruct();
	const UClass* OwnerClass = Cast<UClass>(OwnerStruct);
	UClass* SuperClass = AssetObject && AssetObject->GetClass()
		? AssetObject->GetClass()->GetSuperClass()
		: nullptr;

	if (OwnerClass && SuperClass && SuperClass->IsChildOf(OwnerClass))
	{
		UObject* SuperDefaultObject = SuperClass->GetDefaultObject(false);
		if (SuperDefaultObject)
		{
			const void* SuperValuePtr = Property->ContainerPtrToValuePtr<void>(SuperDefaultObject);
			PropertyObject->SetStringField(TEXT("super_cdo_value_text"), ExportPropertyText(Property, SuperValuePtr, SuperDefaultObject));
			PropertyObject->SetBoolField(TEXT("is_inherited"), OwnerClass != AssetObject->GetClass());
			PropertyObject->SetStringField(TEXT("declaring_class_path"), OwnerClass->GetPathName());
		}
	}
	else if (OwnerClass)
	{
		PropertyObject->SetBoolField(TEXT("is_inherited"), OwnerClass != AssetObject->GetClass());
		PropertyObject->SetStringField(TEXT("declaring_class_path"), OwnerClass->GetPathName());
	}
}
}

void FUObjectReflectionReader::ReadAssetIntoEntity(const FAssetData& AssetData, const FScanOptions& Options, FEntityRecord& Entity)
{
	UObject* AssetObject = AssetData.GetAsset();
	if (!AssetObject)
	{
		FDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("UEPI_ASSET_LOAD_FAILED");
		Diagnostic.Severity = TEXT("error");
		Diagnostic.Message = TEXT("Failed to load asset for UObject reflection.");
		Diagnostic.Context.Add(TEXT("object_path"), AssetData.GetObjectPathString());
		Entity.Diagnostics.Add(MoveTemp(Diagnostic));
		Entity.Completeness.State = ECompletenessState::Failed;
		return;
	}

	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("schema_version"), TEXT("uepi.uobject_reflection.v1"));
	Snapshot->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Snapshot->SetStringField(TEXT("object_path"), AssetObject->GetPathName());
	Snapshot->SetStringField(TEXT("class_path"), AssetObject->GetClass()->GetPathName());
	Snapshot->SetStringField(TEXT("outer_path"), AssetObject->GetOuter() ? AssetObject->GetOuter()->GetPathName() : FString());
	Snapshot->SetStringField(TEXT("class_default_object_path"), AssetObject->GetClass()->GetDefaultObject(false) ? AssetObject->GetClass()->GetDefaultObject(false)->GetPathName() : FString());
	Snapshot->SetStringField(TEXT("super_class_path"), AssetObject->GetClass()->GetSuperClass() ? AssetObject->GetClass()->GetSuperClass()->GetPathName() : FString());
	Snapshot->SetNumberField(TEXT("max_inline_collection_items"), Options.MaxInlineCollectionItems);
	Snapshot->SetNumberField(TEXT("max_reference_depth"), 1);
	Snapshot->SetStringField(TEXT("cycle_guard"), TEXT("direct_object_reference_guard"));

	TArray<TSharedPtr<FJsonValue>> Properties;
	TArray<FProperty*> SortedProperties;

	for (TFieldIterator<FProperty> PropertyIt(AssetObject->GetClass(), EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
	{
		SortedProperties.Add(*PropertyIt);
	}

	SortedProperties.Sort([](const FProperty& Left, const FProperty& Right)
	{
		return Left.GetName() < Right.GetName();
	});

	for (const FProperty* Property : SortedProperties)
	{
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(AssetObject);
		TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
		PropertyObject->SetStringField(TEXT("name"), Property->GetName());
		PropertyObject->SetStringField(TEXT("kind"), PropertyKind(Property));
		PropertyObject->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		PropertyObject->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
		if (const UStruct* OwnerStruct = Property->GetOwnerStruct())
		{
			PropertyObject->SetStringField(TEXT("declaring_struct_path"), OwnerStruct->GetPathName());
		}

		FString FullValueText = ExportPropertyText(Property, ValuePtr, AssetObject);
		bool bTruncated = false;
		FString ValueText = FullValueText;
		if (Options.MaxInlineCollectionItems > 0 && ValueText.Len() > Options.MaxInlineCollectionItems)
		{
			ValueText.LeftInline(Options.MaxInlineCollectionItems);
			bTruncated = true;
		}
		PropertyObject->SetBoolField(TEXT("truncated"), bTruncated);

		PropertyObject->SetStringField(TEXT("value_text"), ValueText);
		const int32 ElementCount = CollectionElementCount(Property, ValuePtr);
		if (ElementCount != INDEX_NONE)
		{
			PropertyObject->SetNumberField(TEXT("collection_count"), ElementCount);
			PropertyObject->SetNumberField(TEXT("inline_limit"), Options.MaxInlineCollectionItems);
			const bool bArtifactRequired = ElementCount > Options.MaxInlineCollectionItems;
			PropertyObject->SetBoolField(TEXT("artifact_required"), bArtifactRequired);
			if (bArtifactRequired)
			{
				FString ArtifactError;
				PropertyObject->SetObjectField(TEXT("collection_artifact"), WriteCollectionArtifact(Options, Entity, AssetObject, Property, ElementCount, FullValueText, ArtifactError));
				if (!ArtifactError.IsEmpty())
				{
					FDiagnostic Diagnostic;
					Diagnostic.Code = TEXT("UEPI_UOBJECT_COLLECTION_ARTIFACT_WRITE_FAILED");
					Diagnostic.Severity = TEXT("warning");
					Diagnostic.Message = TEXT("Failed to write a UObject large collection sidecar artifact.");
					Diagnostic.Context.Add(TEXT("object_path"), AssetObject->GetPathName());
					Diagnostic.Context.Add(TEXT("property_name"), Property->GetName());
					Diagnostic.Context.Add(TEXT("error"), ArtifactError);
					Entity.Diagnostics.Add(MoveTemp(Diagnostic));
				}
			}
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			PropertyObject->SetObjectField(TEXT("struct_value"), StructValueSnapshot(StructProperty, ValuePtr, AssetObject, 0, 2, Options.MaxInlineCollectionItems));
		}
		AddReferenceFields(Property, ValuePtr, AssetObject, PropertyObject);
		AddDefaultComparisonFields(Property, ValuePtr, AssetObject, PropertyObject);
		Properties.Add(MakeShared<FJsonValueObject>(PropertyObject));
	}

	Snapshot->SetNumberField(TEXT("property_count"), Properties.Num());
	Snapshot->SetArrayField(TEXT("properties"), Properties);

	Entity.Attributes.Add(TEXT("object_class_path"), AssetObject->GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Reflection));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered.AddUnique(TEXT("uobject_reflection_properties"));
	Entity.Completeness.Covered.AddUnique(TEXT("cdo_default_comparison"));
	Entity.Completeness.Covered.AddUnique(TEXT("super_default_comparison"));
	Entity.Completeness.Covered.AddUnique(TEXT("direct_subobject_reference_marking"));
	Entity.Completeness.Covered.AddUnique(TEXT("direct_reference_cycle_guard"));
	Entity.Completeness.Covered.AddUnique(TEXT("collection_capacity_metadata"));
	Entity.Completeness.Covered.AddUnique(TEXT("struct_serializer"));
	Entity.Completeness.Covered.AddUnique(TEXT("large_collection_artifacts"));
	Entity.Completeness.Omitted.Remove(TEXT("loaded_uobject_properties"));
	Entity.Completeness.Omitted.AddUnique(TEXT("domain_specific_structure"));
	Entity.Snapshot = Snapshot;
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		AssetObject->GetPathName(),
		TEXT("UObject properties exported through FProperty without saving or compiling assets.")
	});
}
}
