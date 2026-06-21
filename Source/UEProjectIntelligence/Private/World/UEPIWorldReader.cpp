#include "UEPIWorldReader.h"

#include "AssetRegistry/AssetData.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "WorldPartition/ActorDescContainer.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartition/WorldPartitionActorDescUtils.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString FormatVector(const FVector& Vector)
{
	return FString::Printf(TEXT("%.6f,%.6f,%.6f"), Vector.X, Vector.Y, Vector.Z);
}

FString FormatRotator(const FRotator& Rotator)
{
	return FString::Printf(TEXT("%.6f,%.6f,%.6f"), Rotator.Pitch, Rotator.Yaw, Rotator.Roll);
}

FString FormatTransform(const FTransform& Transform)
{
	return FString::Printf(
		TEXT("location=%s;rotation=%s;scale=%s"),
		*FormatVector(Transform.GetLocation()),
		*FormatRotator(Transform.Rotator()),
		*FormatVector(Transform.GetScale3D()));
}

FString WorldTypeString(EWorldType::Type WorldType)
{
	switch (WorldType)
	{
	case EWorldType::Game:
		return TEXT("game");
	case EWorldType::Editor:
		return TEXT("editor");
	case EWorldType::PIE:
		return TEXT("pie");
	case EWorldType::EditorPreview:
		return TEXT("editor_preview");
	case EWorldType::GamePreview:
		return TEXT("game_preview");
	case EWorldType::GameRPC:
		return TEXT("game_rpc");
	case EWorldType::Inactive:
		return TEXT("inactive");
	default:
		return TEXT("unknown");
	}
}

void AddWorldRelation(
	const FString& ProjectId,
	const FString& Type,
	const FString& FromId,
	const FString& ToId,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FRelationRecord>& OutRelations)
{
	const FString RelationId = MakeRelationId(ProjectId, Type, FromId, ToId);
	if (OutRelations.ContainsByPredicate([&RelationId](const FRelationRecord& ExistingRelation)
	{
		return ExistingRelation.Id == RelationId;
	}))
	{
		return;
	}

	FRelationRecord Relation;
	Relation.Id = RelationId;
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

void AddEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

FString AddClassEntity(
	const FString& ProjectId,
	const UClass* Class,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString ClassPath = Class ? Class->GetPathName() : FString();
	const FString ClassName = Class ? Class->GetName() : FString();
	const FString ClassId = MakeStableId(ProjectId, TEXT("u_class"), ClassPath);
	for (const FEntityRecord& Entity : OutEntities)
	{
		if (Entity.Id == ClassId)
		{
			return ClassId;
		}
	}

	FEntityRecord Entity;
	Entity.Id = ClassId;
	Entity.Kind = TEXT("u_class");
	Entity.CanonicalKey = ClassPath;
	Entity.DisplayName = ClassName;
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("class_path"), ClassPath);
	Entity.Attributes.Add(TEXT("class_name"), ClassName);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Reflection));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("class_reference") };
	AddEvidence(Entity, EvidencePath, TEXT("Class reference observed while reading a loaded world-related asset."));
	OutEntities.Add(MoveTemp(Entity));
	return ClassId;
}

FString BoolString(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString LevelInstanceRuntimeBehaviorString(ELevelInstanceRuntimeBehavior RuntimeBehavior)
{
	switch (RuntimeBehavior)
	{
	case ELevelInstanceRuntimeBehavior::None:
		return TEXT("none");
	case ELevelInstanceRuntimeBehavior::Embedded_Deprecated:
		return TEXT("embedded_deprecated");
	case ELevelInstanceRuntimeBehavior::Partitioned:
		return TEXT("partitioned");
	case ELevelInstanceRuntimeBehavior::LevelStreaming:
		return TEXT("level_streaming");
	default:
		return TEXT("unknown");
	}
}

FString LevelStreamingStateString(ELevelStreamingState State)
{
	switch (State)
	{
	case ELevelStreamingState::Removed:
		return TEXT("removed");
	case ELevelStreamingState::Unloaded:
		return TEXT("unloaded");
	case ELevelStreamingState::FailedToLoad:
		return TEXT("failed_to_load");
	case ELevelStreamingState::Loading:
		return TEXT("loading");
	case ELevelStreamingState::LoadedNotVisible:
		return TEXT("loaded_not_visible");
	case ELevelStreamingState::MakingVisible:
		return TEXT("making_visible");
	case ELevelStreamingState::LoadedVisible:
		return TEXT("loaded_visible");
	case ELevelStreamingState::MakingInvisible:
		return TEXT("making_invisible");
	default:
		return TEXT("unknown");
	}
}

FString DataLayerTypeString(EDataLayerType Type)
{
	switch (Type)
	{
	case EDataLayerType::Runtime:
		return TEXT("runtime");
	case EDataLayerType::Editor:
		return TEXT("editor");
	default:
		return TEXT("unknown");
	}
}

FString DataLayerRuntimeStateString(EDataLayerRuntimeState State)
{
	return FString(GetDataLayerRuntimeStateName(State));
}

TSharedRef<FJsonObject> MakeColorObject(const FColor& Color)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("r"), Color.R);
	Object->SetNumberField(TEXT("g"), Color.G);
	Object->SetNumberField(TEXT("b"), Color.B);
	Object->SetNumberField(TEXT("a"), Color.A);
	return Object;
}

TSharedRef<FJsonObject> MakeWorldVectorObject(const FVector& Vector)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Vector.X);
	Object->SetNumberField(TEXT("y"), Vector.Y);
	Object->SetNumberField(TEXT("z"), Vector.Z);
	return Object;
}

TSharedRef<FJsonObject> MakeWorldRotatorObject(const FRotator& Rotator)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("pitch"), Rotator.Pitch);
	Object->SetNumberField(TEXT("yaw"), Rotator.Yaw);
	Object->SetNumberField(TEXT("roll"), Rotator.Roll);
	return Object;
}

TSharedRef<FJsonObject> MakeWorldTransformObject(const FTransform& Transform)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("location"), MakeWorldVectorObject(Transform.GetLocation()));
	Object->SetObjectField(TEXT("rotation"), MakeWorldRotatorObject(Transform.Rotator()));
	Object->SetObjectField(TEXT("scale"), MakeWorldVectorObject(Transform.GetScale3D()));
	return Object;
}

TSharedRef<FJsonObject> MakeWorldBoxObject(const FBox& Box)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("is_valid"), Box.IsValid != 0);
	Object->SetObjectField(TEXT("min"), MakeWorldVectorObject(Box.Min));
	Object->SetObjectField(TEXT("max"), MakeWorldVectorObject(Box.Max));
	return Object;
}

FString WorldGuidString(const FGuid& Guid)
{
	return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
}

TArray<TSharedPtr<FJsonValue>> NameArrayToJson(const TArray<FName>& Names)
{
	TArray<FName> SortedNames = Names;
	SortedNames.Sort(FNameLexicalLess());

	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FName& Name : SortedNames)
	{
		Values.Add(MakeShared<FJsonValueString>(Name.ToString()));
	}
	return Values;
}

TArray<TSharedPtr<FJsonValue>> GuidArrayToJson(const TArray<FGuid>& Guids)
{
	TArray<FGuid> SortedGuids = Guids;
	SortedGuids.Sort([](const FGuid& Left, const FGuid& Right)
	{
		return WorldGuidString(Left) < WorldGuidString(Right);
	});

	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FGuid& Guid : SortedGuids)
	{
		Values.Add(MakeShared<FJsonValueString>(WorldGuidString(Guid)));
	}
	return Values;
}

TArray<FName> GetResolvedDataLayerInstanceNames(const FWorldPartitionActorDesc& ActorDesc)
{
	if (ActorDesc.HasResolvedDataLayerInstanceNames())
	{
		return ActorDesc.GetDataLayerInstanceNames();
	}

	return {};
}

