#include "UEPIAudioReader.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/EngineTypes.h"
#include "Misc/PackageName.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundEffectSubmix.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeAttenuation.h"
#include "Sound/SoundNodeBranch.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeModulator.h"
#include "Sound/SoundNodeRandom.h"
#include "Sound/SoundNodeSoundClass.h"
#include "Sound/SoundNodeSwitch.h"
#include "Sound/SoundNodeWaveParam.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundWave.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString AudioBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString ObjectPathOrEmpty(const UObject* Object)
{
	return Object ? Object->GetPathName() : FString();
}

FString ClassPathOrEmpty(const UObject* Object)
{
	return Object && Object->GetClass() ? Object->GetClass()->GetPathName() : FString();
}

FString AudioEnumValueString(const UEnum* Enum, int64 Value)
{
	return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(static_cast<int32>(Value));
}

FString TruncateAudioValue(FString Value)
{
	constexpr int32 MaxLength = 512;
	if (Value.Len() > MaxLength)
	{
		Value.LeftInline(MaxLength);
		Value.Append(TEXT("..."));
	}
	return Value;
}

FString AudioPropertyValue(const UObject& Object, const FProperty& Property)
{
	FString Value;
	const void* ValuePtr = Property.ContainerPtrToValuePtr<void>(&Object);
	Property.ExportTextItem_Direct(Value, ValuePtr, nullptr, const_cast<UObject*>(&Object), PPF_None);
	return TruncateAudioValue(MoveTemp(Value));
}

FString AudioObjectPropertyPath(const UObject& Object, const FName PropertyName)
{
	const FProperty* Property = Object.GetClass() ? Object.GetClass()->FindPropertyByName(PropertyName) : nullptr;
	if (!Property)
	{
		return FString();
	}

	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		return ObjectPathOrEmpty(ObjectProperty->GetObjectPropertyValue_InContainer(&Object));
	}

	if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
	{
		const FSoftObjectPtr* SoftObject = SoftObjectProperty->ContainerPtrToValuePtr<FSoftObjectPtr>(&Object);
		return SoftObject ? SoftObject->ToSoftObjectPath().ToString() : FString();
	}

	return FString();
}

int32 AudioIntProperty(const UObject& Object, const FName PropertyName)
{
	const FProperty* Property = Object.GetClass() ? Object.GetClass()->FindPropertyByName(PropertyName) : nullptr;
	if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
	{
		return NumericProperty->GetSignedIntPropertyValue(NumericProperty->ContainerPtrToValuePtr<void>(&Object));
	}
	return 0;
}

TSharedRef<FJsonValue> AudioNumberValue(float Value)
{
	return MakeShared<FJsonValueNumber>(Value);
}

TArray<TSharedPtr<FJsonValue>> AudioFloatArray(const TArray<float>& Values)
{
	TArray<TSharedPtr<FJsonValue>> JsonValues;
	for (float Value : Values)
	{
		JsonValues.Add(AudioNumberValue(Value));
	}
	return JsonValues;
}

TSharedRef<FJsonObject> AudioNamedPathObject(int32 Index, const FString& Path, const FString& ClassPath = FString())
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("path"), Path);
	Object->SetStringField(TEXT("class_path"), ClassPath);
	return Object;
}

TSharedRef<FJsonObject> AudioColorObject(const FColor& Color)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("r"), Color.R);
	Object->SetNumberField(TEXT("g"), Color.G);
	Object->SetNumberField(TEXT("b"), Color.B);
	Object->SetNumberField(TEXT("a"), Color.A);
	return Object;
}

TArray<TSharedPtr<FJsonValue>> SoundEffectPresetPropertyValues(const USoundEffectSubmixPreset& Preset)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	int32 PropertyIndex = 0;
	for (TFieldIterator<FProperty> PropertyIt(Preset.GetClass(), EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
	{
		const FProperty* Property = *PropertyIt;
		if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			continue;
		}

		TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
		PropertyObject->SetNumberField(TEXT("index"), PropertyIndex++);
		PropertyObject->SetStringField(TEXT("name"), Property->GetName());
		PropertyObject->SetStringField(TEXT("type"), Property->GetCPPType());
		PropertyObject->SetStringField(TEXT("value"), AudioPropertyValue(Preset, *Property));
		Values.Add(MakeShared<FJsonValueObject>(PropertyObject));
	}
	return Values;
}

void AddAudioEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

void AddAudioRelation(
	const FString& ProjectId,
	const FString& Type,
	const FString& FromId,
	const FString& ToId,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FRelationRecord>& OutRelations)
{
	if (FromId.IsEmpty() || ToId.IsEmpty())
	{
		return;
	}

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

FEntityRecord* FindAudioEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

FString AddSoundWaveReferenceEntity(
	const FString& ProjectId,
	const FString& SoundWavePath,
	const FString& SoundWaveClass,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	if (SoundWavePath.IsEmpty())
	{
		return FString();
	}

	const FString SoundWaveId = MakeStableId(ProjectId, TEXT("sound_wave"), SoundWavePath);
	if (FindAudioEntity(OutEntities, SoundWaveId))
	{
		return SoundWaveId;
	}

	FEntityRecord Entity;
	Entity.Id = SoundWaveId;
	Entity.Kind = TEXT("sound_wave");
	Entity.CanonicalKey = SoundWavePath;
	Entity.DisplayName = FPackageName::ObjectPathToObjectName(SoundWavePath);
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("sound_wave_path"), SoundWavePath);
	Entity.Attributes.Add(TEXT("sound_wave_class"), SoundWaveClass);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Registry));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("sound_wave_reference") };
	Entity.Completeness.Omitted = { TEXT("sound_wave_metadata"), TEXT("decoded_pcm") };
	AddAudioEvidence(Entity, EvidencePath, TEXT("SoundWave reference read from an audio asset."));
	OutEntities.Add(MoveTemp(Entity));
	return SoundWaveId;
}

