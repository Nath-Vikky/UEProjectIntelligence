#include "UEPIRenderAssetReader.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Materials/MaterialInterface.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString RenderBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

TSharedRef<FJsonObject> RenderVectorObject(const FVector& Vector)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Vector.X);
	Object->SetNumberField(TEXT("y"), Vector.Y);
	Object->SetNumberField(TEXT("z"), Vector.Z);
	return Object;
}

TSharedRef<FJsonObject> RenderBoundsObject(const FBoxSphereBounds& Bounds)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("origin"), RenderVectorObject(Bounds.Origin));
	Object->SetObjectField(TEXT("box_extent"), RenderVectorObject(Bounds.BoxExtent));
	Object->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);
	return Object;
}

void AddRenderEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

void AddRenderRelation(
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

FEntityRecord* FindRenderEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

FString AddMaterialEntity(
	const FString& ProjectId,
	const UMaterialInterface* Material,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	if (!Material)
	{
		return FString();
	}

	const FString MaterialPath = Material->GetPathName();
	const FString MaterialId = MakeStableId(ProjectId, TEXT("material"), MaterialPath);
	if (FindRenderEntity(OutEntities, MaterialId))
	{
		return MaterialId;
	}

	FEntityRecord Entity;
	Entity.Id = MaterialId;
	Entity.Kind = TEXT("material");
	Entity.CanonicalKey = MaterialPath;
	Entity.DisplayName = Material->GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("material_path"), MaterialPath);
	Entity.Attributes.Add(TEXT("material_class"), Material->GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Registry));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("material_reference") };
	Entity.Completeness.Omitted = { TEXT("material_graph") };
	AddRenderEvidence(Entity, EvidencePath, TEXT("Material reference read from a render asset."));
	OutEntities.Add(MoveTemp(Entity));
	return MaterialId;
}