bool HasWorldEntity(const TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.ContainsByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

TArray<TSharedPtr<FJsonValue>> ActorTagsToJson(const AActor& Actor)
{
	TArray<FName> Tags = Actor.Tags;
	Tags.Sort(FNameLexicalLess());

	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FName& Tag : Tags)
	{
		Values.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	return Values;
}

FString ActorGuidString(const AActor& Actor)
{
#if WITH_EDITOR
	return WorldGuidString(Actor.GetActorGuid());
#else
	return FString();
#endif
}

FString ActorInstanceGuidString(const AActor& Actor)
{
#if WITH_EDITOR
	return WorldGuidString(Actor.GetActorInstanceGuid());
#else
	return FString();
#endif
}

FString ActorContentBundleGuidString(const AActor& Actor)
{
#if WITH_EDITOR
	return WorldGuidString(Actor.GetContentBundleGuid());
#else
	return FString();
#endif
}

FString ActorFolderPathString(const AActor& Actor)
{
#if WITH_EDITOR
	return Actor.GetFolderPath().ToString();
#else
	return FString();
#endif
}

FString ActorCanonicalKey(const ULevel& Level, const AActor& Actor)
{
#if WITH_EDITOR
	if (Actor.GetActorGuid().IsValid())
	{
		return Level.GetPathName() + TEXT(":actor_guid:") + WorldGuidString(Actor.GetActorGuid());
	}
#endif
	return Level.GetPathName() + TEXT(":actor:") + Actor.GetPathName();
}

FString ActorEntityId(const FString& ProjectId, const ULevel& Level, const AActor& Actor)
{
	return MakeStableId(ProjectId, TEXT("actor"), ActorCanonicalKey(Level, Actor));
}

FString FindActorEntityId(const AActor* Actor, const TMap<FString, FString>& ActorPathToEntityId)
{
	if (!Actor)
	{
		return FString();
	}

	if (const FString* EntityId = ActorPathToEntityId.Find(Actor->GetPathName()))
	{
		return *EntityId;
	}

	return FString();
}

ILevelInstanceInterface* GetLevelInstanceInterface(AActor& Actor)
{
	if (Actor.GetClass() && Actor.GetClass()->ImplementsInterface(ULevelInstanceInterface::StaticClass()))
	{
		return Cast<ILevelInstanceInterface>(&Actor);
	}

	return nullptr;
}

FEntityRecord MakeWorldDataLayersEntity(
	const FString& ProjectId,
	const AWorldDataLayers& WorldDataLayers,
	int32 DataLayerCount)
{
	const FString CanonicalKey = WorldDataLayers.GetPathName();
	FEntityRecord Entity;
	Entity.Id = MakeStableId(ProjectId, TEXT("world_data_layers"), CanonicalKey);
	Entity.Kind = TEXT("world_data_layers");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = WorldDataLayers.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("world_data_layers_path"), WorldDataLayers.GetPathName());
	Entity.Attributes.Add(TEXT("world_data_layers_class"), WorldDataLayers.GetClass() ? WorldDataLayers.GetClass()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("world_path"), WorldDataLayers.GetWorld() ? WorldDataLayers.GetWorld()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("level_path"), WorldDataLayers.GetLevel() ? WorldDataLayers.GetLevel()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("data_layer_count"), FString::FromInt(DataLayerCount));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("data_layer_instances"), TEXT("data_layer_hierarchy") };
	Entity.Completeness.Omitted = { TEXT("actor_data_layer_assignments"), TEXT("world_partition_actor_descriptors") };
	AddEvidence(Entity, WorldDataLayers.GetPathName(), TEXT("AWorldDataLayers actor read from a loaded level package."));
	return Entity;
}

FEntityRecord MakeDataLayerInstanceEntity(
	const FString& ProjectId,
	const UDataLayerInstance& DataLayer,
	int32 ChildCount)
{
	const FString CanonicalKey = DataLayer.GetPathName();
	const UDataLayerAsset* DataLayerAsset = DataLayer.GetAsset();
#if WITH_EDITOR
	const bool bInitiallyLoadedInEditor = DataLayer.IsInitiallyLoadedInEditor();
#else
	const bool bInitiallyLoadedInEditor = false;
#endif

	FEntityRecord Entity;
	Entity.Id = MakeStableId(ProjectId, TEXT("data_layer_instance"), CanonicalKey);
	Entity.Kind = TEXT("data_layer_instance");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = DataLayer.GetDataLayerShortName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("data_layer_path"), DataLayer.GetPathName());
	Entity.Attributes.Add(TEXT("data_layer_name"), DataLayer.GetDataLayerFName().ToString());
	Entity.Attributes.Add(TEXT("data_layer_short_name"), DataLayer.GetDataLayerShortName());
	Entity.Attributes.Add(TEXT("data_layer_full_name"), DataLayer.GetDataLayerFullName());
	Entity.Attributes.Add(TEXT("data_layer_class"), DataLayer.GetClass() ? DataLayer.GetClass()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("data_layer_type"), DataLayerTypeString(DataLayer.GetType()));
	Entity.Attributes.Add(TEXT("is_runtime"), BoolString(DataLayer.IsRuntime()));
	Entity.Attributes.Add(TEXT("initial_runtime_state"), DataLayerRuntimeStateString(DataLayer.GetInitialRuntimeState()));
	Entity.Attributes.Add(TEXT("runtime_state"), DataLayerRuntimeStateString(DataLayer.GetRuntimeState()));
	Entity.Attributes.Add(TEXT("effective_runtime_state"), DataLayerRuntimeStateString(DataLayer.GetEffectiveRuntimeState()));
	Entity.Attributes.Add(TEXT("initially_visible"), BoolString(DataLayer.IsInitiallyVisible()));
	Entity.Attributes.Add(TEXT("initially_loaded_in_editor"), BoolString(bInitiallyLoadedInEditor));
	Entity.Attributes.Add(TEXT("child_count"), FString::FromInt(ChildCount));
	Entity.Attributes.Add(TEXT("parent_data_layer_path"), DataLayer.GetParent() ? DataLayer.GetParent()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("data_layer_asset_path"), DataLayerAsset ? DataLayerAsset->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("data_layer_metadata"), TEXT("data_layer_hierarchy"), TEXT("runtime_state_metadata") };
	Entity.Completeness.Omitted = { TEXT("actor_membership"), TEXT("world_partition_runtime_cells") };
	AddEvidence(Entity, DataLayer.GetPathName(), TEXT("UDataLayerInstance read from AWorldDataLayers."));
	return Entity;
}

FString AddDataLayerAssetEntity(
	const FString& ProjectId,
	const UDataLayerAsset& DataLayerAsset,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = DataLayerAsset.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("data_layer_asset"), CanonicalKey);
	if (HasWorldEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("data_layer_asset");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = DataLayerAsset.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("data_layer_asset_path"), DataLayerAsset.GetPathName());
	Entity.Attributes.Add(TEXT("data_layer_type"), DataLayerTypeString(DataLayerAsset.GetType()));
	Entity.Attributes.Add(TEXT("is_runtime"), BoolString(DataLayerAsset.IsRuntime()));
	Entity.Attributes.Add(TEXT("is_private"), BoolString(DataLayerAsset.IsPrivate()));
	Entity.Attributes.Add(TEXT("supports_actor_filters"), BoolString(DataLayerAsset.SupportsActorFilters()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Reflection));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("data_layer_asset_metadata") };
	AddEvidence(Entity, DataLayerAsset.GetPathName(), TEXT("UDataLayerAsset referenced by a Data Layer instance."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

TArray<const UDataLayerInstance*> GetSortedChildDataLayers(const UDataLayerInstance& DataLayer)
{
	TArray<const UDataLayerInstance*> Children;
	DataLayer.ForEachChild([&Children](const UDataLayerInstance* Child)
	{
		if (Child)
		{
			Children.Add(Child);
		}
		return true;
	});
	Children.Sort([](const UDataLayerInstance& Left, const UDataLayerInstance& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});
	return Children;
}

TSharedRef<FJsonObject> MakeDataLayerObject(
	const FString& ProjectId,
	const UDataLayerInstance& DataLayer,
	const FString& DataLayerId,
	const TArray<const UDataLayerInstance*>& Children)
{
	const UDataLayerInstance* Parent = DataLayer.GetParent();
	const UDataLayerAsset* DataLayerAsset = DataLayer.GetAsset();
#if WITH_EDITOR
	const bool bInitiallyLoadedInEditor = DataLayer.IsInitiallyLoadedInEditor();
#else
	const bool bInitiallyLoadedInEditor = false;
#endif

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), DataLayerId);
	Object->SetStringField(TEXT("path"), DataLayer.GetPathName());
	Object->SetStringField(TEXT("name"), DataLayer.GetDataLayerFName().ToString());
	Object->SetStringField(TEXT("short_name"), DataLayer.GetDataLayerShortName());
	Object->SetStringField(TEXT("full_name"), DataLayer.GetDataLayerFullName());
	Object->SetStringField(TEXT("class"), DataLayer.GetClass() ? DataLayer.GetClass()->GetPathName() : FString());
	Object->SetStringField(TEXT("type"), DataLayerTypeString(DataLayer.GetType()));
	Object->SetBoolField(TEXT("is_runtime"), DataLayer.IsRuntime());
	Object->SetStringField(TEXT("initial_runtime_state"), DataLayerRuntimeStateString(DataLayer.GetInitialRuntimeState()));
	Object->SetStringField(TEXT("runtime_state"), DataLayerRuntimeStateString(DataLayer.GetRuntimeState()));
	Object->SetStringField(TEXT("effective_runtime_state"), DataLayerRuntimeStateString(DataLayer.GetEffectiveRuntimeState()));
	Object->SetBoolField(TEXT("initially_visible"), DataLayer.IsInitiallyVisible());
	Object->SetBoolField(TEXT("initially_loaded_in_editor"), bInitiallyLoadedInEditor);
	Object->SetStringField(TEXT("parent_id"), Parent ? MakeStableId(ProjectId, TEXT("data_layer_instance"), Parent->GetPathName()) : FString());
	Object->SetStringField(TEXT("parent_path"), Parent ? Parent->GetPathName() : FString());
	Object->SetStringField(TEXT("asset_path"), DataLayerAsset ? DataLayerAsset->GetPathName() : FString());
	Object->SetObjectField(TEXT("debug_color"), MakeColorObject(DataLayer.GetDebugColor()));

	TArray<TSharedPtr<FJsonValue>> ChildValues;
	for (const UDataLayerInstance* Child : Children)
	{
		if (!Child)
		{
			continue;
		}
		TSharedRef<FJsonObject> ChildObject = MakeShared<FJsonObject>();
		ChildObject->SetStringField(TEXT("id"), MakeStableId(ProjectId, TEXT("data_layer_instance"), Child->GetPathName()));
		ChildObject->SetStringField(TEXT("path"), Child->GetPathName());
		ChildValues.Add(MakeShared<FJsonValueObject>(ChildObject));
	}
	Object->SetNumberField(TEXT("child_count"), ChildValues.Num());
	Object->SetArrayField(TEXT("children"), ChildValues);
	return Object;
}