FString AddSoundNodeEntity(
	const FString& ProjectId,
	const USoundCue& Cue,
	const USoundNode& Node,
	const int32 NodeIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString NodePath = Node.GetPathName();
	const FString NodeId = MakeStableId(ProjectId, TEXT("sound_node"), NodePath);
	if (FindAudioEntity(OutEntities, NodeId))
	{
		return NodeId;
	}

	FEntityRecord Entity;
	Entity.Id = NodeId;
	Entity.Kind = TEXT("sound_node");
	Entity.CanonicalKey = NodePath;
	Entity.DisplayName = Node.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("sound_cue_path"), Cue.GetPathName());
	Entity.Attributes.Add(TEXT("node_path"), NodePath);
	Entity.Attributes.Add(TEXT("node_class"), Node.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("node_index"), FString::FromInt(NodeIndex));
	Entity.Attributes.Add(TEXT("child_count"), FString::FromInt(Node.ChildNodes.Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("sound_node_class"), TEXT("sound_node_children"), TEXT("common_sound_node_semantics") };
	Entity.Completeness.Omitted = { TEXT("runtime_parse_state"), TEXT("wave_instances") };
	AddAudioEvidence(Entity, NodePath, TEXT("USoundNode metadata read from SoundCue graph."));
	OutEntities.Add(MoveTemp(Entity));
	return NodeId;
}

FString AddSoundCueEntity(
	const FString& ProjectId,
	const USoundCue& Cue,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CuePath = Cue.GetPathName();
	const FString CueId = MakeStableId(ProjectId, TEXT("sound_cue"), CuePath);
	if (FindAudioEntity(OutEntities, CueId))
	{
		return CueId;
	}

	FEntityRecord Entity;
	Entity.Id = CueId;
	Entity.Kind = TEXT("sound_cue");
	Entity.CanonicalKey = CuePath;
	Entity.DisplayName = Cue.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("sound_cue_path"), CuePath);
	Entity.Attributes.Add(TEXT("sound_cue_class"), Cue.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("duration"), FString::SanitizeFloat(Cue.GetDuration()));
	Entity.Attributes.Add(TEXT("first_node_path"), ObjectPathOrEmpty(Cue.FirstNode));
#if WITH_EDITORONLY_DATA
	Entity.Attributes.Add(TEXT("node_count"), FString::FromInt(Cue.AllNodes.Num()));
	Entity.Attributes.Add(TEXT("has_editor_graph"), AudioBool(Cue.SoundCueGraph != nullptr));
#else
	Entity.Attributes.Add(TEXT("node_count"), Cue.FirstNode ? TEXT("1") : TEXT("0"));
	Entity.Attributes.Add(TEXT("has_editor_graph"), TEXT("false"));
#endif
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("sound_cue_metadata"), TEXT("sound_node_graph"), TEXT("wave_player_references") };
	Entity.Completeness.Omitted = { TEXT("runtime_parameter_values"), TEXT("wave_instance_parse"), TEXT("decoded_pcm") };
	AddAudioEvidence(Entity, CuePath, TEXT("USoundCue graph metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return CueId;
}

FString AddSoundWaveEntity(
	const FString& ProjectId,
	const USoundWave& SoundWave,
	TArray<FEntityRecord>& OutEntities)
{
	const FString SoundWavePath = SoundWave.GetPathName();
	const FString SoundWaveId = MakeStableId(ProjectId, TEXT("sound_wave"), SoundWavePath);
	if (FEntityRecord* Existing = FindAudioEntity(OutEntities, SoundWaveId))
	{
		Existing->Attributes.Add(TEXT("duration"), FString::SanitizeFloat(SoundWave.GetDuration()));
		Existing->Attributes.Add(TEXT("sample_rate"), FString::FromInt(AudioIntProperty(SoundWave, TEXT("SampleRate"))));
		Existing->Attributes.Add(TEXT("num_channels"), FString::FromInt(SoundWave.NumChannels));
		Existing->Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
		Existing->Completeness.State = ECompletenessState::Partial;
		Existing->Completeness.Covered = { TEXT("sound_wave_metadata"), TEXT("cue_points"), TEXT("subtitle_summary") };
		Existing->Completeness.Omitted = { TEXT("decoded_pcm"), TEXT("compressed_payload_bytes"), TEXT("runtime_streaming_state") };
		return SoundWaveId;
	}

	FEntityRecord Entity;
	Entity.Id = SoundWaveId;
	Entity.Kind = TEXT("sound_wave");
	Entity.CanonicalKey = SoundWavePath;
	Entity.DisplayName = SoundWave.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("sound_wave_path"), SoundWavePath);
	Entity.Attributes.Add(TEXT("sound_wave_class"), SoundWave.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("duration"), FString::SanitizeFloat(SoundWave.GetDuration()));
	Entity.Attributes.Add(TEXT("sample_rate"), FString::FromInt(AudioIntProperty(SoundWave, TEXT("SampleRate"))));
	Entity.Attributes.Add(TEXT("num_channels"), FString::FromInt(SoundWave.NumChannels));
	Entity.Attributes.Add(TEXT("cue_point_count"), FString::FromInt(SoundWave.CuePoints.Num()));
	Entity.Attributes.Add(TEXT("subtitle_count"), FString::FromInt(SoundWave.Subtitles.Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("sound_wave_metadata"), TEXT("cue_points"), TEXT("subtitle_summary") };
	Entity.Completeness.Omitted = { TEXT("decoded_pcm"), TEXT("compressed_payload_bytes"), TEXT("runtime_streaming_state") };
	AddAudioEvidence(Entity, SoundWavePath, TEXT("USoundWave static metadata read through public Engine API and UObject reflection."));
	OutEntities.Add(MoveTemp(Entity));
	return SoundWaveId;
}

FString AddSoundSubmixReferenceEntity(
	const FString& ProjectId,
	const USoundSubmixBase* Submix,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	if (!Submix)
	{
		return FString();
	}

	const FString SubmixPath = Submix->GetPathName();
	const FString SubmixId = MakeStableId(ProjectId, TEXT("sound_submix"), SubmixPath);
	if (FindAudioEntity(OutEntities, SubmixId))
	{
		return SubmixId;
	}

	FEntityRecord Entity;
	Entity.Id = SubmixId;
	Entity.Kind = TEXT("sound_submix");
	Entity.CanonicalKey = SubmixPath;
	Entity.DisplayName = Submix->GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("sound_submix_path"), SubmixPath);
	Entity.Attributes.Add(TEXT("sound_submix_class"), Submix->GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Registry));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("sound_submix_reference") };
	Entity.Completeness.Omitted = { TEXT("sound_submix_structure") };
	AddAudioEvidence(Entity, EvidencePath, TEXT("SoundSubmix reference read from a sound submix asset."));
	OutEntities.Add(MoveTemp(Entity));
	return SubmixId;
}