FString AddStaticMeshLodEntity(
	const FString& ProjectId,
	const UStaticMesh& Mesh,
	int32 LodIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = FString::Printf(TEXT("%s:lod:%d"), *Mesh.GetPathName(), LodIndex);
	const FString LodId = MakeStableId(ProjectId, TEXT("static_mesh_lod"), CanonicalKey);
	if (FindRenderEntity(OutEntities, LodId))
	{
		return LodId;
	}

	FEntityRecord Entity;
	Entity.Id = LodId;
	Entity.Kind = TEXT("static_mesh_lod");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = FString::Printf(TEXT("LOD %d"), LodIndex);
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("mesh_path"), Mesh.GetPathName());
	Entity.Attributes.Add(TEXT("lod_index"), FString::FromInt(LodIndex));
	Entity.Attributes.Add(TEXT("vertex_count"), FString::FromInt(Mesh.GetNumVertices(LodIndex)));
	Entity.Attributes.Add(TEXT("triangle_count"), FString::FromInt(Mesh.GetNumTriangles(LodIndex)));
	Entity.Attributes.Add(TEXT("section_count"), FString::FromInt(Mesh.GetNumSections(LodIndex)));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("lod_counts") };
	Entity.Completeness.Omitted = { TEXT("vertex_buffers"), TEXT("index_buffers") };
	AddRenderEvidence(Entity, Mesh.GetPathName(), TEXT("Static mesh LOD counts read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return LodId;
}

TSharedRef<FJsonObject> StaticMeshSnapshot(
	const FString& ProjectId,
	const UStaticMesh& Mesh,
	const FString& MeshId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> LodValues;
	for (int32 LodIndex = 0; LodIndex < Mesh.GetNumLODs(); ++LodIndex)
	{
		const FString LodId = AddStaticMeshLodEntity(ProjectId, Mesh, LodIndex, OutEntities);
		AddRenderRelation(ProjectId, TEXT("contains_lod"), MeshId, LodId, Mesh.GetPathName(), TEXT("Static mesh contains this LOD."), OutRelations);

		TSharedRef<FJsonObject> LodObject = MakeShared<FJsonObject>();
		LodObject->SetStringField(TEXT("id"), LodId);
		LodObject->SetNumberField(TEXT("index"), LodIndex);
		LodObject->SetNumberField(TEXT("vertex_count"), Mesh.GetNumVertices(LodIndex));
		LodObject->SetNumberField(TEXT("triangle_count"), Mesh.GetNumTriangles(LodIndex));
		LodObject->SetNumberField(TEXT("section_count"), Mesh.GetNumSections(LodIndex));
		LodValues.Add(MakeShared<FJsonValueObject>(LodObject));
	}

	TArray<TSharedPtr<FJsonValue>> MaterialValues;
	const TArray<FStaticMaterial>& StaticMaterials = Mesh.GetStaticMaterials();
	for (int32 MaterialIndex = 0; MaterialIndex < StaticMaterials.Num(); ++MaterialIndex)
	{
		const FStaticMaterial& Slot = StaticMaterials[MaterialIndex];
		const UMaterialInterface* Material = Mesh.GetMaterial(MaterialIndex);
		const FString MaterialId = AddMaterialEntity(ProjectId, Material, Mesh.GetPathName(), OutEntities);
		if (!MaterialId.IsEmpty())
		{
			AddRenderRelation(ProjectId, TEXT("uses_material"), MeshId, MaterialId, Mesh.GetPathName(), TEXT("Static mesh material slot references this material."), OutRelations);
		}

		TSharedRef<FJsonObject> MaterialObject = MakeShared<FJsonObject>();
		MaterialObject->SetNumberField(TEXT("slot_index"), MaterialIndex);
		MaterialObject->SetStringField(TEXT("slot_name"), Slot.MaterialSlotName.ToString());
		MaterialObject->SetStringField(TEXT("imported_slot_name"), Slot.ImportedMaterialSlotName.ToString());
		MaterialObject->SetStringField(TEXT("material_path"), Material ? Material->GetPathName() : FString());
		MaterialValues.Add(MakeShared<FJsonValueObject>(MaterialObject));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.static_mesh.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("mesh_path"), Mesh.GetPathName());
	Object->SetNumberField(TEXT("lod_count"), Mesh.GetNumLODs());
	Object->SetNumberField(TEXT("material_count"), StaticMaterials.Num());
	Object->SetObjectField(TEXT("bounds"), RenderBoundsObject(Mesh.GetBounds()));
	Object->SetArrayField(TEXT("lods"), LodValues);
	Object->SetArrayField(TEXT("materials"), MaterialValues);
	return Object;
}

FString AddStaticMeshEntity(
	const FString& ProjectId,
	const UStaticMesh& Mesh,
	TArray<FEntityRecord>& OutEntities)
{
	const FString MeshPath = Mesh.GetPathName();
	const FString MeshId = MakeStableId(ProjectId, TEXT("static_mesh"), MeshPath);
	if (FindRenderEntity(OutEntities, MeshId))
	{
		return MeshId;
	}

	FEntityRecord Entity;
	Entity.Id = MeshId;
	Entity.Kind = TEXT("static_mesh");
	Entity.CanonicalKey = MeshPath;
	Entity.DisplayName = Mesh.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("mesh_path"), MeshPath);
	Entity.Attributes.Add(TEXT("lod_count"), FString::FromInt(Mesh.GetNumLODs()));
	Entity.Attributes.Add(TEXT("material_count"), FString::FromInt(Mesh.GetStaticMaterials().Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("static_mesh_metadata"), TEXT("lod_counts"), TEXT("material_slots") };
	Entity.Completeness.Omitted = { TEXT("vertex_buffers"), TEXT("index_buffers"), TEXT("collision_geometry") };
	AddRenderEvidence(Entity, MeshPath, TEXT("UStaticMesh metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return MeshId;
}

FString TextureEnumValue(const TCHAR* Prefix, int32 Value)
{
	return FString::Printf(TEXT("%s%d"), Prefix, Value);
}

FString RenderTextureClassName(ETextureClass TextureClass)
{
	switch (TextureClass)
	{
	case ETextureClass::Invalid:
		return TEXT("Invalid");
	case ETextureClass::TwoD:
		return TEXT("TwoD");
	case ETextureClass::Cube:
		return TEXT("Cube");
	case ETextureClass::Array:
		return TEXT("Array");
	case ETextureClass::CubeArray:
		return TEXT("CubeArray");
	case ETextureClass::Volume:
		return TEXT("Volume");
	case ETextureClass::TwoDDynamic:
		return TEXT("TwoDDynamic");
	case ETextureClass::RenderTarget:
		return TEXT("RenderTarget");
	case ETextureClass::Other2DNoSource:
		return TEXT("Other2DNoSource");
	case ETextureClass::OtherUnknown:
		return TEXT("OtherUnknown");
	default:
		return TextureEnumValue(TEXT("ETextureClass_"), static_cast<int32>(TextureClass));
	}
}

int32 RenderTextureSurfaceSizeToInt(float Value)
{
	return FMath::Max(0, FMath::RoundToInt(Value));
}

FString RenderTextureDimensionSource(const UTexture& Texture)
{
#if WITH_EDITORONLY_DATA
	if (Texture.Source.IsValid())
	{
		return TEXT("editor_source");
	}
#endif
	return TEXT("platform_api");
}

int32 RenderTextureSourceSizeX(const UTexture& Texture)
{
#if WITH_EDITORONLY_DATA
	if (Texture.Source.IsValid())
	{
		return static_cast<int32>(Texture.Source.GetSizeX());
	}
#endif
	return 0;
}

int32 RenderTextureSourceSizeY(const UTexture& Texture)
{
#if WITH_EDITORONLY_DATA
	if (Texture.Source.IsValid())
	{
		return static_cast<int32>(Texture.Source.GetSizeY());
	}
#endif
	return 0;
}

int32 RenderTextureSourceMipCount(const UTexture& Texture)
{
#if WITH_EDITORONLY_DATA
	if (Texture.Source.IsValid())
	{
		return Texture.Source.GetNumMips();
	}
#endif
	return 0;
}

int32 RenderTextureSourceSliceCount(const UTexture& Texture)
{
#if WITH_EDITORONLY_DATA
	if (Texture.Source.IsValid())
	{
		return Texture.Source.GetNumSlices();
	}
#endif
	return 0;
}

FString RenderTextureSourceFormat(const UTexture& Texture)
{
#if WITH_EDITORONLY_DATA
	if (Texture.Source.IsValid())
	{
		return TextureEnumValue(TEXT("TSF_"), static_cast<int32>(Texture.Source.GetFormat()));
	}
#endif
	return FString();
}

bool RenderTextureSourceIsLongLat(const UTexture& Texture)
{
#if WITH_EDITORONLY_DATA
	if (Texture.Source.IsValid())
	{
		return Texture.Source.IsLongLatCubemap();
	}
#endif
	return false;
}

int32 RenderTextureSizeX(const UTexture2D& Texture)
{
#if WITH_EDITORONLY_DATA
	if (Texture.Source.IsValid())
	{
		return static_cast<int32>(Texture.Source.GetSizeX());
	}
#endif
	return Texture.GetSizeX();
}

int32 RenderTextureSizeY(const UTexture2D& Texture)
{
#if WITH_EDITORONLY_DATA
	if (Texture.Source.IsValid())
	{
		return static_cast<int32>(Texture.Source.GetSizeY());
	}
#endif
	return Texture.GetSizeY();
}

int32 RenderTextureMipCount(const UTexture2D& Texture)
{
#if WITH_EDITORONLY_DATA
	if (Texture.Source.IsValid())
	{
		return Texture.Source.GetNumMips();
	}
#endif
	return Texture.GetNumMips();
}

int32 RenderTextureCubeSizeX(const UTextureCube& Texture)
{
	const int32 SourceSizeX = RenderTextureSourceSizeX(Texture);
	return SourceSizeX > 0 ? SourceSizeX : Texture.GetSizeX();
}

int32 RenderTextureCubeSizeY(const UTextureCube& Texture)
{
	const int32 SourceSizeY = RenderTextureSourceSizeY(Texture);
	return SourceSizeY > 0 ? SourceSizeY : Texture.GetSizeY();
}

int32 RenderTextureCubeMipCount(const UTextureCube& Texture)
{
	const int32 SourceMipCount = RenderTextureSourceMipCount(Texture);
	return SourceMipCount > 0 ? SourceMipCount : Texture.GetNumMips();
}

TSharedRef<FJsonObject> TextureSnapshot(const UTexture2D& Texture)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.texture2d.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("texture_path"), Texture.GetPathName());
	Object->SetStringField(TEXT("dimension_source"), RenderTextureDimensionSource(Texture));
	Object->SetNumberField(TEXT("size_x"), RenderTextureSizeX(Texture));
	Object->SetNumberField(TEXT("size_y"), RenderTextureSizeY(Texture));
	Object->SetNumberField(TEXT("mip_count"), RenderTextureMipCount(Texture));
	Object->SetStringField(TEXT("pixel_format"), TextureEnumValue(TEXT("PF_"), static_cast<int32>(Texture.GetPixelFormat())));
	Object->SetStringField(TEXT("compression_settings"), TextureEnumValue(TEXT("TC_"), static_cast<int32>(Texture.CompressionSettings.GetValue())));
	Object->SetStringField(TEXT("address_x"), TextureEnumValue(TEXT("TA_"), static_cast<int32>(Texture.AddressX.GetValue())));
	Object->SetStringField(TEXT("address_y"), TextureEnumValue(TEXT("TA_"), static_cast<int32>(Texture.AddressY.GetValue())));
	Object->SetBoolField(TEXT("srgb"), Texture.SRGB);
	return Object;
}

TSharedRef<FJsonObject> TextureCubeSnapshot(const UTextureCube& Texture)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.texture_cube.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("texture_path"), Texture.GetPathName());
	Object->SetStringField(TEXT("texture_class"), Texture.GetClass()->GetPathName());
	Object->SetStringField(TEXT("dimension_source"), RenderTextureDimensionSource(Texture));
	Object->SetNumberField(TEXT("size_x"), RenderTextureCubeSizeX(Texture));
	Object->SetNumberField(TEXT("size_y"), RenderTextureCubeSizeY(Texture));
	Object->SetNumberField(TEXT("face_count"), 6);
	Object->SetNumberField(TEXT("source_slice_count"), RenderTextureSourceSliceCount(Texture));
	Object->SetNumberField(TEXT("mip_count"), RenderTextureCubeMipCount(Texture));
	Object->SetStringField(TEXT("pixel_format"), TextureEnumValue(TEXT("PF_"), static_cast<int32>(Texture.GetPixelFormat())));
	Object->SetStringField(TEXT("compression_settings"), TextureEnumValue(TEXT("TC_"), static_cast<int32>(Texture.CompressionSettings.GetValue())));
	Object->SetStringField(TEXT("source_format"), RenderTextureSourceFormat(Texture));
	Object->SetBoolField(TEXT("source_is_long_lat"), RenderTextureSourceIsLongLat(Texture));
	Object->SetBoolField(TEXT("srgb"), Texture.SRGB);
	return Object;
}