bool AppendWorldDataLayers(
	AWorldDataLayers& WorldDataLayers,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<UDataLayerInstance*> DataLayers;
	WorldDataLayers.ForEachDataLayer([&DataLayers](UDataLayerInstance* DataLayer)
	{
		if (DataLayer)
		{
			DataLayers.AddUnique(DataLayer);
		}
		return true;
	});
	DataLayers.Sort([](const UDataLayerInstance& Left, const UDataLayerInstance& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});

	FEntityRecord WorldDataLayersEntity = MakeWorldDataLayersEntity(ProjectId, WorldDataLayers, DataLayers.Num());
	const FString WorldDataLayersId = WorldDataLayersEntity.Id;
	if (!HasWorldEntity(OutEntities, WorldDataLayersId))
	{
		OutEntities.Add(MoveTemp(WorldDataLayersEntity));
	}

	AddWorldRelation(
		ProjectId,
		TEXT("contains_world_data_layers"),
		AssetEntity.Id,
		WorldDataLayersId,
		WorldDataLayers.GetPathName(),
		TEXT("Asset contains an AWorldDataLayers actor."),
		OutRelations);
	const FString WorldDataLayersClassId = AddClassEntity(ProjectId, WorldDataLayers.GetClass(), WorldDataLayers.GetPathName(), OutEntities);
	AddWorldRelation(ProjectId, TEXT("instance_of"), WorldDataLayersId, WorldDataLayersClassId, WorldDataLayers.GetPathName(), TEXT("WorldDataLayers is an instance of this UClass."), OutRelations);

	TArray<TSharedPtr<FJsonValue>> DataLayerValues;
	for (UDataLayerInstance* DataLayer : DataLayers)
	{
		if (!DataLayer)
		{
			continue;
		}

		const TArray<const UDataLayerInstance*> Children = GetSortedChildDataLayers(*DataLayer);
		FEntityRecord DataLayerEntity = MakeDataLayerInstanceEntity(ProjectId, *DataLayer, Children.Num());
		const FString DataLayerId = DataLayerEntity.Id;
		if (!HasWorldEntity(OutEntities, DataLayerId))
		{
			OutEntities.Add(MoveTemp(DataLayerEntity));
		}

		AddWorldRelation(ProjectId, TEXT("contains_data_layer"), WorldDataLayersId, DataLayerId, DataLayer->GetPathName(), TEXT("WorldDataLayers contains a Data Layer instance."), OutRelations);
		const FString DataLayerClassId = AddClassEntity(ProjectId, DataLayer->GetClass(), DataLayer->GetPathName(), OutEntities);
		AddWorldRelation(ProjectId, TEXT("instance_of"), DataLayerId, DataLayerClassId, DataLayer->GetPathName(), TEXT("Data Layer instance has this UClass."), OutRelations);

		if (const UDataLayerInstance* Parent = DataLayer->GetParent())
		{
			const FString ParentId = MakeStableId(ProjectId, TEXT("data_layer_instance"), Parent->GetPathName());
			AddWorldRelation(ProjectId, TEXT("data_layer_parent"), DataLayerId, ParentId, DataLayer->GetPathName(), TEXT("Data Layer instance has this parent Data Layer."), OutRelations);
		}

		if (const UDataLayerAsset* DataLayerAsset = DataLayer->GetAsset())
		{
			const FString DataLayerAssetId = AddDataLayerAssetEntity(ProjectId, *DataLayerAsset, OutEntities);
			AddWorldRelation(ProjectId, TEXT("uses_data_layer_asset"), DataLayerId, DataLayerAssetId, DataLayer->GetPathName(), TEXT("Data Layer instance references a Data Layer asset."), OutRelations);
		}

		DataLayerValues.Add(MakeShared<FJsonValueObject>(MakeDataLayerObject(ProjectId, *DataLayer, DataLayerId, Children)));
	}

	TSharedRef<FJsonObject> WorldDataLayersObject = MakeShared<FJsonObject>();
	WorldDataLayersObject->SetStringField(TEXT("schema_version"), TEXT("uepi.world_data_layers.v1"));
	WorldDataLayersObject->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	WorldDataLayersObject->SetStringField(TEXT("world_data_layers_path"), WorldDataLayers.GetPathName());
	WorldDataLayersObject->SetStringField(TEXT("world_data_layers_class"), WorldDataLayers.GetClass() ? WorldDataLayers.GetClass()->GetPathName() : FString());
	WorldDataLayersObject->SetStringField(TEXT("world_path"), WorldDataLayers.GetWorld() ? WorldDataLayers.GetWorld()->GetPathName() : FString());
	WorldDataLayersObject->SetStringField(TEXT("level_path"), WorldDataLayers.GetLevel() ? WorldDataLayers.GetLevel()->GetPathName() : FString());
	WorldDataLayersObject->SetNumberField(TEXT("data_layer_count"), DataLayerValues.Num());
	WorldDataLayersObject->SetArrayField(TEXT("data_layers"), DataLayerValues);

	if (!AssetEntity.Snapshot.IsValid())
	{
		AssetEntity.Snapshot = MakeShared<FJsonObject>();
	}
	AssetEntity.Snapshot->SetObjectField(TEXT("world_data_layers"), WorldDataLayersObject);
	AssetEntity.Attributes.Add(TEXT("world_data_layers_count"), FString::FromInt(1));
	AssetEntity.Attributes.Add(TEXT("data_layer_count"), FString::FromInt(DataLayerValues.Num()));
	AssetEntity.Completeness.State = ECompletenessState::Partial;
	AssetEntity.Completeness.Covered.AddUnique(TEXT("world_data_layers"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("data_layer_instances"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("actor_data_layer_assignments"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("world_partition_actor_descriptors"));
	AssetEntity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		WorldDataLayers.GetPathName(),
		TEXT("Data Layer metadata extracted from a loaded AWorldDataLayers actor.")
	});
	return true;
}

#if WITH_EDITOR
FString WorldPartitionActorDescCanonicalKey(const FString& WorldPath, const FWorldPartitionActorDesc& ActorDesc)
{
	return WorldPath + TEXT(":world_partition_actor_desc:") + WorldGuidString(ActorDesc.GetGuid());
}

FString WorldPartitionActorDescId(const FString& ProjectId, const FString& WorldPath, const FWorldPartitionActorDesc& ActorDesc)
{
	return MakeStableId(ProjectId, TEXT("world_partition_actor_desc"), WorldPartitionActorDescCanonicalKey(WorldPath, ActorDesc));
}

FString InferWorldPathFromActorPath(const FString& ActorPath)
{
	int32 ColonIndex = INDEX_NONE;
	if (ActorPath.FindChar(TEXT(':'), ColonIndex))
	{
		return ActorPath.Left(ColonIndex);
	}

	int32 DotIndex = INDEX_NONE;
	if (ActorPath.FindChar(TEXT('.'), DotIndex))
	{
		return ActorPath.Left(DotIndex);
	}

	return ActorPath;
}

FEntityRecord MakeWorldPartitionEntity(
	const FString& ProjectId,
	const UWorld& World,
	const UWorldPartition& WorldPartition,
	int32 ActorDescContainerCount,
	int32 ActorDescCount)
{
	const FString CanonicalKey = World.GetPathName() + TEXT(":world_partition:") + WorldPartition.GetPathName();
	FEntityRecord Entity;
	Entity.Id = MakeStableId(ProjectId, TEXT("world_partition"), CanonicalKey);
	Entity.Kind = TEXT("world_partition");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = WorldPartition.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("world_path"), World.GetPathName());
	Entity.Attributes.Add(TEXT("world_partition_path"), WorldPartition.GetPathName());
	Entity.Attributes.Add(TEXT("world_partition_class"), WorldPartition.GetClass() ? WorldPartition.GetClass()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("actor_desc_container_count"), FString::FromInt(ActorDescContainerCount));
	Entity.Attributes.Add(TEXT("actor_desc_count"), FString::FromInt(ActorDescCount));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("actor_descriptor_directory"), TEXT("actor_descriptor_references") };
	Entity.Completeness.Omitted = { TEXT("runtime_cells"), TEXT("streaming_generation") };
	AddEvidence(Entity, WorldPartition.GetPathName(), TEXT("UWorldPartition actor descriptor containers read without loading actors."));
	return Entity;
}