FString AddSoundSubmixEffectEntity(
	const FString& ProjectId,
	const USoundEffectSubmixPreset* Effect,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	if (!Effect)
	{
		return FString();
	}

	const FString EffectPath = Effect->GetPathName();
	const FString EffectId = MakeStableId(ProjectId, TEXT("sound_submix_effect_preset"), EffectPath);
	if (FindAudioEntity(OutEntities, EffectId))
	{
		return EffectId;
	}

	FEntityRecord Entity;
	Entity.Id = EffectId;
	Entity.Kind = TEXT("sound_submix_effect_preset");
	Entity.CanonicalKey = EffectPath;
	Entity.DisplayName = Effect->GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("effect_path"), EffectPath);
	Entity.Attributes.Add(TEXT("effect_class"), Effect->GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Registry));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("sound_submix_effect_reference") };
	Entity.Completeness.Omitted = { TEXT("effect_settings") };
	AddAudioEvidence(Entity, EvidencePath, TEXT("Submix effect preset reference read from SoundSubmix effect chain."));
	OutEntities.Add(MoveTemp(Entity));
	return EffectId;
}

FString AddSoundSubmixEntity(
	const FString& ProjectId,
	const USoundSubmix& Submix,
	TArray<FEntityRecord>& OutEntities)
{
	const FString SubmixPath = Submix.GetPathName();
	const FString SubmixId = MakeStableId(ProjectId, TEXT("sound_submix"), SubmixPath);
	if (FindAudioEntity(OutEntities, SubmixId))
	{
		return SubmixId;
	}

	FEntityRecord Entity;
	Entity.Id = SubmixId;
	Entity.Kind = TEXT("sound_submix");
	Entity.CanonicalKey = SubmixPath;
	Entity.DisplayName = Submix.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("sound_submix_path"), SubmixPath);
	Entity.Attributes.Add(TEXT("sound_submix_class"), Submix.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("parent_submix_path"), ObjectPathOrEmpty(Submix.ParentSubmix));
	Entity.Attributes.Add(TEXT("child_submix_count"), FString::FromInt(Submix.ChildSubmixes.Num()));
	Entity.Attributes.Add(TEXT("effect_count"), FString::FromInt(Submix.SubmixEffectChain.Num()));
	Entity.Attributes.Add(TEXT("auto_disable"), AudioBool(Submix.bAutoDisable));
	Entity.Attributes.Add(TEXT("auto_disable_time"), FString::SanitizeFloat(Submix.AutoDisableTime));
	Entity.Attributes.Add(TEXT("mute_when_backgrounded"), AudioBool(Submix.bMuteWhenBackgrounded));
	Entity.Attributes.Add(TEXT("envelope_follower_attack_time"), FString::FromInt(Submix.EnvelopeFollowerAttackTime));
	Entity.Attributes.Add(TEXT("envelope_follower_release_time"), FString::FromInt(Submix.EnvelopeFollowerReleaseTime));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("sound_submix_metadata"), TEXT("submix_hierarchy"), TEXT("effect_chain_references") };
	Entity.Completeness.Omitted = { TEXT("runtime_audio_buffers"), TEXT("spectral_analysis_state"), TEXT("envelope_runtime_values"), TEXT("effect_settings") };
	AddAudioEvidence(Entity, SubmixPath, TEXT("USoundSubmix metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return SubmixId;
}

TSharedRef<FJsonObject> SoundSubmixReferenceObject(const FString& Id, int32 Index, const USoundSubmixBase* Submix)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), Id);
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("submix_path"), ObjectPathOrEmpty(Submix));
	Object->SetStringField(TEXT("submix_class"), Submix ? Submix->GetClass()->GetPathName() : FString());
	return Object;
}

TSharedRef<FJsonObject> SoundSubmixEffectObject(const FString& Id, int32 Index, const USoundEffectSubmixPreset* Effect)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), Id);
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("effect_path"), ObjectPathOrEmpty(Effect));
	Object->SetStringField(TEXT("effect_class"), Effect ? Effect->GetClass()->GetPathName() : FString());
	return Object;
}