TSharedRef<FJsonObject> GenericTextureSnapshot(const UTexture& Texture)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.texture.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("texture_path"), Texture.GetPathName());
	Object->SetStringField(TEXT("texture_class"), Texture.GetClass()->GetPathName());
	Object->SetStringField(TEXT("texture_classification"), RenderTextureClassName(Texture.GetTextureClass()));
	Object->SetStringField(TEXT("material_type"), TextureEnumValue(TEXT("MCT_"), static_cast<int32>(Texture.GetMaterialType())));
	Object->SetStringField(TEXT("dimension_source"), RenderTextureDimensionSource(Texture));
	Object->SetNumberField(TEXT("surface_width"), Texture.GetSurfaceWidth());
	Object->SetNumberField(TEXT("surface_height"), Texture.GetSurfaceHeight());
	Object->SetNumberField(TEXT("surface_depth"), Texture.GetSurfaceDepth());
	Object->SetNumberField(TEXT("surface_array_size"), static_cast<double>(Texture.GetSurfaceArraySize()));
	Object->SetNumberField(TEXT("source_size_x"), RenderTextureSourceSizeX(Texture));
	Object->SetNumberField(TEXT("source_size_y"), RenderTextureSourceSizeY(Texture));
	Object->SetNumberField(TEXT("source_slice_count"), RenderTextureSourceSliceCount(Texture));
	Object->SetNumberField(TEXT("source_mip_count"), RenderTextureSourceMipCount(Texture));
	Object->SetStringField(TEXT("source_format"), RenderTextureSourceFormat(Texture));
	Object->SetBoolField(TEXT("source_is_long_lat"), RenderTextureSourceIsLongLat(Texture));
	Object->SetStringField(TEXT("compression_settings"), TextureEnumValue(TEXT("TC_"), static_cast<int32>(Texture.CompressionSettings.GetValue())));
	Object->SetStringField(TEXT("lod_group"), TextureEnumValue(TEXT("TEXTUREGROUP_"), static_cast<int32>(Texture.LODGroup.GetValue())));
	Object->SetBoolField(TEXT("srgb"), Texture.SRGB);
	return Object;
}