FEntityRecord MakeWorldPartitionActorDescEntity(
	const FString& ProjectId,
	const FString& WorldPath,
	const FWorldPartitionActorDesc& ActorDesc)
{
	const FString CanonicalKey = WorldPartitionActorDescCanonicalKey(WorldPath, ActorDesc);
	FEntityRecord Entity;
	Entity.Id = MakeStableId(ProjectId, TEXT("world_partition_actor_desc"), CanonicalKey);
	Entity.Kind = TEXT("world_partition_actor_desc");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = ActorDesc.GetActorLabelOrName().ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("world_path"), WorldPath);
	Entity.Attributes.Add(TEXT("actor_guid"), WorldGuidString(ActorDesc.GetGuid()));
	Entity.Attributes.Add(TEXT("actor_name"), ActorDesc.GetActorName().ToString());
	Entity.Attributes.Add(TEXT("actor_label"), ActorDesc.GetActorLabel().ToString());
	Entity.Attributes.Add(TEXT("actor_path"), ActorDesc.GetActorSoftPath().ToString());
	Entity.Attributes.Add(TEXT("actor_package"), ActorDesc.GetActorPackage().ToString());
	Entity.Attributes.Add(TEXT("native_class"), ActorDesc.GetNativeClass().ToString());
	Entity.Attributes.Add(TEXT("base_class"), ActorDesc.GetBaseClass().ToString());
	Entity.Attributes.Add(TEXT("display_class_name"), ActorDesc.GetDisplayClassName().ToString());
	Entity.Attributes.Add(TEXT("runtime_grid"), ActorDesc.GetRuntimeGrid().ToString());
	Entity.Attributes.Add(TEXT("is_spatially_loaded"), BoolString(ActorDesc.GetIsSpatiallyLoaded()));
	Entity.Attributes.Add(TEXT("actor_is_editor_only"), BoolString(ActorDesc.GetActorIsEditorOnly()));
	Entity.Attributes.Add(TEXT("actor_is_runtime_only"), BoolString(ActorDesc.GetActorIsRuntimeOnly()));
	Entity.Attributes.Add(TEXT("actor_is_hlod_relevant"), BoolString(ActorDesc.GetActorIsHLODRelevant()));
	Entity.Attributes.Add(TEXT("data_layer_count"), FString::FromInt(GetResolvedDataLayerInstanceNames(ActorDesc).Num()));
	Entity.Attributes.Add(TEXT("reference_count"), FString::FromInt(ActorDesc.GetReferences().Num()));
	Entity.Attributes.Add(TEXT("folder_guid"), WorldGuidString(ActorDesc.GetFolderGuid()));
	Entity.Attributes.Add(TEXT("parent_actor_guid"), WorldGuidString(ActorDesc.GetParentActor()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("actor_descriptor_metadata"), TEXT("actor_descriptor_references") };
	Entity.Completeness.Omitted = { TEXT("loaded_actor_properties"), TEXT("runtime_cell_membership") };
	AddEvidence(Entity, ActorDesc.GetActorSoftPath().ToString(), TEXT("FWorldPartitionActorDesc read from UWorldPartition without loading the actor."));
	return Entity;
}

TSharedRef<FJsonObject> MakeWorldPartitionActorDescObject(
	const FString& ProjectId,
	const FString& WorldPath,
	const FWorldPartitionActorDesc& ActorDesc)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), WorldPartitionActorDescId(ProjectId, WorldPath, ActorDesc));
	Object->SetStringField(TEXT("guid"), WorldGuidString(ActorDesc.GetGuid()));
	Object->SetStringField(TEXT("actor_name"), ActorDesc.GetActorName().ToString());
	Object->SetStringField(TEXT("actor_label"), ActorDesc.GetActorLabel().ToString());
	Object->SetStringField(TEXT("actor_label_or_name"), ActorDesc.GetActorLabelOrName().ToString());
	Object->SetStringField(TEXT("actor_path"), ActorDesc.GetActorSoftPath().ToString());
	Object->SetStringField(TEXT("actor_package"), ActorDesc.GetActorPackage().ToString());
	Object->SetStringField(TEXT("native_class"), ActorDesc.GetNativeClass().ToString());
	Object->SetStringField(TEXT("base_class"), ActorDesc.GetBaseClass().ToString());
	Object->SetStringField(TEXT("display_class_name"), ActorDesc.GetDisplayClassName().ToString());
	Object->SetStringField(TEXT("runtime_grid"), ActorDesc.GetRuntimeGrid().ToString());
	Object->SetBoolField(TEXT("is_spatially_loaded"), ActorDesc.GetIsSpatiallyLoaded());
	Object->SetBoolField(TEXT("is_spatially_loaded_raw"), ActorDesc.GetIsSpatiallyLoadedRaw());
	Object->SetBoolField(TEXT("actor_is_editor_only"), ActorDesc.GetActorIsEditorOnly());
	Object->SetBoolField(TEXT("actor_is_runtime_only"), ActorDesc.GetActorIsRuntimeOnly());
	Object->SetBoolField(TEXT("actor_is_hlod_relevant"), ActorDesc.GetActorIsHLODRelevant());
	Object->SetBoolField(TEXT("is_default_actor_desc"), ActorDesc.IsDefaultActorDesc());
	Object->SetBoolField(TEXT("is_container_instance"), ActorDesc.IsContainerInstance());
	Object->SetStringField(TEXT("hlod_layer"), ActorDesc.GetHLODLayer().ToString());
	Object->SetStringField(TEXT("folder_path"), ActorDesc.GetFolderPath().ToString());
	Object->SetStringField(TEXT("folder_guid"), WorldGuidString(ActorDesc.GetFolderGuid()));
	Object->SetStringField(TEXT("parent_actor_guid"), WorldGuidString(ActorDesc.GetParentActor()));
	Object->SetStringField(TEXT("content_bundle_guid"), WorldGuidString(ActorDesc.GetContentBundleGuid()));
	Object->SetObjectField(TEXT("editor_bounds"), MakeWorldBoxObject(ActorDesc.GetEditorBounds()));
	Object->SetObjectField(TEXT("runtime_bounds"), MakeWorldBoxObject(ActorDesc.GetRuntimeBounds()));
	Object->SetArrayField(TEXT("data_layers"), NameArrayToJson(ActorDesc.GetDataLayers()));
	Object->SetArrayField(TEXT("data_layer_instance_names"), NameArrayToJson(GetResolvedDataLayerInstanceNames(ActorDesc)));
	Object->SetArrayField(TEXT("tags"), NameArrayToJson(ActorDesc.GetTags()));
	Object->SetArrayField(TEXT("references"), GuidArrayToJson(ActorDesc.GetReferences()));
	Object->SetNumberField(TEXT("reference_count"), ActorDesc.GetReferences().Num());
	return Object;
}