TSharedRef<FJsonObject> SoundCueChildObject(
	const int32 Index,
	const FString& ChildId,
	const USoundNode* Child)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("id"), ChildId);
	Object->SetStringField(TEXT("node_path"), ObjectPathOrEmpty(Child));
	Object->SetStringField(TEXT("node_class"), ClassPathOrEmpty(Child));
	return Object;
}

FString SoundNodeSemanticKind(const USoundNode& Node)
{
	if (Node.IsA<USoundNodeWavePlayer>())
	{
		return TEXT("wave_player");
	}
	if (Node.IsA<USoundNodeMixer>())
	{
		return TEXT("mixer");
	}
	if (Node.IsA<USoundNodeRandom>())
	{
		return TEXT("random");
	}
	if (Node.IsA<USoundNodeModulator>())
	{
		return TEXT("modulator");
	}
	if (Node.IsA<USoundNodeBranch>())
	{
		return TEXT("branch");
	}
	if (Node.IsA<USoundNodeSwitch>())
	{
		return TEXT("switch");
	}
	if (Node.IsA<USoundNodeAttenuation>())
	{
		return TEXT("attenuation");
	}
	if (Node.IsA<USoundNodeWaveParam>())
	{
		return TEXT("wave_param");
	}
	if (Node.IsA<USoundNodeSoundClass>())
	{
		return TEXT("sound_class");
	}
	return TEXT("generic");
}

void AppendSoundNodeSemantics(
	const FString& ProjectId,
	const USoundNode& Node,
	const FString& NodeId,
	TSharedRef<FJsonObject>& Object,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (const USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(&Node))
	{
		const USoundWave* Wave = WavePlayer->GetSoundWave();
		const FString WavePath = Wave ? Wave->GetPathName() : AudioObjectPropertyPath(*WavePlayer, TEXT("SoundWaveAssetPtr"));
		const FString WaveClass = Wave ? ClassPathOrEmpty(Wave) : FString();
		const FString WaveId = AddSoundWaveReferenceEntity(ProjectId, WavePath, WaveClass, Node.GetPathName(), OutEntities);
		AddAudioRelation(ProjectId, TEXT("sound_node_uses_sound_wave"), NodeId, WaveId, Node.GetPathName(), TEXT("SoundNodeWavePlayer references this SoundWave."), OutRelations);
		Object->SetStringField(TEXT("sound_wave_id"), WaveId);
		Object->SetStringField(TEXT("sound_wave_path"), WavePath);
		Object->SetBoolField(TEXT("looping"), WavePlayer->bLooping != 0);
	}
	else if (const USoundNodeMixer* Mixer = Cast<USoundNodeMixer>(&Node))
	{
		Object->SetArrayField(TEXT("input_volumes"), AudioFloatArray(Mixer->InputVolume));
	}
	else if (const USoundNodeRandom* Random = Cast<USoundNodeRandom>(&Node))
	{
		Object->SetArrayField(TEXT("weights"), AudioFloatArray(Random->Weights));
		Object->SetNumberField(TEXT("preselect_at_level_load"), Random->PreselectAtLevelLoad);
		Object->SetBoolField(TEXT("randomize_without_replacement"), Random->bRandomizeWithoutReplacement != 0);
		Object->SetBoolField(TEXT("exclude_from_branch_culling"), Random->bShouldExcludeFromBranchCulling != 0);
	}
	else if (const USoundNodeModulator* Modulator = Cast<USoundNodeModulator>(&Node))
	{
		Object->SetNumberField(TEXT("pitch_min"), Modulator->PitchMin);
		Object->SetNumberField(TEXT("pitch_max"), Modulator->PitchMax);
		Object->SetNumberField(TEXT("volume_min"), Modulator->VolumeMin);
		Object->SetNumberField(TEXT("volume_max"), Modulator->VolumeMax);
	}
	else if (const USoundNodeBranch* Branch = Cast<USoundNodeBranch>(&Node))
	{
		Object->SetStringField(TEXT("bool_parameter_name"), Branch->BoolParameterName.ToString());
	}
	else if (const USoundNodeSwitch* Switch = Cast<USoundNodeSwitch>(&Node))
	{
		Object->SetStringField(TEXT("int_parameter_name"), Switch->IntParameterName.ToString());
	}
	else if (const USoundNodeAttenuation* Attenuation = Cast<USoundNodeAttenuation>(&Node))
	{
		Object->SetStringField(TEXT("attenuation_settings_path"), ObjectPathOrEmpty(Attenuation->AttenuationSettings));
		Object->SetBoolField(TEXT("override_attenuation"), Attenuation->bOverrideAttenuation != 0);
	}
	else if (const USoundNodeWaveParam* WaveParam = Cast<USoundNodeWaveParam>(&Node))
	{
		Object->SetStringField(TEXT("wave_parameter_name"), WaveParam->WaveParameterName.ToString());
	}
	else if (const USoundNodeSoundClass* SoundClass = Cast<USoundNodeSoundClass>(&Node))
	{
		Object->SetStringField(TEXT("sound_class_override_path"), ObjectPathOrEmpty(SoundClass->SoundClassOverride.Get()));
	}
}

