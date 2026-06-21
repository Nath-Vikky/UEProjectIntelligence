#include "UEPICinematicsReader.h"

#include "Animation/AnimSequenceBase.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneChannelEditorData.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Evaluation/Blending/MovieSceneBlendType.h"
#include "Evaluation/MovieSceneSectionParameters.h"
#include "Engine/Blueprint.h"
#include "LevelSequence.h"
#include "Misc/FrameTime.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneFolder.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTimeHelpers.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Tracks/MovieSceneEventTrack.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString CineBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString CineGuidString(const FGuid& Guid)
{
	return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString();
}

FString CineClassPath(const UObject* Object)
{
	return Object && Object->GetClass() ? Object->GetClass()->GetPathName() : FString();
}

FString CineClassPath(const UClass* Class)
{
	return Class ? Class->GetPathName() : FString();
}

FString CineObjectPath(const UObject* Object)
{
	return Object ? Object->GetPathName() : FString();
}

FString CineEnumValueString(const UEnum* Enum, int64 Value)
{
	if (!Enum)
	{
		return FString::FromInt(static_cast<int32>(Value));
	}
	return Enum->GetNameStringByValue(Value);
}

template <typename EnumType>
FString CineEnumString(EnumType Value)
{
	return CineEnumValueString(StaticEnum<EnumType>(), static_cast<int64>(Value));
}

void AddCineEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

void AddCineRelation(
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

FEntityRecord* FindCineEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

FString AddAssetReferenceEntity(
	const FString& ProjectId,
	const FString& AssetPath,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FEntityRecord>& OutEntities)
{
	if (AssetPath.IsEmpty())
	{
		return FString();
	}

	const FString CanonicalKey = TEXT("asset_reference:") + AssetPath;
	const FString EntityId = MakeStableId(ProjectId, TEXT("asset_reference"), CanonicalKey);
	if (FindCineEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("asset_reference");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = AssetPath;
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("reference_path"), AssetPath);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Registry));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("referenced_asset_path") };
	AddCineEvidence(Entity, EvidencePath, EvidenceDetail);
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

TSharedRef<FJsonObject> FrameRateObject(const FFrameRate& Rate)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("numerator"), Rate.Numerator);
	Object->SetNumberField(TEXT("denominator"), Rate.Denominator);
	Object->SetStringField(TEXT("text"), Rate.ToPrettyText().ToString());
	return Object;
}

FString BoundKind(const TRangeBound<FFrameNumber>& Bound)
{
	if (Bound.IsOpen())
	{
		return TEXT("open");
	}
	return Bound.IsInclusive() ? TEXT("inclusive") : TEXT("exclusive");
}

int32 BoundValue(const TRangeBound<FFrameNumber>& Bound)
{
	return Bound.IsOpen() ? 0 : Bound.GetValue().Value;
}

TSharedRef<FJsonObject> FrameRangeObject(const TRange<FFrameNumber>& Range)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	const TRangeBound<FFrameNumber> Lower = Range.GetLowerBound();
	const TRangeBound<FFrameNumber> Upper = Range.GetUpperBound();
	Object->SetBoolField(TEXT("empty"), Range.IsEmpty());
	Object->SetStringField(TEXT("lower_bound_type"), BoundKind(Lower));
	Object->SetNumberField(TEXT("lower"), BoundValue(Lower));
	Object->SetStringField(TEXT("upper_bound_type"), BoundKind(Upper));
	Object->SetNumberField(TEXT("upper"), BoundValue(Upper));
	Object->SetStringField(TEXT("text"), ::LexToString(Range));
	return Object;
}

TSharedRef<FJsonObject> KeyArtifactManifest(
	const FString& ProjectId,
	const FString& ScopeKind,
	const FString& ScopeId,
	int32 ItemCount)
{
	const FString ArtifactId = MakeStableId(ProjectId, TEXT("cinematics_key_artifact"), ScopeKind + TEXT(":") + ScopeId);
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.cinematics_key_artifact_manifest.v1"));
	Object->SetStringField(TEXT("artifact_id"), ArtifactId);
	Object->SetStringField(TEXT("artifact_uri"), TEXT("uepi://cinematics-key-artifact/") + ArtifactId);
	Object->SetStringField(TEXT("storage"), TEXT("daemon_materialized_json"));
	Object->SetStringField(TEXT("scope_kind"), ScopeKind);
	Object->SetStringField(TEXT("scope_id"), ScopeId);
	Object->SetNumberField(TEXT("item_count"), ItemCount);
	Object->SetStringField(TEXT("encoding"), TEXT("json"));
	return Object;
}

TArray<FFrameNumber> ChannelKeyTimes(FMovieSceneChannel& Channel)
{
	TArray<FFrameNumber> KeyTimes;
	Channel.GetKeys(TRange<FFrameNumber>::All(), &KeyTimes, nullptr);
	KeyTimes.Sort([](const FFrameNumber& A, const FFrameNumber& B)
	{
		return A.Value < B.Value;
	});
	return KeyTimes;
}

double FrameNumberSeconds(const FFrameNumber& FrameNumber, const FFrameRate& TickResolution)
{
	if (TickResolution.Numerator <= 0 || TickResolution.Denominator <= 0)
	{
		return 0.0;
	}
	return TickResolution.AsSeconds(FFrameTime(FrameNumber));
}

FString BindingIdString(const FMovieSceneObjectBindingID& BindingId)
{
	if (!BindingId.IsValid())
	{
		return FString();
	}

	return FString::Printf(
		TEXT("%s@sequence:%d%s"),
		*CineGuidString(BindingId.GetGuid()),
		BindingId.GetRelativeSequenceID().GetInternalValue(),
		BindingId.IsFixedBinding() ? TEXT(":fixed") : TEXT(""));
}

TSharedRef<FJsonObject> BindingReferenceObject(const FMovieSceneObjectBindingID& BindingId)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("binding_id"), BindingIdString(BindingId));
	Object->SetStringField(TEXT("guid"), CineGuidString(BindingId.GetGuid()));
	Object->SetNumberField(TEXT("relative_sequence_id"), BindingId.GetRelativeSequenceID().GetInternalValue());
	Object->SetBoolField(TEXT("fixed_binding"), BindingId.IsFixedBinding());
	return Object;
}

FString TrackSemanticKind(const UMovieSceneTrack& Track)
{
	const FString ClassName = Track.GetClass() ? Track.GetClass()->GetName() : FString();
	if (ClassName.Contains(TEXT("CameraCut")))
	{
		return TEXT("camera_cut");
	}
	if (ClassName.Contains(TEXT("CinematicShot")) || ClassName.Contains(TEXT("SubTrack")) || ClassName.Contains(TEXT("TemplateSequence")))
	{
		return TEXT("subsequence");
	}
	if (ClassName.Contains(TEXT("Event")))
	{
		return TEXT("event");
	}
	if (ClassName.Contains(TEXT("Audio")))
	{
		return TEXT("audio");
	}
	if (ClassName.Contains(TEXT("SkeletalAnimation")) || ClassName.Contains(TEXT("Animation")))
	{
		return TEXT("animation");
	}
	if (ClassName.Contains(TEXT("ControlRig")))
	{
		return TEXT("control_rig");
	}
	if (ClassName.Contains(TEXT("Transform")))
	{
		return TEXT("transform");
	}
	if (ClassName.Contains(TEXT("Property")))
	{
		return TEXT("property");
	}
	return TEXT("generic");
}