bool AppendWorldPartition(
	UWorld& World,
	const TMap<FString, FString>& ActorPathToEntityId,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	UWorldPartition* WorldPartition = World.GetWorldPartition();
	if (!WorldPartition)
	{
		return false;
	}

	TArray<const FWorldPartitionActorDesc*> ActorDescs;
	WorldPartition->ForEachActorDescContainer([&ActorDescs](UActorDescContainer* ActorDescContainer)
	{
		if (!ActorDescContainer)
		{
			return;
		}

		for (UActorDescContainer::TConstIterator<> It(ActorDescContainer); It; ++It)
		{
			if (const FWorldPartitionActorDesc* ActorDesc = *It)
			{
				ActorDescs.Add(ActorDesc);
			}
		}
	});
	ActorDescs.Sort([](const FWorldPartitionActorDesc& Left, const FWorldPartitionActorDesc& Right)
	{
		return WorldGuidString(Left.GetGuid()) < WorldGuidString(Right.GetGuid());
	});

	FEntityRecord WorldPartitionEntity = MakeWorldPartitionEntity(
		ProjectId,
		World,
		*WorldPartition,
		static_cast<int32>(WorldPartition->GetActorDescContainerCount()),
		ActorDescs.Num());
	const FString WorldPartitionId = WorldPartitionEntity.Id;
	if (!HasWorldEntity(OutEntities, WorldPartitionId))
	{
		OutEntities.Add(MoveTemp(WorldPartitionEntity));
	}

	AddWorldRelation(ProjectId, TEXT("contains_world_partition"), AssetEntity.Id, WorldPartitionId, WorldPartition->GetPathName(), TEXT("World asset owns a UWorldPartition."), OutRelations);
	const FString WorldPartitionClassId = AddClassEntity(ProjectId, WorldPartition->GetClass(), WorldPartition->GetPathName(), OutEntities);
	AddWorldRelation(ProjectId, TEXT("instance_of"), WorldPartitionId, WorldPartitionClassId, WorldPartition->GetPathName(), TEXT("WorldPartition is an instance of this UClass."), OutRelations);

	TMap<FGuid, FString> ActorDescIdsByGuid;
	TArray<TSharedPtr<FJsonValue>> ActorDescValues;
	for (const FWorldPartitionActorDesc* ActorDesc : ActorDescs)
	{
		if (!ActorDesc)
		{
			continue;
		}

		FEntityRecord ActorDescEntity = MakeWorldPartitionActorDescEntity(ProjectId, World.GetPathName(), *ActorDesc);
		const FString ActorDescId = ActorDescEntity.Id;
		ActorDescIdsByGuid.Add(ActorDesc->GetGuid(), ActorDescId);
		if (!HasWorldEntity(OutEntities, ActorDescId))
		{
			OutEntities.Add(MoveTemp(ActorDescEntity));
		}

		AddWorldRelation(ProjectId, TEXT("contains_actor_desc"), WorldPartitionId, ActorDescId, ActorDesc->GetActorSoftPath().ToString(), TEXT("WorldPartition contains this actor descriptor."), OutRelations);
		if (const UClass* ActorClass = ActorDesc->GetActorNativeClass())
		{
			const FString ActorClassId = AddClassEntity(ProjectId, ActorClass, ActorDesc->GetActorSoftPath().ToString(), OutEntities);
			AddWorldRelation(ProjectId, TEXT("describes_actor_class"), ActorDescId, ActorClassId, ActorDesc->GetActorSoftPath().ToString(), TEXT("Actor descriptor records this native actor class."), OutRelations);
		}

		if (const FString* ActorEntityId = ActorPathToEntityId.Find(ActorDesc->GetActorSoftPath().ToString()))
		{
			AddWorldRelation(ProjectId, TEXT("describes_actor"), ActorDescId, *ActorEntityId, ActorDesc->GetActorSoftPath().ToString(), TEXT("Actor descriptor corresponds to this loaded actor entity."), OutRelations);
		}

		ActorDescValues.Add(MakeShared<FJsonValueObject>(MakeWorldPartitionActorDescObject(ProjectId, World.GetPathName(), *ActorDesc)));
	}

	for (const FWorldPartitionActorDesc* ActorDesc : ActorDescs)
	{
		if (!ActorDesc)
		{
			continue;
		}

		const FString* FromId = ActorDescIdsByGuid.Find(ActorDesc->GetGuid());
		if (!FromId)
		{
			continue;
		}

		TArray<FGuid> References = ActorDesc->GetReferences();
		References.Sort([](const FGuid& Left, const FGuid& Right)
		{
			return WorldGuidString(Left) < WorldGuidString(Right);
		});
		for (const FGuid& ReferenceGuid : References)
		{
			if (const FString* ToId = ActorDescIdsByGuid.Find(ReferenceGuid))
			{
				AddWorldRelation(ProjectId, TEXT("actor_desc_references"), *FromId, *ToId, ActorDesc->GetActorSoftPath().ToString(), TEXT("World Partition actor descriptor references another actor descriptor."), OutRelations);
			}
		}
	}

	TSharedRef<FJsonObject> WorldPartitionObject = MakeShared<FJsonObject>();
	WorldPartitionObject->SetStringField(TEXT("schema_version"), TEXT("uepi.world_partition.v1"));
	WorldPartitionObject->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	WorldPartitionObject->SetStringField(TEXT("world_path"), World.GetPathName());
	WorldPartitionObject->SetStringField(TEXT("world_partition_path"), WorldPartition->GetPathName());
	WorldPartitionObject->SetStringField(TEXT("world_partition_class"), WorldPartition->GetClass() ? WorldPartition->GetClass()->GetPathName() : FString());
	WorldPartitionObject->SetNumberField(TEXT("actor_desc_container_count"), WorldPartition->GetActorDescContainerCount());
	WorldPartitionObject->SetNumberField(TEXT("actor_desc_count"), ActorDescValues.Num());
	WorldPartitionObject->SetArrayField(TEXT("actor_descs"), ActorDescValues);

	if (!AssetEntity.Snapshot.IsValid())
	{
		AssetEntity.Snapshot = MakeShared<FJsonObject>();
	}
	AssetEntity.Snapshot->SetObjectField(TEXT("world_partition"), WorldPartitionObject);
	AssetEntity.Attributes.Add(TEXT("world_partition_count"), FString::FromInt(1));
	AssetEntity.Attributes.Add(TEXT("world_partition_actor_desc_count"), FString::FromInt(ActorDescValues.Num()));
	AssetEntity.Completeness.State = ECompletenessState::Partial;
	AssetEntity.Completeness.Covered.AddUnique(TEXT("world_partition"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("world_partition_actor_descriptors"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("world_partition_runtime_cells"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("streaming_generation"));
	AssetEntity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		WorldPartition->GetPathName(),
		TEXT("World Partition actor descriptors extracted without loading descriptor actors.")
	});
	return true;
}
#endif

FEntityRecord MakeLevelEntity(const FString& ProjectId, const UWorld& World, const ULevel& Level, int32 LevelIndex)
{
	const FString CanonicalKey = World.GetPathName() + TEXT(":level:") + Level.GetPathName();
	FEntityRecord Entity;
	Entity.Id = MakeStableId(ProjectId, TEXT("level"), CanonicalKey);
	Entity.Kind = TEXT("level");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Level.GetOuter() ? Level.GetOuter()->GetName() : Level.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("world_path"), World.GetPathName());
	Entity.Attributes.Add(TEXT("level_path"), Level.GetPathName());
	Entity.Attributes.Add(TEXT("level_index"), FString::FromInt(LevelIndex));
	Entity.Attributes.Add(TEXT("actor_count"), FString::FromInt(Level.Actors.Num()));
	if (ALevelScriptActor* LevelScriptActor = Level.GetLevelScriptActor())
	{
		Entity.Attributes.Add(TEXT("level_script_actor_path"), LevelScriptActor->GetPathName());
		Entity.Attributes.Add(TEXT("level_script_actor_class"), LevelScriptActor->GetClass() ? LevelScriptActor->GetClass()->GetPathName() : FString());
	}
	else
	{
		Entity.Attributes.Add(TEXT("level_script_actor_path"), FString());
		Entity.Attributes.Add(TEXT("level_script_actor_class"), FString());
	}
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("level_metadata"), TEXT("actor_membership"), TEXT("level_script_actor") };
	Entity.Completeness.Omitted = { TEXT("world_partition_cells") };
	AddEvidence(Entity, Level.GetPathName(), TEXT("ULevel read from a loaded UWorld asset."));
	return Entity;
}

FEntityRecord MakeStreamingLevelEntity(
	const FString& ProjectId,
	const UWorld& World,
	const ULevelStreaming& StreamingLevel,
	int32 StreamingLevelIndex)
{
	const FString CanonicalKey = World.GetPathName() + TEXT(":streaming_level:") + StreamingLevel.GetPathName();
	FEntityRecord Entity;
	Entity.Id = MakeStableId(ProjectId, TEXT("streaming_level"), CanonicalKey);
	Entity.Kind = TEXT("streaming_level");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = StreamingLevel.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("world_path"), World.GetPathName());
	Entity.Attributes.Add(TEXT("streaming_level_path"), StreamingLevel.GetPathName());
	Entity.Attributes.Add(TEXT("streaming_level_class"), StreamingLevel.GetClass() ? StreamingLevel.GetClass()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("streaming_level_index"), FString::FromInt(StreamingLevelIndex));
	Entity.Attributes.Add(TEXT("world_asset"), StreamingLevel.GetWorldAsset().ToSoftObjectPath().ToString());
	Entity.Attributes.Add(TEXT("world_asset_package"), StreamingLevel.GetWorldAssetPackageName());
	Entity.Attributes.Add(TEXT("loaded_level_path"), StreamingLevel.GetLoadedLevel() ? StreamingLevel.GetLoadedLevel()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("should_be_loaded"), BoolString(StreamingLevel.ShouldBeLoaded()));
	Entity.Attributes.Add(TEXT("should_be_visible"), BoolString(StreamingLevel.ShouldBeVisible()));
	Entity.Attributes.Add(TEXT("should_block_on_load"), BoolString(StreamingLevel.bShouldBlockOnLoad));
	Entity.Attributes.Add(TEXT("should_block_on_unload"), BoolString(StreamingLevel.ShouldBlockOnUnload()));
	Entity.Attributes.Add(TEXT("should_be_always_loaded"), BoolString(StreamingLevel.ShouldBeAlwaysLoaded()));
	Entity.Attributes.Add(TEXT("is_level_loaded"), BoolString(StreamingLevel.IsLevelLoaded()));
	Entity.Attributes.Add(TEXT("is_level_visible"), BoolString(StreamingLevel.IsLevelVisible()));
	Entity.Attributes.Add(TEXT("is_static"), BoolString(StreamingLevel.bIsStatic));
	Entity.Attributes.Add(TEXT("locked"), BoolString(StreamingLevel.bLocked));
	Entity.Attributes.Add(TEXT("level_lod_index"), FString::FromInt(StreamingLevel.GetLevelLODIndex()));
	Entity.Attributes.Add(TEXT("state"), LevelStreamingStateString(StreamingLevel.GetLevelStreamingState()));
	Entity.Attributes.Add(TEXT("transform"), FormatTransform(StreamingLevel.LevelTransform));