TSharedRef<FJsonObject> SoundNodeObject(
	const FString& ProjectId,
	const USoundCue& Cue,
	const USoundNode& Node,
	const FString& NodeId,
	const int32 NodeIndex,
	TMap<const USoundNode*, FString>& NodeIds,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> ChildValues;
	const int32 MaxChildNodes = Node.GetMaxChildNodes();
	for (int32 ChildIndex = 0; ChildIndex < Node.ChildNodes.Num() && ChildIndex < MaxChildNodes; ++ChildIndex)
	{
		const USoundNode* Child = Node.ChildNodes[ChildIndex];
		if (!Child)
		{
			continue;
		}

		FString ChildId = NodeIds.FindRef(Child);
		if (ChildId.IsEmpty())
		{
			ChildId = AddSoundNodeEntity(ProjectId, Cue, *Child, NodeIds.Num(), OutEntities);
			NodeIds.Add(Child, ChildId);
		}
		AddAudioRelation(ProjectId, TEXT("sound_node_child"), NodeId, ChildId, Node.GetPathName(), TEXT("SoundNode child edge."), OutRelations);
		ChildValues.Add(MakeShared<FJsonValueObject>(SoundCueChildObject(ChildIndex, ChildId, Child)));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), NodeId);
	Object->SetNumberField(TEXT("index"), NodeIndex);
	Object->SetStringField(TEXT("node_path"), Node.GetPathName());
	Object->SetStringField(TEXT("node_class"), Node.GetClass()->GetPathName());
	Object->SetStringField(TEXT("semantic_kind"), SoundNodeSemanticKind(Node));
	Object->SetNumberField(TEXT("child_count"), ChildValues.Num());
	Object->SetNumberField(TEXT("max_child_count"), MaxChildNodes);
	Object->SetNumberField(TEXT("min_child_count"), Node.GetMinChildNodes());
	Object->SetNumberField(TEXT("duration"), const_cast<USoundNode&>(Node).GetDuration());
	Object->SetArrayField(TEXT("children"), ChildValues);
#if WITH_EDITORONLY_DATA
	if (const UEdGraphNode* GraphNode = Node.GetGraphNode())
	{
		Object->SetNumberField(TEXT("graph_pos_x"), GraphNode->NodePosX);
		Object->SetNumberField(TEXT("graph_pos_y"), GraphNode->NodePosY);
	}
	else
#endif
	{
		Object->SetNumberField(TEXT("graph_pos_x"), 0);
		Object->SetNumberField(TEXT("graph_pos_y"), 0);
	}

	AppendSoundNodeSemantics(ProjectId, Node, NodeId, Object, OutEntities, OutRelations);
	return Object;
}

void GatherSoundCueNodes(const USoundNode* Node, TArray<const USoundNode*>& OutNodes)
{
	if (!Node || OutNodes.Contains(Node))
	{
		return;
	}
	OutNodes.Add(Node);
	const int32 MaxChildNodes = Node->GetMaxChildNodes();
	for (int32 ChildIndex = 0; ChildIndex < Node->ChildNodes.Num() && ChildIndex < MaxChildNodes; ++ChildIndex)
	{
		GatherSoundCueNodes(Node->ChildNodes[ChildIndex], OutNodes);
	}
}

TSharedRef<FJsonObject> SoundCueSnapshot(
	const FString& ProjectId,
	const USoundCue& Cue,
	const FString& CueId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<const USoundNode*> Nodes;
#if WITH_EDITORONLY_DATA
	for (const USoundNode* Node : Cue.AllNodes)
	{
		GatherSoundCueNodes(Node, Nodes);
	}
#endif
	GatherSoundCueNodes(Cue.FirstNode, Nodes);

	TMap<const USoundNode*, FString> NodeIds;
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		const USoundNode* Node = Nodes[NodeIndex];
		if (!Node)
		{
			continue;
		}
		const FString NodeId = AddSoundNodeEntity(ProjectId, Cue, *Node, NodeIndex, OutEntities);
		NodeIds.Add(Node, NodeId);
		AddAudioRelation(ProjectId, TEXT("contains_sound_node"), CueId, NodeId, Node->GetPathName(), TEXT("SoundCue contains this SoundNode."), OutRelations);
	}

	TArray<TSharedPtr<FJsonValue>> NodeValues;
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		const USoundNode* Node = Nodes[NodeIndex];
		if (!Node)
		{
			continue;
		}
		const FString NodeId = NodeIds.FindRef(Node);
		NodeValues.Add(MakeShared<FJsonValueObject>(SoundNodeObject(ProjectId, Cue, *Node, NodeId, NodeIndex, NodeIds, OutEntities, OutRelations)));
	}

	TArray<TSharedPtr<FJsonValue>> ConcurrencyValues;
	int32 ConcurrencyIndex = 0;
	for (const USoundConcurrency* Concurrency : Cue.ConcurrencySet)
	{
		if (Concurrency)
		{
			ConcurrencyValues.Add(MakeShared<FJsonValueObject>(AudioNamedPathObject(ConcurrencyIndex++, Concurrency->GetPathName(), ClassPathOrEmpty(Concurrency))));
		}
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.sound_cue.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), CueId);
	Object->SetStringField(TEXT("sound_cue_path"), Cue.GetPathName());
	Object->SetStringField(TEXT("sound_cue_class"), Cue.GetClass()->GetPathName());
	Object->SetStringField(TEXT("first_node_id"), Cue.FirstNode ? NodeIds.FindRef(Cue.FirstNode) : FString());
	Object->SetStringField(TEXT("first_node_path"), ObjectPathOrEmpty(Cue.FirstNode));
	Object->SetNumberField(TEXT("duration"), Cue.GetDuration());
	Object->SetNumberField(TEXT("volume_multiplier"), Cue.VolumeMultiplier);
	Object->SetNumberField(TEXT("pitch_multiplier"), Cue.PitchMultiplier);
	Object->SetStringField(TEXT("sound_class_path"), ObjectPathOrEmpty(Cue.SoundClassObject.Get()));
	Object->SetStringField(TEXT("attenuation_settings_path"), ObjectPathOrEmpty(Cue.AttenuationSettings));
	Object->SetStringField(TEXT("sound_submix_path"), ObjectPathOrEmpty(Cue.SoundSubmixObject));
	Object->SetStringField(TEXT("source_effect_chain_path"), ObjectPathOrEmpty(Cue.SourceEffectChain));
	Object->SetBoolField(TEXT("override_attenuation"), Cue.bOverrideAttenuation != 0);
	Object->SetBoolField(TEXT("override_concurrency"), Cue.bOverrideConcurrency != 0);
	Object->SetBoolField(TEXT("prime_on_load"), Cue.bPrimeOnLoad != 0);