FString AddTextureEntity(
	const FString& ProjectId,
	const UTexture& Texture,
	const FString& TextureType,
	const FString& DimensionSource,
	int32 SizeX,
	int32 SizeY,
	int32 MipCount,
	const FString& PixelFormat,
	const FString& EvidenceDetail,
	TArray<FEntityRecord>& OutEntities)
{
	const FString TexturePath = Texture.GetPathName();
	const FString TextureId = MakeStableId(ProjectId, TEXT("texture"), TexturePath);
	if (FindRenderEntity(OutEntities, TextureId))
	{
		return TextureId;
	}

	FEntityRecord Entity;
	Entity.Id = TextureId;
	Entity.Kind = TEXT("texture");
	Entity.CanonicalKey = TexturePath;
	Entity.DisplayName = Texture.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("texture_path"), TexturePath);
	Entity.Attributes.Add(TEXT("texture_class"), Texture.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("texture_type"), TextureType);
	Entity.Attributes.Add(TEXT("dimension_source"), DimensionSource);
	Entity.Attributes.Add(TEXT("size_x"), FString::FromInt(SizeX));
	Entity.Attributes.Add(TEXT("size_y"), FString::FromInt(SizeY));
	Entity.Attributes.Add(TEXT("mip_count"), FString::FromInt(MipCount));
	Entity.Attributes.Add(TEXT("pixel_format"), PixelFormat);
	Entity.Attributes.Add(TEXT("compression_settings"), TextureEnumValue(TEXT("TC_"), static_cast<int32>(Texture.CompressionSettings.GetValue())));
	Entity.Attributes.Add(TEXT("srgb"), RenderBool(Texture.SRGB));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("texture_metadata"), TEXT("texture_dimensions") };
	Entity.Completeness.Omitted = { TEXT("source_pixels"), TEXT("derived_platform_data") };
	AddRenderEvidence(Entity, TexturePath, EvidenceDetail);
	OutEntities.Add(MoveTemp(Entity));
	return TextureId;
}
}