#if WITH_EDITOR
	Entity.Attributes.Add(TEXT("folder_path"), StreamingLevel.GetFolderPath().ToString());
	Entity.Attributes.Add(TEXT("should_be_visible_in_editor"), BoolString(StreamingLevel.GetShouldBeVisibleInEditor()));
#else
	Entity.Attributes.Add(TEXT("folder_path"), FString());
	Entity.Attributes.Add(TEXT("should_be_visible_in_editor"), FString());
#endif
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("streaming_level_metadata"), TEXT("streaming_target_world") };
	Entity.Completeness.Omitted = { TEXT("streaming_runtime_decision_trace"), TEXT("loaded_level_actor_expansion") };
	AddEvidence(Entity, StreamingLevel.GetPathName(), TEXT("ULevelStreaming metadata read from a loaded UWorld asset."));
	return Entity;
}

FEntityRecord MakeActorEntity(const FString& ProjectId, const ULevel& Level, AActor& Actor, const TMap<FString, FString>& ActorPathToEntityId)
{
	const FString CanonicalKey = ActorCanonicalKey(Level, Actor);
	AActor* Owner = Actor.GetOwner();
	AActor* AttachParent = Actor.GetAttachParentActor();
	const FString OwnerId = FindActorEntityId(Owner, ActorPathToEntityId);
	const FString AttachParentId = FindActorEntityId(AttachParent, ActorPathToEntityId);
	ILevelInstanceInterface* LevelInstance = GetLevelInstanceInterface(Actor);

	FEntityRecord Entity;
	Entity.Id = MakeStableId(ProjectId, TEXT("actor"), CanonicalKey);
	Entity.Kind = TEXT("actor");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Actor.GetActorLabel();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("actor_path"), Actor.GetPathName());
	Entity.Attributes.Add(TEXT("actor_name"), Actor.GetName());
	Entity.Attributes.Add(TEXT("actor_label"), Actor.GetActorLabel());
	Entity.Attributes.Add(TEXT("actor_class"), Actor.GetClass() ? Actor.GetClass()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("actor_guid"), ActorGuidString(Actor));
	Entity.Attributes.Add(TEXT("actor_instance_guid"), ActorInstanceGuidString(Actor));
	Entity.Attributes.Add(TEXT("content_bundle_guid"), ActorContentBundleGuidString(Actor));
	Entity.Attributes.Add(TEXT("level_path"), Level.GetPathName());
	Entity.Attributes.Add(TEXT("location"), FormatVector(Actor.GetActorLocation()));
	Entity.Attributes.Add(TEXT("rotation"), FormatRotator(Actor.GetActorRotation()));
	Entity.Attributes.Add(TEXT("scale"), FormatVector(Actor.GetActorScale3D()));
	Entity.Attributes.Add(TEXT("hidden"), Actor.IsHiddenEd() ? TEXT("true") : TEXT("false"));
	Entity.Attributes.Add(TEXT("folder_path"), ActorFolderPathString(Actor));
	Entity.Attributes.Add(TEXT("owner_id"), OwnerId);
	Entity.Attributes.Add(TEXT("owner_path"), Owner ? Owner->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("attach_parent_id"), AttachParentId);
	Entity.Attributes.Add(TEXT("attach_parent_path"), AttachParent ? AttachParent->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("attach_parent_socket"), Actor.GetAttachParentSocketName().ToString());
	Entity.Attributes.Add(TEXT("tag_count"), FString::FromInt(Actor.Tags.Num()));
	Entity.Attributes.Add(TEXT("is_level_script_actor"), BoolString(Actor.IsA<ALevelScriptActor>()));
	Entity.Attributes.Add(TEXT("is_level_instance"), BoolString(LevelInstance != nullptr));
	Entity.Attributes.Add(TEXT("level_instance_guid"), LevelInstance ? WorldGuidString(LevelInstance->GetLevelInstanceGuid()) : FString());
	Entity.Attributes.Add(TEXT("level_instance_world_asset"), LevelInstance ? LevelInstance->GetWorldAsset().ToSoftObjectPath().ToString() : FString());
	Entity.Attributes.Add(TEXT("level_instance_world_package"), LevelInstance ? LevelInstance->GetWorldAssetPackage() : FString());
#if WITH_EDITOR
	Entity.Attributes.Add(TEXT("level_instance_runtime_behavior"), LevelInstance ? LevelInstanceRuntimeBehaviorString(LevelInstance->GetDesiredRuntimeBehavior()) : FString());
#else
	Entity.Attributes.Add(TEXT("level_instance_runtime_behavior"), FString());
#endif
	TArray<UActorComponent*> Components;
	Actor.GetComponents(Components);
	Entity.Attributes.Add(TEXT("component_count"), FString::FromInt(Components.Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("actor_metadata"), TEXT("component_membership"), TEXT("static_transform"), TEXT("actor_folder"), TEXT("actor_attachment"), TEXT("actor_owner"), TEXT("actor_guid") };
	if (LevelInstance)
	{
		Entity.Completeness.Covered.AddUnique(TEXT("level_instance_reference"));
	}
	Entity.Completeness.Omitted = { TEXT("runtime_state") };
	AddEvidence(Entity, Actor.GetPathName(), TEXT("AActor read from a loaded UWorld asset."));
	return Entity;
}

FEntityRecord MakeComponentEntity(const FString& ProjectId, const AActor& Actor, const UActorComponent& Component)
{
	const FString CanonicalKey = Actor.GetPathName() + TEXT(":component:") + Component.GetPathName();
	FEntityRecord Entity;
	Entity.Id = MakeStableId(ProjectId, TEXT("component"), CanonicalKey);
	Entity.Kind = TEXT("component");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Component.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("actor_path"), Actor.GetPathName());
	Entity.Attributes.Add(TEXT("component_path"), Component.GetPathName());
	Entity.Attributes.Add(TEXT("component_name"), Component.GetName());
	Entity.Attributes.Add(TEXT("component_class"), Component.GetClass() ? Component.GetClass()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("registered"), Component.IsRegistered() ? TEXT("true") : TEXT("false"));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("component_metadata") };
	AddEvidence(Entity, Component.GetPathName(), TEXT("UActorComponent read from an Actor in a loaded UWorld asset."));
	return Entity;
}
}

bool FWorldReader::AppendWorldPartitionActorDescAssetData(
	const FAssetData& AssetData,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
#if WITH_EDITOR
	if (!FWorldPartitionActorDescUtils::IsValidActorDescriptorFromAssetData(AssetData))
	{
		return false;
	}

	TUniquePtr<FWorldPartitionActorDesc> ActorDesc = FWorldPartitionActorDescUtils::GetActorDescriptorFromAssetData(AssetData);
	if (!ActorDesc.IsValid())
	{
		return false;
	}

	const FString ActorPath = ActorDesc->GetActorSoftPath().ToString();
	const FString WorldPath = InferWorldPathFromActorPath(ActorPath);
	FEntityRecord ActorDescEntity = MakeWorldPartitionActorDescEntity(ProjectId, WorldPath, *ActorDesc);
	const FString ActorDescId = ActorDescEntity.Id;
	if (!HasWorldEntity(OutEntities, ActorDescId))
	{
		OutEntities.Add(MoveTemp(ActorDescEntity));
	}

	AddWorldRelation(ProjectId, TEXT("asset_describes_actor_desc"), AssetEntity.Id, ActorDescId, AssetData.PackageName.ToString(), TEXT("Asset Registry metadata contains this World Partition actor descriptor."), OutRelations);
	if (const UClass* ActorClass = ActorDesc->GetActorNativeClass())
	{
		const FString ActorClassId = AddClassEntity(ProjectId, ActorClass, ActorPath, OutEntities);
		AddWorldRelation(ProjectId, TEXT("describes_actor_class"), ActorDescId, ActorClassId, ActorPath, TEXT("Actor descriptor records this native actor class."), OutRelations);
	}

	if (!AssetEntity.Snapshot.IsValid())
	{
		AssetEntity.Snapshot = MakeShared<FJsonObject>();
	}
	AssetEntity.Snapshot->SetObjectField(TEXT("world_partition_actor_desc"), MakeWorldPartitionActorDescObject(ProjectId, WorldPath, *ActorDesc));
	AssetEntity.Attributes.Add(TEXT("world_partition_actor_desc_guid"), WorldGuidString(ActorDesc->GetGuid()));
	AssetEntity.Attributes.Add(TEXT("world_partition_actor_desc_world_path"), WorldPath);
	AssetEntity.Attributes.Add(TEXT("world_partition_actor_desc_actor_path"), ActorPath);
	AssetEntity.Completeness.State = ECompletenessState::Partial;
	AssetEntity.Completeness.Covered.AddUnique(TEXT("world_partition_actor_descriptor"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("loaded_actor_properties"));
	AssetEntity.Evidence.Add({
		LexToString(ESourceLayer::AssetRegistry),
		AssetData.PackageName.ToString(),
		TEXT("World Partition actor descriptor extracted from Asset Registry tags.")
	});
	return true;
#else
	return false;
#endif
}

bool FWorldReader::AppendWorldAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (UWorld* World = Cast<UWorld>(&Asset))
	{
		AppendWorld(*World, ProjectId, AssetEntity, OutEntities, OutRelations);
		return true;
	}

	if (AWorldDataLayers* WorldDataLayers = Cast<AWorldDataLayers>(&Asset))
	{
		return AppendWorldDataLayers(*WorldDataLayers, ProjectId, AssetEntity, OutEntities, OutRelations);
	}

	return false;
}