#if WITH_EDITORONLY_DATA
	Object->SetBoolField(TEXT("has_editor_graph"), Cue.SoundCueGraph != nullptr);
#else
	Object->SetBoolField(TEXT("has_editor_graph"), false);
#endif
	Object->SetNumberField(TEXT("node_count"), NodeValues.Num());
	Object->SetNumberField(TEXT("concurrency_count"), ConcurrencyValues.Num());
	Object->SetArrayField(TEXT("nodes"), NodeValues);
	Object->SetArrayField(TEXT("concurrency"), ConcurrencyValues);
	return Object;
}

TSharedRef<FJsonObject> SoundWaveCuePointObject(const FSoundWaveCuePoint& CuePoint, const int32 Index)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetNumberField(TEXT("cue_point_id"), CuePoint.CuePointID);
	Object->SetStringField(TEXT("label"), CuePoint.Label);
	Object->SetNumberField(TEXT("frame_position"), CuePoint.FramePosition);
	Object->SetNumberField(TEXT("frame_length"), CuePoint.FrameLength);
	return Object;
}

TSharedRef<FJsonObject> SoundWaveSubtitleObject(const FSubtitleCue& Subtitle, const int32 Index)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetNumberField(TEXT("time"), Subtitle.Time);
	Object->SetStringField(TEXT("text"), Subtitle.Text.ToString());
	return Object;
}