bool FRenderAssetReader::AppendRenderAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (UStaticMesh* Mesh = Cast<UStaticMesh>(&Asset))
	{
		const FString MeshId = AddStaticMeshEntity(ProjectId, *Mesh, OutEntities);
		AddRenderRelation(ProjectId, TEXT("contains_static_mesh"), AssetEntity.Id, MeshId, Mesh->GetPathName(), TEXT("Static mesh asset contains the extracted static mesh record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("static_mesh"), StaticMeshSnapshot(ProjectId, *Mesh, MeshId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("static_mesh_lod_count"), FString::FromInt(Mesh->GetNumLODs()));
		AssetEntity.Attributes.Add(TEXT("static_mesh_material_count"), FString::FromInt(Mesh->GetStaticMaterials().Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("static_mesh_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("static_mesh_lods"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("static_mesh_material_slots"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("static_mesh_vertex_buffers"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("static_mesh_collision_geometry"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Mesh->GetPathName(),
			TEXT("UStaticMesh structure extracted through public Engine API.")
		});
		return true;
	}

	if (UTexture2D* Texture = Cast<UTexture2D>(&Asset))
	{
		const FString TextureId = AddTextureEntity(
			ProjectId,
			*Texture,
			TEXT("texture2d"),
			RenderTextureDimensionSource(*Texture),
			RenderTextureSizeX(*Texture),
			RenderTextureSizeY(*Texture),
			RenderTextureMipCount(*Texture),
			TextureEnumValue(TEXT("PF_"), static_cast<int32>(Texture->GetPixelFormat())),
			TEXT("UTexture2D metadata read through public Engine API."),
			OutEntities);
		AddRenderRelation(ProjectId, TEXT("contains_texture"), AssetEntity.Id, TextureId, Texture->GetPathName(), TEXT("Texture asset contains the extracted texture record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("texture2d"), TextureSnapshot(*Texture));
		AssetEntity.Attributes.Add(TEXT("texture_dimension_source"), RenderTextureDimensionSource(*Texture));
		AssetEntity.Attributes.Add(TEXT("texture_size_x"), FString::FromInt(RenderTextureSizeX(*Texture)));
		AssetEntity.Attributes.Add(TEXT("texture_size_y"), FString::FromInt(RenderTextureSizeY(*Texture)));
		AssetEntity.Attributes.Add(TEXT("texture_mip_count"), FString::FromInt(RenderTextureMipCount(*Texture)));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("texture_metadata"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("texture_source_pixels"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Texture->GetPathName(),
			TEXT("UTexture2D metadata extracted through public Engine API.")
		});
		return true;
	}

	if (UTextureCube* Texture = Cast<UTextureCube>(&Asset))
	{
		const FString TextureId = AddTextureEntity(
			ProjectId,
			*Texture,
			TEXT("texture_cube"),
			RenderTextureDimensionSource(*Texture),
			RenderTextureCubeSizeX(*Texture),
			RenderTextureCubeSizeY(*Texture),
			RenderTextureCubeMipCount(*Texture),
			TextureEnumValue(TEXT("PF_"), static_cast<int32>(Texture->GetPixelFormat())),
			TEXT("UTextureCube metadata read through public Engine API."),
			OutEntities);
		AddRenderRelation(ProjectId, TEXT("contains_texture"), AssetEntity.Id, TextureId, Texture->GetPathName(), TEXT("Texture cube asset contains the extracted texture record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("texture_cube"), TextureCubeSnapshot(*Texture));
		AssetEntity.Attributes.Add(TEXT("texture_type"), TEXT("texture_cube"));
		AssetEntity.Attributes.Add(TEXT("texture_dimension_source"), RenderTextureDimensionSource(*Texture));
		AssetEntity.Attributes.Add(TEXT("texture_size_x"), FString::FromInt(RenderTextureCubeSizeX(*Texture)));
		AssetEntity.Attributes.Add(TEXT("texture_size_y"), FString::FromInt(RenderTextureCubeSizeY(*Texture)));
		AssetEntity.Attributes.Add(TEXT("texture_mip_count"), FString::FromInt(RenderTextureCubeMipCount(*Texture)));
		AssetEntity.Attributes.Add(TEXT("texture_face_count"), TEXT("6"));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("texture_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("texture_cube_dimensions"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("texture_source_pixels"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Texture->GetPathName(),
			TEXT("UTextureCube metadata extracted through public Engine API.")
		});
		return true;
	}

	if (UTexture* Texture = Cast<UTexture>(&Asset))
	{
		const int32 SurfaceSizeX = RenderTextureSurfaceSizeToInt(Texture->GetSurfaceWidth());
		const int32 SurfaceSizeY = RenderTextureSurfaceSizeToInt(Texture->GetSurfaceHeight());
		const int32 SourceMipCount = RenderTextureSourceMipCount(*Texture);
		const FString TextureId = AddTextureEntity(
			ProjectId,
			*Texture,
			TEXT("texture_generic"),
			RenderTextureDimensionSource(*Texture),
			SurfaceSizeX,
			SurfaceSizeY,
			SourceMipCount,
			TEXT("PF_Unknown"),
			TEXT("Generic UTexture metadata read through public Engine API."),
			OutEntities);
		AddRenderRelation(ProjectId, TEXT("contains_texture"), AssetEntity.Id, TextureId, Texture->GetPathName(), TEXT("Texture asset contains the extracted generic texture record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("texture"), GenericTextureSnapshot(*Texture));
		AssetEntity.Attributes.Add(TEXT("texture_type"), TEXT("texture_generic"));
		AssetEntity.Attributes.Add(TEXT("texture_classification"), RenderTextureClassName(Texture->GetTextureClass()));
		AssetEntity.Attributes.Add(TEXT("texture_dimension_source"), RenderTextureDimensionSource(*Texture));
		AssetEntity.Attributes.Add(TEXT("texture_size_x"), FString::FromInt(SurfaceSizeX));
		AssetEntity.Attributes.Add(TEXT("texture_size_y"), FString::FromInt(SurfaceSizeY));
		AssetEntity.Attributes.Add(TEXT("texture_mip_count"), FString::FromInt(SourceMipCount));
		AssetEntity.Attributes.Add(TEXT("texture_surface_depth"), FString::SanitizeFloat(Texture->GetSurfaceDepth()));
		AssetEntity.Attributes.Add(TEXT("texture_surface_array_size"), FString::FromInt(static_cast<int32>(Texture->GetSurfaceArraySize())));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("texture_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("texture_surface_summary"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("texture_source_pixels"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("texture_runtime_resource_state"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Texture->GetPathName(),
			TEXT("Generic UTexture metadata extracted through public Engine API.")
		});
		return true;
	}

	return false;
}
}