FString SectionSemanticKind(const UMovieSceneSection& Section)
{
	if (Section.IsA<UMovieSceneCameraCutSection>())
	{
		return TEXT("camera_cut");
	}
	if (Section.IsA<UMovieSceneSubSection>())
	{
		return TEXT("subsequence");
	}
	if (Section.IsA<UMovieSceneAudioSection>())
	{
		return TEXT("audio");
	}
	if (Section.IsA<UMovieSceneSkeletalAnimationSection>())
	{
		return TEXT("animation");
	}

	const FString ClassName = Section.GetClass() ? Section.GetClass()->GetName() : FString();
	if (ClassName.Contains(TEXT("Event")))
	{
		return TEXT("event");
	}
	if (ClassName.Contains(TEXT("ControlRig")))
	{
		return TEXT("control_rig");
	}
	if (ClassName.Contains(TEXT("Transform")))
	{
		return TEXT("transform");
	}
	return TEXT("generic");
}

FString AddLevelSequenceEntity(
	const FString& ProjectId,
	const ULevelSequence& Sequence,
	const UMovieScene* MovieScene,
	TArray<FEntityRecord>& OutEntities)
{
	const FString SequencePath = Sequence.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("level_sequence"), SequencePath);
	if (FindCineEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("level_sequence");
	Entity.CanonicalKey = SequencePath;
	Entity.DisplayName = Sequence.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("sequence_path"), SequencePath);
	Entity.Attributes.Add(TEXT("sequence_class"), CineClassPath(&Sequence));
	Entity.Attributes.Add(TEXT("movie_scene_path"), MovieScene ? MovieScene->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = MovieScene ? ECompletenessState::Partial : ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("level_sequence_metadata"), TEXT("movie_scene_reference") };
	Entity.Completeness.Omitted = { TEXT("runtime_bound_world_actors"), TEXT("evaluated_camera_state"), TEXT("typed_channel_key_values") };
	AddCineEvidence(Entity, SequencePath, TEXT("LevelSequence metadata read from ULevelSequence."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddMovieSceneEntity(
	const FString& ProjectId,
	const UMovieScene& MovieScene,
	TArray<FEntityRecord>& OutEntities)
{
	const FString MovieScenePath = MovieScene.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("movie_scene"), MovieScenePath);
	if (FindCineEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("movie_scene");
	Entity.CanonicalKey = MovieScenePath;
	Entity.DisplayName = MovieScene.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("movie_scene_path"), MovieScenePath);
	Entity.Attributes.Add(TEXT("movie_scene_class"), CineClassPath(&MovieScene));
	Entity.Attributes.Add(TEXT("display_rate"), MovieScene.GetDisplayRate().ToPrettyText().ToString());
	Entity.Attributes.Add(TEXT("tick_resolution"), MovieScene.GetTickResolution().ToPrettyText().ToString());
	Entity.Attributes.Add(TEXT("spawnable_count"), FString::FromInt(MovieScene.GetSpawnableCount()));
	Entity.Attributes.Add(TEXT("possessable_count"), FString::FromInt(MovieScene.GetPossessableCount()));
	Entity.Attributes.Add(TEXT("binding_count"), FString::FromInt(MovieScene.GetBindings().Num()));
	Entity.Attributes.Add(TEXT("root_track_count"), FString::FromInt(MovieScene.GetTracks().Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("movie_scene_timing"), TEXT("bindings"), TEXT("tracks"), TEXT("sections"), TEXT("channels"), TEXT("channel_key_times") };
	Entity.Completeness.Omitted = { TEXT("runtime_bound_world_actors"), TEXT("typed_channel_key_values") };
	AddCineEvidence(Entity, MovieScenePath, TEXT("MovieScene structure read from UMovieScene."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddSpawnableEntity(
	const FString& ProjectId,
	const UMovieScene& MovieScene,
	const FMovieSceneSpawnable& Spawnable,
	int32 SpawnableIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString Guid = CineGuidString(Spawnable.GetGuid());
	const FString CanonicalKey = MovieScene.GetPathName() + TEXT(":spawnable:") + Guid;
	const FString EntityId = MakeStableId(ProjectId, TEXT("movie_scene_spawnable"), CanonicalKey);
	if (FindCineEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	const UObject* Template = Spawnable.GetObjectTemplate();
	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("movie_scene_spawnable");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Spawnable.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("movie_scene_path"), MovieScene.GetPathName());
	Entity.Attributes.Add(TEXT("guid"), Guid);
	Entity.Attributes.Add(TEXT("index"), FString::FromInt(SpawnableIndex));
	Entity.Attributes.Add(TEXT("template_path"), CineObjectPath(Template));
	Entity.Attributes.Add(TEXT("template_class_path"), CineClassPath(Template));
	Entity.Attributes.Add(TEXT("child_possessable_count"), FString::FromInt(Spawnable.GetChildPossessables().Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("spawnable_guid"), TEXT("spawn_template_reference"), TEXT("child_possessables") };
	Entity.Completeness.Omitted = { TEXT("runtime_spawned_actor_instance") };
	AddCineEvidence(Entity, MovieScene.GetPathName(), TEXT("MovieScene spawnable read from UMovieScene."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddPossessableEntity(
	const FString& ProjectId,
	const UMovieScene& MovieScene,
	const FMovieScenePossessable& Possessable,
	int32 PossessableIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString Guid = CineGuidString(Possessable.GetGuid());
	const FString CanonicalKey = MovieScene.GetPathName() + TEXT(":possessable:") + Guid;
	const FString EntityId = MakeStableId(ProjectId, TEXT("movie_scene_possessable"), CanonicalKey);
	if (FindCineEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("movie_scene_possessable");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Possessable.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("movie_scene_path"), MovieScene.GetPathName());
	Entity.Attributes.Add(TEXT("guid"), Guid);
	Entity.Attributes.Add(TEXT("index"), FString::FromInt(PossessableIndex));
	Entity.Attributes.Add(TEXT("possessed_object_class_path"), CineClassPath(Possessable.GetPossessedObjectClass()));
	Entity.Attributes.Add(TEXT("parent_guid"), CineGuidString(Possessable.GetParent()));
	Entity.Attributes.Add(TEXT("spawnable_binding_id"), BindingIdString(Possessable.GetSpawnableObjectBindingID()));
	Entity.Attributes.Add(TEXT("world_actor_resolution_state"), TEXT("not_resolved_static_scan"));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("possessable_guid"), TEXT("object_class"), TEXT("parent_binding_reference") };
	Entity.Completeness.Omitted = { TEXT("runtime_world_actor_resolution") };
	AddCineEvidence(Entity, MovieScene.GetPathName(), TEXT("MovieScene possessable read from UMovieScene."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddBindingEntity(
	const FString& ProjectId,
	const UMovieScene& MovieScene,
	const FMovieSceneBinding& Binding,
	int32 BindingIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString Guid = CineGuidString(Binding.GetObjectGuid());
	const FString CanonicalKey = MovieScene.GetPathName() + TEXT(":binding:") + Guid;
	const FString EntityId = MakeStableId(ProjectId, TEXT("movie_scene_binding"), CanonicalKey);
	if (FindCineEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("movie_scene_binding");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Binding.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("movie_scene_path"), MovieScene.GetPathName());
	Entity.Attributes.Add(TEXT("guid"), Guid);
	Entity.Attributes.Add(TEXT("index"), FString::FromInt(BindingIndex));
	Entity.Attributes.Add(TEXT("track_count"), FString::FromInt(Binding.GetTracks().Num()));
	Entity.Attributes.Add(TEXT("world_actor_resolution_state"), TEXT("not_resolved_static_scan"));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("binding_guid"), TEXT("binding_tracks") };
	Entity.Completeness.Omitted = { TEXT("runtime_world_actor_resolution") };
	AddCineEvidence(Entity, MovieScene.GetPathName(), TEXT("MovieScene object binding read from UMovieScene."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddTrackEntity(
	const FString& ProjectId,
	const UMovieScene& MovieScene,
	const UMovieSceneTrack& Track,
	int32 TrackIndex,
	const FString& OwnerId,
	const FString& OwnerKind,
	TArray<FEntityRecord>& OutEntities)
{
	const FString TrackPath = Track.GetPathName();
	const FString CanonicalKey = MovieScene.GetPathName() + TEXT(":track:") + TrackPath;
	const FString EntityId = MakeStableId(ProjectId, TEXT("movie_scene_track"), CanonicalKey);
	if (FindCineEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	const FString SemanticKind = TrackSemanticKind(Track);
	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("movie_scene_track");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Track.GetDisplayName().ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("movie_scene_path"), MovieScene.GetPathName());
	Entity.Attributes.Add(TEXT("track_path"), TrackPath);
	Entity.Attributes.Add(TEXT("track_class"), CineClassPath(&Track));
	Entity.Attributes.Add(TEXT("track_name"), Track.GetTrackName().ToString());
	Entity.Attributes.Add(TEXT("semantic_kind"), SemanticKind);
	Entity.Attributes.Add(TEXT("index"), FString::FromInt(TrackIndex));
	Entity.Attributes.Add(TEXT("owner_id"), OwnerId);
	Entity.Attributes.Add(TEXT("owner_kind"), OwnerKind);
	Entity.Attributes.Add(TEXT("section_count"), FString::FromInt(Track.GetAllSections().Num()));
	Entity.Attributes.Add(TEXT("supports_multiple_rows"), CineBool(Track.SupportsMultipleRows()));
	Entity.Attributes.Add(TEXT("is_eval_disabled"), CineBool(Track.IsEvalDisabled()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("track_metadata"), TEXT("sections"), TEXT("semantic_kind") };
	Entity.Completeness.Omitted = { TEXT("runtime_evaluation_field") };
	if (const UMovieSceneEventTrack* EventTrack = Cast<UMovieSceneEventTrack>(&Track))
	{
		Entity.Attributes.Add(TEXT("fire_events_when_forwards"), CineBool(EventTrack->bFireEventsWhenForwards));
		Entity.Attributes.Add(TEXT("fire_events_when_backwards"), CineBool(EventTrack->bFireEventsWhenBackwards));
		Entity.Attributes.Add(TEXT("event_position"), CineEnumString(EventTrack->EventPosition));
	}
	AddCineEvidence(Entity, MovieScene.GetPathName(), TEXT("MovieScene track read from UMovieSceneTrack."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

TSharedRef<FJsonObject> ChannelSnapshot(
	const FString& ProjectId,
	const FString& ChannelId,
	const FString& SectionId,
	const FString& TrackId,
	FMovieSceneChannel& Channel,
	const FMovieSceneChannelMetaData* MetaData,
	int32 ChannelIndex,
	const FFrameRate& TickResolution)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	const TRange<FFrameNumber> EffectiveRange = Channel.ComputeEffectiveRange();
	const FString ChannelName = MetaData ? MetaData->Name.ToString() : FString();
	const FString DisplayName = MetaData ? MetaData->DisplayText.ToString() : FString();
	const TArray<FFrameNumber> KeyTimes = ChannelKeyTimes(Channel);
	Object->SetStringField(TEXT("id"), ChannelId);
	Object->SetNumberField(TEXT("index"), ChannelIndex);
	Object->SetStringField(TEXT("channel_name"), ChannelName);
	Object->SetStringField(TEXT("display_name"), DisplayName);
	Object->SetStringField(TEXT("group"), MetaData ? MetaData->Group.ToString() : FString());
	Object->SetStringField(TEXT("intent_name"), MetaData ? MetaData->IntentName.ToString() : FString());
	Object->SetBoolField(TEXT("enabled"), MetaData ? static_cast<bool>(MetaData->bEnabled) : true);
	Object->SetBoolField(TEXT("can_collapse_to_track"), MetaData ? static_cast<bool>(MetaData->bCanCollapseToTrack) : false);
	Object->SetNumberField(TEXT("sort_order"), MetaData ? static_cast<double>(MetaData->SortOrder) : 0.0);
	Object->SetNumberField(TEXT("key_count"), KeyTimes.Num());
	Object->SetNumberField(TEXT("key_time_count"), KeyTimes.Num());
	Object->SetObjectField(TEXT("effective_range"), FrameRangeObject(EffectiveRange));

	TArray<TSharedPtr<FJsonValue>> KeyValues;
	for (int32 KeyIndex = 0; KeyIndex < KeyTimes.Num(); ++KeyIndex)
	{
		const FFrameNumber KeyTime = KeyTimes[KeyIndex];
		const FString KeyCanonical = FString::Printf(TEXT("%s:key:%d:%d"), *ChannelId, KeyIndex, KeyTime.Value);
		TSharedRef<FJsonObject> KeyObject = MakeShared<FJsonObject>();
		KeyObject->SetStringField(TEXT("id"), MakeStableId(ProjectId, TEXT("movie_scene_key"), KeyCanonical));
		KeyObject->SetNumberField(TEXT("index"), KeyIndex);
		KeyObject->SetStringField(TEXT("track_id"), TrackId);
		KeyObject->SetStringField(TEXT("section_id"), SectionId);
		KeyObject->SetStringField(TEXT("channel_id"), ChannelId);
		KeyObject->SetNumberField(TEXT("channel_index"), ChannelIndex);
		KeyObject->SetStringField(TEXT("channel_name"), ChannelName);
		KeyObject->SetStringField(TEXT("display_name"), DisplayName);
		KeyObject->SetNumberField(TEXT("frame_number"), KeyTime.Value);
		KeyObject->SetNumberField(TEXT("time_seconds"), FrameNumberSeconds(KeyTime, TickResolution));
		KeyValues.Add(MakeShared<FJsonValueObject>(KeyObject));
	}
	Object->SetArrayField(TEXT("keys"), KeyValues);
	if (KeyTimes.Num() > 0)
	{
		Object->SetObjectField(TEXT("key_artifact"), KeyArtifactManifest(ProjectId, TEXT("channel"), ChannelId, KeyTimes.Num()));
	}
	return Object;
}

FString AddSectionEntity(
	const FString& ProjectId,
	const UMovieScene& MovieScene,
	const UMovieSceneTrack& Track,
	const UMovieSceneSection& Section,
	int32 SectionIndex,
	const FString& TrackId,
	TArray<FEntityRecord>& OutEntities)
{
	const FString SectionPath = Section.GetPathName();
	const FString CanonicalKey = MovieScene.GetPathName() + TEXT(":section:") + SectionPath;
	const FString EntityId = MakeStableId(ProjectId, TEXT("movie_scene_section"), CanonicalKey);
	if (FindCineEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	int32 ChannelCount = 0;
	int32 KeyCount = 0;
	for (const FMovieSceneChannelEntry& Entry : Section.GetChannelProxy().GetAllEntries())
	{
		for (FMovieSceneChannel* Channel : Entry.GetChannels())
		{
			if (!Channel)
			{
				continue;
			}
			++ChannelCount;
			KeyCount += ChannelKeyTimes(*Channel).Num();
		}
	}

	const FOptionalMovieSceneBlendType BlendType = Section.GetBlendType();
	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("movie_scene_section");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Section.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("movie_scene_path"), MovieScene.GetPathName());
	Entity.Attributes.Add(TEXT("track_id"), TrackId);
	Entity.Attributes.Add(TEXT("track_path"), Track.GetPathName());
	Entity.Attributes.Add(TEXT("section_path"), SectionPath);
	Entity.Attributes.Add(TEXT("section_class"), CineClassPath(&Section));
	Entity.Attributes.Add(TEXT("semantic_kind"), SectionSemanticKind(Section));
	Entity.Attributes.Add(TEXT("index"), FString::FromInt(SectionIndex));
	Entity.Attributes.Add(TEXT("row_index"), FString::FromInt(Section.GetRowIndex()));
	Entity.Attributes.Add(TEXT("active"), CineBool(Section.IsActive()));
	Entity.Attributes.Add(TEXT("locked"), CineBool(Section.IsLocked()));
	Entity.Attributes.Add(TEXT("completion_mode"), CineEnumString(Section.GetCompletionMode()));
	Entity.Attributes.Add(TEXT("blend_type"), BlendType.IsValid() ? CineEnumString(BlendType.Get()) : FString());
	Entity.Attributes.Add(TEXT("pre_roll_frames"), FString::FromInt(Section.GetPreRollFrames()));
	Entity.Attributes.Add(TEXT("post_roll_frames"), FString::FromInt(Section.GetPostRollFrames()));
	Entity.Attributes.Add(TEXT("channel_count"), FString::FromInt(ChannelCount));
	Entity.Attributes.Add(TEXT("key_count"), FString::FromInt(KeyCount));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("section_range"), TEXT("section_metadata"), TEXT("channel_key_counts"), TEXT("channel_key_times") };
	Entity.Completeness.Omitted = { TEXT("typed_channel_key_values"), TEXT("runtime_evaluation") };
	AddCineEvidence(Entity, MovieScene.GetPathName(), TEXT("MovieScene section read from UMovieSceneSection."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

TSharedRef<FJsonObject> SpawnableSnapshot(
	const FString& SpawnableId,
	const FMovieSceneSpawnable& Spawnable,
	int32 SpawnableIndex)
{
	const UObject* Template = Spawnable.GetObjectTemplate();
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), SpawnableId);
	Object->SetNumberField(TEXT("index"), SpawnableIndex);
	Object->SetStringField(TEXT("guid"), CineGuidString(Spawnable.GetGuid()));
	Object->SetStringField(TEXT("name"), Spawnable.GetName());
	Object->SetStringField(TEXT("template_path"), CineObjectPath(Template));
	Object->SetStringField(TEXT("template_class_path"), CineClassPath(Template));
	Object->SetNumberField(TEXT("child_possessable_count"), Spawnable.GetChildPossessables().Num());

	TArray<TSharedPtr<FJsonValue>> ChildValues;
	for (const FGuid& ChildGuid : Spawnable.GetChildPossessables())
	{
		ChildValues.Add(MakeShared<FJsonValueString>(CineGuidString(ChildGuid)));
	}
	Object->SetArrayField(TEXT("child_possessable_guids"), ChildValues);
	return Object;
}

TSharedRef<FJsonObject> PossessableSnapshot(
	const FString& PossessableId,
	const FMovieScenePossessable& Possessable,
	int32 PossessableIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), PossessableId);
	Object->SetNumberField(TEXT("index"), PossessableIndex);
	Object->SetStringField(TEXT("guid"), CineGuidString(Possessable.GetGuid()));
	Object->SetStringField(TEXT("name"), Possessable.GetName());
	Object->SetStringField(TEXT("possessed_object_class_path"), CineClassPath(Possessable.GetPossessedObjectClass()));
	Object->SetStringField(TEXT("parent_guid"), CineGuidString(Possessable.GetParent()));
	Object->SetStringField(TEXT("spawnable_binding_id"), BindingIdString(Possessable.GetSpawnableObjectBindingID()));
	Object->SetStringField(TEXT("world_actor_resolution_state"), TEXT("not_resolved_static_scan"));
	return Object;
}

TSharedRef<FJsonObject> BindingSnapshot(
	const FString& BindingId,
	const FMovieSceneBinding& Binding,
	int32 BindingIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), BindingId);
	Object->SetNumberField(TEXT("index"), BindingIndex);
	Object->SetStringField(TEXT("guid"), CineGuidString(Binding.GetObjectGuid()));
	Object->SetStringField(TEXT("name"), Binding.GetName());
	Object->SetNumberField(TEXT("track_count"), Binding.GetTracks().Num());
	Object->SetStringField(TEXT("world_actor_resolution_state"), TEXT("not_resolved_static_scan"));
	return Object;
}

TSharedRef<FJsonObject> TrackSnapshot(
	const FString& TrackId,
	const UMovieSceneTrack& Track,
	int32 TrackIndex,
	const FString& OwnerId,
	const FString& OwnerKind)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), TrackId);
	Object->SetNumberField(TEXT("index"), TrackIndex);
	Object->SetStringField(TEXT("track_path"), Track.GetPathName());
	Object->SetStringField(TEXT("track_class"), CineClassPath(&Track));
	Object->SetStringField(TEXT("track_name"), Track.GetTrackName().ToString());
	Object->SetStringField(TEXT("display_name"), Track.GetDisplayName().ToString());
	Object->SetStringField(TEXT("semantic_kind"), TrackSemanticKind(Track));
	Object->SetStringField(TEXT("owner_id"), OwnerId);
	Object->SetStringField(TEXT("owner_kind"), OwnerKind);
	Object->SetBoolField(TEXT("supports_multiple_rows"), Track.SupportsMultipleRows());
	Object->SetBoolField(TEXT("is_empty"), Track.IsEmpty());
	Object->SetBoolField(TEXT("is_eval_disabled"), Track.IsEvalDisabled());
	Object->SetNumberField(TEXT("section_count"), Track.GetAllSections().Num());
	if (const UMovieSceneEventTrack* EventTrack = Cast<UMovieSceneEventTrack>(&Track))
	{
		Object->SetBoolField(TEXT("fire_events_when_forwards"), EventTrack->bFireEventsWhenForwards);
		Object->SetBoolField(TEXT("fire_events_when_backwards"), EventTrack->bFireEventsWhenBackwards);
		Object->SetStringField(TEXT("event_position"), CineEnumString(EventTrack->EventPosition));
	}
	return Object;
}

void AddSpecializedSectionFields(
	const FString& ProjectId,
	const UMovieScene& MovieScene,
	const FString& SectionId,
	const UMovieSceneSection& Section,
	TSharedRef<FJsonObject> Object,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString EvidencePath = MovieScene.GetPathName();
	if (const UMovieSceneCameraCutSection* CameraCutSection = Cast<UMovieSceneCameraCutSection>(&Section))
	{
		Object->SetObjectField(TEXT("camera_binding"), BindingReferenceObject(CameraCutSection->GetCameraBindingID()));
		const FString CameraGuid = CineGuidString(CameraCutSection->GetCameraBindingID().GetGuid());
		if (!CameraGuid.IsEmpty())
		{
			Object->SetStringField(TEXT("camera_binding_guid"), CameraGuid);
		}
	}
	else if (const UMovieSceneSubSection* SubSection = Cast<UMovieSceneSubSection>(&Section))
	{
		const UMovieSceneSequence* SubSequence = SubSection->GetSequence();
		const FString SubSequencePath = CineObjectPath(SubSequence);
		TRange<FFrameNumber> InnerPlaybackRange = TRange<FFrameNumber>::Empty();
		SubSection->GetValidatedInnerPlaybackRange(InnerPlaybackRange);
		Object->SetStringField(TEXT("sub_sequence_path"), SubSequencePath);
		Object->SetStringField(TEXT("sub_sequence_class"), CineClassPath(SubSequence));
		Object->SetObjectField(TEXT("sub_sequence_playback_range"), FrameRangeObject(InnerPlaybackRange));
		Object->SetNumberField(TEXT("sub_sequence_start_frame_offset"), SubSection->Parameters.StartFrameOffset.Value);
		Object->SetNumberField(TEXT("sub_sequence_end_frame_offset"), SubSection->Parameters.EndFrameOffset.Value);
		Object->SetNumberField(TEXT("sub_sequence_first_loop_start_frame_offset"), SubSection->Parameters.FirstLoopStartFrameOffset.Value);
		Object->SetNumberField(TEXT("sub_sequence_time_scale"), SubSection->Parameters.TimeScale);
		Object->SetNumberField(TEXT("sub_sequence_hierarchical_bias"), SubSection->Parameters.HierarchicalBias);
		Object->SetBoolField(TEXT("sub_sequence_can_loop"), SubSection->Parameters.bCanLoop);
		if (!SubSequencePath.IsEmpty())
		{
			const FString ReferenceId = AddAssetReferenceEntity(
				ProjectId,
				SubSequencePath,
				EvidencePath,
				TEXT("Sub-sequence reference read from MovieScene sub section."),
				OutEntities);
			AddCineRelation(
				ProjectId,
				TEXT("movie_scene_section_uses_subsequence"),
				SectionId,
				ReferenceId,
				EvidencePath,
				TEXT("MovieScene sub section references another sequence."),
				OutRelations);
		}
	}
	else if (const UMovieSceneAudioSection* AudioSection = Cast<UMovieSceneAudioSection>(&Section))
	{
		const USoundBase* Sound = AudioSection->GetSound();
		const FString SoundPath = CineObjectPath(Sound);
		Object->SetStringField(TEXT("sound_path"), SoundPath);
		Object->SetStringField(TEXT("sound_class"), CineClassPath(Sound));
		Object->SetStringField(TEXT("attenuation_settings_path"), CineObjectPath(AudioSection->GetAttenuationSettings()));
		Object->SetNumberField(TEXT("audio_start_frame_offset"), AudioSection->GetStartOffset().Value);
		if (!SoundPath.IsEmpty())
		{
			const FString ReferenceId = AddAssetReferenceEntity(
				ProjectId,
				SoundPath,
				EvidencePath,
				TEXT("Sound reference read from MovieScene audio section."),
				OutEntities);
			AddCineRelation(
				ProjectId,
				TEXT("movie_scene_section_uses_sound"),
				SectionId,
				ReferenceId,
				EvidencePath,
				TEXT("MovieScene audio section references a sound asset."),
				OutRelations);
		}
	}
	else if (const UMovieSceneSkeletalAnimationSection* AnimSection = Cast<UMovieSceneSkeletalAnimationSection>(&Section))
	{
		const UAnimSequenceBase* Animation = AnimSection->Params.Animation;
		const FString AnimationPath = CineObjectPath(Animation);
		Object->SetStringField(TEXT("animation_path"), AnimationPath);
		Object->SetStringField(TEXT("animation_class"), CineClassPath(Animation));
		Object->SetNumberField(TEXT("animation_play_rate"), AnimSection->Params.PlayRate);
		Object->SetBoolField(TEXT("animation_reverse"), AnimSection->Params.bReverse != 0);
		Object->SetStringField(TEXT("animation_slot_name"), AnimSection->Params.SlotName.ToString());
		Object->SetNumberField(TEXT("animation_first_loop_start_frame_offset"), AnimSection->Params.FirstLoopStartFrameOffset.Value);
		Object->SetNumberField(TEXT("animation_start_frame_offset"), AnimSection->Params.StartFrameOffset.Value);
		Object->SetNumberField(TEXT("animation_end_frame_offset"), AnimSection->Params.EndFrameOffset.Value);
		if (!AnimationPath.IsEmpty())
		{
			const FString ReferenceId = AddAssetReferenceEntity(
				ProjectId,
				AnimationPath,
				EvidencePath,
				TEXT("Animation reference read from MovieScene skeletal animation section."),
				OutEntities);
			AddCineRelation(
				ProjectId,
				TEXT("movie_scene_section_uses_animation"),
				SectionId,
				ReferenceId,
				EvidencePath,
				TEXT("MovieScene skeletal animation section references an animation asset."),
				OutRelations);
		}
	}
}

TSharedRef<FJsonObject> SectionSnapshot(
	const FString& ProjectId,
	const FString& SectionId,
	const UMovieScene& MovieScene,
	const UMovieSceneTrack& Track,
	const UMovieSceneSection& Section,
	int32 SectionIndex,
	const FString& TrackId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	const FOptionalMovieSceneBlendType BlendType = Section.GetBlendType();

	Object->SetStringField(TEXT("id"), SectionId);
	Object->SetNumberField(TEXT("index"), SectionIndex);
	Object->SetStringField(TEXT("track_id"), TrackId);
	Object->SetStringField(TEXT("track_path"), Track.GetPathName());
	Object->SetStringField(TEXT("section_path"), Section.GetPathName());
	Object->SetStringField(TEXT("section_class"), CineClassPath(&Section));
	Object->SetStringField(TEXT("semantic_kind"), SectionSemanticKind(Section));
	Object->SetStringField(TEXT("name"), Section.GetName());
	Object->SetObjectField(TEXT("range"), FrameRangeObject(Section.GetRange()));
	Object->SetObjectField(TEXT("effective_range"), FrameRangeObject(Section.ComputeEffectiveRange()));
	Object->SetNumberField(TEXT("row_index"), Section.GetRowIndex());
	Object->SetBoolField(TEXT("active"), Section.IsActive());
	Object->SetBoolField(TEXT("locked"), Section.IsLocked());
	Object->SetStringField(TEXT("completion_mode"), CineEnumString(Section.GetCompletionMode()));
	Object->SetStringField(TEXT("blend_type"), BlendType.IsValid() ? CineEnumString(BlendType.Get()) : FString());
	Object->SetNumberField(TEXT("pre_roll_frames"), Section.GetPreRollFrames());
	Object->SetNumberField(TEXT("post_roll_frames"), Section.GetPostRollFrames());

	TArray<TSharedPtr<FJsonValue>> ChannelValues;
	int32 KeyCount = 0;
	int32 ChannelIndex = 0;
	for (const FMovieSceneChannelEntry& Entry : Section.GetChannelProxy().GetAllEntries())
	{
		TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();
#if WITH_EDITOR
		TArrayView<const FMovieSceneChannelMetaData> MetaData = Entry.GetMetaData();
#endif
		for (int32 EntryChannelIndex = 0; EntryChannelIndex < Channels.Num(); ++EntryChannelIndex)
		{
			FMovieSceneChannel* Channel = Channels[EntryChannelIndex];
			if (!Channel)
			{
				continue;
			}

			const FString ChannelKey = Section.GetPathName() + TEXT(":channel:") + FString::FromInt(ChannelIndex);
			const FString ChannelId = MakeStableId(ProjectId, TEXT("movie_scene_channel"), ChannelKey);
#if WITH_EDITOR
			const FMovieSceneChannelMetaData* ChannelMetaData = MetaData.IsValidIndex(EntryChannelIndex) ? &MetaData[EntryChannelIndex] : nullptr;
#else
			const FMovieSceneChannelMetaData* ChannelMetaData = nullptr;
#endif
			TSharedRef<FJsonObject> ChannelObject = ChannelSnapshot(
				ProjectId,
				ChannelId,
				SectionId,
				TrackId,
				*Channel,
				ChannelMetaData,
				ChannelIndex,
				MovieScene.GetTickResolution());
			KeyCount += static_cast<int32>(ChannelObject->GetNumberField(TEXT("key_count")));
			ChannelValues.Add(MakeShared<FJsonValueObject>(ChannelObject));
			++ChannelIndex;
		}
	}

	Object->SetNumberField(TEXT("channel_count"), ChannelValues.Num());
	Object->SetNumberField(TEXT("key_count"), KeyCount);
	Object->SetStringField(TEXT("key_storage"), KeyCount > 0 ? TEXT("inline_key_times_daemon_artifact") : TEXT("no_keys"));
	if (KeyCount > 0)
	{
		Object->SetObjectField(TEXT("key_artifact"), KeyArtifactManifest(ProjectId, TEXT("section"), SectionId, KeyCount));
	}
	Object->SetArrayField(TEXT("channels"), ChannelValues);
	AddSpecializedSectionFields(ProjectId, MovieScene, SectionId, Section, Object, OutEntities, OutRelations);
	return Object;
}

TSharedRef<FJsonObject> BindingTagSnapshot(const FName& Tag, const FMovieSceneObjectBindingIDs& BindingIds, int32 TagIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), TagIndex);
	Object->SetStringField(TEXT("tag"), Tag.ToString());
	Object->SetNumberField(TEXT("binding_count"), BindingIds.IDs.Num());

	TArray<TSharedPtr<FJsonValue>> BindingValues;
	for (const FMovieSceneObjectBindingID& BindingId : BindingIds.IDs)
	{
		BindingValues.Add(MakeShared<FJsonValueObject>(BindingReferenceObject(BindingId)));
	}
	Object->SetArrayField(TEXT("bindings"), BindingValues);
	return Object;
}

TSharedRef<FJsonObject> MarkedFrameSnapshot(const FMovieSceneMarkedFrame& MarkedFrame, int32 MarkedFrameIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), MarkedFrameIndex);
	Object->SetNumberField(TEXT("frame_number"), MarkedFrame.FrameNumber.Value);
	Object->SetStringField(TEXT("label"), MarkedFrame.Label);
	return Object;
}

TSharedRef<FJsonObject> LevelSequenceSnapshot(
	const FString& ProjectId,
	const ULevelSequence& Sequence,
	UMovieScene& MovieScene,
	const FString& SequenceId,
	const FString& MovieSceneId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.level_sequence.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), SequenceId);
	Object->SetStringField(TEXT("sequence_path"), Sequence.GetPathName());
	Object->SetStringField(TEXT("sequence_class"), CineClassPath(&Sequence));
	Object->SetStringField(TEXT("movie_scene_id"), MovieSceneId);
	Object->SetStringField(TEXT("movie_scene_path"), MovieScene.GetPathName());
	Object->SetStringField(TEXT("movie_scene_class"), CineClassPath(&MovieScene));
#if WITH_EDITOR
	const UBlueprint* DirectorBlueprint = Sequence.GetDirectorBlueprint();
	Object->SetStringField(TEXT("director_blueprint_path"), CineObjectPath(DirectorBlueprint));
	Object->SetStringField(TEXT("director_blueprint_name"), Sequence.GetDirectorBlueprintName());
	Object->SetStringField(TEXT("director_class_path"), DirectorBlueprint && DirectorBlueprint->GeneratedClass ? DirectorBlueprint->GeneratedClass->GetPathName() : FString());
#else
	Object->SetStringField(TEXT("director_blueprint_path"), FString());
	Object->SetStringField(TEXT("director_blueprint_name"), FString());
	Object->SetStringField(TEXT("director_class_path"), FString());
#endif
	Object->SetObjectField(TEXT("display_rate"), FrameRateObject(MovieScene.GetDisplayRate()));
	Object->SetObjectField(TEXT("tick_resolution"), FrameRateObject(MovieScene.GetTickResolution()));
	Object->SetObjectField(TEXT("playback_range"), FrameRangeObject(MovieScene.GetPlaybackRange()));
	Object->SetObjectField(TEXT("selection_range"), FrameRangeObject(MovieScene.GetSelectionRange()));
	Object->SetStringField(TEXT("world_actor_resolution_state"), TEXT("not_resolved_static_scan"));
	Object->SetStringField(TEXT("key_storage"), TEXT("no_keys"));

	TArray<TSharedPtr<FJsonValue>> SpawnableValues;
	TArray<TSharedPtr<FJsonValue>> PossessableValues;
	TArray<TSharedPtr<FJsonValue>> BindingValues;
	TArray<TSharedPtr<FJsonValue>> TrackValues;
	TArray<TSharedPtr<FJsonValue>> SectionValues;
	TArray<TSharedPtr<FJsonValue>> BindingTagValues;
	TArray<TSharedPtr<FJsonValue>> MarkedFrameValues;

	TMap<FGuid, FString> GuidToBindingId;
	TMap<FGuid, FString> GuidToPossessableId;
	TMap<FGuid, FString> GuidToSpawnableId;
	TMap<const UMovieSceneTrack*, FString> TrackIds;
	TMap<const UMovieSceneSection*, FString> SectionIds;
	TArray<const UMovieSceneTrack*> OrderedTracks;

	for (int32 SpawnableIndex = 0; SpawnableIndex < MovieScene.GetSpawnableCount(); ++SpawnableIndex)
	{
		const FMovieSceneSpawnable& Spawnable = MovieScene.GetSpawnable(SpawnableIndex);
		const FString SpawnableId = AddSpawnableEntity(ProjectId, MovieScene, Spawnable, SpawnableIndex, OutEntities);
		GuidToSpawnableId.Add(Spawnable.GetGuid(), SpawnableId);
		SpawnableValues.Add(MakeShared<FJsonValueObject>(SpawnableSnapshot(SpawnableId, Spawnable, SpawnableIndex)));
		AddCineRelation(
			ProjectId,
			TEXT("contains_movie_scene_spawnable"),
			MovieSceneId,
			SpawnableId,
			MovieScene.GetPathName(),
			TEXT("MovieScene contains a spawnable binding."),
			OutRelations);
	}

	for (int32 PossessableIndex = 0; PossessableIndex < MovieScene.GetPossessableCount(); ++PossessableIndex)
	{
		const FMovieScenePossessable& Possessable = MovieScene.GetPossessable(PossessableIndex);
		const FString PossessableId = AddPossessableEntity(ProjectId, MovieScene, Possessable, PossessableIndex, OutEntities);
		GuidToPossessableId.Add(Possessable.GetGuid(), PossessableId);
		PossessableValues.Add(MakeShared<FJsonValueObject>(PossessableSnapshot(PossessableId, Possessable, PossessableIndex)));
		AddCineRelation(
			ProjectId,
			TEXT("contains_movie_scene_possessable"),
			MovieSceneId,
			PossessableId,
			MovieScene.GetPathName(),
			TEXT("MovieScene contains a possessable binding."),
			OutRelations);
	}

	for (const TPair<FGuid, FString>& SpawnablePair : GuidToSpawnableId)
	{
		const int32 SpawnableIndex = [&MovieScene, &SpawnablePair]() -> int32
		{
			for (int32 Index = 0; Index < MovieScene.GetSpawnableCount(); ++Index)
			{
				if (MovieScene.GetSpawnable(Index).GetGuid() == SpawnablePair.Key)
				{
					return Index;
				}
			}
			return static_cast<int32>(INDEX_NONE);
		}();
		if (SpawnableIndex == INDEX_NONE)
		{
			continue;
		}
		for (const FGuid& ChildGuid : MovieScene.GetSpawnable(SpawnableIndex).GetChildPossessables())
		{
			if (const FString* ChildId = GuidToPossessableId.Find(ChildGuid))
			{
				AddCineRelation(
					ProjectId,
					TEXT("movie_scene_spawnable_child_possessable"),
					SpawnablePair.Value,
					*ChildId,
					MovieScene.GetPathName(),
					TEXT("MovieScene spawnable declares a child possessable."),
					OutRelations);
			}
		}
	}

	for (const TPair<FGuid, FString>& PossessablePair : GuidToPossessableId)
	{
		for (int32 PossessableIndex = 0; PossessableIndex < MovieScene.GetPossessableCount(); ++PossessableIndex)
		{
			const FMovieScenePossessable& Possessable = MovieScene.GetPossessable(PossessableIndex);
			if (Possessable.GetGuid() != PossessablePair.Key)
			{
				continue;
			}
			if (const FString* ParentId = GuidToPossessableId.Find(Possessable.GetParent()))
			{
				AddCineRelation(
					ProjectId,
					TEXT("movie_scene_possessable_parent"),
					PossessablePair.Value,
					*ParentId,
					MovieScene.GetPathName(),
					TEXT("MovieScene possessable has a parent possessable."),
					OutRelations);
			}
			else if (const FString* ParentSpawnableId = GuidToSpawnableId.Find(Possessable.GetParent()))
			{
				AddCineRelation(
					ProjectId,
					TEXT("movie_scene_possessable_parent"),
					PossessablePair.Value,
					*ParentSpawnableId,
					MovieScene.GetPathName(),
					TEXT("MovieScene possessable has a parent spawnable."),
					OutRelations);
			}
		}
	}

	const TArray<FMovieSceneBinding>& Bindings = MovieScene.GetBindings();
	for (int32 BindingIndex = 0; BindingIndex < Bindings.Num(); ++BindingIndex)
	{
		const FMovieSceneBinding& Binding = Bindings[BindingIndex];
		const FString BindingId = AddBindingEntity(ProjectId, MovieScene, Binding, BindingIndex, OutEntities);
		GuidToBindingId.Add(Binding.GetObjectGuid(), BindingId);
		BindingValues.Add(MakeShared<FJsonValueObject>(BindingSnapshot(BindingId, Binding, BindingIndex)));
		AddCineRelation(
			ProjectId,
			TEXT("contains_movie_scene_binding"),
			MovieSceneId,
			BindingId,
			MovieScene.GetPathName(),
			TEXT("MovieScene contains an object binding."),
			OutRelations);

		int32 BindingTrackIndex = 0;
		for (UMovieSceneTrack* Track : Binding.GetTracks())
		{
			if (!Track)
			{
				continue;
			}
			const FString TrackId = AddTrackEntity(ProjectId, MovieScene, *Track, BindingTrackIndex, BindingId, TEXT("binding"), OutEntities);
			TrackIds.Add(Track, TrackId);
			OrderedTracks.AddUnique(Track);
			TrackValues.Add(MakeShared<FJsonValueObject>(TrackSnapshot(TrackId, *Track, TrackValues.Num(), BindingId, TEXT("binding"))));
			AddCineRelation(
				ProjectId,
				TEXT("movie_scene_binding_owns_track"),
				BindingId,
				TrackId,
				MovieScene.GetPathName(),
				TEXT("MovieScene object binding owns a track."),
				OutRelations);
			++BindingTrackIndex;
		}
	}

	int32 RootTrackIndex = 0;
	for (UMovieSceneTrack* Track : MovieScene.GetTracks())
	{
		if (!Track)
		{
			continue;
		}
		const FString TrackId = AddTrackEntity(ProjectId, MovieScene, *Track, RootTrackIndex, MovieSceneId, TEXT("movie_scene"), OutEntities);
		TrackIds.Add(Track, TrackId);
		OrderedTracks.AddUnique(Track);
		TrackValues.Add(MakeShared<FJsonValueObject>(TrackSnapshot(TrackId, *Track, TrackValues.Num(), MovieSceneId, TEXT("movie_scene"))));
		AddCineRelation(
			ProjectId,
			TEXT("contains_movie_scene_track"),
			MovieSceneId,
			TrackId,
			MovieScene.GetPathName(),
			TEXT("MovieScene contains a root track."),
			OutRelations);
		++RootTrackIndex;
	}

	if (UMovieSceneTrack* CameraCutTrack = MovieScene.GetCameraCutTrack())
	{
		if (!TrackIds.Contains(CameraCutTrack))
		{
			const FString TrackId = AddTrackEntity(ProjectId, MovieScene, *CameraCutTrack, RootTrackIndex, MovieSceneId, TEXT("movie_scene"), OutEntities);
			TrackIds.Add(CameraCutTrack, TrackId);
			OrderedTracks.AddUnique(CameraCutTrack);
			TrackValues.Add(MakeShared<FJsonValueObject>(TrackSnapshot(TrackId, *CameraCutTrack, TrackValues.Num(), MovieSceneId, TEXT("movie_scene"))));
			AddCineRelation(
				ProjectId,
				TEXT("contains_movie_scene_track"),
				MovieSceneId,
				TrackId,
				MovieScene.GetPathName(),
				TEXT("MovieScene contains a camera cut track."),
				OutRelations);
			++RootTrackIndex;
		}
	}

	if (UMovieSceneTrack* CameraCutTrack = MovieScene.GetCameraCutTrack())
	{
		if (const FString* CameraCutTrackId = TrackIds.Find(CameraCutTrack))
		{
			Object->SetStringField(TEXT("camera_cut_track_id"), *CameraCutTrackId);
			AddCineRelation(
				ProjectId,
				TEXT("movie_scene_camera_cut_track"),
				MovieSceneId,
				*CameraCutTrackId,
				MovieScene.GetPathName(),
				TEXT("MovieScene camera cut track reference."),
				OutRelations);
		}
		else
		{
			Object->SetStringField(TEXT("camera_cut_track_id"), FString());
		}
	}
	else
	{
		Object->SetStringField(TEXT("camera_cut_track_id"), FString());
	}

	int32 SectionIndex = 0;
	int32 TotalChannelCount = 0;
	int32 TotalKeyCount = 0;
	for (const UMovieSceneTrack* Track : OrderedTracks)
	{
		if (!Track)
		{
			continue;
		}
		const FString* TrackId = TrackIds.Find(Track);
		if (!TrackId)
		{
			continue;
		}
		for (UMovieSceneSection* Section : Track->GetAllSections())
		{
			if (!Section || SectionIds.Contains(Section))
			{
				continue;
			}

			const FString SectionId = AddSectionEntity(ProjectId, MovieScene, *Track, *Section, SectionIndex, *TrackId, OutEntities);
			SectionIds.Add(Section, SectionId);
			TSharedRef<FJsonObject> SectionObject = SectionSnapshot(
				ProjectId,
				SectionId,
				MovieScene,
				*Track,
				*Section,
				SectionIndex,
				*TrackId,
				OutEntities,
				OutRelations);
			TotalChannelCount += static_cast<int32>(SectionObject->GetNumberField(TEXT("channel_count")));
			TotalKeyCount += static_cast<int32>(SectionObject->GetNumberField(TEXT("key_count")));
			SectionValues.Add(MakeShared<FJsonValueObject>(SectionObject));
			AddCineRelation(
				ProjectId,
				TEXT("movie_scene_track_owns_section"),
				*TrackId,
				SectionId,
				MovieScene.GetPathName(),
				TEXT("MovieScene track owns a section."),
				OutRelations);
			if (const UMovieSceneCameraCutSection* CameraCutSection = Cast<UMovieSceneCameraCutSection>(Section))
			{
				if (const FString* CameraBindingId = GuidToBindingId.Find(CameraCutSection->GetCameraBindingID().GetGuid()))
				{
					AddCineRelation(
						ProjectId,
						TEXT("movie_scene_camera_cut_targets_binding"),
						SectionId,
						*CameraBindingId,
						MovieScene.GetPathName(),
						TEXT("MovieScene camera cut section targets an object binding."),
						OutRelations);
				}
			}
			++SectionIndex;
		}
	}

	int32 BindingTagIndex = 0;
	for (const TPair<FName, FMovieSceneObjectBindingIDs>& BindingTag : MovieScene.AllTaggedBindings())
	{
		BindingTagValues.Add(MakeShared<FJsonValueObject>(BindingTagSnapshot(BindingTag.Key, BindingTag.Value, BindingTagIndex++)));
		for (const FMovieSceneObjectBindingID& TaggedBindingId : BindingTag.Value.IDs)
		{
			if (const FString* BindingId = GuidToBindingId.Find(TaggedBindingId.GetGuid()))
			{
				TMap<FString, FString> Attributes;
				Attributes.Add(TEXT("binding_tag"), BindingTag.Key.ToString());
				AddCineRelation(
					ProjectId,
					TEXT("movie_scene_binding_tag"),
					MovieSceneId,
					*BindingId,
					MovieScene.GetPathName(),
					TEXT("MovieScene exposes a binding tag for an object binding."),
					OutRelations,
					&Attributes);
			}
		}
	}

	const TArray<FMovieSceneMarkedFrame>& MarkedFrames = MovieScene.GetMarkedFrames();
	for (int32 MarkedFrameIndex = 0; MarkedFrameIndex < MarkedFrames.Num(); ++MarkedFrameIndex)
	{
		MarkedFrameValues.Add(MakeShared<FJsonValueObject>(MarkedFrameSnapshot(MarkedFrames[MarkedFrameIndex], MarkedFrameIndex)));
	}

	Object->SetNumberField(TEXT("spawnable_count"), SpawnableValues.Num());
	Object->SetNumberField(TEXT("possessable_count"), PossessableValues.Num());
	Object->SetNumberField(TEXT("binding_count"), BindingValues.Num());
	Object->SetNumberField(TEXT("track_count"), TrackValues.Num());
	Object->SetNumberField(TEXT("section_count"), SectionValues.Num());
	Object->SetNumberField(TEXT("channel_count"), TotalChannelCount);
	Object->SetNumberField(TEXT("key_count"), TotalKeyCount);
	Object->SetStringField(TEXT("key_storage"), TotalKeyCount > 0 ? TEXT("inline_key_times_daemon_artifact") : TEXT("no_keys"));
	if (TotalKeyCount > 0)
	{
		Object->SetObjectField(TEXT("key_artifact"), KeyArtifactManifest(ProjectId, TEXT("level_sequence"), SequenceId, TotalKeyCount));
	}
	Object->SetNumberField(TEXT("binding_tag_count"), BindingTagValues.Num());
	Object->SetNumberField(TEXT("marked_frame_count"), MarkedFrameValues.Num());
	Object->SetNumberField(TEXT("root_folder_count"), MovieScene.GetRootFolders().Num());
#if WITH_EDITORONLY_DATA
	Object->SetNumberField(TEXT("node_group_count"), MovieScene.GetNodeGroups().Num());
#else
	Object->SetNumberField(TEXT("node_group_count"), 0);
#endif
	Object->SetArrayField(TEXT("spawnables"), SpawnableValues);
	Object->SetArrayField(TEXT("possessables"), PossessableValues);
	Object->SetArrayField(TEXT("bindings"), BindingValues);
	Object->SetArrayField(TEXT("tracks"), TrackValues);
	Object->SetArrayField(TEXT("sections"), SectionValues);
	Object->SetArrayField(TEXT("binding_tags"), BindingTagValues);
	Object->SetArrayField(TEXT("marked_frames"), MarkedFrameValues);
	return Object;
}
}

bool FCinematicsReader::AppendCinematicsAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	ULevelSequence* LevelSequence = Cast<ULevelSequence>(&Asset);
	if (!LevelSequence)
	{
		return false;
	}

	UMovieScene* MovieScene = LevelSequence->GetMovieScene();
	const FString SequenceId = AddLevelSequenceEntity(ProjectId, *LevelSequence, MovieScene, OutEntities);
	AddCineRelation(
		ProjectId,
		TEXT("contains_level_sequence"),
		AssetEntity.Id,
		SequenceId,
		LevelSequence->GetPathName(),
		TEXT("Asset contains a LevelSequence object."),
		OutRelations);

	AssetEntity.Kind = TEXT("asset");
	AssetEntity.Attributes.Add(TEXT("cinematics_reader"), TEXT("level_sequence"));
	AssetEntity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	AssetEntity.Completeness.State = MovieScene ? ECompletenessState::Partial : ECompletenessState::MetadataOnly;
	AssetEntity.Completeness.Covered.AddUnique(TEXT("level_sequence"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("movie_scene_structure"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("movie_scene_key_times"));
	AssetEntity.Completeness.Omitted.Remove(TEXT("loaded_uobject_properties"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_bound_world_actors"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("typed_channel_key_values"));

	if (!MovieScene)
	{
		TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		Snapshot->SetStringField(TEXT("schema_version"), TEXT("uepi.level_sequence.v1"));
		Snapshot->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
		Snapshot->SetStringField(TEXT("id"), SequenceId);
		Snapshot->SetStringField(TEXT("sequence_path"), LevelSequence->GetPathName());
		Snapshot->SetStringField(TEXT("sequence_class"), CineClassPath(LevelSequence));
		Snapshot->SetStringField(TEXT("movie_scene_id"), FString());
		Snapshot->SetStringField(TEXT("movie_scene_path"), FString());
		Snapshot->SetStringField(TEXT("movie_scene_class"), FString());
		Snapshot->SetStringField(TEXT("director_blueprint_path"), FString());
		Snapshot->SetStringField(TEXT("director_blueprint_name"), FString());
		Snapshot->SetStringField(TEXT("director_class_path"), FString());
		Snapshot->SetObjectField(TEXT("display_rate"), FrameRateObject(FFrameRate(0, 1)));
		Snapshot->SetObjectField(TEXT("tick_resolution"), FrameRateObject(FFrameRate(0, 1)));
		Snapshot->SetObjectField(TEXT("playback_range"), FrameRangeObject(TRange<FFrameNumber>::Empty()));
		Snapshot->SetObjectField(TEXT("selection_range"), FrameRangeObject(TRange<FFrameNumber>::Empty()));
		Snapshot->SetStringField(TEXT("world_actor_resolution_state"), TEXT("not_resolved_static_scan"));
		Snapshot->SetStringField(TEXT("key_storage"), TEXT("no_keys"));
		Snapshot->SetStringField(TEXT("camera_cut_track_id"), FString());
		Snapshot->SetNumberField(TEXT("spawnable_count"), 0);
		Snapshot->SetNumberField(TEXT("possessable_count"), 0);
		Snapshot->SetNumberField(TEXT("binding_count"), 0);
		Snapshot->SetNumberField(TEXT("track_count"), 0);
		Snapshot->SetNumberField(TEXT("section_count"), 0);
		Snapshot->SetNumberField(TEXT("channel_count"), 0);
		Snapshot->SetNumberField(TEXT("key_count"), 0);
		Snapshot->SetNumberField(TEXT("binding_tag_count"), 0);
		Snapshot->SetNumberField(TEXT("marked_frame_count"), 0);
		Snapshot->SetNumberField(TEXT("root_folder_count"), 0);
		Snapshot->SetNumberField(TEXT("node_group_count"), 0);
		Snapshot->SetArrayField(TEXT("spawnables"), TArray<TSharedPtr<FJsonValue>>());
		Snapshot->SetArrayField(TEXT("possessables"), TArray<TSharedPtr<FJsonValue>>());
		Snapshot->SetArrayField(TEXT("bindings"), TArray<TSharedPtr<FJsonValue>>());
		Snapshot->SetArrayField(TEXT("tracks"), TArray<TSharedPtr<FJsonValue>>());
		Snapshot->SetArrayField(TEXT("sections"), TArray<TSharedPtr<FJsonValue>>());
		Snapshot->SetArrayField(TEXT("binding_tags"), TArray<TSharedPtr<FJsonValue>>());
		Snapshot->SetArrayField(TEXT("marked_frames"), TArray<TSharedPtr<FJsonValue>>());
		AssetEntity.Snapshot = Snapshot;
		return true;
	}

	const FString MovieSceneId = AddMovieSceneEntity(ProjectId, *MovieScene, OutEntities);
	AddCineRelation(
		ProjectId,
		TEXT("contains_movie_scene"),
		SequenceId,
		MovieSceneId,
		LevelSequence->GetPathName(),
		TEXT("LevelSequence owns a MovieScene object."),
		OutRelations);

	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetObjectField(
		TEXT("level_sequence"),
		LevelSequenceSnapshot(ProjectId, *LevelSequence, *MovieScene, SequenceId, MovieSceneId, OutEntities, OutRelations));
	AssetEntity.Snapshot = Snapshot;
	return true;
}
}