void FWorldReader::AppendWorld(
	UWorld& World,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TSharedRef<FJsonObject> WorldObject = MakeShared<FJsonObject>();
	WorldObject->SetStringField(TEXT("schema_version"), TEXT("uepi.world.v1"));
	WorldObject->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	WorldObject->SetStringField(TEXT("world_path"), World.GetPathName());
	WorldObject->SetStringField(TEXT("world_type"), WorldTypeString(World.WorldType));

	TArray<TSharedPtr<FJsonValue>> LevelValues;
	TMap<FString, FString> ActorPathToEntityId;
	int32 ActorCount = 0;
	int32 ComponentCount = 0;

	TArray<ULevel*> Levels = World.GetLevels();
	if (Levels.Num() == 0 && World.PersistentLevel)
	{
		Levels.Add(World.PersistentLevel);
	}
	Levels.RemoveAll([](const ULevel* Level)
	{
		return Level == nullptr;
	});
	Levels.Sort([](const ULevel& Left, const ULevel& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});

	for (ULevel* Level : Levels)
	{
		for (AActor* Actor : Level->Actors)
		{
			if (Actor)
			{
				ActorPathToEntityId.Add(Actor->GetPathName(), ActorEntityId(ProjectId, *Level, *Actor));
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> StreamingLevelValues;
	TArray<ULevelStreaming*> StreamingLevels = World.GetStreamingLevels();
	StreamingLevels.RemoveAll([](const ULevelStreaming* StreamingLevel)
	{
		return StreamingLevel == nullptr;
	});
	StreamingLevels.Sort([](const ULevelStreaming& Left, const ULevelStreaming& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});

	for (int32 StreamingLevelIndex = 0; StreamingLevelIndex < StreamingLevels.Num(); ++StreamingLevelIndex)
	{
		ULevelStreaming* StreamingLevel = StreamingLevels[StreamingLevelIndex];
		FEntityRecord StreamingLevelEntity = MakeStreamingLevelEntity(ProjectId, World, *StreamingLevel, StreamingLevelIndex);
		const FString StreamingLevelId = StreamingLevelEntity.Id;
		if (!HasWorldEntity(OutEntities, StreamingLevelId))
		{
			OutEntities.Add(MoveTemp(StreamingLevelEntity));
		}
		AddWorldRelation(ProjectId, TEXT("contains_streaming_level"), AssetEntity.Id, StreamingLevelId, StreamingLevel->GetPathName(), TEXT("World asset owns a streaming level descriptor."), OutRelations);

		if (ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel())
		{
			const FString LoadedLevelId = MakeStableId(ProjectId, TEXT("level"), World.GetPathName() + TEXT(":level:") + LoadedLevel->GetPathName());
			AddWorldRelation(ProjectId, TEXT("streaming_level_loads_level"), StreamingLevelId, LoadedLevelId, StreamingLevel->GetPathName(), TEXT("Streaming level currently references this loaded level."), OutRelations);
		}

		TSharedRef<FJsonObject> StreamingLevelObject = MakeShared<FJsonObject>();
		StreamingLevelObject->SetStringField(TEXT("id"), StreamingLevelId);
		StreamingLevelObject->SetStringField(TEXT("path"), StreamingLevel->GetPathName());
		StreamingLevelObject->SetStringField(TEXT("name"), StreamingLevel->GetName());
		StreamingLevelObject->SetStringField(TEXT("class"), StreamingLevel->GetClass() ? StreamingLevel->GetClass()->GetPathName() : FString());
		StreamingLevelObject->SetNumberField(TEXT("index"), StreamingLevelIndex);
		StreamingLevelObject->SetStringField(TEXT("world_asset"), StreamingLevel->GetWorldAsset().ToSoftObjectPath().ToString());
		StreamingLevelObject->SetStringField(TEXT("world_asset_package"), StreamingLevel->GetWorldAssetPackageName());
		StreamingLevelObject->SetStringField(TEXT("loaded_level_path"), StreamingLevel->GetLoadedLevel() ? StreamingLevel->GetLoadedLevel()->GetPathName() : FString());
		StreamingLevelObject->SetBoolField(TEXT("should_be_loaded"), StreamingLevel->ShouldBeLoaded());
		StreamingLevelObject->SetBoolField(TEXT("should_be_visible"), StreamingLevel->ShouldBeVisible());
		StreamingLevelObject->SetBoolField(TEXT("should_be_visible_flag"), StreamingLevel->GetShouldBeVisibleFlag());
		StreamingLevelObject->SetBoolField(TEXT("should_block_on_load"), StreamingLevel->bShouldBlockOnLoad);
		StreamingLevelObject->SetBoolField(TEXT("should_block_on_unload"), StreamingLevel->ShouldBlockOnUnload());
		StreamingLevelObject->SetBoolField(TEXT("should_be_always_loaded"), StreamingLevel->ShouldBeAlwaysLoaded());
		StreamingLevelObject->SetBoolField(TEXT("is_level_loaded"), StreamingLevel->IsLevelLoaded());
		StreamingLevelObject->SetBoolField(TEXT("is_level_visible"), StreamingLevel->IsLevelVisible());
		StreamingLevelObject->SetBoolField(TEXT("is_static"), StreamingLevel->bIsStatic);
		StreamingLevelObject->SetBoolField(TEXT("locked"), StreamingLevel->bLocked);
		StreamingLevelObject->SetNumberField(TEXT("level_lod_index"), StreamingLevel->GetLevelLODIndex());
		StreamingLevelObject->SetStringField(TEXT("state"), LevelStreamingStateString(StreamingLevel->GetLevelStreamingState()));
		StreamingLevelObject->SetObjectField(TEXT("transform"), MakeWorldTransformObject(StreamingLevel->LevelTransform));
#if WITH_EDITOR
		StreamingLevelObject->SetStringField(TEXT("folder_path"), StreamingLevel->GetFolderPath().ToString());
		StreamingLevelObject->SetBoolField(TEXT("should_be_visible_in_editor"), StreamingLevel->GetShouldBeVisibleInEditor());
#else
		StreamingLevelObject->SetStringField(TEXT("folder_path"), FString());
		StreamingLevelObject->SetBoolField(TEXT("should_be_visible_in_editor"), false);
#endif
		StreamingLevelValues.Add(MakeShared<FJsonValueObject>(StreamingLevelObject));
	}

	for (int32 LevelIndex = 0; LevelIndex < Levels.Num(); ++LevelIndex)
	{
		ULevel* Level = Levels[LevelIndex];
		FEntityRecord LevelEntity = MakeLevelEntity(ProjectId, World, *Level, LevelIndex);
		const FString LevelId = LevelEntity.Id;
		OutEntities.Add(MoveTemp(LevelEntity));
		AddWorldRelation(ProjectId, TEXT("contains_level"), AssetEntity.Id, LevelId, Level->GetPathName(), TEXT("World asset contains a level."), OutRelations);

		TSharedRef<FJsonObject> LevelObject = MakeShared<FJsonObject>();
		LevelObject->SetStringField(TEXT("id"), LevelId);
		LevelObject->SetStringField(TEXT("path"), Level->GetPathName());
		LevelObject->SetNumberField(TEXT("index"), LevelIndex);
		ALevelScriptActor* LevelScriptActor = Level->GetLevelScriptActor();
		const FString LevelScriptActorId = FindActorEntityId(LevelScriptActor, ActorPathToEntityId);
		LevelObject->SetStringField(TEXT("level_script_actor_id"), LevelScriptActorId);
		LevelObject->SetStringField(TEXT("level_script_actor_path"), LevelScriptActor ? LevelScriptActor->GetPathName() : FString());
		LevelObject->SetStringField(TEXT("level_script_actor_class"), LevelScriptActor && LevelScriptActor->GetClass() ? LevelScriptActor->GetClass()->GetPathName() : FString());
		if (LevelScriptActor && !LevelScriptActorId.IsEmpty())
		{
			AddWorldRelation(ProjectId, TEXT("has_level_script_actor"), LevelId, LevelScriptActorId, LevelScriptActor->GetPathName(), TEXT("Level has this Level Script Actor instance."), OutRelations);
		}

		TArray<TSharedPtr<FJsonValue>> ActorValues;
		for (AActor* Actor : Level->Actors)
		{
			if (!Actor)
			{
				continue;
			}

			FEntityRecord ActorEntity = MakeActorEntity(ProjectId, *Level, *Actor, ActorPathToEntityId);
			const FString ActorId = ActorEntity.Id;
			OutEntities.Add(MoveTemp(ActorEntity));
			++ActorCount;

			AddWorldRelation(ProjectId, TEXT("contains_actor"), LevelId, ActorId, Actor->GetPathName(), TEXT("Level contains an Actor."), OutRelations);
			const FString ActorClassId = AddClassEntity(ProjectId, Actor->GetClass(), Actor->GetPathName(), OutEntities);
			AddWorldRelation(ProjectId, TEXT("instance_of"), ActorId, ActorClassId, Actor->GetPathName(), TEXT("Actor is an instance of this UClass."), OutRelations);
			if (AActor* Owner = Actor->GetOwner())
			{
				const FString OwnerId = FindActorEntityId(Owner, ActorPathToEntityId);
				if (!OwnerId.IsEmpty() && OwnerId != ActorId)
				{
					AddWorldRelation(ProjectId, TEXT("owned_by"), ActorId, OwnerId, Actor->GetPathName(), TEXT("Actor ownership points to another loaded actor."), OutRelations);
				}
			}
			if (AActor* AttachParent = Actor->GetAttachParentActor())
			{
				const FString AttachParentId = FindActorEntityId(AttachParent, ActorPathToEntityId);
				if (!AttachParentId.IsEmpty() && AttachParentId != ActorId)
				{
					AddWorldRelation(ProjectId, TEXT("attached_to"), ActorId, AttachParentId, Actor->GetPathName(), TEXT("Actor root component is attached to another loaded actor."), OutRelations);
				}
			}
			if (AWorldDataLayers* WorldDataLayers = Cast<AWorldDataLayers>(Actor))
			{
				AppendWorldDataLayers(*WorldDataLayers, ProjectId, AssetEntity, OutEntities, OutRelations);
			}

			AActor* Owner = Actor->GetOwner();
			AActor* AttachParent = Actor->GetAttachParentActor();
			ILevelInstanceInterface* LevelInstance = GetLevelInstanceInterface(*Actor);
			TSharedRef<FJsonObject> ActorObject = MakeShared<FJsonObject>();
			ActorObject->SetStringField(TEXT("id"), ActorId);
			ActorObject->SetStringField(TEXT("path"), Actor->GetPathName());
			ActorObject->SetStringField(TEXT("name"), Actor->GetName());
			ActorObject->SetStringField(TEXT("label"), Actor->GetActorLabel());
			ActorObject->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
			ActorObject->SetStringField(TEXT("actor_guid"), ActorGuidString(*Actor));
			ActorObject->SetStringField(TEXT("actor_instance_guid"), ActorInstanceGuidString(*Actor));
			ActorObject->SetStringField(TEXT("content_bundle_guid"), ActorContentBundleGuidString(*Actor));
			ActorObject->SetStringField(TEXT("folder_path"), ActorFolderPathString(*Actor));
			ActorObject->SetStringField(TEXT("owner_id"), FindActorEntityId(Owner, ActorPathToEntityId));
			ActorObject->SetStringField(TEXT("owner_path"), Owner ? Owner->GetPathName() : FString());
			ActorObject->SetStringField(TEXT("attach_parent_id"), FindActorEntityId(AttachParent, ActorPathToEntityId));
			ActorObject->SetStringField(TEXT("attach_parent_path"), AttachParent ? AttachParent->GetPathName() : FString());
			ActorObject->SetStringField(TEXT("attach_parent_socket"), Actor->GetAttachParentSocketName().ToString());
			ActorObject->SetArrayField(TEXT("tags"), ActorTagsToJson(*Actor));
			ActorObject->SetBoolField(TEXT("is_level_script_actor"), Actor->IsA<ALevelScriptActor>());
			ActorObject->SetBoolField(TEXT("is_level_instance"), LevelInstance != nullptr);
			ActorObject->SetStringField(TEXT("level_instance_guid"), LevelInstance ? WorldGuidString(LevelInstance->GetLevelInstanceGuid()) : FString());
			ActorObject->SetStringField(TEXT("level_instance_world_asset"), LevelInstance ? LevelInstance->GetWorldAsset().ToSoftObjectPath().ToString() : FString());
			ActorObject->SetStringField(TEXT("level_instance_world_package"), LevelInstance ? LevelInstance->GetWorldAssetPackage() : FString());
#if WITH_EDITOR
			ActorObject->SetStringField(TEXT("level_instance_runtime_behavior"), LevelInstance ? LevelInstanceRuntimeBehaviorString(LevelInstance->GetDesiredRuntimeBehavior()) : FString());
#else
			ActorObject->SetStringField(TEXT("level_instance_runtime_behavior"), FString());
#endif

			TArray<TSharedPtr<FJsonValue>> ComponentValues;
			TArray<UActorComponent*> SortedComponents;
			Actor->GetComponents(SortedComponents);
			SortedComponents.Sort([](const UActorComponent& Left, const UActorComponent& Right)
			{
				return Left.GetPathName() < Right.GetPathName();
			});

			for (UActorComponent* Component : SortedComponents)
			{
				if (!Component)
				{
					continue;
				}

				FEntityRecord ComponentEntity = MakeComponentEntity(ProjectId, *Actor, *Component);
				const FString ComponentId = ComponentEntity.Id;
				OutEntities.Add(MoveTemp(ComponentEntity));
				++ComponentCount;

				AddWorldRelation(ProjectId, TEXT("contains_component"), ActorId, ComponentId, Component->GetPathName(), TEXT("Actor owns an ActorComponent."), OutRelations);
				const FString ComponentClassId = AddClassEntity(ProjectId, Component->GetClass(), Component->GetPathName(), OutEntities);
				AddWorldRelation(ProjectId, TEXT("instance_of"), ComponentId, ComponentClassId, Component->GetPathName(), TEXT("Component is an instance of this UClass."), OutRelations);

				TSharedRef<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
				ComponentObject->SetStringField(TEXT("id"), ComponentId);
				ComponentObject->SetStringField(TEXT("path"), Component->GetPathName());
				ComponentObject->SetStringField(TEXT("name"), Component->GetName());
				ComponentObject->SetStringField(TEXT("class"), Component->GetClass() ? Component->GetClass()->GetPathName() : FString());
				ComponentValues.Add(MakeShared<FJsonValueObject>(ComponentObject));
			}

			ActorObject->SetNumberField(TEXT("component_count"), ComponentValues.Num());
			ActorObject->SetArrayField(TEXT("components"), ComponentValues);
			ActorValues.Add(MakeShared<FJsonValueObject>(ActorObject));
		}

		LevelObject->SetNumberField(TEXT("actor_count"), ActorValues.Num());
		LevelObject->SetArrayField(TEXT("actors"), ActorValues);
		LevelValues.Add(MakeShared<FJsonValueObject>(LevelObject));
	}

	WorldObject->SetNumberField(TEXT("level_count"), LevelValues.Num());
	WorldObject->SetNumberField(TEXT("streaming_level_count"), StreamingLevelValues.Num());
	WorldObject->SetNumberField(TEXT("actor_count"), ActorCount);
	WorldObject->SetNumberField(TEXT("component_count"), ComponentCount);
	WorldObject->SetArrayField(TEXT("levels"), LevelValues);
	WorldObject->SetArrayField(TEXT("streaming_levels"), StreamingLevelValues);

	if (!AssetEntity.Snapshot.IsValid())
	{
		AssetEntity.Snapshot = MakeShared<FJsonObject>();
	}
	AssetEntity.Snapshot->SetObjectField(TEXT("world"), WorldObject);
#if WITH_EDITOR
	AppendWorldPartition(World, ActorPathToEntityId, ProjectId, AssetEntity, OutEntities, OutRelations);
#endif
	AssetEntity.Attributes.Add(TEXT("world_level_count"), FString::FromInt(LevelValues.Num()));
	AssetEntity.Attributes.Add(TEXT("world_streaming_level_count"), FString::FromInt(StreamingLevelValues.Num()));
	AssetEntity.Attributes.Add(TEXT("world_actor_count"), FString::FromInt(ActorCount));
	AssetEntity.Attributes.Add(TEXT("world_component_count"), FString::FromInt(ComponentCount));
	AssetEntity.Completeness.State = ECompletenessState::Partial;
	AssetEntity.Completeness.Covered.AddUnique(TEXT("world_levels"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("world_streaming_levels"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("world_actors"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("world_components"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("world_partition_cells"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_actor_state"));
	AssetEntity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		World.GetPathName(),
		TEXT("World/Level/Actor/Component structure extracted from a loaded UWorld asset.")
	});
}
}