TSharedRef<FJsonObject> SoundWaveSnapshot(const USoundWave& SoundWave, const FString& SoundWaveId)
{
	TArray<TSharedPtr<FJsonValue>> CuePointValues;
	for (int32 CuePointIndex = 0; CuePointIndex < SoundWave.CuePoints.Num(); ++CuePointIndex)
	{
		CuePointValues.Add(MakeShared<FJsonValueObject>(SoundWaveCuePointObject(SoundWave.CuePoints[CuePointIndex], CuePointIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> SubtitleValues;
	for (int32 SubtitleIndex = 0; SubtitleIndex < SoundWave.Subtitles.Num(); ++SubtitleIndex)
	{
		SubtitleValues.Add(MakeShared<FJsonValueObject>(SoundWaveSubtitleObject(SoundWave.Subtitles[SubtitleIndex], SubtitleIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> ConcurrencyValues;
	int32 ConcurrencyIndex = 0;
	for (const USoundConcurrency* Concurrency : SoundWave.ConcurrencySet)
	{
		if (Concurrency)
		{
			ConcurrencyValues.Add(MakeShared<FJsonValueObject>(AudioNamedPathObject(ConcurrencyIndex++, Concurrency->GetPathName(), ClassPathOrEmpty(Concurrency))));
		}
	}

	const UEnum* CompressionEnum = StaticEnum<ESoundAssetCompressionType>();
	const UEnum* LoadingEnum = StaticEnum<ESoundWaveLoadingBehavior>();

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.sound_wave.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), SoundWaveId);
	Object->SetStringField(TEXT("sound_wave_path"), SoundWave.GetPathName());
	Object->SetStringField(TEXT("sound_wave_class"), SoundWave.GetClass()->GetPathName());
	Object->SetNumberField(TEXT("duration"), SoundWave.GetDuration());
	Object->SetNumberField(TEXT("sample_rate"), AudioIntProperty(SoundWave, TEXT("SampleRate")));
	Object->SetNumberField(TEXT("imported_sample_rate"), AudioIntProperty(SoundWave, TEXT("ImportedSampleRate")));
	Object->SetNumberField(TEXT("num_channels"), SoundWave.NumChannels);
	Object->SetNumberField(TEXT("volume"), SoundWave.Volume);
	Object->SetNumberField(TEXT("pitch"), SoundWave.Pitch);
	Object->SetNumberField(TEXT("compression_quality"), SoundWave.GetCompressionQuality());
	Object->SetStringField(TEXT("compression_type"), AudioEnumValueString(CompressionEnum, static_cast<int64>(SoundWave.GetSoundAssetCompressionTypeEnum())));
	Object->SetStringField(TEXT("loading_behavior"), AudioEnumValueString(LoadingEnum, static_cast<int64>(SoundWave.GetLoadingBehavior(false))));
	Object->SetBoolField(TEXT("streaming"), SoundWave.bStreaming != 0);
	Object->SetBoolField(TEXT("looping"), SoundWave.IsLooping());
	Object->SetBoolField(TEXT("mature"), SoundWave.bMature != 0);
	Object->SetBoolField(TEXT("manual_word_wrap"), SoundWave.bManualWordWrap != 0);
	Object->SetBoolField(TEXT("single_line"), SoundWave.bSingleLine != 0);
	Object->SetBoolField(TEXT("is_ambisonics"), SoundWave.bIsAmbisonics != 0);
	Object->SetStringField(TEXT("sound_class_path"), ObjectPathOrEmpty(SoundWave.SoundClassObject.Get()));
	Object->SetStringField(TEXT("attenuation_settings_path"), ObjectPathOrEmpty(SoundWave.AttenuationSettings));
	Object->SetStringField(TEXT("sound_submix_path"), ObjectPathOrEmpty(SoundWave.SoundSubmixObject));
	Object->SetStringField(TEXT("source_effect_chain_path"), ObjectPathOrEmpty(SoundWave.SourceEffectChain));
	Object->SetNumberField(TEXT("cue_point_count"), CuePointValues.Num());
	Object->SetNumberField(TEXT("subtitle_count"), SubtitleValues.Num());
	Object->SetNumberField(TEXT("concurrency_count"), ConcurrencyValues.Num());
	Object->SetArrayField(TEXT("cue_points"), CuePointValues);
	Object->SetArrayField(TEXT("subtitles"), SubtitleValues);
	Object->SetArrayField(TEXT("concurrency"), ConcurrencyValues);
	return Object;
}

TSharedRef<FJsonObject> SoundSubmixEffectPresetSnapshot(const USoundEffectSubmixPreset& Preset)
{
	TArray<TSharedPtr<FJsonValue>> PropertyValues = SoundEffectPresetPropertyValues(Preset);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.sound_submix_effect_preset.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("effect_path"), Preset.GetPathName());
	Object->SetStringField(TEXT("effect_class"), Preset.GetClass()->GetPathName());
	Object->SetObjectField(TEXT("preset_color"), AudioColorObject(Preset.GetPresetColor()));
	Object->SetNumberField(TEXT("setting_property_count"), PropertyValues.Num());
	Object->SetArrayField(TEXT("setting_properties"), PropertyValues);
	return Object;
}

TSharedRef<FJsonObject> SoundSubmixSnapshot(
	const FString& ProjectId,
	const USoundSubmix& Submix,
	const FString& SubmixId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> ChildValues;
	for (int32 ChildIndex = 0; ChildIndex < Submix.ChildSubmixes.Num(); ++ChildIndex)
	{
		const USoundSubmixBase* Child = Submix.ChildSubmixes[ChildIndex];
		const FString ChildId = AddSoundSubmixReferenceEntity(ProjectId, Child, Submix.GetPathName(), OutEntities);
		AddAudioRelation(ProjectId, TEXT("contains_child_submix"), SubmixId, ChildId, Submix.GetPathName(), TEXT("SoundSubmix lists this child submix."), OutRelations);
		ChildValues.Add(MakeShared<FJsonValueObject>(SoundSubmixReferenceObject(ChildId, ChildIndex, Child)));
	}

	TArray<TSharedPtr<FJsonValue>> EffectValues;
	for (int32 EffectIndex = 0; EffectIndex < Submix.SubmixEffectChain.Num(); ++EffectIndex)
	{
		const USoundEffectSubmixPreset* Effect = Submix.SubmixEffectChain[EffectIndex];
		const FString EffectId = AddSoundSubmixEffectEntity(ProjectId, Effect, Submix.GetPathName(), OutEntities);
		AddAudioRelation(ProjectId, TEXT("uses_sound_submix_effect"), SubmixId, EffectId, Submix.GetPathName(), TEXT("SoundSubmix effect chain references this effect preset."), OutRelations);
		EffectValues.Add(MakeShared<FJsonValueObject>(SoundSubmixEffectObject(EffectId, EffectIndex, Effect)));
	}

	const FString ParentSubmixId = AddSoundSubmixReferenceEntity(ProjectId, Submix.ParentSubmix, Submix.GetPathName(), OutEntities);
	AddAudioRelation(ProjectId, TEXT("parent_submix"), SubmixId, ParentSubmixId, Submix.GetPathName(), TEXT("SoundSubmix references this parent submix."), OutRelations);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.sound_submix.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("sound_submix_path"), Submix.GetPathName());
	Object->SetStringField(TEXT("sound_submix_class"), Submix.GetClass()->GetPathName());
	Object->SetStringField(TEXT("parent_submix_id"), ParentSubmixId);
	Object->SetStringField(TEXT("parent_submix_path"), ObjectPathOrEmpty(Submix.ParentSubmix));
	Object->SetStringField(TEXT("ambisonics_plugin_settings_path"), ObjectPathOrEmpty(Submix.AmbisonicsPluginSettings));
	Object->SetBoolField(TEXT("auto_disable"), Submix.bAutoDisable);
	Object->SetNumberField(TEXT("auto_disable_time"), Submix.AutoDisableTime);
	Object->SetBoolField(TEXT("mute_when_backgrounded"), Submix.bMuteWhenBackgrounded);
	Object->SetNumberField(TEXT("envelope_follower_attack_time"), Submix.EnvelopeFollowerAttackTime);
	Object->SetNumberField(TEXT("envelope_follower_release_time"), Submix.EnvelopeFollowerReleaseTime);
#if WITH_EDITORONLY_DATA
	Object->SetBoolField(TEXT("has_editor_graph"), Submix.SoundSubmixGraph != nullptr);
#else
	Object->SetBoolField(TEXT("has_editor_graph"), false);
#endif
	Object->SetNumberField(TEXT("child_submix_count"), ChildValues.Num());
	Object->SetNumberField(TEXT("effect_count"), EffectValues.Num());
	Object->SetArrayField(TEXT("child_submixes"), ChildValues);
	Object->SetArrayField(TEXT("effects"), EffectValues);
	return Object;
}
}

bool FAudioReader::AppendAudioAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (USoundCue* Cue = Cast<USoundCue>(&Asset))
	{
		const FString CueId = AddSoundCueEntity(ProjectId, *Cue, OutEntities);
		AddAudioRelation(ProjectId, TEXT("contains_sound_cue"), AssetEntity.Id, CueId, Cue->GetPathName(), TEXT("SoundCue asset contains the extracted sound cue graph."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("sound_cue"), SoundCueSnapshot(ProjectId, *Cue, CueId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("sound_cue_duration"), FString::SanitizeFloat(Cue->GetDuration()));
		AssetEntity.Attributes.Add(TEXT("sound_cue_first_node_path"), ObjectPathOrEmpty(Cue->FirstNode));
#if WITH_EDITORONLY_DATA
		AssetEntity.Attributes.Add(TEXT("sound_cue_node_count"), FString::FromInt(Cue->AllNodes.Num()));
#endif
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("sound_cue_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("sound_node_graph"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("wave_player_references"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_parameter_values"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("wave_instance_parse"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Cue->GetPathName(),
			TEXT("USoundCue graph metadata extracted through public Engine API.")
		});
		return true;
	}

	if (USoundWave* SoundWave = Cast<USoundWave>(&Asset))
	{
		const FString SoundWaveId = AddSoundWaveEntity(ProjectId, *SoundWave, OutEntities);
		AddAudioRelation(ProjectId, TEXT("contains_sound_wave"), AssetEntity.Id, SoundWaveId, SoundWave->GetPathName(), TEXT("SoundWave asset contains the extracted sound wave metadata."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("sound_wave"), SoundWaveSnapshot(*SoundWave, SoundWaveId));
		AssetEntity.Attributes.Add(TEXT("sound_wave_duration"), FString::SanitizeFloat(SoundWave->GetDuration()));
		AssetEntity.Attributes.Add(TEXT("sound_wave_sample_rate"), FString::FromInt(AudioIntProperty(*SoundWave, TEXT("SampleRate"))));
		AssetEntity.Attributes.Add(TEXT("sound_wave_num_channels"), FString::FromInt(SoundWave->NumChannels));
		AssetEntity.Attributes.Add(TEXT("sound_wave_cue_point_count"), FString::FromInt(SoundWave->CuePoints.Num()));
		AssetEntity.Attributes.Add(TEXT("sound_wave_subtitle_count"), FString::FromInt(SoundWave->Subtitles.Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("sound_wave_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("cue_points"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("subtitle_summary"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("decoded_pcm"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("compressed_payload_bytes"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			SoundWave->GetPathName(),
			TEXT("USoundWave static metadata extracted without decoding PCM.")
		});
		return true;
	}

	if (USoundSubmix* Submix = Cast<USoundSubmix>(&Asset))
	{
		const FString SubmixId = AddSoundSubmixEntity(ProjectId, *Submix, OutEntities);
		AddAudioRelation(ProjectId, TEXT("contains_sound_submix"), AssetEntity.Id, SubmixId, Submix->GetPathName(), TEXT("SoundSubmix asset contains the extracted sound submix record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("sound_submix"), SoundSubmixSnapshot(ProjectId, *Submix, SubmixId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("sound_submix_child_count"), FString::FromInt(Submix->ChildSubmixes.Num()));
		AssetEntity.Attributes.Add(TEXT("sound_submix_effect_count"), FString::FromInt(Submix->SubmixEffectChain.Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("sound_submix_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("sound_submix_hierarchy"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("sound_submix_effect_chain"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_audio_buffers"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("effect_settings"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Submix->GetPathName(),
			TEXT("USoundSubmix metadata extracted through public Engine API.")
		});
		return true;
	}

	if (USoundEffectSubmixPreset* Preset = Cast<USoundEffectSubmixPreset>(&Asset))
	{
		const TArray<TSharedPtr<FJsonValue>> PropertyValues = SoundEffectPresetPropertyValues(*Preset);
		const FString PresetId = AddSoundSubmixEffectEntity(ProjectId, Preset, Preset->GetPathName(), OutEntities);
		if (FEntityRecord* PresetEntity = FindAudioEntity(OutEntities, PresetId))
		{
			PresetEntity->Attributes.Add(TEXT("setting_property_count"), FString::FromInt(PropertyValues.Num()));
			PresetEntity->Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
			PresetEntity->Completeness.State = ECompletenessState::Partial;
			PresetEntity->Completeness.Covered = { TEXT("sound_submix_effect_preset_metadata"), TEXT("editable_setting_summary") };
			PresetEntity->Completeness.Omitted = { TEXT("runtime_effect_instances"), TEXT("audio_processing_state"), TEXT("full_nested_setting_schema") };
		}
		AddAudioRelation(ProjectId, TEXT("contains_sound_submix_effect"), AssetEntity.Id, PresetId, Preset->GetPathName(), TEXT("SoundSubmix effect preset asset contains the extracted preset record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("sound_submix_effect_preset"), SoundSubmixEffectPresetSnapshot(*Preset));
		AssetEntity.Attributes.Add(TEXT("sound_submix_effect_setting_property_count"), FString::FromInt(PropertyValues.Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("sound_submix_effect_preset_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("sound_submix_effect_setting_summary"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_effect_instances"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("audio_processing_state"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Preset->GetPathName(),
			TEXT("USoundEffectSubmixPreset metadata extracted through public Engine API and UObject reflection.")
		});
		return true;
	}

	return false;
}
}
