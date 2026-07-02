#include "UEPIAnimationReader.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimLinkableElement.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "ReferenceSkeleton.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#if UEPI_WITH_IK_RIG
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigSolver.h"
#endif

namespace UE::ProjectIntelligence
{
namespace
{
FString AnimNumber(double Value)
{
	return FString::SanitizeFloat(Value);
}

FString AnimBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString AnimFrameRateString(const FFrameRate& FrameRate)
{
	return FString::Printf(TEXT("%d/%d"), FrameRate.Numerator, FrameRate.Denominator);
}

double AnimFrameSeconds(const FFrameRate& FrameRate, int32 FrameNumber)
{
	if (FrameRate.Numerator <= 0 || FrameRate.Denominator <= 0)
	{
		return 0.0;
	}
	return static_cast<double>(FrameNumber) * static_cast<double>(FrameRate.Denominator) / static_cast<double>(FrameRate.Numerator);
}

FString AnimEnumValue(const UEnum* Enum, int64 Value)
{
	if (Enum)
	{
		return Enum->GetNameStringByValue(Value);
	}
	return FString::FromInt(static_cast<int32>(Value));
}

TSharedRef<FJsonObject> AnimVectorObject(const FVector& Vector)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Vector.X);
	Object->SetNumberField(TEXT("y"), Vector.Y);
	Object->SetNumberField(TEXT("z"), Vector.Z);
	return Object;
}

TSharedRef<FJsonObject> AnimQuatObject(const FQuat& Quat)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Quat.X);
	Object->SetNumberField(TEXT("y"), Quat.Y);
	Object->SetNumberField(TEXT("z"), Quat.Z);
	Object->SetNumberField(TEXT("w"), Quat.W);
	return Object;
}

TSharedRef<FJsonObject> AnimRotatorObject(const FRotator& Rotator)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("pitch"), Rotator.Pitch);
	Object->SetNumberField(TEXT("yaw"), Rotator.Yaw);
	Object->SetNumberField(TEXT("roll"), Rotator.Roll);
	return Object;
}

TSharedRef<FJsonObject> AnimTransformObject(const FTransform& Transform)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("translation"), AnimVectorObject(Transform.GetTranslation()));
	Object->SetObjectField(TEXT("rotation"), AnimQuatObject(Transform.GetRotation()));
	Object->SetObjectField(TEXT("scale3d"), AnimVectorObject(Transform.GetScale3D()));
	return Object;
}

struct FAnimTrackMotionMetrics
{
	FName BoneName;
	int32 TrackIndex = 0;
	int32 KeyCount = 0;
	double TranslationRange = 0.0;
	double TranslationMaxDelta = 0.0;
	double TranslationStepMaxDelta = 0.0;
	double RotationRangeDegrees = 0.0;
	double RotationStepMaxDeltaDegrees = 0.0;
	double ScaleRange = 0.0;
	double ScaleMaxDelta = 0.0;
	double ScaleStepMaxDelta = 0.0;
	bool bTranslationChanges = false;
	bool bRotationChanges = false;
	bool bScaleChanges = false;

	bool Changes() const
	{
		return bTranslationChanges || bRotationChanges || bScaleChanges;
	}

	double Score() const
	{
		return TranslationMaxDelta + (RotationRangeDegrees * 0.01) + ScaleMaxDelta;
	}
};

void ExpandVectorBounds(FVector& MinValue, FVector& MaxValue, const FVector& Value)
{
	MinValue.X = FMath::Min(MinValue.X, Value.X);
	MinValue.Y = FMath::Min(MinValue.Y, Value.Y);
	MinValue.Z = FMath::Min(MinValue.Z, Value.Z);
	MaxValue.X = FMath::Max(MaxValue.X, Value.X);
	MaxValue.Y = FMath::Max(MaxValue.Y, Value.Y);
	MaxValue.Z = FMath::Max(MaxValue.Z, Value.Z);
}

double QuatAngularDistanceDegrees(const FQuat& Left, const FQuat& Right)
{
	const FQuat NormalizedLeft = Left.GetNormalized();
	const FQuat NormalizedRight = Right.GetNormalized();
	const double Dot = FMath::Clamp(FMath::Abs(
		NormalizedLeft.X * NormalizedRight.X +
		NormalizedLeft.Y * NormalizedRight.Y +
		NormalizedLeft.Z * NormalizedRight.Z +
		NormalizedLeft.W * NormalizedRight.W), 0.0, 1.0);
	return FMath::RadiansToDegrees(2.0 * FMath::Acos(Dot));
}

FAnimTrackMotionMetrics CalculateTrackMotionMetrics(
	const FName& BoneName,
	int32 TrackIndex,
	const TArray<FTransform>& RawLocalTransforms)
{
	FAnimTrackMotionMetrics Metrics;
	Metrics.BoneName = BoneName;
	Metrics.TrackIndex = TrackIndex;
	Metrics.KeyCount = RawLocalTransforms.Num();
	if (RawLocalTransforms.Num() == 0)
	{
		return Metrics;
	}

	const FTransform& FirstTransform = RawLocalTransforms[0];
	const FVector FirstTranslation = FirstTransform.GetTranslation();
	const FQuat FirstRotation = FirstTransform.GetRotation();
	const FVector FirstScale = FirstTransform.GetScale3D();
	FVector MinTranslation = FirstTranslation;
	FVector MaxTranslation = FirstTranslation;
	FVector MinScale = FirstScale;
	FVector MaxScale = FirstScale;
	FTransform PreviousTransform = FirstTransform;

	for (int32 KeyIndex = 0; KeyIndex < RawLocalTransforms.Num(); ++KeyIndex)
	{
		const FTransform& Transform = RawLocalTransforms[KeyIndex];
		const FVector Translation = Transform.GetTranslation();
		const FQuat Rotation = Transform.GetRotation();
		const FVector Scale = Transform.GetScale3D();
		ExpandVectorBounds(MinTranslation, MaxTranslation, Translation);
		ExpandVectorBounds(MinScale, MaxScale, Scale);
		Metrics.TranslationMaxDelta = FMath::Max(Metrics.TranslationMaxDelta, (Translation - FirstTranslation).Size());
		Metrics.RotationRangeDegrees = FMath::Max(Metrics.RotationRangeDegrees, QuatAngularDistanceDegrees(FirstRotation, Rotation));
		Metrics.ScaleMaxDelta = FMath::Max(Metrics.ScaleMaxDelta, (Scale - FirstScale).Size());
		if (KeyIndex > 0)
		{
			Metrics.TranslationStepMaxDelta = FMath::Max(Metrics.TranslationStepMaxDelta, (Translation - PreviousTransform.GetTranslation()).Size());
			Metrics.RotationStepMaxDeltaDegrees = FMath::Max(Metrics.RotationStepMaxDeltaDegrees, QuatAngularDistanceDegrees(PreviousTransform.GetRotation(), Rotation));
			Metrics.ScaleStepMaxDelta = FMath::Max(Metrics.ScaleStepMaxDelta, (Scale - PreviousTransform.GetScale3D()).Size());
		}
		PreviousTransform = Transform;
	}

	Metrics.TranslationRange = (MaxTranslation - MinTranslation).Size();
	Metrics.ScaleRange = (MaxScale - MinScale).Size();
	Metrics.bTranslationChanges = Metrics.TranslationRange > 0.001 || Metrics.TranslationMaxDelta > 0.001;
	Metrics.bRotationChanges = Metrics.RotationRangeDegrees > 0.01;
	Metrics.bScaleChanges = Metrics.ScaleRange > 0.001 || Metrics.ScaleMaxDelta > 0.001;
	return Metrics;
}

TSharedRef<FJsonObject> AnimTrackMotionObject(const FAnimTrackMotionMetrics& Metrics)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("bone_name"), Metrics.BoneName.ToString());
	Object->SetNumberField(TEXT("track_index"), Metrics.TrackIndex);
	Object->SetNumberField(TEXT("raw_local_key_count"), Metrics.KeyCount);
	Object->SetBoolField(TEXT("changes"), Metrics.Changes());
	Object->SetBoolField(TEXT("translation_changes"), Metrics.bTranslationChanges);
	Object->SetBoolField(TEXT("rotation_changes"), Metrics.bRotationChanges);
	Object->SetBoolField(TEXT("scale_changes"), Metrics.bScaleChanges);
	Object->SetNumberField(TEXT("translation_range"), Metrics.TranslationRange);
	Object->SetNumberField(TEXT("translation_max_delta"), Metrics.TranslationMaxDelta);
	Object->SetNumberField(TEXT("translation_step_max_delta"), Metrics.TranslationStepMaxDelta);
	Object->SetNumberField(TEXT("rotation_range_degrees"), Metrics.RotationRangeDegrees);
	Object->SetNumberField(TEXT("rotation_step_max_delta_degrees"), Metrics.RotationStepMaxDeltaDegrees);
	Object->SetNumberField(TEXT("scale_range"), Metrics.ScaleRange);
	Object->SetNumberField(TEXT("scale_max_delta"), Metrics.ScaleMaxDelta);
	Object->SetNumberField(TEXT("scale_step_max_delta"), Metrics.ScaleStepMaxDelta);

	TArray<TSharedPtr<FJsonValue>> ChannelValues;
	if (Metrics.bTranslationChanges)
	{
		ChannelValues.Add(MakeShared<FJsonValueString>(TEXT("translation")));
	}
	if (Metrics.bRotationChanges)
	{
		ChannelValues.Add(MakeShared<FJsonValueString>(TEXT("rotation")));
	}
	if (Metrics.bScaleChanges)
	{
		ChannelValues.Add(MakeShared<FJsonValueString>(TEXT("scale")));
	}
	Object->SetArrayField(TEXT("change_channels"), MoveTemp(ChannelValues));
	return Object;
}

TSharedRef<FJsonObject> AnimationMotionSummaryObject(
	const UAnimSequence& Sequence,
	const TArray<FAnimTrackMotionMetrics>& MotionMetrics)
{
	TArray<FAnimTrackMotionMetrics> ChangingMetrics;
	for (const FAnimTrackMotionMetrics& Metrics : MotionMetrics)
	{
		if (Metrics.Changes())
		{
			ChangingMetrics.Add(Metrics);
		}
	}
	ChangingMetrics.Sort([](const FAnimTrackMotionMetrics& Left, const FAnimTrackMotionMetrics& Right)
	{
		return Left.Score() > Right.Score();
	});

	TArray<TSharedPtr<FJsonValue>> ChangingBoneValues;
	for (const FAnimTrackMotionMetrics& Metrics : ChangingMetrics)
	{
		ChangingBoneValues.Add(MakeShared<FJsonValueObject>(AnimTrackMotionObject(Metrics)));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.animation_motion_summary.v1"));
	Object->SetStringField(TEXT("source"), TEXT("raw_local_all_keys"));
	Object->SetStringField(TEXT("sequence_path"), Sequence.GetPathName());
	Object->SetNumberField(TEXT("track_count"), MotionMetrics.Num());
	Object->SetNumberField(TEXT("changing_bone_count"), ChangingMetrics.Num());
	Object->SetNumberField(TEXT("static_bone_count"), FMath::Max(MotionMetrics.Num() - ChangingMetrics.Num(), 0));
	Object->SetArrayField(TEXT("changing_bones"), MoveTemp(ChangingBoneValues));
	return Object;
}

TSharedRef<FJsonObject> AnimBoundsObject(const FBoxSphereBounds& Bounds)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("origin"), AnimVectorObject(Bounds.Origin));
	Object->SetObjectField(TEXT("box_extent"), AnimVectorObject(Bounds.BoxExtent));
	Object->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);
	return Object;
}

void AddAnimEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::AnimationDataModel),
		Path,
		Detail
	});
}

void AddAnimRelation(
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
	Relation.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Relation.bDerived = false;
	Relation.Confidence = 1.0f;
	Relation.Evidence.Add({
		LexToString(ESourceLayer::AnimationDataModel),
		EvidencePath,
		EvidenceDetail
	});
	OutRelations.Add(MoveTemp(Relation));
}

FEntityRecord* FindEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

FString BoneKey(const FString& OwnerPath, int32 BoneIndex, const FName& BoneName)
{
	return FString::Printf(TEXT("%s:bone:%d:%s"), *OwnerPath, BoneIndex, *BoneName.ToString());
}

FString VirtualBoneKey(const FString& OwnerPath, int32 VirtualBoneIndex, const FName& VirtualBoneName)
{
	return FString::Printf(TEXT("%s:virtual_bone:%d:%s"), *OwnerPath, VirtualBoneIndex, *VirtualBoneName.ToString());
}

FString SocketKey(const FString& OwnerPath, const FString& SourceKind, int32 SocketIndex, const FName& SocketName)
{
	return FString::Printf(TEXT("%s:socket:%s:%d:%s"), *OwnerPath, *SourceKind, SocketIndex, *SocketName.ToString());
}

TArray<FTransform> BuildComponentSpacePose(const FReferenceSkeleton& RefSkeleton, const TArray<FTransform>& LocalPoses)
{
	TArray<FTransform> ComponentPoses;
	ComponentPoses.SetNum(RefSkeleton.GetNum());

	for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
	{
		const FTransform LocalTransform = LocalPoses.IsValidIndex(BoneIndex) ? LocalPoses[BoneIndex] : FTransform::Identity;
		const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
		if (RefSkeleton.IsValidIndex(ParentIndex) && ComponentPoses.IsValidIndex(ParentIndex))
		{
			ComponentPoses[BoneIndex] = LocalTransform * ComponentPoses[ParentIndex];
		}
		else
		{
			ComponentPoses[BoneIndex] = LocalTransform;
		}
	}

	return ComponentPoses;
}

TArray<FTransform> BuildComponentSpaceRefPose(const FReferenceSkeleton& RefSkeleton)
{
	return BuildComponentSpacePose(RefSkeleton, RefSkeleton.GetRefBonePose());
}

TArray<int32> AnimSequenceSampleFrames(const IAnimationDataModel& DataModel)
{
	TArray<int32> FrameNumbers;
	const int32 LastFrame = FMath::Max(0, DataModel.GetNumberOfFrames());
	FrameNumbers.AddUnique(0);
	if (LastFrame > 0)
	{
		FrameNumbers.AddUnique(LastFrame / 2);
		FrameNumbers.AddUnique(LastFrame);
	}
	return FrameNumbers;
}

TArray<int32> AnimSequenceProfileSampleFrames(const IAnimationDataModel& DataModel)
{
	TArray<int32> FrameNumbers;
	const int32 LastFrame = FMath::Max(0, DataModel.GetNumberOfFrames());
	const int32 MaxProfileSamples = 17;
	const int32 DesiredSampleCount = FMath::Clamp(LastFrame + 1, 1, MaxProfileSamples);
	for (int32 SampleIndex = 0; SampleIndex < DesiredSampleCount; ++SampleIndex)
	{
		const double Alpha = DesiredSampleCount <= 1
			? 0.0
			: static_cast<double>(SampleIndex) / static_cast<double>(DesiredSampleCount - 1);
		FrameNumbers.AddUnique(FMath::Clamp(FMath::RoundToInt(static_cast<double>(LastFrame) * Alpha), 0, LastFrame));
	}
	FrameNumbers.AddUnique(0);
	FrameNumbers.AddUnique(LastFrame);
	FrameNumbers.Sort();
	return FrameNumbers;
}

FString AnimDominantAxis(const FVector& Delta)
{
	const FVector Abs(FMath::Abs(Delta.X), FMath::Abs(Delta.Y), FMath::Abs(Delta.Z));
	if (Abs.X < 0.001 && Abs.Y < 0.001 && Abs.Z < 0.001)
	{
		return TEXT("none");
	}
	if (Abs.X >= Abs.Y && Abs.X >= Abs.Z)
	{
		return Delta.X >= 0.0 ? TEXT("+X") : TEXT("-X");
	}
	if (Abs.Y >= Abs.X && Abs.Y >= Abs.Z)
	{
		return Delta.Y >= 0.0 ? TEXT("+Y") : TEXT("-Y");
	}
	return Delta.Z >= 0.0 ? TEXT("+Z") : TEXT("-Z");
}

TSharedRef<FJsonObject> AnimVectorDeltaObject(const FVector& Delta)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("vector"), AnimVectorObject(Delta));
	Object->SetNumberField(TEXT("length"), Delta.Size());
	Object->SetStringField(TEXT("dominant_axis"), AnimDominantAxis(Delta));
	return Object;
}

TSharedRef<FJsonObject> AnimTransformForLLMObject(const FTransform& Transform)
{
	TSharedRef<FJsonObject> Object = AnimTransformObject(Transform);
	Object->SetObjectField(TEXT("rotation_euler_degrees"), AnimRotatorObject(Transform.Rotator()));
	return Object;
}

TSharedRef<FJsonObject> AnimTransformEntryForLLMJson(
	int32 Index,
	const FName& BoneName,
	const FTransform& LocalTransform,
	const FTransform& ComponentTransform)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("bone_name"), BoneName.ToString());
	Object->SetObjectField(TEXT("local_transform"), AnimTransformForLLMObject(LocalTransform));
	Object->SetObjectField(TEXT("component_transform"), AnimTransformForLLMObject(ComponentTransform));
	return Object;
}

TSharedRef<FJsonObject> AnimMotionSampleJson(
	int32 FrameNumber,
	double TimeSeconds,
	double NormalizedTime,
	const FTransform& LocalTransform,
	const FTransform& ComponentTransform,
	const FTransform& InitialLocalTransform,
	const FTransform& InitialComponentTransform)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("frame_number"), FrameNumber);
	Object->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	Object->SetNumberField(TEXT("normalized_time"), NormalizedTime);
	Object->SetObjectField(TEXT("local_transform"), AnimTransformForLLMObject(LocalTransform));
	Object->SetObjectField(TEXT("component_transform"), AnimTransformForLLMObject(ComponentTransform));
	Object->SetObjectField(TEXT("local_translation_delta_from_initial"), AnimVectorDeltaObject(LocalTransform.GetTranslation() - InitialLocalTransform.GetTranslation()));
	Object->SetObjectField(TEXT("component_translation_delta_from_initial"), AnimVectorDeltaObject(ComponentTransform.GetTranslation() - InitialComponentTransform.GetTranslation()));
	Object->SetNumberField(TEXT("local_rotation_delta_degrees_from_initial"), QuatAngularDistanceDegrees(InitialLocalTransform.GetRotation(), LocalTransform.GetRotation()));
	Object->SetNumberField(TEXT("component_rotation_delta_degrees_from_initial"), QuatAngularDistanceDegrees(InitialComponentTransform.GetRotation(), ComponentTransform.GetRotation()));
	return Object;
}

TSharedRef<FJsonObject> AnimVectorBoundsObject(const FVector& MinValue, const FVector& MaxValue)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("min"), AnimVectorObject(MinValue));
	Object->SetObjectField(TEXT("max"), AnimVectorObject(MaxValue));
	Object->SetObjectField(TEXT("extent"), AnimVectorObject(MaxValue - MinValue));
	Object->SetNumberField(TEXT("diagonal_length"), (MaxValue - MinValue).Size());
	return Object;
}

TArray<FTransform> BuildLocalBonePoseForFrame(
	const IAnimationDataModel& DataModel,
	const TArray<FName>& TrackNames,
	const FReferenceSkeleton& RefSkeleton,
	int32 FrameNumber)
{
	TArray<FTransform> TrackTransforms;
	if (TrackNames.Num() > 0)
	{
		DataModel.GetBoneTracksTransform(TrackNames, FFrameNumber(FrameNumber), TrackTransforms);
	}

	TArray<FTransform> LocalBonePoses = RefSkeleton.GetRefBonePose();
	const int32 ExistingPoseCount = LocalBonePoses.Num();
	LocalBonePoses.SetNum(RefSkeleton.GetNum());
	for (int32 BoneIndex = ExistingPoseCount; BoneIndex < LocalBonePoses.Num(); ++BoneIndex)
	{
		LocalBonePoses[BoneIndex] = FTransform::Identity;
	}

	for (int32 TrackIndex = 0; TrackIndex < TrackNames.Num(); ++TrackIndex)
	{
		const int32 BoneIndex = RefSkeleton.FindBoneIndex(TrackNames[TrackIndex]);
		if (RefSkeleton.IsValidIndex(BoneIndex) && TrackTransforms.IsValidIndex(TrackIndex))
		{
			LocalBonePoses[BoneIndex] = TrackTransforms[TrackIndex];
		}
	}

	return LocalBonePoses;
}

struct FChangedBoneProfile
{
	double Score = 0.0;
	double DriverScore = 0.0;
	double InheritedScore = 0.0;
	int32 BoneIndex = INDEX_NONE;
	int32 ParentIndex = INDEX_NONE;
	FName BoneName;
	FName ParentName;
	bool bPositionChanges = false;
	bool bDirectLocalMotion = false;
	bool bInheritedMotion = false;
	FString MotionRole;
	FString GenerationPriority;
	TSharedPtr<FJsonObject> Object;
};

struct FAnimMotionIntentGroupSummary
{
	FString Id;
	FString Label;
	FString Description;
	int32 DriverCount = 0;
	int32 InheritedMotionCount = 0;
	double DriverScore = 0.0;
	double ComponentMotionScore = 0.0;
	TArray<FString> DominantDriverBones;
	TArray<FString> DominantInheritedBones;
};

FString AnimationBoneMotionArtifactRoot()
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UEProjectIntelligence"),
		TEXT("store"),
		TEXT("artifacts"),
		TEXT("animation_bone_motion"));
}

FString AnimBoneIntentGroupId(const FName& BoneName)
{
	const FString LowerName = BoneName.ToString().ToLower();
	const bool bRight = LowerName.EndsWith(TEXT("_r"));
	const bool bLeft = LowerName.EndsWith(TEXT("_l"));
	const bool bArm = LowerName.Contains(TEXT("clavicle")) || LowerName.Contains(TEXT("upperarm")) || LowerName.Contains(TEXT("lowerarm")) || LowerName.Contains(TEXT("hand")) || LowerName.Contains(TEXT("thumb")) || LowerName.Contains(TEXT("index")) || LowerName.Contains(TEXT("middle")) || LowerName.Contains(TEXT("ring")) || LowerName.Contains(TEXT("pinky"));
	const bool bLeg = LowerName.Contains(TEXT("thigh")) || LowerName.Contains(TEXT("calf")) || LowerName.Contains(TEXT("foot")) || LowerName.Contains(TEXT("ball")) || LowerName.Contains(TEXT("toe"));
	if (bArm && bRight)
	{
		return TEXT("right_arm_hand");
	}
	if (bArm && bLeft)
	{
		return TEXT("left_arm_hand");
	}
	if (bLeg && bRight)
	{
		return TEXT("right_leg_foot");
	}
	if (bLeg && bLeft)
	{
		return TEXT("left_leg_foot");
	}
	if (LowerName.Contains(TEXT("spine")) || LowerName.Contains(TEXT("pelvis")) || LowerName.Contains(TEXT("neck")) || LowerName.Contains(TEXT("head")) || LowerName == TEXT("root"))
	{
		return TEXT("body_spine_head");
	}
	return TEXT("other");
}

FString AnimMotionGroupLabel(const FString& GroupId)
{
	if (GroupId == TEXT("right_arm_hand"))
	{
		return TEXT("Right arm and hand");
	}
	if (GroupId == TEXT("left_arm_hand"))
	{
		return TEXT("Left arm and hand");
	}
	if (GroupId == TEXT("right_leg_foot"))
	{
		return TEXT("Right leg and foot");
	}
	if (GroupId == TEXT("left_leg_foot"))
	{
		return TEXT("Left leg and foot");
	}
	if (GroupId == TEXT("body_spine_head"))
	{
		return TEXT("Body, spine, neck, and head");
	}
	return TEXT("Other bones");
}

FString AnimMotionGroupDescription(const FString& GroupId)
{
	if (GroupId == TEXT("right_arm_hand"))
	{
		return TEXT("Use these bones first for right-sided arm, hand, finger, or gesture motion.");
	}
	if (GroupId == TEXT("left_arm_hand"))
	{
		return TEXT("Use these bones first for left-sided arm, hand, finger, or gesture motion.");
	}
	if (GroupId == TEXT("right_leg_foot"))
	{
		return TEXT("Use these bones for right-sided stance, step, and foot placement changes.");
	}
	if (GroupId == TEXT("left_leg_foot"))
	{
		return TEXT("Use these bones for left-sided stance, step, and foot placement changes.");
	}
	if (GroupId == TEXT("body_spine_head"))
	{
		return TEXT("Use these bones for posture, balance, torso, neck, and head motion.");
	}
	return TEXT("Use these bones only after reviewing their parent chain and motion role.");
}

FAnimMotionIntentGroupSummary& FindOrAddMotionIntentGroup(TArray<FAnimMotionIntentGroupSummary>& Groups, const FString& GroupId)
{
	for (FAnimMotionIntentGroupSummary& Group : Groups)
	{
		if (Group.Id == GroupId)
		{
			return Group;
		}
	}

	FAnimMotionIntentGroupSummary NewGroup;
	NewGroup.Id = GroupId;
	NewGroup.Label = AnimMotionGroupLabel(GroupId);
	NewGroup.Description = AnimMotionGroupDescription(GroupId);
	Groups.Add(MoveTemp(NewGroup));
	return Groups.Last();
}

TArray<TSharedPtr<FJsonValue>> AnimStringArrayValues(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> JsonValues;
	for (const FString& Value : Values)
	{
		JsonValues.Add(MakeShared<FJsonValueString>(Value));
	}
	return JsonValues;
}

TArray<TSharedPtr<FJsonValue>> AnimBoneChainValues(const FReferenceSkeleton& RefSkeleton, int32 BoneIndex)
{
	TArray<int32> Chain;
	int32 CurrentIndex = BoneIndex;
	while (RefSkeleton.IsValidIndex(CurrentIndex) && Chain.Num() < 32)
	{
		Chain.Add(CurrentIndex);
		CurrentIndex = RefSkeleton.GetParentIndex(CurrentIndex);
	}

	TArray<TSharedPtr<FJsonValue>> ChainValues;
	for (int32 ChainIndex = Chain.Num() - 1; ChainIndex >= 0; --ChainIndex)
	{
		const int32 ChainBoneIndex = Chain[ChainIndex];
		TSharedRef<FJsonObject> BoneObject = MakeShared<FJsonObject>();
		BoneObject->SetNumberField(TEXT("bone_index"), ChainBoneIndex);
		BoneObject->SetStringField(TEXT("bone_name"), RefSkeleton.GetBoneName(ChainBoneIndex).ToString());
		ChainValues.Add(MakeShared<FJsonValueObject>(BoneObject));
	}
	return ChainValues;
}

TSharedRef<FJsonObject> AnimCompactMotionBoneObject(const FReferenceSkeleton& RefSkeleton, const FChangedBoneProfile& Changed, int32 Rank)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("rank"), Rank);
	Object->SetNumberField(TEXT("bone_index"), Changed.BoneIndex);
	Object->SetStringField(TEXT("bone_name"), Changed.BoneName.ToString());
	Object->SetNumberField(TEXT("parent_index"), Changed.ParentIndex);
	Object->SetStringField(TEXT("parent_name"), Changed.ParentName.ToString());
	Object->SetStringField(TEXT("motion_role"), Changed.MotionRole);
	Object->SetStringField(TEXT("generation_priority"), Changed.GenerationPriority);
	Object->SetNumberField(TEXT("driver_score"), Changed.DriverScore);
	Object->SetNumberField(TEXT("component_motion_score"), Changed.Score);
	Object->SetNumberField(TEXT("inherited_motion_score"), Changed.InheritedScore);
	Object->SetBoolField(TEXT("position_changes"), Changed.bPositionChanges);
	Object->SetBoolField(TEXT("direct_local_motion"), Changed.bDirectLocalMotion);
	Object->SetBoolField(TEXT("inherited_component_motion"), Changed.bInheritedMotion);
	Object->SetStringField(TEXT("intent_group"), AnimBoneIntentGroupId(Changed.BoneName));
	Object->SetArrayField(TEXT("skeleton_chain"), AnimBoneChainValues(RefSkeleton, Changed.BoneIndex));
	if (Changed.Object.IsValid())
	{
		Object->SetNumberField(TEXT("component_translation_range"), Changed.Object->GetNumberField(TEXT("component_translation_range")));
		Object->SetNumberField(TEXT("component_path_length"), Changed.Object->GetNumberField(TEXT("component_path_length")));
		Object->SetNumberField(TEXT("component_rotation_range_degrees"), Changed.Object->GetNumberField(TEXT("component_rotation_range_degrees")));
		Object->SetNumberField(TEXT("local_translation_range"), Changed.Object->GetNumberField(TEXT("local_translation_range")));
		Object->SetNumberField(TEXT("local_rotation_range_degrees"), Changed.Object->GetNumberField(TEXT("local_rotation_range_degrees")));
	}
	return Object;
}

TArray<TSharedPtr<FJsonValue>> AnimMotionIntentGroupValues(TArray<FAnimMotionIntentGroupSummary>& Groups)
{
	Groups.Sort([](const FAnimMotionIntentGroupSummary& Left, const FAnimMotionIntentGroupSummary& Right)
	{
		const double LeftScore = Left.DriverScore + (Left.ComponentMotionScore * 0.01);
		const double RightScore = Right.DriverScore + (Right.ComponentMotionScore * 0.01);
		return LeftScore > RightScore;
	});

	TArray<TSharedPtr<FJsonValue>> GroupValues;
	for (const FAnimMotionIntentGroupSummary& Group : Groups)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("id"), Group.Id);
		Object->SetStringField(TEXT("label"), Group.Label);
		Object->SetStringField(TEXT("description"), Group.Description);
		Object->SetNumberField(TEXT("driver_count"), Group.DriverCount);
		Object->SetNumberField(TEXT("inherited_motion_count"), Group.InheritedMotionCount);
		Object->SetNumberField(TEXT("driver_score"), Group.DriverScore);
		Object->SetNumberField(TEXT("component_motion_score"), Group.ComponentMotionScore);
		Object->SetArrayField(TEXT("dominant_driver_bones"), AnimStringArrayValues(Group.DominantDriverBones));
		Object->SetArrayField(TEXT("dominant_inherited_bones"), AnimStringArrayValues(Group.DominantInheritedBones));
		GroupValues.Add(MakeShared<FJsonValueObject>(Object));
	}
	return GroupValues;
}

TSharedRef<FJsonObject> AnimationBoneMotionProfileObject(
	const UAnimSequence& Sequence,
	const TArray<FName>& TrackNames,
	const TArray<FAnimTrackMotionMetrics>& MotionMetrics)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.animation_bone_motion_profile.v1"));
	Object->SetStringField(TEXT("intended_reader"), TEXT("LLM"));
	Object->SetStringField(TEXT("coordinate_space_note"), TEXT("local_transform is parent-relative; component_transform is skeleton component-space in Unreal centimeters."));
	Object->SetStringField(TEXT("sequence_path"), Sequence.GetPathName());
	Object->SetStringField(TEXT("sequence_name"), Sequence.GetName());
	Object->SetStringField(TEXT("skeleton_path"), Sequence.GetSkeleton() ? Sequence.GetSkeleton()->GetPathName() : FString());
	Object->SetNumberField(TEXT("play_length_seconds"), Sequence.GetPlayLength());
	Object->SetStringField(TEXT("sampling_frame_rate"), AnimFrameRateString(Sequence.GetSamplingFrameRate()));
	Object->SetNumberField(TEXT("sample_key_count"), Sequence.GetNumberOfSampledKeys());
	Object->SetNumberField(TEXT("bone_track_count"), TrackNames.Num());

	const IAnimationDataModel* DataModel = Sequence.GetDataModel();
	const USkeleton* Skeleton = Sequence.GetSkeleton();
	if (!DataModel || !Skeleton)
	{
		Object->SetStringField(TEXT("analysis_state"), TEXT("unavailable"));
		Object->SetStringField(TEXT("unavailable_reason"), !DataModel ? TEXT("missing_animation_data_model") : TEXT("missing_skeleton"));
		Object->SetNumberField(TEXT("analysis_sample_count"), 0);
		Object->SetNumberField(TEXT("bone_count"), 0);
		Object->SetNumberField(TEXT("changed_bone_count"), 0);
		Object->SetNumberField(TEXT("position_changed_bone_count"), 0);
		Object->SetNumberField(TEXT("driver_bone_count"), 0);
		Object->SetNumberField(TEXT("inherited_motion_bone_count"), 0);
		return Object;
	}

	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
	const FFrameRate DataFrameRate = DataModel->GetFrameRate();
	const TArray<int32> ProfileFrames = AnimSequenceProfileSampleFrames(*DataModel);
	const double PlayLength = FMath::Max(Sequence.GetPlayLength(), 0.0f);

	TMap<FName, int32> TrackIndexByName;
	for (int32 TrackIndex = 0; TrackIndex < TrackNames.Num(); ++TrackIndex)
	{
		TrackIndexByName.Add(TrackNames[TrackIndex], TrackIndex);
	}

	TMap<FName, FAnimTrackMotionMetrics> MotionMetricsByName;
	for (const FAnimTrackMotionMetrics& Metrics : MotionMetrics)
	{
		MotionMetricsByName.Add(Metrics.BoneName, Metrics);
	}

	TArray<TArray<FTransform>> LocalPoseSamples;
	TArray<TArray<FTransform>> ComponentPoseSamples;
	TArray<double> SampleTimes;
	for (const int32 FrameNumber : ProfileFrames)
	{
		TArray<FTransform> LocalPose = BuildLocalBonePoseForFrame(*DataModel, TrackNames, RefSkeleton, FrameNumber);
		TArray<FTransform> ComponentPose = BuildComponentSpacePose(RefSkeleton, LocalPose);
		LocalPoseSamples.Add(MoveTemp(LocalPose));
		ComponentPoseSamples.Add(MoveTemp(ComponentPose));
		SampleTimes.Add(AnimFrameSeconds(DataFrameRate, FrameNumber));
	}

	TArray<TSharedPtr<FJsonValue>> SampleFrameValues;
	for (int32 SampleIndex = 0; SampleIndex < ProfileFrames.Num(); ++SampleIndex)
	{
		TSharedRef<FJsonObject> SampleObject = MakeShared<FJsonObject>();
		SampleObject->SetNumberField(TEXT("index"), SampleIndex);
		SampleObject->SetNumberField(TEXT("frame_number"), ProfileFrames[SampleIndex]);
		SampleObject->SetNumberField(TEXT("time_seconds"), SampleTimes.IsValidIndex(SampleIndex) ? SampleTimes[SampleIndex] : 0.0);
		SampleObject->SetNumberField(TEXT("normalized_time"), PlayLength > 0.0 ? (SampleTimes.IsValidIndex(SampleIndex) ? SampleTimes[SampleIndex] / PlayLength : 0.0) : 0.0);
		SampleFrameValues.Add(MakeShared<FJsonValueObject>(SampleObject));
	}

	TArray<TSharedPtr<FJsonValue>> InitialPoseValues;
	TArray<TSharedPtr<FJsonValue>> EndPoseValues;
	TArray<FChangedBoneProfile> ChangedBoneProfiles;
	int32 PositionChangedBoneCount = 0;
	constexpr double PositionThreshold = 0.05;
	constexpr double RotationThresholdDegrees = 0.05;

	for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
	{
		const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
		const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
		const FName ParentName = RefSkeleton.IsValidIndex(ParentIndex) ? RefSkeleton.GetBoneName(ParentIndex) : NAME_None;
		const FTransform InitialLocal = LocalPoseSamples.Num() > 0 && LocalPoseSamples[0].IsValidIndex(BoneIndex) ? LocalPoseSamples[0][BoneIndex] : FTransform::Identity;
		const FTransform InitialComponent = ComponentPoseSamples.Num() > 0 && ComponentPoseSamples[0].IsValidIndex(BoneIndex) ? ComponentPoseSamples[0][BoneIndex] : InitialLocal;
		const FTransform EndLocal = LocalPoseSamples.Num() > 0 && LocalPoseSamples.Last().IsValidIndex(BoneIndex) ? LocalPoseSamples.Last()[BoneIndex] : InitialLocal;
		const FTransform EndComponent = ComponentPoseSamples.Num() > 0 && ComponentPoseSamples.Last().IsValidIndex(BoneIndex) ? ComponentPoseSamples.Last()[BoneIndex] : InitialComponent;

		InitialPoseValues.Add(MakeShared<FJsonValueObject>(AnimTransformEntryForLLMJson(BoneIndex, BoneName, InitialLocal, InitialComponent)));
		EndPoseValues.Add(MakeShared<FJsonValueObject>(AnimTransformEntryForLLMJson(BoneIndex, BoneName, EndLocal, EndComponent)));

		FVector ComponentMin = InitialComponent.GetTranslation();
		FVector ComponentMax = InitialComponent.GetTranslation();
		FVector LocalMin = InitialLocal.GetTranslation();
		FVector LocalMax = InitialLocal.GetTranslation();
		double ComponentPathLength = 0.0;
		double ComponentMaxStep = 0.0;
		double ComponentRotationRange = 0.0;
		double LocalRotationRange = 0.0;
		FTransform PreviousComponent = InitialComponent;

		TArray<TSharedPtr<FJsonValue>> BoneSampleValues;
		for (int32 SampleIndex = 0; SampleIndex < ProfileFrames.Num(); ++SampleIndex)
		{
			const FTransform LocalTransform = LocalPoseSamples.IsValidIndex(SampleIndex) && LocalPoseSamples[SampleIndex].IsValidIndex(BoneIndex)
				? LocalPoseSamples[SampleIndex][BoneIndex]
				: InitialLocal;
			const FTransform ComponentTransform = ComponentPoseSamples.IsValidIndex(SampleIndex) && ComponentPoseSamples[SampleIndex].IsValidIndex(BoneIndex)
				? ComponentPoseSamples[SampleIndex][BoneIndex]
				: InitialComponent;
			ExpandVectorBounds(ComponentMin, ComponentMax, ComponentTransform.GetTranslation());
			ExpandVectorBounds(LocalMin, LocalMax, LocalTransform.GetTranslation());
			ComponentRotationRange = FMath::Max(ComponentRotationRange, QuatAngularDistanceDegrees(InitialComponent.GetRotation(), ComponentTransform.GetRotation()));
			LocalRotationRange = FMath::Max(LocalRotationRange, QuatAngularDistanceDegrees(InitialLocal.GetRotation(), LocalTransform.GetRotation()));
			if (SampleIndex > 0)
			{
				const double StepDistance = (ComponentTransform.GetTranslation() - PreviousComponent.GetTranslation()).Size();
				ComponentPathLength += StepDistance;
				ComponentMaxStep = FMath::Max(ComponentMaxStep, StepDistance);
			}
			PreviousComponent = ComponentTransform;
			const double TimeSeconds = SampleTimes.IsValidIndex(SampleIndex) ? SampleTimes[SampleIndex] : 0.0;
			BoneSampleValues.Add(MakeShared<FJsonValueObject>(AnimMotionSampleJson(
				ProfileFrames[SampleIndex],
				TimeSeconds,
				PlayLength > 0.0 ? TimeSeconds / PlayLength : 0.0,
				LocalTransform,
				ComponentTransform,
				InitialLocal,
				InitialComponent)));
		}

		const FVector ComponentDisplacement = EndComponent.GetTranslation() - InitialComponent.GetTranslation();
		const double ComponentTranslationRange = (ComponentMax - ComponentMin).Size();
		const double ComponentDisplacementLength = ComponentDisplacement.Size();
		const double LocalTranslationRange = (LocalMax - LocalMin).Size();
		const bool bPositionChanges = ComponentTranslationRange > PositionThreshold || ComponentDisplacementLength > PositionThreshold;
		const bool bRotationChanges = ComponentRotationRange > RotationThresholdDegrees || LocalRotationRange > RotationThresholdDegrees;
		const FAnimTrackMotionMetrics* DirectMotion = MotionMetricsByName.Find(BoneName);
		const bool bDirectTrackChanges = DirectMotion && DirectMotion->Changes();
		const double DirectTranslationRange = DirectMotion ? DirectMotion->TranslationRange : 0.0;
		const double DirectRotationRange = DirectMotion ? DirectMotion->RotationRangeDegrees : 0.0;
		const double DirectScaleRange = DirectMotion ? DirectMotion->ScaleRange : 0.0;
		const double DriverRotationRange = FMath::Max(LocalRotationRange, DirectRotationRange);
		const double DriverScore = DirectTranslationRange + LocalTranslationRange + DriverRotationRange + (DirectScaleRange * 10.0);
		const double ComponentScore = ComponentPathLength + ComponentTranslationRange + (ComponentRotationRange * 0.01);
		const bool bDirectLocalMotion = bDirectTrackChanges && DriverScore > 0.05;
		const bool bInheritedMotion = bPositionChanges && !bDirectLocalMotion;
		const TCHAR* MotionRole = bDirectLocalMotion ? TEXT("direct_driver") : (bInheritedMotion ? TEXT("inherited_component_motion") : TEXT("minor_pose_adjustment"));
		const TCHAR* GenerationPriority = DriverScore >= 30.0 ? TEXT("primary_driver") : (DriverScore >= 5.0 ? TEXT("secondary_driver") : (bInheritedMotion ? TEXT("inherited_follow_through") : TEXT("low_priority_pose_detail")));
		if (!bPositionChanges && !bRotationChanges && !bDirectTrackChanges)
		{
			continue;
		}

		if (bPositionChanges)
		{
			++PositionChangedBoneCount;
		}

		TArray<TSharedPtr<FJsonValue>> ChangeChannelValues;
		if (bPositionChanges)
		{
			ChangeChannelValues.Add(MakeShared<FJsonValueString>(TEXT("component_translation")));
		}
		if (bRotationChanges)
		{
			ChangeChannelValues.Add(MakeShared<FJsonValueString>(TEXT("component_rotation")));
		}
		if (DirectMotion && DirectMotion->bTranslationChanges)
		{
			ChangeChannelValues.Add(MakeShared<FJsonValueString>(TEXT("local_translation_track")));
		}
		if (DirectMotion && DirectMotion->bRotationChanges)
		{
			ChangeChannelValues.Add(MakeShared<FJsonValueString>(TEXT("local_rotation_track")));
		}
		if (DirectMotion && DirectMotion->bScaleChanges)
		{
			ChangeChannelValues.Add(MakeShared<FJsonValueString>(TEXT("local_scale_track")));
		}

		TSharedRef<FJsonObject> BoneObject = MakeShared<FJsonObject>();
		BoneObject->SetNumberField(TEXT("bone_index"), BoneIndex);
		BoneObject->SetStringField(TEXT("bone_name"), BoneName.ToString());
		BoneObject->SetNumberField(TEXT("parent_index"), ParentIndex);
		BoneObject->SetStringField(TEXT("parent_name"), ParentName.ToString());
		const int32* DirectTrackIndex = TrackIndexByName.Find(BoneName);
		BoneObject->SetBoolField(TEXT("has_direct_track"), DirectTrackIndex != nullptr);
		BoneObject->SetNumberField(TEXT("track_index"), DirectTrackIndex ? *DirectTrackIndex : -1);
		BoneObject->SetBoolField(TEXT("position_changes"), bPositionChanges);
		BoneObject->SetBoolField(TEXT("rotation_changes"), bRotationChanges);
		BoneObject->SetStringField(TEXT("motion_role"), MotionRole);
		BoneObject->SetStringField(TEXT("generation_priority"), GenerationPriority);
		BoneObject->SetNumberField(TEXT("driver_score"), DriverScore);
		BoneObject->SetNumberField(TEXT("component_motion_score"), ComponentScore);
		BoneObject->SetNumberField(TEXT("inherited_motion_score"), bInheritedMotion ? ComponentScore : 0.0);
		BoneObject->SetArrayField(TEXT("change_channels"), ChangeChannelValues);
		BoneObject->SetNumberField(TEXT("component_translation_range"), ComponentTranslationRange);
		BoneObject->SetNumberField(TEXT("component_displacement_length"), ComponentDisplacementLength);
		BoneObject->SetNumberField(TEXT("component_path_length"), ComponentPathLength);
		BoneObject->SetNumberField(TEXT("component_max_step"), ComponentMaxStep);
		BoneObject->SetNumberField(TEXT("component_rotation_range_degrees"), ComponentRotationRange);
		BoneObject->SetNumberField(TEXT("local_translation_range"), LocalTranslationRange);
		BoneObject->SetNumberField(TEXT("local_rotation_range_degrees"), LocalRotationRange);
		BoneObject->SetObjectField(TEXT("component_translation_bounds"), AnimVectorBoundsObject(ComponentMin, ComponentMax));
		BoneObject->SetObjectField(TEXT("component_translation_delta_start_to_end"), AnimVectorDeltaObject(ComponentDisplacement));
		BoneObject->SetObjectField(TEXT("initial_local_transform"), AnimTransformForLLMObject(InitialLocal));
		BoneObject->SetObjectField(TEXT("end_local_transform"), AnimTransformForLLMObject(EndLocal));
		BoneObject->SetObjectField(TEXT("initial_component_transform"), AnimTransformForLLMObject(InitialComponent));
		BoneObject->SetObjectField(TEXT("end_component_transform"), AnimTransformForLLMObject(EndComponent));
		BoneObject->SetNumberField(TEXT("sample_count"), BoneSampleValues.Num());
		BoneObject->SetArrayField(TEXT("samples"), BoneSampleValues);
		BoneObject->SetStringField(
			TEXT("llm_summary"),
			FString::Printf(
				TEXT("%s moves %.3f cm end-to-start toward %s, spans %.3f cm across sampled component-space positions, and rotates %.3f degrees in component space."),
				*BoneName.ToString(),
				ComponentDisplacementLength,
				*AnimDominantAxis(ComponentDisplacement),
				ComponentTranslationRange,
				ComponentRotationRange));

		FChangedBoneProfile Changed;
		Changed.Score = ComponentScore;
		Changed.DriverScore = DriverScore;
		Changed.InheritedScore = bInheritedMotion ? ComponentScore : 0.0;
		Changed.BoneIndex = BoneIndex;
		Changed.ParentIndex = ParentIndex;
		Changed.BoneName = BoneName;
		Changed.ParentName = ParentName;
		Changed.bPositionChanges = bPositionChanges;
		Changed.bDirectLocalMotion = bDirectLocalMotion;
		Changed.bInheritedMotion = bInheritedMotion;
		Changed.MotionRole = MotionRole;
		Changed.GenerationPriority = GenerationPriority;
		Changed.Object = BoneObject;
		ChangedBoneProfiles.Add(MoveTemp(Changed));
	}

	ChangedBoneProfiles.Sort([](const FChangedBoneProfile& Left, const FChangedBoneProfile& Right)
	{
		return Left.Score > Right.Score;
	});

	TArray<TSharedPtr<FJsonValue>> ChangedBoneValues;
	for (const FChangedBoneProfile& Changed : ChangedBoneProfiles)
	{
		if (Changed.Object.IsValid())
		{
			ChangedBoneValues.Add(MakeShared<FJsonValueObject>(Changed.Object.ToSharedRef()));
		}
	}

	TArray<FChangedBoneProfile> DriverBoneProfiles;
	TArray<FChangedBoneProfile> InheritedMotionProfiles;
	for (const FChangedBoneProfile& Changed : ChangedBoneProfiles)
	{
		if (Changed.bDirectLocalMotion)
		{
			DriverBoneProfiles.Add(Changed);
		}
		else if (Changed.bInheritedMotion)
		{
			InheritedMotionProfiles.Add(Changed);
		}
	}

	DriverBoneProfiles.Sort([](const FChangedBoneProfile& Left, const FChangedBoneProfile& Right)
	{
		if (!FMath::IsNearlyEqual(Left.DriverScore, Right.DriverScore))
		{
			return Left.DriverScore > Right.DriverScore;
		}
		return Left.Score > Right.Score;
	});

	InheritedMotionProfiles.Sort([](const FChangedBoneProfile& Left, const FChangedBoneProfile& Right)
	{
		return Left.InheritedScore > Right.InheritedScore;
	});

	TArray<TSharedPtr<FJsonValue>> DriverBoneValues;
	for (int32 Index = 0; Index < DriverBoneProfiles.Num(); ++Index)
	{
		DriverBoneValues.Add(MakeShared<FJsonValueObject>(AnimCompactMotionBoneObject(RefSkeleton, DriverBoneProfiles[Index], Index)));
	}

	TArray<TSharedPtr<FJsonValue>> InheritedMotionValues;
	for (int32 Index = 0; Index < InheritedMotionProfiles.Num(); ++Index)
	{
		InheritedMotionValues.Add(MakeShared<FJsonValueObject>(AnimCompactMotionBoneObject(RefSkeleton, InheritedMotionProfiles[Index], Index)));
	}

	TArray<FAnimMotionIntentGroupSummary> IntentGroups;
	for (const FChangedBoneProfile& Changed : DriverBoneProfiles)
	{
		FAnimMotionIntentGroupSummary& Group = FindOrAddMotionIntentGroup(IntentGroups, AnimBoneIntentGroupId(Changed.BoneName));
		++Group.DriverCount;
		Group.DriverScore += Changed.DriverScore;
		Group.ComponentMotionScore += Changed.Score;
		if (Group.DominantDriverBones.Num() < 8)
		{
			Group.DominantDriverBones.Add(Changed.BoneName.ToString());
		}
	}
	for (const FChangedBoneProfile& Changed : InheritedMotionProfiles)
	{
		FAnimMotionIntentGroupSummary& Group = FindOrAddMotionIntentGroup(IntentGroups, AnimBoneIntentGroupId(Changed.BoneName));
		++Group.InheritedMotionCount;
		Group.ComponentMotionScore += Changed.Score;
		if (Group.DominantInheritedBones.Num() < 8)
		{
			Group.DominantInheritedBones.Add(Changed.BoneName.ToString());
		}
	}

	TArray<FString> PrimaryDriverNames;
	for (int32 Index = 0; Index < FMath::Min(DriverBoneProfiles.Num(), 6); ++Index)
	{
		PrimaryDriverNames.Add(DriverBoneProfiles[Index].BoneName.ToString());
	}

	TArray<TSharedPtr<FJsonValue>> GuidelineValues;
	GuidelineValues.Add(MakeShared<FJsonValueString>(TEXT("Use initial_pose and end_pose to anchor the animation on this skeleton.")));
	GuidelineValues.Add(MakeShared<FJsonValueString>(TEXT("Start procedural generation from driver_bones; they are locally keyed controls sorted by local motion strength.")));
	GuidelineValues.Add(MakeShared<FJsonValueString>(TEXT("Use inherited_motion_bones as follow-through or end-effector evidence, not as the first set of bones to key.")));
	GuidelineValues.Add(MakeShared<FJsonValueString>(TEXT("Use changed_bones samples as sparse keyframes; interpolate transforms between normalized_time values.")));
	GuidelineValues.Add(MakeShared<FJsonValueString>(TEXT("Prefer component_translation for human-readable motion intent, and local_transform for programmatic bone animation output.")));

	Object->SetStringField(TEXT("analysis_state"), TEXT("ready"));
	Object->SetStringField(TEXT("skeleton_key"), Skeleton->GetPathName());
	Object->SetNumberField(TEXT("frame_count"), DataModel->GetNumberOfFrames());
	Object->SetNumberField(TEXT("data_model_key_count"), DataModel->GetNumberOfKeys());
	Object->SetNumberField(TEXT("analysis_sample_count"), ProfileFrames.Num());
	Object->SetNumberField(TEXT("bone_count"), RefSkeleton.GetNum());
	Object->SetNumberField(TEXT("changed_bone_count"), ChangedBoneValues.Num());
	Object->SetNumberField(TEXT("position_changed_bone_count"), PositionChangedBoneCount);
	Object->SetNumberField(TEXT("driver_bone_count"), DriverBoneValues.Num());
	Object->SetNumberField(TEXT("inherited_motion_bone_count"), InheritedMotionValues.Num());
	Object->SetStringField(TEXT("motion_intent_summary"), PrimaryDriverNames.Num() > 0
		? FString::Printf(TEXT("Primary local driver bones: %s. Use these before inherited end-effectors when generating procedural animation."), *FString::Join(PrimaryDriverNames, TEXT(", ")))
		: TEXT("No strong local driver bones were detected; use changed_bones as sparse sampled evidence."));
	Object->SetArrayField(TEXT("sample_frames"), SampleFrameValues);
	Object->SetArrayField(TEXT("initial_pose"), InitialPoseValues);
	Object->SetArrayField(TEXT("end_pose"), EndPoseValues);
	Object->SetArrayField(TEXT("changed_bones"), ChangedBoneValues);
	Object->SetArrayField(TEXT("driver_bones"), DriverBoneValues);
	Object->SetArrayField(TEXT("inherited_motion_bones"), InheritedMotionValues);
	Object->SetArrayField(TEXT("motion_intent_groups"), AnimMotionIntentGroupValues(IntentGroups));
	Object->SetArrayField(TEXT("llm_generation_guidelines"), GuidelineValues);
	return Object;
}

TSharedRef<FJsonObject> WriteAnimationBoneMotionProfileArtifact(
	const FString& ProjectId,
	const UAnimSequence& Sequence,
	const TArray<FName>& TrackNames,
	const TArray<FAnimTrackMotionMetrics>& MotionMetrics)
{
	const FString ArtifactId = MakeStableId(ProjectId, TEXT("animation_bone_motion_profile"), Sequence.GetPathName());
	const FString ArtifactDir = AnimationBoneMotionArtifactRoot();
	const FString ArtifactPath = FPaths::Combine(ArtifactDir, ArtifactId + TEXT(".json"));
	TSharedRef<FJsonObject> Payload = AnimationBoneMotionProfileObject(Sequence, TrackNames, MotionMetrics);
	Payload->SetStringField(TEXT("artifact_id"), ArtifactId);
	Payload->SetStringField(TEXT("artifact_uri"), TEXT("uepi://animation-bone-motion-profile/") + ArtifactId);
	Payload->SetStringField(TEXT("generated_at_utc"), FDateTime::UtcNow().ToIso8601());

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Payload, Writer);

	TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("schema_version"), TEXT("uepi.animation_bone_motion_profile_manifest.v1"));
	Manifest->SetStringField(TEXT("profile_schema_version"), TEXT("uepi.animation_bone_motion_profile.v1"));
	Manifest->SetStringField(TEXT("artifact_id"), ArtifactId);
	Manifest->SetStringField(TEXT("artifact_uri"), TEXT("uepi://animation-bone-motion-profile/") + ArtifactId);
	Manifest->SetStringField(TEXT("storage"), TEXT("snapshot_store_artifact_json"));
	Manifest->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(ArtifactPath));
	Manifest->SetStringField(TEXT("sequence_path"), Sequence.GetPathName());
	Manifest->SetStringField(TEXT("skeleton_path"), Sequence.GetSkeleton() ? Sequence.GetSkeleton()->GetPathName() : FString());
	Manifest->SetNumberField(TEXT("analysis_sample_count"), Payload->GetNumberField(TEXT("analysis_sample_count")));
	Manifest->SetNumberField(TEXT("bone_count"), Payload->GetNumberField(TEXT("bone_count")));
	Manifest->SetNumberField(TEXT("changed_bone_count"), Payload->GetNumberField(TEXT("changed_bone_count")));
	Manifest->SetNumberField(TEXT("position_changed_bone_count"), Payload->GetNumberField(TEXT("position_changed_bone_count")));
	Manifest->SetNumberField(TEXT("driver_bone_count"), Payload->GetNumberField(TEXT("driver_bone_count")));
	Manifest->SetNumberField(TEXT("inherited_motion_bone_count"), Payload->GetNumberField(TEXT("inherited_motion_bone_count")));
	Manifest->SetNumberField(TEXT("byte_count"), FTCHARToUTF8(*Json).Length());
	Manifest->SetStringField(TEXT("encoding"), TEXT("json"));

	IFileManager::Get().MakeDirectory(*ArtifactDir, true);
	if (!FFileHelper::SaveStringToFile(Json, *ArtifactPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Manifest->SetStringField(TEXT("storage"), TEXT("snapshot_store_artifact_json_write_failed"));
		Manifest->SetStringField(TEXT("write_error"), FString::Printf(TEXT("Failed to write animation bone motion profile artifact: %s"), *ArtifactPath));
	}
	return Manifest;
}

TSharedRef<FJsonObject> AnimTransformEntryJson(
	int32 Index,
	const FName& BoneName,
	const FTransform& Transform)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("bone_name"), BoneName.ToString());
	Object->SetObjectField(TEXT("transform"), AnimTransformObject(Transform));
	return Object;
}

TSharedRef<FJsonObject> AnimFrameTransformJson(
	int32 FrameNumber,
	double TimeSeconds,
	const FTransform& Transform)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("frame_number"), FrameNumber);
	Object->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	Object->SetObjectField(TEXT("transform"), AnimTransformObject(Transform));
	return Object;
}

FString AddBoneEntity(
	const FString& ProjectId,
	const FString& OwnerPath,
	const FReferenceSkeleton& RefSkeleton,
	int32 BoneIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
	const FString CanonicalKey = BoneKey(OwnerPath, BoneIndex, BoneName);
	const FString BoneId = MakeStableId(ProjectId, TEXT("bone"), CanonicalKey);
	if (FindEntity(OutEntities, BoneId))
	{
		return BoneId;
	}

	const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
	const FName ParentName = RefSkeleton.IsValidIndex(ParentIndex) ? RefSkeleton.GetBoneName(ParentIndex) : NAME_None;

	FEntityRecord Entity;
	Entity.Id = BoneId;
	Entity.Kind = TEXT("bone");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = BoneName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("owner_path"), OwnerPath);
	Entity.Attributes.Add(TEXT("bone_index"), FString::FromInt(BoneIndex));
	Entity.Attributes.Add(TEXT("bone_name"), BoneName.ToString());
	Entity.Attributes.Add(TEXT("parent_index"), FString::FromInt(ParentIndex));
	Entity.Attributes.Add(TEXT("parent_name"), ParentName.ToString());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Complete;
	Entity.Completeness.Covered = { TEXT("bone_metadata"), TEXT("parent_index"), TEXT("ref_local_pose"), TEXT("ref_component_pose") };
	AddAnimEvidence(Entity, OwnerPath, TEXT("Bone entry read from FReferenceSkeleton."));
	OutEntities.Add(MoveTemp(Entity));
	return BoneId;
}

FString AddVirtualBoneEntity(
	const FString& ProjectId,
	const FString& OwnerPath,
	const FVirtualBone& VirtualBone,
	int32 VirtualBoneIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = VirtualBoneKey(OwnerPath, VirtualBoneIndex, VirtualBone.VirtualBoneName);
	const FString VirtualBoneId = MakeStableId(ProjectId, TEXT("virtual_bone"), CanonicalKey);
	if (FindEntity(OutEntities, VirtualBoneId))
	{
		return VirtualBoneId;
	}

	FEntityRecord Entity;
	Entity.Id = VirtualBoneId;
	Entity.Kind = TEXT("virtual_bone");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = VirtualBone.VirtualBoneName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("owner_path"), OwnerPath);
	Entity.Attributes.Add(TEXT("virtual_bone_index"), FString::FromInt(VirtualBoneIndex));
	Entity.Attributes.Add(TEXT("virtual_bone_name"), VirtualBone.VirtualBoneName.ToString());
	Entity.Attributes.Add(TEXT("source_bone_name"), VirtualBone.SourceBoneName.ToString());
	Entity.Attributes.Add(TEXT("target_bone_name"), VirtualBone.TargetBoneName.ToString());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Complete;
	Entity.Completeness.Covered = { TEXT("virtual_bone_metadata"), TEXT("source_bone"), TEXT("target_bone") };
	AddAnimEvidence(Entity, OwnerPath, TEXT("Virtual bone entry read from USkeleton."));
	OutEntities.Add(MoveTemp(Entity));
	return VirtualBoneId;
}

FString AddSocketEntity(
	const FString& ProjectId,
	const FString& OwnerPath,
	const FString& SourceKind,
	const USkeletalMeshSocket& Socket,
	int32 SocketIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = SocketKey(OwnerPath, SourceKind, SocketIndex, Socket.SocketName);
	const FString SocketId = MakeStableId(ProjectId, TEXT("skeletal_socket"), CanonicalKey);
	if (FindEntity(OutEntities, SocketId))
	{
		return SocketId;
	}

	FEntityRecord Entity;
	Entity.Id = SocketId;
	Entity.Kind = TEXT("skeletal_socket");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Socket.SocketName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("owner_path"), OwnerPath);
	Entity.Attributes.Add(TEXT("socket_index"), FString::FromInt(SocketIndex));
	Entity.Attributes.Add(TEXT("socket_name"), Socket.SocketName.ToString());
	Entity.Attributes.Add(TEXT("bone_name"), Socket.BoneName.ToString());
	Entity.Attributes.Add(TEXT("socket_source"), SourceKind);
	Entity.Attributes.Add(TEXT("relative_location"), FString::Printf(TEXT("%s,%s,%s"), *AnimNumber(Socket.RelativeLocation.X), *AnimNumber(Socket.RelativeLocation.Y), *AnimNumber(Socket.RelativeLocation.Z)));
	Entity.Attributes.Add(TEXT("relative_rotation"), FString::Printf(TEXT("%s,%s,%s"), *AnimNumber(Socket.RelativeRotation.Pitch), *AnimNumber(Socket.RelativeRotation.Yaw), *AnimNumber(Socket.RelativeRotation.Roll)));
	Entity.Attributes.Add(TEXT("relative_scale"), FString::Printf(TEXT("%s,%s,%s"), *AnimNumber(Socket.RelativeScale.X), *AnimNumber(Socket.RelativeScale.Y), *AnimNumber(Socket.RelativeScale.Z)));
	Entity.Attributes.Add(TEXT("force_always_animated"), AnimBool(Socket.bForceAlwaysAnimated));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Complete;
	Entity.Completeness.Covered = { TEXT("socket_metadata"), TEXT("relative_transform"), TEXT("bound_bone") };
	AddAnimEvidence(Entity, OwnerPath, TEXT("Skeletal mesh socket read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return SocketId;
}

TSharedRef<FJsonObject> SocketJson(
	const USkeletalMeshSocket& Socket,
	int32 SocketIndex,
	const FString& SocketId,
	const FString& SourceKind)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), SocketId);
	Object->SetNumberField(TEXT("index"), SocketIndex);
	Object->SetStringField(TEXT("name"), Socket.SocketName.ToString());
	Object->SetStringField(TEXT("bone_name"), Socket.BoneName.ToString());
	Object->SetStringField(TEXT("source"), SourceKind);
	Object->SetObjectField(TEXT("relative_location"), AnimVectorObject(Socket.RelativeLocation));
	Object->SetObjectField(TEXT("relative_rotation"), AnimRotatorObject(Socket.RelativeRotation));
	Object->SetObjectField(TEXT("relative_scale"), AnimVectorObject(Socket.RelativeScale));
	Object->SetBoolField(TEXT("force_always_animated"), Socket.bForceAlwaysAnimated);
	Object->SetObjectField(TEXT("local_transform"), AnimTransformObject(Socket.GetSocketLocalTransform()));
	return Object;
}

TArray<TSharedPtr<FJsonValue>> SkeletonBonesJson(
	const FString& ProjectId,
	const FString& OwnerPath,
	const FReferenceSkeleton& RefSkeleton,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations,
	const FString& SkeletonId,
	TMap<FName, FString>* OutBoneNameToId)
{
	TArray<TSharedPtr<FJsonValue>> BoneValues;
	TArray<FString> BoneIds;
	BoneIds.SetNum(RefSkeleton.GetNum());
	const TArray<FTransform>& LocalRefPoses = RefSkeleton.GetRefBonePose();
	const TArray<FTransform> ComponentRefPoses = BuildComponentSpaceRefPose(RefSkeleton);

	for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
	{
		const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
		const FString BoneId = AddBoneEntity(ProjectId, OwnerPath, RefSkeleton, BoneIndex, OutEntities);
		BoneIds[BoneIndex] = BoneId;
		if (OutBoneNameToId)
		{
			OutBoneNameToId->Add(BoneName, BoneId);
		}

		AddAnimRelation(ProjectId, TEXT("contains_bone"), SkeletonId, BoneId, OwnerPath, TEXT("Skeleton contains a reference bone."), OutRelations);

		const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
		const FName ParentName = RefSkeleton.IsValidIndex(ParentIndex) ? RefSkeleton.GetBoneName(ParentIndex) : NAME_None;

		TSharedRef<FJsonObject> BoneObject = MakeShared<FJsonObject>();
		BoneObject->SetStringField(TEXT("id"), BoneId);
		BoneObject->SetNumberField(TEXT("index"), BoneIndex);
		BoneObject->SetStringField(TEXT("name"), BoneName.ToString());
		BoneObject->SetNumberField(TEXT("parent_index"), ParentIndex);
		BoneObject->SetStringField(TEXT("parent_name"), ParentName.ToString());
		BoneObject->SetObjectField(TEXT("ref_local"), AnimTransformObject(LocalRefPoses.IsValidIndex(BoneIndex) ? LocalRefPoses[BoneIndex] : FTransform::Identity));
		BoneObject->SetObjectField(TEXT("ref_component"), AnimTransformObject(ComponentRefPoses.IsValidIndex(BoneIndex) ? ComponentRefPoses[BoneIndex] : FTransform::Identity));
		BoneValues.Add(MakeShared<FJsonValueObject>(BoneObject));
	}

	for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
	{
		const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
		if (RefSkeleton.IsValidIndex(ParentIndex))
		{
			AddAnimRelation(ProjectId, TEXT("parent_bone"), BoneIds[BoneIndex], BoneIds[ParentIndex], OwnerPath, TEXT("Reference bone stores this parent index."), OutRelations);
		}
	}

	return BoneValues;
}

TArray<TSharedPtr<FJsonValue>> SkeletonVirtualBonesJson(
	const FString& ProjectId,
	const FString& OwnerPath,
	const USkeleton& Skeleton,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations,
	const FString& SkeletonId,
	const TMap<FName, FString>* BoneNameToId)
{
	TArray<TSharedPtr<FJsonValue>> VirtualBoneValues;
	const TArray<FVirtualBone>& VirtualBones = Skeleton.GetVirtualBones();
	for (int32 VirtualBoneIndex = 0; VirtualBoneIndex < VirtualBones.Num(); ++VirtualBoneIndex)
	{
		const FVirtualBone& VirtualBone = VirtualBones[VirtualBoneIndex];
		const FString VirtualBoneId = AddVirtualBoneEntity(ProjectId, OwnerPath, VirtualBone, VirtualBoneIndex, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_virtual_bone"), SkeletonId, VirtualBoneId, OwnerPath, TEXT("Skeleton contains a virtual bone."), OutRelations);

		if (BoneNameToId)
		{
			if (const FString* SourceBoneId = BoneNameToId->Find(VirtualBone.SourceBoneName))
			{
				AddAnimRelation(ProjectId, TEXT("virtual_bone_source"), VirtualBoneId, *SourceBoneId, OwnerPath, TEXT("Virtual bone stores this source bone."), OutRelations);
			}
			if (const FString* TargetBoneId = BoneNameToId->Find(VirtualBone.TargetBoneName))
			{
				AddAnimRelation(ProjectId, TEXT("virtual_bone_target"), VirtualBoneId, *TargetBoneId, OwnerPath, TEXT("Virtual bone stores this target bone."), OutRelations);
			}
		}

		TSharedRef<FJsonObject> VirtualBoneObject = MakeShared<FJsonObject>();
		VirtualBoneObject->SetStringField(TEXT("id"), VirtualBoneId);
		VirtualBoneObject->SetNumberField(TEXT("index"), VirtualBoneIndex);
		VirtualBoneObject->SetStringField(TEXT("name"), VirtualBone.VirtualBoneName.ToString());
		VirtualBoneObject->SetStringField(TEXT("source_bone"), VirtualBone.SourceBoneName.ToString());
		VirtualBoneObject->SetStringField(TEXT("target_bone"), VirtualBone.TargetBoneName.ToString());
		VirtualBoneValues.Add(MakeShared<FJsonValueObject>(VirtualBoneObject));
	}

	return VirtualBoneValues;
}

TArray<TSharedPtr<FJsonValue>> SocketArrayJson(
	const FString& ProjectId,
	const FString& OwnerPath,
	const FString& OwnerId,
	const FString& SourceKind,
	const TArray<const USkeletalMeshSocket*>& Sockets,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations,
	const TMap<FName, FString>* BoneNameToId)
{
	TArray<TSharedPtr<FJsonValue>> SocketValues;
	for (const USkeletalMeshSocket* Socket : Sockets)
	{
		if (!Socket)
		{
			continue;
		}

		const int32 SocketIndex = SocketValues.Num();
		const FString SocketId = AddSocketEntity(ProjectId, OwnerPath, SourceKind, *Socket, SocketIndex, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_socket"), OwnerId, SocketId, OwnerPath, TEXT("Animation owner contains a skeletal socket."), OutRelations);
		if (BoneNameToId)
		{
			if (const FString* BoneId = BoneNameToId->Find(Socket->BoneName))
			{
				AddAnimRelation(ProjectId, TEXT("binds_bone"), SocketId, *BoneId, OwnerPath, TEXT("Socket is bound to this reference bone."), OutRelations);
			}
		}

		SocketValues.Add(MakeShared<FJsonValueObject>(SocketJson(*Socket, SocketIndex, SocketId, SourceKind)));
	}

	return SocketValues;
}

TArray<const USkeletalMeshSocket*> SkeletonSocketPointers(const USkeleton& Skeleton)
{
	TArray<const USkeletalMeshSocket*> Sockets;
	Sockets.Reserve(Skeleton.Sockets.Num());
	for (const TObjectPtr<USkeletalMeshSocket>& Socket : Skeleton.Sockets)
	{
		Sockets.Add(Socket.Get());
	}
	return Sockets;
}

TArray<const USkeletalMeshSocket*> MeshOnlySocketPointers(const USkeletalMesh& Mesh)
{
	TArray<const USkeletalMeshSocket*> Sockets;
	const TArray<USkeletalMeshSocket*>& MeshSockets = Mesh.GetMeshOnlySocketList();
	Sockets.Reserve(MeshSockets.Num());
	for (const USkeletalMeshSocket* Socket : MeshSockets)
	{
		Sockets.Add(Socket);
	}
	return Sockets;
}

TArray<TSharedPtr<FJsonValue>> ActiveMeshSocketsJson(const FString& ProjectId, const USkeletalMesh& Mesh)
{
	TArray<TSharedPtr<FJsonValue>> SocketValues;
	const TArray<USkeletalMeshSocket*> ActiveSockets = Mesh.GetActiveSocketList();
	for (const USkeletalMeshSocket* Socket : ActiveSockets)
	{
		if (!Socket)
		{
			continue;
		}

		const int32 SocketIndex = SocketValues.Num();
		const bool bMeshSocket = Socket->GetOuter() == &Mesh || Socket->IsIn(&Mesh);
		const FString SourceKind = bMeshSocket ? TEXT("mesh") : TEXT("skeleton");
		const FString OwnerPath = (!bMeshSocket && Mesh.GetSkeleton()) ? Mesh.GetSkeleton()->GetPathName() : Mesh.GetPathName();
		const FString SocketId = MakeStableId(ProjectId, TEXT("skeletal_socket"), SocketKey(OwnerPath, FString::Printf(TEXT("active_%s"), *SourceKind), SocketIndex, Socket->SocketName));
		SocketValues.Add(MakeShared<FJsonValueObject>(SocketJson(*Socket, SocketIndex, SocketId, SourceKind)));
	}

	return SocketValues;
}

TSharedRef<FJsonObject> SkeletonSnapshot(
	const FString& ProjectId,
	const USkeleton& Skeleton,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations,
	const FString& SkeletonId,
	TMap<FName, FString>* OutBoneNameToId)
{
	const FReferenceSkeleton& RefSkeleton = Skeleton.GetReferenceSkeleton();
	TMap<FName, FString> LocalBoneNameToId;
	TMap<FName, FString>* BoneNameToId = OutBoneNameToId ? OutBoneNameToId : &LocalBoneNameToId;
	TArray<TSharedPtr<FJsonValue>> BoneValues = SkeletonBonesJson(ProjectId, Skeleton.GetPathName(), RefSkeleton, OutEntities, OutRelations, SkeletonId, BoneNameToId);
	TArray<TSharedPtr<FJsonValue>> VirtualBoneValues = SkeletonVirtualBonesJson(ProjectId, Skeleton.GetPathName(), Skeleton, OutEntities, OutRelations, SkeletonId, BoneNameToId);
	TArray<TSharedPtr<FJsonValue>> SocketValues = SocketArrayJson(ProjectId, Skeleton.GetPathName(), SkeletonId, TEXT("skeleton"), SkeletonSocketPointers(Skeleton), OutEntities, OutRelations, BoneNameToId);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.skeleton.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::AnimationDataModel));
	Object->SetStringField(TEXT("skeleton_path"), Skeleton.GetPathName());
	Object->SetNumberField(TEXT("bone_count"), RefSkeleton.GetNum());
	Object->SetNumberField(TEXT("raw_bone_count"), RefSkeleton.GetRawBoneNum());
	Object->SetNumberField(TEXT("virtual_bone_count"), VirtualBoneValues.Num());
	Object->SetNumberField(TEXT("socket_count"), SocketValues.Num());
	Object->SetArrayField(TEXT("bones"), MoveTemp(BoneValues));
	Object->SetArrayField(TEXT("virtual_bones"), MoveTemp(VirtualBoneValues));
	Object->SetArrayField(TEXT("sockets"), MoveTemp(SocketValues));
	return Object;
}

FString AddSkeletonEntity(
	const FString& ProjectId,
	const USkeleton* Skeleton,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations,
	TMap<FName, FString>* OutBoneNameToId = nullptr)
{
	if (!Skeleton)
	{
		return FString();
	}

	const FString SkeletonPath = Skeleton->GetPathName();
	const FString SkeletonId = MakeStableId(ProjectId, TEXT("skeleton"), SkeletonPath);
	FEntityRecord* ExistingEntity = FindEntity(OutEntities, SkeletonId);
	if (!ExistingEntity)
	{
		FEntityRecord Entity;
		Entity.Id = SkeletonId;
		Entity.Kind = TEXT("skeleton");
		Entity.CanonicalKey = SkeletonPath;
		Entity.DisplayName = Skeleton->GetName();
		Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
		Entity.Attributes.Add(TEXT("skeleton_path"), SkeletonPath);
		Entity.Attributes.Add(TEXT("bone_count"), FString::FromInt(Skeleton->GetReferenceSkeleton().GetNum()));
		Entity.Attributes.Add(TEXT("raw_bone_count"), FString::FromInt(Skeleton->GetReferenceSkeleton().GetRawBoneNum()));
		Entity.Attributes.Add(TEXT("virtual_bone_count"), FString::FromInt(Skeleton->GetVirtualBones().Num()));
		Entity.Attributes.Add(TEXT("socket_count"), FString::FromInt(Skeleton->Sockets.Num()));
		Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
		Entity.Completeness.State = ECompletenessState::Complete;
		Entity.Completeness.Covered = { TEXT("reference_skeleton"), TEXT("bone_hierarchy"), TEXT("ref_pose"), TEXT("virtual_bones"), TEXT("sockets") };
		AddAnimEvidence(Entity, SkeletonPath, TEXT("USkeleton reference skeleton read through public Engine API."));
		Entity.Snapshot = SkeletonSnapshot(ProjectId, *Skeleton, OutEntities, OutRelations, SkeletonId, OutBoneNameToId);
		OutEntities.Add(MoveTemp(Entity));
	}
	else
	{
		ExistingEntity->Completeness.Covered.AddUnique(TEXT("reference_skeleton"));
		ExistingEntity->Completeness.Covered.AddUnique(TEXT("ref_pose"));
		ExistingEntity->Completeness.Covered.AddUnique(TEXT("virtual_bones"));
		ExistingEntity->Completeness.Covered.AddUnique(TEXT("sockets"));
		SkeletonSnapshot(ProjectId, *Skeleton, OutEntities, OutRelations, SkeletonId, OutBoneNameToId);
	}

	return SkeletonId;
}

TSharedRef<FJsonObject> SkeletalMeshSnapshot(
	const FString& ProjectId,
	const USkeletalMesh& Mesh,
	const FString& MeshId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations,
	const TMap<FName, FString>* BoneNameToId)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	const FReferenceSkeleton& RefSkeleton = Mesh.GetRefSkeleton();
	TArray<TSharedPtr<FJsonValue>> MeshSocketValues = SocketArrayJson(ProjectId, Mesh.GetPathName(), MeshId, TEXT("mesh"), MeshOnlySocketPointers(Mesh), OutEntities, OutRelations, BoneNameToId);
	TArray<TSharedPtr<FJsonValue>> ActiveSocketValues = ActiveMeshSocketsJson(ProjectId, Mesh);
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.skeletal_mesh.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::AnimationDataModel));
	Object->SetStringField(TEXT("mesh_path"), Mesh.GetPathName());
	Object->SetStringField(TEXT("skeleton_path"), Mesh.GetSkeleton() ? Mesh.GetSkeleton()->GetPathName() : FString());
	Object->SetNumberField(TEXT("bone_count"), RefSkeleton.GetNum());
	Object->SetNumberField(TEXT("lod_count"), Mesh.GetLODNum());
	Object->SetNumberField(TEXT("material_count"), Mesh.GetMaterials().Num());
	Object->SetNumberField(TEXT("morph_target_count"), Mesh.GetMorphTargets().Num());
	Object->SetNumberField(TEXT("socket_count"), ActiveSocketValues.Num());
	Object->SetNumberField(TEXT("mesh_socket_count"), MeshSocketValues.Num());
	Object->SetNumberField(TEXT("active_socket_count"), ActiveSocketValues.Num());
	Object->SetObjectField(TEXT("imported_bounds"), AnimBoundsObject(Mesh.GetImportedBounds()));
	Object->SetArrayField(TEXT("mesh_sockets"), MoveTemp(MeshSocketValues));
	Object->SetArrayField(TEXT("active_sockets"), MoveTemp(ActiveSocketValues));
	return Object;
}

FString AddSkeletalMeshEntity(
	const FString& ProjectId,
	const USkeletalMesh& Mesh,
	TArray<FEntityRecord>& OutEntities)
{
	const FString MeshPath = Mesh.GetPathName();
	const FString MeshId = MakeStableId(ProjectId, TEXT("skeletal_mesh"), MeshPath);
	if (FindEntity(OutEntities, MeshId))
	{
		return MeshId;
	}

	FEntityRecord Entity;
	Entity.Id = MeshId;
	Entity.Kind = TEXT("skeletal_mesh");
	Entity.CanonicalKey = MeshPath;
	Entity.DisplayName = Mesh.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("mesh_path"), MeshPath);
	Entity.Attributes.Add(TEXT("skeleton_path"), Mesh.GetSkeleton() ? Mesh.GetSkeleton()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("bone_count"), FString::FromInt(Mesh.GetRefSkeleton().GetNum()));
	Entity.Attributes.Add(TEXT("lod_count"), FString::FromInt(Mesh.GetLODNum()));
	Entity.Attributes.Add(TEXT("material_count"), FString::FromInt(Mesh.GetMaterials().Num()));
	Entity.Attributes.Add(TEXT("morph_target_count"), FString::FromInt(Mesh.GetMorphTargets().Num()));
	Entity.Attributes.Add(TEXT("mesh_socket_count"), FString::FromInt(Mesh.GetMeshOnlySocketList().Num()));
	Entity.Attributes.Add(TEXT("active_socket_count"), FString::FromInt(Mesh.GetActiveSocketList().Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("skeletal_mesh_metadata"), TEXT("skeleton_reference"), TEXT("mesh_sockets") };
	Entity.Completeness.Omitted = { TEXT("skin_weight_buffers"), TEXT("lod_section_geometry") };
	AddAnimEvidence(Entity, MeshPath, TEXT("USkeletalMesh metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return MeshId;
}

TSharedRef<FJsonObject> NotifyJson(const FAnimNotifyEvent& Notify, int32 NotifyIndex, const FString& NotifyId)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), NotifyId);
	Object->SetNumberField(TEXT("index"), NotifyIndex);
	Object->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
	Object->SetNumberField(TEXT("time"), Notify.GetTime());
	Object->SetNumberField(TEXT("trigger_time"), Notify.GetTriggerTime());
	Object->SetNumberField(TEXT("duration"), Notify.GetDuration());
	Object->SetNumberField(TEXT("track_index"), Notify.TrackIndex);
	Object->SetStringField(TEXT("notify_class"), Notify.Notify ? Notify.Notify->GetClass()->GetPathName() : FString());
	Object->SetStringField(TEXT("notify_state_class"), Notify.NotifyStateClass ? Notify.NotifyStateClass->GetClass()->GetPathName() : FString());
	return Object;
}

FString AddNotifyEntity(
	const FString& ProjectId,
	const UAnimSequence& Sequence,
	const FAnimNotifyEvent& Notify,
	int32 NotifyIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString NotifyName = Notify.NotifyName.IsNone()
		? FString::Printf(TEXT("notify_%d"), NotifyIndex)
		: Notify.NotifyName.ToString();
	const FString CanonicalKey = FString::Printf(TEXT("%s:notify:%d:%s:%s"), *Sequence.GetPathName(), NotifyIndex, *NotifyName, *AnimNumber(Notify.GetTriggerTime()));
	const FString NotifyId = MakeStableId(ProjectId, TEXT("anim_notify"), CanonicalKey);
	if (FindEntity(OutEntities, NotifyId))
	{
		return NotifyId;
	}

	FEntityRecord Entity;
	Entity.Id = NotifyId;
	Entity.Kind = TEXT("anim_notify");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = NotifyName;
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("sequence_path"), Sequence.GetPathName());
	Entity.Attributes.Add(TEXT("notify_name"), NotifyName);
	Entity.Attributes.Add(TEXT("time"), AnimNumber(Notify.GetTime()));
	Entity.Attributes.Add(TEXT("trigger_time"), AnimNumber(Notify.GetTriggerTime()));
	Entity.Attributes.Add(TEXT("duration"), AnimNumber(Notify.GetDuration()));
	Entity.Attributes.Add(TEXT("track_index"), FString::FromInt(Notify.TrackIndex));
	Entity.Attributes.Add(TEXT("notify_class"), Notify.Notify ? Notify.Notify->GetClass()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("notify_state_class"), Notify.NotifyStateClass ? Notify.NotifyStateClass->GetClass()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Complete;
	Entity.Completeness.Covered = { TEXT("notify_metadata") };
	AddAnimEvidence(Entity, Sequence.GetPathName(), TEXT("Animation notify event read from UAnimSequenceBase."));
	OutEntities.Add(MoveTemp(Entity));
	return NotifyId;
}

FString AddAnimationSequenceEntity(
	const FString& ProjectId,
	const UAnimSequence& Sequence,
	TArray<FEntityRecord>& OutEntities)
{
	const FString SequencePath = Sequence.GetPathName();
	const FString SequenceId = MakeStableId(ProjectId, TEXT("animation_sequence"), SequencePath);
	if (FindEntity(OutEntities, SequenceId))
	{
		return SequenceId;
	}

	const IAnimationDataModel* DataModel = Sequence.GetDataModel();
	FEntityRecord Entity;
	Entity.Id = SequenceId;
	Entity.Kind = TEXT("animation_sequence");
	Entity.CanonicalKey = SequencePath;
	Entity.DisplayName = Sequence.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("sequence_path"), SequencePath);
	Entity.Attributes.Add(TEXT("skeleton_path"), Sequence.GetSkeleton() ? Sequence.GetSkeleton()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("play_length_seconds"), AnimNumber(Sequence.GetPlayLength()));
	Entity.Attributes.Add(TEXT("sample_key_count"), FString::FromInt(Sequence.GetNumberOfSampledKeys()));
	Entity.Attributes.Add(TEXT("sampling_frame_rate"), AnimFrameRateString(Sequence.GetSamplingFrameRate()));
	Entity.Attributes.Add(TEXT("frame_count"), DataModel ? FString::FromInt(DataModel->GetNumberOfFrames()) : TEXT("0"));
	Entity.Attributes.Add(TEXT("data_model_key_count"), DataModel ? FString::FromInt(DataModel->GetNumberOfKeys()) : TEXT("0"));
	Entity.Attributes.Add(TEXT("bone_track_count"), DataModel ? FString::FromInt(DataModel->GetNumBoneTracks()) : TEXT("0"));
	Entity.Attributes.Add(TEXT("additive_anim_type"), AnimEnumValue(StaticEnum<EAdditiveAnimationType>(), static_cast<int64>(Sequence.AdditiveAnimType.GetValue())));
	Entity.Attributes.Add(TEXT("additive_base_pose_type"), AnimEnumValue(StaticEnum<EAdditiveBasePoseType>(), static_cast<int64>(Sequence.RefPoseType.GetValue())));
	Entity.Attributes.Add(TEXT("root_motion_enabled"), AnimBool(Sequence.HasRootMotion()));
	Entity.Attributes.Add(TEXT("root_motion_root_lock"), AnimEnumValue(StaticEnum<ERootMotionRootLock::Type>(), static_cast<int64>(Sequence.RootMotionRootLock.GetValue())));
	Entity.Attributes.Add(TEXT("notify_count"), FString::FromInt(Sequence.Notifies.Num()));
	Entity.Attributes.Add(TEXT("curve_count"), DataModel ? FString::FromInt(DataModel->GetNumberOfFloatCurves() + DataModel->GetNumberOfTransformCurves()) : TEXT("0"));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("sequence_metadata"), TEXT("bone_track_metadata"), TEXT("frame_time_samples"), TEXT("raw_local_track_samples"), TEXT("component_space_pose_samples"), TEXT("root_motion_metadata"), TEXT("additive_metadata"), TEXT("notify_metadata"), TEXT("curve_counts"), TEXT("motion_summary"), TEXT("bone_motion_profile_artifact") };
	Entity.Completeness.Omitted = { TEXT("full_per_frame_pose_samples"), TEXT("raw_curve_keys"), TEXT("compressed_track_data") };
	AddAnimEvidence(Entity, SequencePath, TEXT("UAnimSequence metadata read through Animation Data Model."));
	OutEntities.Add(MoveTemp(Entity));
	return SequenceId;
}

int32 AnimTrackSegmentCount(const FAnimTrack& Track)
{
	return Track.AnimSegments.Num();
}

int32 MontageSegmentCount(const UAnimMontage& Montage)
{
	int32 SegmentCount = 0;
	for (const FSlotAnimationTrack& SlotTrack : Montage.SlotAnimTracks)
	{
		SegmentCount += SlotTrack.AnimTrack.AnimSegments.Num();
	}
	return SegmentCount;
}

FString AddAnimationCompositeEntity(
	const FString& ProjectId,
	const UAnimComposite& Composite,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CompositePath = Composite.GetPathName();
	const FString CompositeId = MakeStableId(ProjectId, TEXT("animation_composite"), CompositePath);
	if (FindEntity(OutEntities, CompositeId))
	{
		return CompositeId;
	}

	FEntityRecord Entity;
	Entity.Id = CompositeId;
	Entity.Kind = TEXT("animation_composite");
	Entity.CanonicalKey = CompositePath;
	Entity.DisplayName = Composite.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("composite_path"), CompositePath);
	Entity.Attributes.Add(TEXT("skeleton_path"), Composite.GetSkeleton() ? Composite.GetSkeleton()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("play_length_seconds"), AnimNumber(Composite.GetPlayLength()));
	Entity.Attributes.Add(TEXT("sampling_frame_rate"), AnimFrameRateString(Composite.GetSamplingFrameRate()));
	Entity.Attributes.Add(TEXT("segment_count"), FString::FromInt(AnimTrackSegmentCount(Composite.AnimationTrack)));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("composite_metadata"), TEXT("segment_timeline"), TEXT("animation_references") };
	Entity.Completeness.Omitted = { TEXT("runtime_pose_evaluation") };
	AddAnimEvidence(Entity, CompositePath, TEXT("UAnimComposite metadata and segment timeline read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return CompositeId;
}

FString AddAnimationMontageEntity(
	const FString& ProjectId,
	const UAnimMontage& Montage,
	TArray<FEntityRecord>& OutEntities)
{
	const FString MontagePath = Montage.GetPathName();
	const FString MontageId = MakeStableId(ProjectId, TEXT("animation_montage"), MontagePath);
	if (FindEntity(OutEntities, MontageId))
	{
		return MontageId;
	}

	FEntityRecord Entity;
	Entity.Id = MontageId;
	Entity.Kind = TEXT("animation_montage");
	Entity.CanonicalKey = MontagePath;
	Entity.DisplayName = Montage.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("montage_path"), MontagePath);
	Entity.Attributes.Add(TEXT("skeleton_path"), Montage.GetSkeleton() ? Montage.GetSkeleton()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("play_length_seconds"), AnimNumber(Montage.GetPlayLength()));
	Entity.Attributes.Add(TEXT("sampling_frame_rate"), AnimFrameRateString(Montage.GetSamplingFrameRate()));
	Entity.Attributes.Add(TEXT("section_count"), FString::FromInt(Montage.CompositeSections.Num()));
	Entity.Attributes.Add(TEXT("slot_count"), FString::FromInt(Montage.SlotAnimTracks.Num()));
	Entity.Attributes.Add(TEXT("segment_count"), FString::FromInt(MontageSegmentCount(Montage)));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("montage_metadata"), TEXT("sections"), TEXT("slot_tracks"), TEXT("segment_timeline"), TEXT("animation_references") };
	Entity.Completeness.Omitted = { TEXT("runtime_montage_state") };
	AddAnimEvidence(Entity, MontagePath, TEXT("UAnimMontage metadata, sections, slots, and segment timeline read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return MontageId;
}

FString AddAnimSequenceBaseReferenceEntity(
	const FString& ProjectId,
	const UAnimSequenceBase& Animation,
	TArray<FEntityRecord>& OutEntities)
{
	if (const UAnimSequence* Sequence = Cast<UAnimSequence>(&Animation))
	{
		return AddAnimationSequenceEntity(ProjectId, *Sequence, OutEntities);
	}
	if (const UAnimComposite* Composite = Cast<UAnimComposite>(&Animation))
	{
		return AddAnimationCompositeEntity(ProjectId, *Composite, OutEntities);
	}
	if (const UAnimMontage* Montage = Cast<UAnimMontage>(&Animation))
	{
		return AddAnimationMontageEntity(ProjectId, *Montage, OutEntities);
	}

	const FString AnimationPath = Animation.GetPathName();
	const FString AnimationId = MakeStableId(ProjectId, TEXT("animation_asset"), AnimationPath);
	if (FindEntity(OutEntities, AnimationId))
	{
		return AnimationId;
	}

	FEntityRecord Entity;
	Entity.Id = AnimationId;
	Entity.Kind = TEXT("animation_asset");
	Entity.CanonicalKey = AnimationPath;
	Entity.DisplayName = Animation.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("animation_path"), AnimationPath);
	Entity.Attributes.Add(TEXT("animation_class"), Animation.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("play_length_seconds"), AnimNumber(Animation.GetPlayLength()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("animation_reference_metadata") };
	AddAnimEvidence(Entity, AnimationPath, TEXT("Referenced UAnimSequenceBase metadata read from an animation segment."));
	OutEntities.Add(MoveTemp(Entity));
	return AnimationId;
}

int32 BlendSpaceDimensionCount(const UBlendSpace& BlendSpace)
{
	return BlendSpace.IsA<UBlendSpace1D>() ? 1 : 2;
}

FString BlendSpaceSampleKey(const UBlendSpace& BlendSpace, const FBlendSample& Sample, int32 SampleIndex)
{
	const FString AnimationPath = Sample.Animation ? Sample.Animation->GetPathName() : FString();
	return FString::Printf(
		TEXT("%s:sample:%d:%s:%s:%s:%s"),
		*BlendSpace.GetPathName(),
		SampleIndex,
		*AnimationPath,
		*AnimNumber(Sample.SampleValue.X),
		*AnimNumber(Sample.SampleValue.Y),
		*AnimNumber(Sample.SampleValue.Z));
}

FString AddBlendSpaceSampleEntity(
	const FString& ProjectId,
	const UBlendSpace& BlendSpace,
	const FBlendSample& Sample,
	int32 SampleIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = BlendSpaceSampleKey(BlendSpace, Sample, SampleIndex);
	const FString SampleId = MakeStableId(ProjectId, TEXT("blend_space_sample"), CanonicalKey);
	if (FindEntity(OutEntities, SampleId))
	{
		return SampleId;
	}

	const FString AnimationPath = Sample.Animation ? Sample.Animation->GetPathName() : FString();
	FEntityRecord Entity;
	Entity.Id = SampleId;
	Entity.Kind = TEXT("blend_space_sample");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = FString::Printf(TEXT("Sample %d"), SampleIndex);
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("blend_space_path"), BlendSpace.GetPathName());
	Entity.Attributes.Add(TEXT("sample_index"), FString::FromInt(SampleIndex));
	Entity.Attributes.Add(TEXT("animation_path"), AnimationPath);
	Entity.Attributes.Add(TEXT("sample_x"), AnimNumber(Sample.SampleValue.X));
	Entity.Attributes.Add(TEXT("sample_y"), AnimNumber(Sample.SampleValue.Y));
	Entity.Attributes.Add(TEXT("sample_z"), AnimNumber(Sample.SampleValue.Z));
	Entity.Attributes.Add(TEXT("rate_scale"), AnimNumber(Sample.RateScale));
#if WITH_EDITORONLY_DATA
	Entity.Attributes.Add(TEXT("sample_valid"), AnimBool(Sample.bIsValid));
#endif
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("blend_sample_metadata"), TEXT("animation_reference") };
	Entity.Completeness.Omitted = { TEXT("runtime_sample_weight") };
	AddAnimEvidence(Entity, BlendSpace.GetPathName(), TEXT("BlendSpace sample read from FBlendSample data."));
	OutEntities.Add(MoveTemp(Entity));
	return SampleId;
}

TSharedRef<FJsonObject> BlendAxisJson(const UBlendSpace& BlendSpace, int32 AxisIndex)
{
	const FBlendParameter& Parameter = BlendSpace.GetBlendParameter(AxisIndex);
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), AxisIndex);
	Object->SetStringField(TEXT("name"), Parameter.DisplayName);
	Object->SetNumberField(TEXT("min"), Parameter.Min);
	Object->SetNumberField(TEXT("max"), Parameter.Max);
	Object->SetNumberField(TEXT("grid_num"), Parameter.GridNum);
	Object->SetBoolField(TEXT("snap_to_grid"), Parameter.bSnapToGrid);
	Object->SetBoolField(TEXT("wrap_input"), Parameter.bWrapInput);
	return Object;
}

TSharedRef<FJsonObject> BlendSampleJson(const FBlendSample& Sample, int32 SampleIndex, const FString& SampleId)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), SampleId);
	Object->SetNumberField(TEXT("index"), SampleIndex);
	Object->SetStringField(TEXT("animation_path"), Sample.Animation ? Sample.Animation->GetPathName() : FString());
	Object->SetObjectField(TEXT("sample_value"), AnimVectorObject(Sample.SampleValue));
	Object->SetNumberField(TEXT("rate_scale"), Sample.RateScale);
#if WITH_EDITORONLY_DATA
	Object->SetBoolField(TEXT("sample_valid"), Sample.bIsValid);
#else
	Object->SetBoolField(TEXT("sample_valid"), true);
#endif
	return Object;
}

FString AddBlendSpaceEntity(
	const FString& ProjectId,
	const UBlendSpace& BlendSpace,
	TArray<FEntityRecord>& OutEntities)
{
	const FString BlendSpacePath = BlendSpace.GetPathName();
	const FString BlendSpaceId = MakeStableId(ProjectId, TEXT("blend_space"), BlendSpacePath);
	if (FindEntity(OutEntities, BlendSpaceId))
	{
		return BlendSpaceId;
	}

	FEntityRecord Entity;
	Entity.Id = BlendSpaceId;
	Entity.Kind = TEXT("blend_space");
	Entity.CanonicalKey = BlendSpacePath;
	Entity.DisplayName = BlendSpace.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("blend_space_path"), BlendSpacePath);
	Entity.Attributes.Add(TEXT("blend_space_class"), BlendSpace.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("skeleton_path"), BlendSpace.GetSkeleton() ? BlendSpace.GetSkeleton()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("dimension_count"), FString::FromInt(BlendSpaceDimensionCount(BlendSpace)));
	Entity.Attributes.Add(TEXT("sample_count"), FString::FromInt(BlendSpace.GetBlendSamples().Num()));
	Entity.Attributes.Add(TEXT("loop"), AnimBool(BlendSpace.bLoop));
	Entity.Attributes.Add(TEXT("interpolate_using_grid"), AnimBool(BlendSpace.bInterpolateUsingGrid));
	Entity.Attributes.Add(TEXT("target_weight_interpolation_speed"), AnimNumber(BlendSpace.TargetWeightInterpolationSpeedPerSec));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("blend_space_axes"), TEXT("blend_space_samples"), TEXT("sample_animation_references") };
	Entity.Completeness.Omitted = { TEXT("runtime_sample_weights"), TEXT("evaluated_pose") };
	AddAnimEvidence(Entity, BlendSpacePath, TEXT("UBlendSpace structural metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return BlendSpaceId;
}

TSharedRef<FJsonObject> BlendSpaceSnapshot(
	const FString& ProjectId,
	const UBlendSpace& BlendSpace,
	const FString& BlendSpaceId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> AxisValues;
	const int32 DimensionCount = BlendSpaceDimensionCount(BlendSpace);
	for (int32 AxisIndex = 0; AxisIndex < DimensionCount; ++AxisIndex)
	{
		AxisValues.Add(MakeShared<FJsonValueObject>(BlendAxisJson(BlendSpace, AxisIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> SampleValues;
	const TArray<FBlendSample>& Samples = BlendSpace.GetBlendSamples();
	for (int32 SampleIndex = 0; SampleIndex < Samples.Num(); ++SampleIndex)
	{
		const FBlendSample& Sample = Samples[SampleIndex];
		const FString SampleId = AddBlendSpaceSampleEntity(ProjectId, BlendSpace, Sample, SampleIndex, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_blend_sample"), BlendSpaceId, SampleId, BlendSpace.GetPathName(), TEXT("BlendSpace contains this sample point."), OutRelations);
		if (Sample.Animation)
		{
			const FString AnimationId = AddAnimationSequenceEntity(ProjectId, *Sample.Animation, OutEntities);
			AddAnimRelation(ProjectId, TEXT("samples_animation"), SampleId, AnimationId, BlendSpace.GetPathName(), TEXT("BlendSpace sample references this animation sequence."), OutRelations);
		}
		SampleValues.Add(MakeShared<FJsonValueObject>(BlendSampleJson(Sample, SampleIndex, SampleId)));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.blend_space.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::AnimationDataModel));
	Object->SetStringField(TEXT("blend_space_path"), BlendSpace.GetPathName());
	Object->SetStringField(TEXT("blend_space_class"), BlendSpace.GetClass()->GetPathName());
	Object->SetStringField(TEXT("skeleton_path"), BlendSpace.GetSkeleton() ? BlendSpace.GetSkeleton()->GetPathName() : FString());
	Object->SetNumberField(TEXT("dimension_count"), DimensionCount);
	Object->SetNumberField(TEXT("axis_count"), AxisValues.Num());
	Object->SetNumberField(TEXT("sample_count"), SampleValues.Num());
	Object->SetBoolField(TEXT("loop"), BlendSpace.bLoop);
	Object->SetBoolField(TEXT("interpolate_using_grid"), BlendSpace.bInterpolateUsingGrid);
	Object->SetNumberField(TEXT("target_weight_interpolation_speed"), BlendSpace.TargetWeightInterpolationSpeedPerSec);
	Object->SetBoolField(TEXT("target_weight_interpolation_ease_in_out"), BlendSpace.bTargetWeightInterpolationEaseInOut);
	Object->SetBoolField(TEXT("allow_mesh_space_blending"), BlendSpace.bAllowMeshSpaceBlending);
	Object->SetStringField(TEXT("notify_trigger_mode"), AnimEnumValue(StaticEnum<ENotifyTriggerMode::Type>(), static_cast<int64>(BlendSpace.NotifyTriggerMode.GetValue())));
	Object->SetStringField(TEXT("preferred_triangulation_direction"), AnimEnumValue(StaticEnum<EPreferredTriangulationDirection>(), static_cast<int64>(BlendSpace.PreferredTriangulationDirection)));
#if WITH_EDITORONLY_DATA
	Object->SetStringField(TEXT("preview_base_pose_path"), BlendSpace.PreviewBasePose ? BlendSpace.PreviewBasePose->GetPathName() : FString());
#else
	Object->SetStringField(TEXT("preview_base_pose_path"), FString());
#endif
	Object->SetNumberField(TEXT("anim_length"), BlendSpace.AnimLength);
	Object->SetArrayField(TEXT("axes"), AxisValues);
	Object->SetArrayField(TEXT("samples"), SampleValues);
	return Object;
}

FString PoseAssetPoseKey(const UPoseAsset& PoseAsset, int32 PoseIndex, const FName& PoseName)
{
	return FString::Printf(TEXT("%s:pose:%d:%s"), *PoseAsset.GetPathName(), PoseIndex, *PoseName.ToString());
}

FString PoseAssetCurveKey(const UPoseAsset& PoseAsset, int32 CurveIndex, const FName& CurveName)
{
	return FString::Printf(TEXT("%s:curve:%d:%s"), *PoseAsset.GetPathName(), CurveIndex, *CurveName.ToString());
}

FString AddPoseAssetEntity(
	const FString& ProjectId,
	const UPoseAsset& PoseAsset,
	TArray<FEntityRecord>& OutEntities)
{
	const FString PoseAssetPath = PoseAsset.GetPathName();
	const FString PoseAssetId = MakeStableId(ProjectId, TEXT("pose_asset"), PoseAssetPath);
	if (FindEntity(OutEntities, PoseAssetId))
	{
		return PoseAssetId;
	}

	FEntityRecord Entity;
	Entity.Id = PoseAssetId;
	Entity.Kind = TEXT("pose_asset");
	Entity.CanonicalKey = PoseAssetPath;
	Entity.DisplayName = PoseAsset.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("pose_asset_path"), PoseAssetPath);
	Entity.Attributes.Add(TEXT("pose_asset_class"), PoseAsset.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("skeleton_path"), PoseAsset.GetSkeleton() ? PoseAsset.GetSkeleton()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("pose_count"), FString::FromInt(PoseAsset.GetNumPoses()));
	Entity.Attributes.Add(TEXT("curve_count"), FString::FromInt(PoseAsset.GetNumCurves()));
	Entity.Attributes.Add(TEXT("track_count"), FString::FromInt(PoseAsset.GetNumTracks()));
	Entity.Attributes.Add(TEXT("additive"), AnimBool(PoseAsset.IsValidAdditive()));
	Entity.Attributes.Add(TEXT("base_pose_index"), FString::FromInt(PoseAsset.GetBasePoseIndex()));
#if WITH_EDITORONLY_DATA
	Entity.Attributes.Add(TEXT("source_animation_path"), PoseAsset.SourceAnimation ? PoseAsset.SourceAnimation->GetPathName() : FString());
#endif
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("pose_names"), TEXT("curve_names"), TEXT("track_names"), TEXT("source_animation_reference") };
	Entity.Completeness.Omitted = { TEXT("runtime_pose_evaluation"), TEXT("component_space_pose_samples") };
	AddAnimEvidence(Entity, PoseAssetPath, TEXT("UPoseAsset structural metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return PoseAssetId;
}

FString AddPoseAssetPoseEntity(
	const FString& ProjectId,
	const UPoseAsset& PoseAsset,
	int32 PoseIndex,
	const FName& PoseName,
	int32 CurveValueCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = PoseAssetPoseKey(PoseAsset, PoseIndex, PoseName);
	const FString PoseId = MakeStableId(ProjectId, TEXT("pose_asset_pose"), CanonicalKey);
	if (FindEntity(OutEntities, PoseId))
	{
		return PoseId;
	}

	FEntityRecord Entity;
	Entity.Id = PoseId;
	Entity.Kind = TEXT("pose_asset_pose");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = PoseName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("pose_asset_path"), PoseAsset.GetPathName());
	Entity.Attributes.Add(TEXT("pose_index"), FString::FromInt(PoseIndex));
	Entity.Attributes.Add(TEXT("pose_name"), PoseName.ToString());
	Entity.Attributes.Add(TEXT("curve_value_count"), FString::FromInt(CurveValueCount));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("pose_metadata"), TEXT("curve_values") };
	Entity.Completeness.Omitted = { TEXT("local_space_pose_transforms"), TEXT("runtime_pose_evaluation") };
	AddAnimEvidence(Entity, PoseAsset.GetPathName(), TEXT("Pose entry read from UPoseAsset."));
	OutEntities.Add(MoveTemp(Entity));
	return PoseId;
}

FString AddPoseAssetCurveEntity(
	const FString& ProjectId,
	const UPoseAsset& PoseAsset,
	int32 CurveIndex,
	const FName& CurveName,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = PoseAssetCurveKey(PoseAsset, CurveIndex, CurveName);
	const FString CurveId = MakeStableId(ProjectId, TEXT("pose_asset_curve"), CanonicalKey);
	if (FindEntity(OutEntities, CurveId))
	{
		return CurveId;
	}

	FEntityRecord Entity;
	Entity.Id = CurveId;
	Entity.Kind = TEXT("pose_asset_curve");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = CurveName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("pose_asset_path"), PoseAsset.GetPathName());
	Entity.Attributes.Add(TEXT("curve_index"), FString::FromInt(CurveIndex));
	Entity.Attributes.Add(TEXT("curve_name"), CurveName.ToString());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Complete;
	Entity.Completeness.Covered = { TEXT("curve_metadata") };
	AddAnimEvidence(Entity, PoseAsset.GetPathName(), TEXT("PoseAsset curve entry read from UPoseAsset."));
	OutEntities.Add(MoveTemp(Entity));
	return CurveId;
}

TSharedRef<FJsonObject> PoseCurveValueJson(const FName& CurveName, int32 CurveIndex, float Value)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), CurveIndex);
	Object->SetStringField(TEXT("curve_name"), CurveName.ToString());
	Object->SetNumberField(TEXT("value"), Value);
	return Object;
}

TSharedRef<FJsonObject> PoseAssetSnapshot(
	const FString& ProjectId,
	const UPoseAsset& PoseAsset,
	const FString& PoseAssetId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const TArray<FName>& PoseNames = PoseAsset.GetPoseFNames();
	TArray<FName> CurveNames = PoseAsset.GetCurveFNames();
	const TArray<FName>& TrackNames = PoseAsset.GetTrackNames();

	TArray<TSharedPtr<FJsonValue>> CurveValues;
	for (int32 CurveIndex = 0; CurveIndex < CurveNames.Num(); ++CurveIndex)
	{
		const FName CurveName = CurveNames[CurveIndex];
		const FString CurveId = AddPoseAssetCurveEntity(ProjectId, PoseAsset, CurveIndex, CurveName, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_curve"), PoseAssetId, CurveId, PoseAsset.GetPathName(), TEXT("PoseAsset contains this curve entry."), OutRelations);

		TSharedRef<FJsonObject> CurveObject = MakeShared<FJsonObject>();
		CurveObject->SetStringField(TEXT("id"), CurveId);
		CurveObject->SetNumberField(TEXT("index"), CurveIndex);
		CurveObject->SetStringField(TEXT("name"), CurveName.ToString());
		CurveValues.Add(MakeShared<FJsonValueObject>(CurveObject));
	}

	TArray<TSharedPtr<FJsonValue>> PoseValues;
	for (int32 PoseIndex = 0; PoseIndex < PoseNames.Num(); ++PoseIndex)
	{
		const FName PoseName = PoseNames[PoseIndex];
		const TArray<float> Values = PoseAsset.GetCurveValues(PoseIndex);
		const FString PoseId = AddPoseAssetPoseEntity(ProjectId, PoseAsset, PoseIndex, PoseName, Values.Num(), OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_pose"), PoseAssetId, PoseId, PoseAsset.GetPathName(), TEXT("PoseAsset contains this named pose."), OutRelations);

		TArray<TSharedPtr<FJsonValue>> PoseCurveValues;
		const int32 CurveValueCount = FMath::Min(CurveNames.Num(), Values.Num());
		for (int32 CurveIndex = 0; CurveIndex < CurveValueCount; ++CurveIndex)
		{
			PoseCurveValues.Add(MakeShared<FJsonValueObject>(PoseCurveValueJson(CurveNames[CurveIndex], CurveIndex, Values[CurveIndex])));
		}

		TSharedRef<FJsonObject> PoseObject = MakeShared<FJsonObject>();
		PoseObject->SetStringField(TEXT("id"), PoseId);
		PoseObject->SetNumberField(TEXT("index"), PoseIndex);
		PoseObject->SetStringField(TEXT("name"), PoseName.ToString());
		PoseObject->SetNumberField(TEXT("curve_value_count"), PoseCurveValues.Num());
		PoseObject->SetArrayField(TEXT("curve_values"), PoseCurveValues);
		PoseValues.Add(MakeShared<FJsonValueObject>(PoseObject));
	}

	TArray<TSharedPtr<FJsonValue>> TrackValues;
	for (int32 TrackIndex = 0; TrackIndex < TrackNames.Num(); ++TrackIndex)
	{
		TSharedRef<FJsonObject> TrackObject = MakeShared<FJsonObject>();
		TrackObject->SetNumberField(TEXT("index"), TrackIndex);
		TrackObject->SetStringField(TEXT("bone_name"), TrackNames[TrackIndex].ToString());
		TrackValues.Add(MakeShared<FJsonValueObject>(TrackObject));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.pose_asset.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::AnimationDataModel));
	Object->SetStringField(TEXT("pose_asset_path"), PoseAsset.GetPathName());
	Object->SetStringField(TEXT("pose_asset_class"), PoseAsset.GetClass()->GetPathName());
	Object->SetStringField(TEXT("skeleton_path"), PoseAsset.GetSkeleton() ? PoseAsset.GetSkeleton()->GetPathName() : FString());
	Object->SetBoolField(TEXT("additive"), PoseAsset.IsValidAdditive());
	Object->SetNumberField(TEXT("base_pose_index"), PoseAsset.GetBasePoseIndex());
	const int32 BasePoseIndex = PoseAsset.GetBasePoseIndex();
	Object->SetStringField(TEXT("base_pose_name"), PoseNames.IsValidIndex(BasePoseIndex) ? PoseNames[BasePoseIndex].ToString() : FString());
	Object->SetStringField(TEXT("retarget_source"), PoseAsset.RetargetSource.ToString());
#if WITH_EDITORONLY_DATA
	Object->SetStringField(TEXT("source_animation_path"), PoseAsset.SourceAnimation ? PoseAsset.SourceAnimation->GetPathName() : FString());
	Object->SetStringField(TEXT("source_animation_raw_data_guid"), PoseAsset.SourceAnimationRawDataGUID.IsValid() ? PoseAsset.SourceAnimationRawDataGUID.ToString(EGuidFormats::DigitsWithHyphens) : FString());
#else
	Object->SetStringField(TEXT("source_animation_path"), FString());
	Object->SetStringField(TEXT("source_animation_raw_data_guid"), FString());
#endif
	Object->SetNumberField(TEXT("pose_count"), PoseValues.Num());
	Object->SetNumberField(TEXT("curve_count"), CurveValues.Num());
	Object->SetNumberField(TEXT("track_count"), TrackValues.Num());
	Object->SetArrayField(TEXT("poses"), PoseValues);
	Object->SetArrayField(TEXT("curves"), CurveValues);
	Object->SetArrayField(TEXT("tracks"), TrackValues);
	return Object;
}

FString PhysicsAssetPreviewMeshPath(const UPhysicsAsset& PhysicsAsset)
{
#if WITH_EDITORONLY_DATA
	return PhysicsAsset.PreviewSkeletalMesh.ToSoftObjectPath().ToString();
#else
	return FString();
#endif
}

FString PhysicsShapeKey(const UPhysicsAsset& PhysicsAsset, int32 BodyIndex, const FString& ShapeType, int32 ShapeIndex)
{
	return FString::Printf(TEXT("%s:physics_shape:%d:%s:%d"), *PhysicsAsset.GetPathName(), BodyIndex, *ShapeType, ShapeIndex);
}

FString PhysicsBodyKey(const UPhysicsAsset& PhysicsAsset, int32 BodyIndex, const USkeletalBodySetup& BodySetup)
{
	return FString::Printf(TEXT("%s:physics_body:%d:%s"), *PhysicsAsset.GetPathName(), BodyIndex, *BodySetup.BoneName.ToString());
}

FString PhysicsConstraintKey(const UPhysicsAsset& PhysicsAsset, int32 ConstraintIndex, const FConstraintInstance& Constraint)
{
	return FString::Printf(TEXT("%s:physics_constraint:%d:%s"), *PhysicsAsset.GetPathName(), ConstraintIndex, *Constraint.JointName.ToString());
}

int32 PhysicsShapeCount(const FKAggregateGeom& AggGeom)
{
	return AggGeom.SphereElems.Num()
		+ AggGeom.BoxElems.Num()
		+ AggGeom.SphylElems.Num()
		+ AggGeom.ConvexElems.Num()
		+ AggGeom.TaperedCapsuleElems.Num()
		+ AggGeom.LevelSetElems.Num()
		+ AggGeom.SkinnedLevelSetElems.Num();
}

int32 PhysicsAssetShapeCount(const UPhysicsAsset& PhysicsAsset)
{
	int32 ShapeCount = 0;
	for (const TObjectPtr<USkeletalBodySetup>& BodySetup : PhysicsAsset.SkeletalBodySetups)
	{
		if (BodySetup)
		{
			ShapeCount += PhysicsShapeCount(BodySetup->AggGeom);
		}
	}
	return ShapeCount;
}

FString PhysicsTypeString(const TEnumAsByte<EPhysicsType>& PhysicsType)
{
	switch (PhysicsType.GetValue())
	{
	case PhysType_Default:
		return TEXT("PhysType_Default");
	case PhysType_Kinematic:
		return TEXT("PhysType_Kinematic");
	case PhysType_Simulated:
		return TEXT("PhysType_Simulated");
	default:
		return FString::FromInt(static_cast<int32>(PhysicsType.GetValue()));
	}
}

FString CollisionTraceString(const TEnumAsByte<ECollisionTraceFlag>& CollisionTraceFlag)
{
	switch (CollisionTraceFlag.GetValue())
	{
	case CTF_UseDefault:
		return TEXT("CTF_UseDefault");
	case CTF_UseSimpleAndComplex:
		return TEXT("CTF_UseSimpleAndComplex");
	case CTF_UseSimpleAsComplex:
		return TEXT("CTF_UseSimpleAsComplex");
	case CTF_UseComplexAsSimple:
		return TEXT("CTF_UseComplexAsSimple");
	default:
		return FString::FromInt(static_cast<int32>(CollisionTraceFlag.GetValue()));
	}
}

FString BodyCollisionResponseString(const TEnumAsByte<EBodyCollisionResponse::Type>& CollisionResponse)
{
	switch (CollisionResponse.GetValue())
	{
	case EBodyCollisionResponse::BodyCollision_Enabled:
		return TEXT("BodyCollision_Enabled");
	case EBodyCollisionResponse::BodyCollision_Disabled:
		return TEXT("BodyCollision_Disabled");
	default:
		return FString::FromInt(static_cast<int32>(CollisionResponse.GetValue()));
	}
}

FString LinearMotionString(TEnumAsByte<ELinearConstraintMotion> Motion)
{
	switch (Motion.GetValue())
	{
	case LCM_Free:
		return TEXT("LCM_Free");
	case LCM_Limited:
		return TEXT("LCM_Limited");
	case LCM_Locked:
		return TEXT("LCM_Locked");
	default:
		return FString::FromInt(static_cast<int32>(Motion.GetValue()));
	}
}

FString AngularMotionString(TEnumAsByte<EAngularConstraintMotion> Motion)
{
	switch (Motion.GetValue())
	{
	case ACM_Free:
		return TEXT("ACM_Free");
	case ACM_Limited:
		return TEXT("ACM_Limited");
	case ACM_Locked:
		return TEXT("ACM_Locked");
	default:
		return FString::FromInt(static_cast<int32>(Motion.GetValue()));
	}
}

FString AddPhysicsAssetEntity(
	const FString& ProjectId,
	const UPhysicsAsset& PhysicsAsset,
	TArray<FEntityRecord>& OutEntities)
{
	const FString PhysicsAssetPath = PhysicsAsset.GetPathName();
	const FString PhysicsAssetId = MakeStableId(ProjectId, TEXT("physics_asset"), PhysicsAssetPath);
	if (FindEntity(OutEntities, PhysicsAssetId))
	{
		return PhysicsAssetId;
	}

	FEntityRecord Entity;
	Entity.Id = PhysicsAssetId;
	Entity.Kind = TEXT("physics_asset");
	Entity.CanonicalKey = PhysicsAssetPath;
	Entity.DisplayName = PhysicsAsset.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("physics_asset_path"), PhysicsAssetPath);
	Entity.Attributes.Add(TEXT("physics_asset_class"), PhysicsAsset.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("preview_skeletal_mesh_path"), PhysicsAssetPreviewMeshPath(PhysicsAsset));
	Entity.Attributes.Add(TEXT("body_count"), FString::FromInt(PhysicsAsset.SkeletalBodySetups.Num()));
	Entity.Attributes.Add(TEXT("constraint_count"), FString::FromInt(PhysicsAsset.ConstraintSetup.Num()));
	Entity.Attributes.Add(TEXT("shape_count"), FString::FromInt(PhysicsAssetShapeCount(PhysicsAsset)));
	Entity.Attributes.Add(TEXT("bounds_body_count"), FString::FromInt(PhysicsAsset.BoundsBodies.Num()));
	Entity.Attributes.Add(TEXT("solver_type"), AnimEnumValue(StaticEnum<EPhysicsAssetSolverType>(), static_cast<int64>(PhysicsAsset.SolverType)));
	Entity.Attributes.Add(TEXT("not_for_dedicated_server"), AnimBool(PhysicsAsset.bNotForDedicatedServer));
#if WITH_EDITORONLY_DATA
	Entity.Attributes.Add(TEXT("physical_animation_profile_count"), FString::FromInt(PhysicsAsset.PhysicalAnimationProfiles.Num()));
	Entity.Attributes.Add(TEXT("constraint_profile_count"), FString::FromInt(PhysicsAsset.ConstraintProfiles.Num()));
#else
	Entity.Attributes.Add(TEXT("physical_animation_profile_count"), TEXT("0"));
	Entity.Attributes.Add(TEXT("constraint_profile_count"), TEXT("0"));
#endif
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("body_setups"), TEXT("collision_shape_counts"), TEXT("constraint_setups"), TEXT("solver_settings"), TEXT("preview_mesh_reference") };
	Entity.Completeness.Omitted = { TEXT("cooked_chaos_meshes"), TEXT("runtime_simulation_state") };
	AddAnimEvidence(Entity, PhysicsAssetPath, TEXT("UPhysicsAsset structural metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return PhysicsAssetId;
}

FString AddPhysicsBodyEntity(
	const FString& ProjectId,
	const UPhysicsAsset& PhysicsAsset,
	const USkeletalBodySetup& BodySetup,
	int32 BodyIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = PhysicsBodyKey(PhysicsAsset, BodyIndex, BodySetup);
	const FString BodyId = MakeStableId(ProjectId, TEXT("physics_body"), CanonicalKey);
	if (FindEntity(OutEntities, BodyId))
	{
		return BodyId;
	}

	FEntityRecord Entity;
	Entity.Id = BodyId;
	Entity.Kind = TEXT("physics_body");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = BodySetup.BoneName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("physics_asset_path"), PhysicsAsset.GetPathName());
	Entity.Attributes.Add(TEXT("body_index"), FString::FromInt(BodyIndex));
	Entity.Attributes.Add(TEXT("bone_name"), BodySetup.BoneName.ToString());
	Entity.Attributes.Add(TEXT("physics_type"), PhysicsTypeString(BodySetup.PhysicsType));
	Entity.Attributes.Add(TEXT("collision_trace_flag"), CollisionTraceString(BodySetup.CollisionTraceFlag));
	Entity.Attributes.Add(TEXT("collision_response"), BodyCollisionResponseString(BodySetup.CollisionReponse));
	Entity.Attributes.Add(TEXT("shape_count"), FString::FromInt(PhysicsShapeCount(BodySetup.AggGeom)));
	Entity.Attributes.Add(TEXT("consider_for_bounds"), AnimBool(BodySetup.bConsiderForBounds));
	Entity.Attributes.Add(TEXT("skip_scale_from_animation"), AnimBool(BodySetup.bSkipScaleFromAnimation));
	Entity.Attributes.Add(TEXT("physical_animation_profile_count"), FString::FromInt(BodySetup.GetPhysicalAnimationProfiles().Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("body_bone_binding"), TEXT("collision_settings"), TEXT("shape_counts"), TEXT("physical_animation_profiles") };
	Entity.Completeness.Omitted = { TEXT("cooked_collision_geometry"), TEXT("runtime_body_state") };
	AddAnimEvidence(Entity, PhysicsAsset.GetPathName(), TEXT("Physics body setup read from USkeletalBodySetup."));
	OutEntities.Add(MoveTemp(Entity));
	return BodyId;
}

FString AddPhysicsShapeEntity(
	const FString& ProjectId,
	const UPhysicsAsset& PhysicsAsset,
	const USkeletalBodySetup& BodySetup,
	int32 BodyIndex,
	const FString& ShapeType,
	int32 ShapeIndex,
	const TMap<FString, FString>& ExtraAttributes,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = PhysicsShapeKey(PhysicsAsset, BodyIndex, ShapeType, ShapeIndex);
	const FString ShapeId = MakeStableId(ProjectId, TEXT("physics_shape"), CanonicalKey);
	if (FindEntity(OutEntities, ShapeId))
	{
		return ShapeId;
	}

	FEntityRecord Entity;
	Entity.Id = ShapeId;
	Entity.Kind = TEXT("physics_shape");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = FString::Printf(TEXT("%s %d"), *ShapeType, ShapeIndex);
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("physics_asset_path"), PhysicsAsset.GetPathName());
	Entity.Attributes.Add(TEXT("body_index"), FString::FromInt(BodyIndex));
	Entity.Attributes.Add(TEXT("body_bone_name"), BodySetup.BoneName.ToString());
	Entity.Attributes.Add(TEXT("shape_type"), ShapeType);
	Entity.Attributes.Add(TEXT("shape_index"), FString::FromInt(ShapeIndex));
	for (const TPair<FString, FString>& Pair : ExtraAttributes)
	{
		Entity.Attributes.Add(Pair.Key, Pair.Value);
	}
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("shape_type"), TEXT("shape_dimensions") };
	Entity.Completeness.Omitted = { TEXT("cooked_collision_mesh") };
	AddAnimEvidence(Entity, PhysicsAsset.GetPathName(), TEXT("Physics collision shape read from aggregate geometry."));
	OutEntities.Add(MoveTemp(Entity));
	return ShapeId;
}

FString AddPhysicsConstraintEntity(
	const FString& ProjectId,
	const UPhysicsAsset& PhysicsAsset,
	const UPhysicsConstraintTemplate& ConstraintTemplate,
	int32 ConstraintIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FConstraintInstance& Constraint = ConstraintTemplate.DefaultInstance;
	const FString CanonicalKey = PhysicsConstraintKey(PhysicsAsset, ConstraintIndex, Constraint);
	const FString ConstraintId = MakeStableId(ProjectId, TEXT("physics_constraint"), CanonicalKey);
	if (FindEntity(OutEntities, ConstraintId))
	{
		return ConstraintId;
	}

	const FConstraintProfileProperties& Profile = Constraint.ProfileInstance;
	FEntityRecord Entity;
	Entity.Id = ConstraintId;
	Entity.Kind = TEXT("physics_constraint");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Constraint.JointName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("physics_asset_path"), PhysicsAsset.GetPathName());
	Entity.Attributes.Add(TEXT("constraint_index"), FString::FromInt(ConstraintIndex));
	Entity.Attributes.Add(TEXT("joint_name"), Constraint.JointName.ToString());
	Entity.Attributes.Add(TEXT("child_bone_name"), Constraint.GetChildBoneName().ToString());
	Entity.Attributes.Add(TEXT("parent_bone_name"), Constraint.GetParentBoneName().ToString());
	Entity.Attributes.Add(TEXT("disable_collision"), AnimBool(Profile.bDisableCollision));
	Entity.Attributes.Add(TEXT("parent_dominates"), AnimBool(Profile.bParentDominates));
	Entity.Attributes.Add(TEXT("linear_x_motion"), LinearMotionString(Profile.LinearLimit.XMotion));
	Entity.Attributes.Add(TEXT("linear_y_motion"), LinearMotionString(Profile.LinearLimit.YMotion));
	Entity.Attributes.Add(TEXT("linear_z_motion"), LinearMotionString(Profile.LinearLimit.ZMotion));
	Entity.Attributes.Add(TEXT("linear_limit"), AnimNumber(Profile.LinearLimit.Limit));
	Entity.Attributes.Add(TEXT("swing1_motion"), AngularMotionString(Profile.ConeLimit.Swing1Motion));
	Entity.Attributes.Add(TEXT("swing2_motion"), AngularMotionString(Profile.ConeLimit.Swing2Motion));
	Entity.Attributes.Add(TEXT("twist_motion"), AngularMotionString(Profile.TwistLimit.TwistMotion));
	Entity.Attributes.Add(TEXT("swing1_limit_degrees"), AnimNumber(Profile.ConeLimit.Swing1LimitDegrees));
	Entity.Attributes.Add(TEXT("swing2_limit_degrees"), AnimNumber(Profile.ConeLimit.Swing2LimitDegrees));
	Entity.Attributes.Add(TEXT("twist_limit_degrees"), AnimNumber(Profile.TwistLimit.TwistLimitDegrees));
	Entity.Attributes.Add(TEXT("constraint_profile_count"), FString::FromInt(ConstraintTemplate.ProfileHandles.Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("bone_bindings"), TEXT("linear_limits"), TEXT("angular_limits"), TEXT("profile_count") };
	Entity.Completeness.Omitted = { TEXT("runtime_constraint_state"), TEXT("drive_targets") };
	AddAnimEvidence(Entity, PhysicsAsset.GetPathName(), TEXT("Physics constraint template read from UPhysicsConstraintTemplate."));
	OutEntities.Add(MoveTemp(Entity));
	return ConstraintId;
}

TSharedRef<FJsonObject> PhysicsShapeBaseJson(const FString& ShapeId, const FString& ShapeType, int32 ShapeIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), ShapeId);
	Object->SetNumberField(TEXT("index"), ShapeIndex);
	Object->SetStringField(TEXT("type"), ShapeType);
	return Object;
}

void AddShapeAttributes(TMap<FString, FString>& Attributes, const FString& Key, double Value)
{
	Attributes.Add(Key, AnimNumber(Value));
}

TSharedRef<FJsonObject> PhysicsShapeJson(
	const FString& ProjectId,
	const UPhysicsAsset& PhysicsAsset,
	const USkeletalBodySetup& BodySetup,
	int32 BodyIndex,
	const FString& ShapeType,
	int32 ShapeIndex,
	const TFunctionRef<void(TSharedRef<FJsonObject>, TMap<FString, FString>&)>& FillShape,
	TArray<FEntityRecord>& OutEntities)
{
	TMap<FString, FString> ExtraAttributes;
	TSharedRef<FJsonObject> Object = PhysicsShapeBaseJson(TEXT(""), ShapeType, ShapeIndex);
	FillShape(Object, ExtraAttributes);
	const FString ShapeId = AddPhysicsShapeEntity(ProjectId, PhysicsAsset, BodySetup, BodyIndex, ShapeType, ShapeIndex, ExtraAttributes, OutEntities);
	Object->SetStringField(TEXT("id"), ShapeId);
	return Object;
}

TArray<TSharedPtr<FJsonValue>> PhysicsShapeJsonArray(
	const FString& ProjectId,
	const UPhysicsAsset& PhysicsAsset,
	const USkeletalBodySetup& BodySetup,
	int32 BodyIndex,
	TArray<FEntityRecord>& OutEntities,
	const FString& BodyId,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> ShapeValues;
	const FKAggregateGeom& AggGeom = BodySetup.AggGeom;

	for (int32 ShapeIndex = 0; ShapeIndex < AggGeom.SphereElems.Num(); ++ShapeIndex)
	{
		const FKSphereElem& Sphere = AggGeom.SphereElems[ShapeIndex];
		TSharedRef<FJsonObject> ShapeObject = PhysicsShapeJson(ProjectId, PhysicsAsset, BodySetup, BodyIndex, TEXT("sphere"), ShapeIndex,
			[&Sphere](TSharedRef<FJsonObject> Object, TMap<FString, FString>& Attributes)
			{
				Object->SetObjectField(TEXT("center"), AnimVectorObject(Sphere.Center));
				Object->SetNumberField(TEXT("radius"), Sphere.Radius);
				AddShapeAttributes(Attributes, TEXT("radius"), Sphere.Radius);
			},
			OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_physics_shape"), BodyId, ShapeObject->GetStringField(TEXT("id")), PhysicsAsset.GetPathName(), TEXT("Physics body contains this sphere shape."), OutRelations);
		ShapeValues.Add(MakeShared<FJsonValueObject>(ShapeObject));
	}

	for (int32 ShapeIndex = 0; ShapeIndex < AggGeom.BoxElems.Num(); ++ShapeIndex)
	{
		const FKBoxElem& Box = AggGeom.BoxElems[ShapeIndex];
		TSharedRef<FJsonObject> ShapeObject = PhysicsShapeJson(ProjectId, PhysicsAsset, BodySetup, BodyIndex, TEXT("box"), ShapeIndex,
			[&Box](TSharedRef<FJsonObject> Object, TMap<FString, FString>& Attributes)
			{
				Object->SetObjectField(TEXT("center"), AnimVectorObject(Box.Center));
				Object->SetObjectField(TEXT("rotation"), AnimRotatorObject(Box.Rotation));
				Object->SetObjectField(TEXT("extent"), AnimVectorObject(FVector(Box.X, Box.Y, Box.Z)));
				AddShapeAttributes(Attributes, TEXT("extent_x"), Box.X);
				AddShapeAttributes(Attributes, TEXT("extent_y"), Box.Y);
				AddShapeAttributes(Attributes, TEXT("extent_z"), Box.Z);
			},
			OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_physics_shape"), BodyId, ShapeObject->GetStringField(TEXT("id")), PhysicsAsset.GetPathName(), TEXT("Physics body contains this box shape."), OutRelations);
		ShapeValues.Add(MakeShared<FJsonValueObject>(ShapeObject));
	}

	for (int32 ShapeIndex = 0; ShapeIndex < AggGeom.SphylElems.Num(); ++ShapeIndex)
	{
		const FKSphylElem& Capsule = AggGeom.SphylElems[ShapeIndex];
		TSharedRef<FJsonObject> ShapeObject = PhysicsShapeJson(ProjectId, PhysicsAsset, BodySetup, BodyIndex, TEXT("capsule"), ShapeIndex,
			[&Capsule](TSharedRef<FJsonObject> Object, TMap<FString, FString>& Attributes)
			{
				Object->SetObjectField(TEXT("center"), AnimVectorObject(Capsule.Center));
				Object->SetObjectField(TEXT("rotation"), AnimRotatorObject(Capsule.Rotation));
				Object->SetNumberField(TEXT("radius"), Capsule.Radius);
				Object->SetNumberField(TEXT("length"), Capsule.Length);
				AddShapeAttributes(Attributes, TEXT("radius"), Capsule.Radius);
				AddShapeAttributes(Attributes, TEXT("length"), Capsule.Length);
			},
			OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_physics_shape"), BodyId, ShapeObject->GetStringField(TEXT("id")), PhysicsAsset.GetPathName(), TEXT("Physics body contains this capsule shape."), OutRelations);
		ShapeValues.Add(MakeShared<FJsonValueObject>(ShapeObject));
	}

	for (int32 ShapeIndex = 0; ShapeIndex < AggGeom.ConvexElems.Num(); ++ShapeIndex)
	{
		const FKConvexElem& Convex = AggGeom.ConvexElems[ShapeIndex];
		TSharedRef<FJsonObject> ShapeObject = PhysicsShapeJson(ProjectId, PhysicsAsset, BodySetup, BodyIndex, TEXT("convex"), ShapeIndex,
			[&Convex](TSharedRef<FJsonObject> Object, TMap<FString, FString>& Attributes)
			{
				Object->SetNumberField(TEXT("vertex_count"), Convex.VertexData.Num());
				Object->SetNumberField(TEXT("index_count"), Convex.IndexData.Num());
				AddShapeAttributes(Attributes, TEXT("vertex_count"), Convex.VertexData.Num());
				AddShapeAttributes(Attributes, TEXT("index_count"), Convex.IndexData.Num());
			},
			OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_physics_shape"), BodyId, ShapeObject->GetStringField(TEXT("id")), PhysicsAsset.GetPathName(), TEXT("Physics body contains this convex shape."), OutRelations);
		ShapeValues.Add(MakeShared<FJsonValueObject>(ShapeObject));
	}

	for (int32 ShapeIndex = 0; ShapeIndex < AggGeom.TaperedCapsuleElems.Num(); ++ShapeIndex)
	{
		const FKTaperedCapsuleElem& TaperedCapsule = AggGeom.TaperedCapsuleElems[ShapeIndex];
		TSharedRef<FJsonObject> ShapeObject = PhysicsShapeJson(ProjectId, PhysicsAsset, BodySetup, BodyIndex, TEXT("tapered_capsule"), ShapeIndex,
			[&TaperedCapsule](TSharedRef<FJsonObject> Object, TMap<FString, FString>& Attributes)
			{
				Object->SetObjectField(TEXT("center"), AnimVectorObject(TaperedCapsule.Center));
				Object->SetObjectField(TEXT("rotation"), AnimRotatorObject(TaperedCapsule.Rotation));
				Object->SetNumberField(TEXT("radius0"), TaperedCapsule.Radius0);
				Object->SetNumberField(TEXT("radius1"), TaperedCapsule.Radius1);
				Object->SetNumberField(TEXT("length"), TaperedCapsule.Length);
				AddShapeAttributes(Attributes, TEXT("radius0"), TaperedCapsule.Radius0);
				AddShapeAttributes(Attributes, TEXT("radius1"), TaperedCapsule.Radius1);
				AddShapeAttributes(Attributes, TEXT("length"), TaperedCapsule.Length);
			},
			OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_physics_shape"), BodyId, ShapeObject->GetStringField(TEXT("id")), PhysicsAsset.GetPathName(), TEXT("Physics body contains this tapered capsule shape."), OutRelations);
		ShapeValues.Add(MakeShared<FJsonValueObject>(ShapeObject));
	}

	return ShapeValues;
}

TSharedRef<FJsonObject> PhysicsBodyJson(
	const FString& ProjectId,
	const UPhysicsAsset& PhysicsAsset,
	const USkeletalBodySetup& BodySetup,
	int32 BodyIndex,
	const FString& BodyId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> Shapes = PhysicsShapeJsonArray(ProjectId, PhysicsAsset, BodySetup, BodyIndex, OutEntities, BodyId, OutRelations);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), BodyId);
	Object->SetNumberField(TEXT("index"), BodyIndex);
	Object->SetStringField(TEXT("bone_name"), BodySetup.BoneName.ToString());
	Object->SetStringField(TEXT("physics_type"), PhysicsTypeString(BodySetup.PhysicsType));
	Object->SetStringField(TEXT("collision_trace_flag"), CollisionTraceString(BodySetup.CollisionTraceFlag));
	Object->SetStringField(TEXT("collision_response"), BodyCollisionResponseString(BodySetup.CollisionReponse));
	Object->SetBoolField(TEXT("consider_for_bounds"), BodySetup.bConsiderForBounds);
	Object->SetBoolField(TEXT("skip_scale_from_animation"), BodySetup.bSkipScaleFromAnimation);
	Object->SetNumberField(TEXT("physical_animation_profile_count"), BodySetup.GetPhysicalAnimationProfiles().Num());
	Object->SetNumberField(TEXT("shape_count"), Shapes.Num());
	Object->SetArrayField(TEXT("shapes"), Shapes);
	return Object;
}

TSharedRef<FJsonObject> PhysicsConstraintJson(const UPhysicsConstraintTemplate& ConstraintTemplate, int32 ConstraintIndex, const FString& ConstraintId)
{
	const FConstraintInstance& Constraint = ConstraintTemplate.DefaultInstance;
	const FConstraintProfileProperties& Profile = Constraint.ProfileInstance;

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), ConstraintId);
	Object->SetNumberField(TEXT("index"), ConstraintIndex);
	Object->SetStringField(TEXT("joint_name"), Constraint.JointName.ToString());
	Object->SetStringField(TEXT("child_bone_name"), Constraint.GetChildBoneName().ToString());
	Object->SetStringField(TEXT("parent_bone_name"), Constraint.GetParentBoneName().ToString());
	Object->SetBoolField(TEXT("disable_collision"), Profile.bDisableCollision);
	Object->SetBoolField(TEXT("parent_dominates"), Profile.bParentDominates);
	Object->SetStringField(TEXT("linear_x_motion"), LinearMotionString(Profile.LinearLimit.XMotion));
	Object->SetStringField(TEXT("linear_y_motion"), LinearMotionString(Profile.LinearLimit.YMotion));
	Object->SetStringField(TEXT("linear_z_motion"), LinearMotionString(Profile.LinearLimit.ZMotion));
	Object->SetNumberField(TEXT("linear_limit"), Profile.LinearLimit.Limit);
	Object->SetStringField(TEXT("swing1_motion"), AngularMotionString(Profile.ConeLimit.Swing1Motion));
	Object->SetStringField(TEXT("swing2_motion"), AngularMotionString(Profile.ConeLimit.Swing2Motion));
	Object->SetStringField(TEXT("twist_motion"), AngularMotionString(Profile.TwistLimit.TwistMotion));
	Object->SetNumberField(TEXT("swing1_limit_degrees"), Profile.ConeLimit.Swing1LimitDegrees);
	Object->SetNumberField(TEXT("swing2_limit_degrees"), Profile.ConeLimit.Swing2LimitDegrees);
	Object->SetNumberField(TEXT("twist_limit_degrees"), Profile.TwistLimit.TwistLimitDegrees);
	Object->SetNumberField(TEXT("constraint_profile_count"), ConstraintTemplate.ProfileHandles.Num());
	return Object;
}

TSharedRef<FJsonObject> PhysicsSolverSettingsJson(const UPhysicsAsset& PhysicsAsset)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("solver_type"), AnimEnumValue(StaticEnum<EPhysicsAssetSolverType>(), static_cast<int64>(PhysicsAsset.SolverType)));
	Object->SetNumberField(TEXT("position_iterations"), PhysicsAsset.SolverSettings.PositionIterations);
	Object->SetNumberField(TEXT("velocity_iterations"), PhysicsAsset.SolverSettings.VelocityIterations);
	Object->SetNumberField(TEXT("projection_iterations"), PhysicsAsset.SolverSettings.ProjectionIterations);
	Object->SetNumberField(TEXT("cull_distance"), PhysicsAsset.SolverSettings.CullDistance);
	Object->SetNumberField(TEXT("max_depenetration_velocity"), PhysicsAsset.SolverSettings.MaxDepenetrationVelocity);
	Object->SetNumberField(TEXT("fixed_time_step"), PhysicsAsset.SolverSettings.FixedTimeStep);
	Object->SetBoolField(TEXT("use_linear_joint_solver"), PhysicsAsset.SolverSettings.bUseLinearJointSolver);
	return Object;
}

TSharedRef<FJsonObject> PhysicsAssetSnapshot(
	const FString& ProjectId,
	const UPhysicsAsset& PhysicsAsset,
	const FString& PhysicsAssetId,
	const TMap<FName, FString>& BoneNameToId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TMap<FName, FString> BodyNameToId;
	TArray<TSharedPtr<FJsonValue>> BodyValues;
	for (int32 BodyIndex = 0; BodyIndex < PhysicsAsset.SkeletalBodySetups.Num(); ++BodyIndex)
	{
		const USkeletalBodySetup* BodySetup = PhysicsAsset.SkeletalBodySetups[BodyIndex].Get();
		if (!BodySetup)
		{
			continue;
		}

		const FString BodyId = AddPhysicsBodyEntity(ProjectId, PhysicsAsset, *BodySetup, BodyIndex, OutEntities);
		BodyNameToId.Add(BodySetup->BoneName, BodyId);
		AddAnimRelation(ProjectId, TEXT("contains_physics_body"), PhysicsAssetId, BodyId, PhysicsAsset.GetPathName(), TEXT("PhysicsAsset contains this skeletal body setup."), OutRelations);
		if (const FString* BoneId = BoneNameToId.Find(BodySetup->BoneName))
		{
			AddAnimRelation(ProjectId, TEXT("binds_bone"), BodyId, *BoneId, PhysicsAsset.GetPathName(), TEXT("Physics body is bound to this skeleton bone."), OutRelations);
		}
		BodyValues.Add(MakeShared<FJsonValueObject>(PhysicsBodyJson(ProjectId, PhysicsAsset, *BodySetup, BodyIndex, BodyId, OutEntities, OutRelations)));
	}

	TArray<TSharedPtr<FJsonValue>> ConstraintValues;
	for (int32 ConstraintIndex = 0; ConstraintIndex < PhysicsAsset.ConstraintSetup.Num(); ++ConstraintIndex)
	{
		const UPhysicsConstraintTemplate* ConstraintTemplate = PhysicsAsset.ConstraintSetup[ConstraintIndex].Get();
		if (!ConstraintTemplate)
		{
			continue;
		}

		const FConstraintInstance& Constraint = ConstraintTemplate->DefaultInstance;
		const FString ConstraintId = AddPhysicsConstraintEntity(ProjectId, PhysicsAsset, *ConstraintTemplate, ConstraintIndex, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_physics_constraint"), PhysicsAssetId, ConstraintId, PhysicsAsset.GetPathName(), TEXT("PhysicsAsset contains this constraint setup."), OutRelations);
		if (const FString* ChildBodyId = BodyNameToId.Find(Constraint.GetChildBoneName()))
		{
			AddAnimRelation(ProjectId, TEXT("constrains_body"), ConstraintId, *ChildBodyId, PhysicsAsset.GetPathName(), TEXT("Physics constraint references this child body."), OutRelations);
		}
		if (const FString* ParentBodyId = BodyNameToId.Find(Constraint.GetParentBoneName()))
		{
			AddAnimRelation(ProjectId, TEXT("constrains_body"), ConstraintId, *ParentBodyId, PhysicsAsset.GetPathName(), TEXT("Physics constraint references this parent body."), OutRelations);
		}
		ConstraintValues.Add(MakeShared<FJsonValueObject>(PhysicsConstraintJson(*ConstraintTemplate, ConstraintIndex, ConstraintId)));
	}

#if WITH_EDITORONLY_DATA
	const int32 PhysicalAnimationProfileCount = PhysicsAsset.PhysicalAnimationProfiles.Num();
	const int32 ConstraintProfileCount = PhysicsAsset.ConstraintProfiles.Num();
#else
	const int32 PhysicalAnimationProfileCount = 0;
	const int32 ConstraintProfileCount = 0;
#endif

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.physics_asset.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::AnimationDataModel));
	Object->SetStringField(TEXT("physics_asset_path"), PhysicsAsset.GetPathName());
	Object->SetStringField(TEXT("physics_asset_class"), PhysicsAsset.GetClass()->GetPathName());
	Object->SetStringField(TEXT("preview_skeletal_mesh_path"), PhysicsAssetPreviewMeshPath(PhysicsAsset));
	Object->SetNumberField(TEXT("body_count"), BodyValues.Num());
	Object->SetNumberField(TEXT("constraint_count"), ConstraintValues.Num());
	Object->SetNumberField(TEXT("shape_count"), PhysicsAssetShapeCount(PhysicsAsset));
	Object->SetNumberField(TEXT("bounds_body_count"), PhysicsAsset.BoundsBodies.Num());
	Object->SetNumberField(TEXT("physical_animation_profile_count"), PhysicalAnimationProfileCount);
	Object->SetNumberField(TEXT("constraint_profile_count"), ConstraintProfileCount);
	Object->SetBoolField(TEXT("not_for_dedicated_server"), PhysicsAsset.bNotForDedicatedServer);
	Object->SetObjectField(TEXT("solver_settings"), PhysicsSolverSettingsJson(PhysicsAsset));
	Object->SetArrayField(TEXT("bodies"), BodyValues);
	Object->SetArrayField(TEXT("constraints"), ConstraintValues);
	return Object;
}

#if UEPI_WITH_IK_RIG

FString IKRigPreviewMeshPath(const UIKRigDefinition& IKRig)
{
	return IKRig.PreviewSkeletalMesh.ToSoftObjectPath().ToString();
}

FString AddIKRigEntity(
	const FString& ProjectId,
	const UIKRigDefinition& IKRig,
	TArray<FEntityRecord>& OutEntities)
{
	const FString IKRigPath = IKRig.GetPathName();
	const FString IKRigId = MakeStableId(ProjectId, TEXT("ik_rig"), IKRigPath);
	if (FindEntity(OutEntities, IKRigId))
	{
		return IKRigId;
	}

	const FIKRigSkeleton& RigSkeleton = IKRig.GetSkeleton();
	FEntityRecord Entity;
	Entity.Id = IKRigId;
	Entity.Kind = TEXT("ik_rig");
	Entity.CanonicalKey = IKRigPath;
	Entity.DisplayName = IKRig.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("ik_rig_path"), IKRigPath);
	Entity.Attributes.Add(TEXT("preview_skeletal_mesh_path"), IKRigPreviewMeshPath(IKRig));
	Entity.Attributes.Add(TEXT("retarget_root"), IKRig.GetRetargetRoot().ToString());
	Entity.Attributes.Add(TEXT("bone_count"), FString::FromInt(RigSkeleton.BoneNames.Num()));
	Entity.Attributes.Add(TEXT("excluded_bone_count"), FString::FromInt(RigSkeleton.ExcludedBones.Num()));
	Entity.Attributes.Add(TEXT("goal_count"), FString::FromInt(IKRig.GetGoalArray().Num()));
	Entity.Attributes.Add(TEXT("solver_count"), FString::FromInt(IKRig.GetSolverArray().Num()));
	Entity.Attributes.Add(TEXT("chain_count"), FString::FromInt(IKRig.GetRetargetChains().Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("retarget_root"), TEXT("chains"), TEXT("goals"), TEXT("solvers"), TEXT("preview_mesh_reference") };
	Entity.Completeness.Omitted = { TEXT("runtime_ik_solve"), TEXT("evaluated_pose") };
	AddAnimEvidence(Entity, IKRigPath, TEXT("UIKRigDefinition structural metadata read through public IKRig API."));
	OutEntities.Add(MoveTemp(Entity));
	return IKRigId;
}

FString IKRigChainKey(const UIKRigDefinition& IKRig, int32 ChainIndex, const FBoneChain& Chain)
{
	return FString::Printf(TEXT("%s:ik_chain:%d:%s"), *IKRig.GetPathName(), ChainIndex, *Chain.ChainName.ToString());
}

FString AddIKRigChainEntity(
	const FString& ProjectId,
	const UIKRigDefinition& IKRig,
	const FBoneChain& Chain,
	int32 ChainIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = IKRigChainKey(IKRig, ChainIndex, Chain);
	const FString ChainId = MakeStableId(ProjectId, TEXT("ik_rig_chain"), CanonicalKey);
	if (FindEntity(OutEntities, ChainId))
	{
		return ChainId;
	}

	FEntityRecord Entity;
	Entity.Id = ChainId;
	Entity.Kind = TEXT("ik_rig_chain");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Chain.ChainName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("ik_rig_path"), IKRig.GetPathName());
	Entity.Attributes.Add(TEXT("chain_index"), FString::FromInt(ChainIndex));
	Entity.Attributes.Add(TEXT("chain_name"), Chain.ChainName.ToString());
	Entity.Attributes.Add(TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
	Entity.Attributes.Add(TEXT("end_bone"), Chain.EndBone.BoneName.ToString());
	Entity.Attributes.Add(TEXT("ik_goal_name"), Chain.IKGoalName.ToString());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Complete;
	Entity.Completeness.Covered = { TEXT("chain_metadata") };
	AddAnimEvidence(Entity, IKRig.GetPathName(), TEXT("IK Rig retarget chain read from FBoneChain data."));
	OutEntities.Add(MoveTemp(Entity));
	return ChainId;
}

FString IKRigGoalKey(const UIKRigDefinition& IKRig, int32 GoalIndex, const UIKRigEffectorGoal& Goal)
{
	return FString::Printf(TEXT("%s:ik_goal:%d:%s"), *IKRig.GetPathName(), GoalIndex, *Goal.GoalName.ToString());
}

FString AddIKRigGoalEntity(
	const FString& ProjectId,
	const UIKRigDefinition& IKRig,
	const UIKRigEffectorGoal& Goal,
	int32 GoalIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = IKRigGoalKey(IKRig, GoalIndex, Goal);
	const FString GoalId = MakeStableId(ProjectId, TEXT("ik_rig_goal"), CanonicalKey);
	if (FindEntity(OutEntities, GoalId))
	{
		return GoalId;
	}

	FEntityRecord Entity;
	Entity.Id = GoalId;
	Entity.Kind = TEXT("ik_rig_goal");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Goal.GoalName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("ik_rig_path"), IKRig.GetPathName());
	Entity.Attributes.Add(TEXT("goal_index"), FString::FromInt(GoalIndex));
	Entity.Attributes.Add(TEXT("goal_name"), Goal.GoalName.ToString());
	Entity.Attributes.Add(TEXT("bone_name"), Goal.BoneName.ToString());
	Entity.Attributes.Add(TEXT("position_alpha"), AnimNumber(Goal.PositionAlpha));
	Entity.Attributes.Add(TEXT("rotation_alpha"), AnimNumber(Goal.RotationAlpha));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("goal_metadata"), TEXT("default_transform") };
	Entity.Completeness.Omitted = { TEXT("runtime_goal_transform") };
	AddAnimEvidence(Entity, IKRig.GetPathName(), TEXT("IK Rig effector goal read from UIKRigEffectorGoal data."));
	OutEntities.Add(MoveTemp(Entity));
	return GoalId;
}

FString IKRigSolverKey(const UIKRigDefinition& IKRig, int32 SolverIndex, const UIKRigSolver& Solver)
{
	return FString::Printf(TEXT("%s:ik_solver:%d:%s"), *IKRig.GetPathName(), SolverIndex, *Solver.GetClass()->GetPathName());
}

void IKRigSolverFlags(const UIKRigSolver& Solver, bool& bOutRequiresRoot, bool& bOutRequiresEnd, bool& bOutUsesBoneSettings, int32& OutBoneSettingCount, FString& OutEndBone)
{
	TSet<FName> BonesWithSettings;
	Solver.GetBonesWithSettings(BonesWithSettings);
	OutBoneSettingCount = BonesWithSettings.Num();
#if WITH_EDITORONLY_DATA
	bOutRequiresRoot = Solver.RequiresRootBone();
	bOutRequiresEnd = Solver.RequiresEndBone();
	bOutUsesBoneSettings = Solver.UsesBoneSettings();
	OutEndBone = Solver.GetEndBone().ToString();
#else
	bOutRequiresRoot = false;
	bOutRequiresEnd = false;
	bOutUsesBoneSettings = OutBoneSettingCount > 0;
	OutEndBone = FString();
#endif
}

FString AddIKRigSolverEntity(
	const FString& ProjectId,
	const UIKRigDefinition& IKRig,
	const UIKRigSolver& Solver,
	int32 SolverIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = IKRigSolverKey(IKRig, SolverIndex, Solver);
	const FString SolverId = MakeStableId(ProjectId, TEXT("ik_rig_solver"), CanonicalKey);
	if (FindEntity(OutEntities, SolverId))
	{
		return SolverId;
	}

	bool bRequiresRoot = false;
	bool bRequiresEnd = false;
	bool bUsesBoneSettings = false;
	int32 BoneSettingCount = 0;
	FString EndBone;
	IKRigSolverFlags(Solver, bRequiresRoot, bRequiresEnd, bUsesBoneSettings, BoneSettingCount, EndBone);

	FEntityRecord Entity;
	Entity.Id = SolverId;
	Entity.Kind = TEXT("ik_rig_solver");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Solver.GetClass()->GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("ik_rig_path"), IKRig.GetPathName());
	Entity.Attributes.Add(TEXT("solver_index"), FString::FromInt(SolverIndex));
	Entity.Attributes.Add(TEXT("solver_class"), Solver.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("enabled"), AnimBool(Solver.IsEnabled()));
	Entity.Attributes.Add(TEXT("root_bone"), Solver.GetRootBone().ToString());
	Entity.Attributes.Add(TEXT("end_bone"), EndBone);
	Entity.Attributes.Add(TEXT("requires_root_bone"), AnimBool(bRequiresRoot));
	Entity.Attributes.Add(TEXT("requires_end_bone"), AnimBool(bRequiresEnd));
	Entity.Attributes.Add(TEXT("uses_bone_settings"), AnimBool(bUsesBoneSettings));
	Entity.Attributes.Add(TEXT("bone_setting_count"), FString::FromInt(BoneSettingCount));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("solver_metadata"), TEXT("solver_bone_bindings") };
	Entity.Completeness.Omitted = { TEXT("runtime_solver_state") };
	AddAnimEvidence(Entity, IKRig.GetPathName(), TEXT("IK Rig solver metadata read from UIKRigSolver."));
	OutEntities.Add(MoveTemp(Entity));
	return SolverId;
}

TSharedRef<FJsonObject> IKRigChainJson(const FBoneChain& Chain, int32 ChainIndex, const FString& ChainId)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), ChainId);
	Object->SetNumberField(TEXT("index"), ChainIndex);
	Object->SetStringField(TEXT("name"), Chain.ChainName.ToString());
	Object->SetStringField(TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
	Object->SetStringField(TEXT("end_bone"), Chain.EndBone.BoneName.ToString());
	Object->SetStringField(TEXT("ik_goal_name"), Chain.IKGoalName.ToString());
	return Object;
}

TSharedRef<FJsonObject> IKRigGoalJson(const UIKRigEffectorGoal& Goal, int32 GoalIndex, const FString& GoalId)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), GoalId);
	Object->SetNumberField(TEXT("index"), GoalIndex);
	Object->SetStringField(TEXT("name"), Goal.GoalName.ToString());
	Object->SetStringField(TEXT("bone_name"), Goal.BoneName.ToString());
	Object->SetNumberField(TEXT("position_alpha"), Goal.PositionAlpha);
	Object->SetNumberField(TEXT("rotation_alpha"), Goal.RotationAlpha);
	Object->SetObjectField(TEXT("current_position"), AnimVectorObject(Goal.CurrentTransform.GetTranslation()));
	Object->SetObjectField(TEXT("initial_position"), AnimVectorObject(Goal.InitialTransform.GetTranslation()));
	return Object;
}

TSharedRef<FJsonObject> IKRigSolverJson(const UIKRigSolver& Solver, int32 SolverIndex, const FString& SolverId)
{
	bool bRequiresRoot = false;
	bool bRequiresEnd = false;
	bool bUsesBoneSettings = false;
	int32 BoneSettingCount = 0;
	FString EndBone;
	IKRigSolverFlags(Solver, bRequiresRoot, bRequiresEnd, bUsesBoneSettings, BoneSettingCount, EndBone);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), SolverId);
	Object->SetNumberField(TEXT("index"), SolverIndex);
	Object->SetStringField(TEXT("class"), Solver.GetClass()->GetPathName());
	Object->SetBoolField(TEXT("enabled"), Solver.IsEnabled());
	Object->SetStringField(TEXT("root_bone"), Solver.GetRootBone().ToString());
	Object->SetStringField(TEXT("end_bone"), EndBone);
	Object->SetBoolField(TEXT("requires_root_bone"), bRequiresRoot);
	Object->SetBoolField(TEXT("requires_end_bone"), bRequiresEnd);
	Object->SetBoolField(TEXT("uses_bone_settings"), bUsesBoneSettings);
	Object->SetNumberField(TEXT("bone_setting_count"), BoneSettingCount);
	return Object;
}

TSharedRef<FJsonObject> IKRigSnapshot(
	const FString& ProjectId,
	const UIKRigDefinition& IKRig,
	const FString& IKRigId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> ChainValues;
	const TArray<FBoneChain>& Chains = IKRig.GetRetargetChains();
	for (int32 ChainIndex = 0; ChainIndex < Chains.Num(); ++ChainIndex)
	{
		const FBoneChain& Chain = Chains[ChainIndex];
		const FString ChainId = AddIKRigChainEntity(ProjectId, IKRig, Chain, ChainIndex, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_ik_chain"), IKRigId, ChainId, IKRig.GetPathName(), TEXT("IK Rig contains this retarget chain."), OutRelations);
		ChainValues.Add(MakeShared<FJsonValueObject>(IKRigChainJson(Chain, ChainIndex, ChainId)));
	}

	TArray<TSharedPtr<FJsonValue>> GoalValues;
	const TArray<UIKRigEffectorGoal*>& Goals = IKRig.GetGoalArray();
	for (int32 GoalIndex = 0; GoalIndex < Goals.Num(); ++GoalIndex)
	{
		const UIKRigEffectorGoal* Goal = Goals[GoalIndex];
		if (!Goal)
		{
			continue;
		}
		const FString GoalId = AddIKRigGoalEntity(ProjectId, IKRig, *Goal, GoalIndex, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_ik_goal"), IKRigId, GoalId, IKRig.GetPathName(), TEXT("IK Rig contains this effector goal."), OutRelations);
		GoalValues.Add(MakeShared<FJsonValueObject>(IKRigGoalJson(*Goal, GoalIndex, GoalId)));
	}

	TArray<TSharedPtr<FJsonValue>> SolverValues;
	const TArray<UIKRigSolver*>& Solvers = IKRig.GetSolverArray();
	for (int32 SolverIndex = 0; SolverIndex < Solvers.Num(); ++SolverIndex)
	{
		const UIKRigSolver* Solver = Solvers[SolverIndex];
		if (!Solver)
		{
			continue;
		}
		const FString SolverId = AddIKRigSolverEntity(ProjectId, IKRig, *Solver, SolverIndex, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_ik_solver"), IKRigId, SolverId, IKRig.GetPathName(), TEXT("IK Rig contains this solver."), OutRelations);
		SolverValues.Add(MakeShared<FJsonValueObject>(IKRigSolverJson(*Solver, SolverIndex, SolverId)));
	}

	TArray<TSharedPtr<FJsonValue>> ExcludedBoneValues;
	const FIKRigSkeleton& RigSkeleton = IKRig.GetSkeleton();
	for (const FName& ExcludedBone : RigSkeleton.ExcludedBones)
	{
		ExcludedBoneValues.Add(MakeShared<FJsonValueString>(ExcludedBone.ToString()));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.ik_rig.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::AnimationDataModel));
	Object->SetStringField(TEXT("ik_rig_path"), IKRig.GetPathName());
	Object->SetStringField(TEXT("preview_skeletal_mesh_path"), IKRigPreviewMeshPath(IKRig));
	Object->SetStringField(TEXT("retarget_root"), IKRig.GetRetargetRoot().ToString());
	Object->SetNumberField(TEXT("bone_count"), RigSkeleton.BoneNames.Num());
	Object->SetNumberField(TEXT("excluded_bone_count"), ExcludedBoneValues.Num());
	Object->SetNumberField(TEXT("goal_count"), GoalValues.Num());
	Object->SetNumberField(TEXT("solver_count"), SolverValues.Num());
	Object->SetNumberField(TEXT("chain_count"), ChainValues.Num());
	Object->SetArrayField(TEXT("excluded_bones"), ExcludedBoneValues);
	Object->SetArrayField(TEXT("goals"), GoalValues);
	Object->SetArrayField(TEXT("solvers"), SolverValues);
	Object->SetArrayField(TEXT("chains"), ChainValues);
	return Object;
}

FString AddIKRetargeterEntity(
	const FString& ProjectId,
	const UIKRetargeter& Retargeter,
	TArray<FEntityRecord>& OutEntities)
{
	const FString RetargeterPath = Retargeter.GetPathName();
	const FString RetargeterId = MakeStableId(ProjectId, TEXT("ik_retargeter"), RetargeterPath);
	if (FindEntity(OutEntities, RetargeterId))
	{
		return RetargeterId;
	}

	const UIKRigDefinition* SourceIKRig = Retargeter.GetSourceIKRig();
	const UIKRigDefinition* TargetIKRig = Retargeter.GetTargetIKRig();
	FEntityRecord Entity;
	Entity.Id = RetargeterId;
	Entity.Kind = TEXT("ik_retargeter");
	Entity.CanonicalKey = RetargeterPath;
	Entity.DisplayName = Retargeter.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("ik_retargeter_path"), RetargeterPath);
	Entity.Attributes.Add(TEXT("source_ik_rig_path"), SourceIKRig ? SourceIKRig->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("target_ik_rig_path"), TargetIKRig ? TargetIKRig->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("chain_map_count"), FString::FromInt(Retargeter.GetAllChainSettings().Num()));
	Entity.Attributes.Add(TEXT("source_current_pose_name"), Retargeter.GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Source).ToString());
	Entity.Attributes.Add(TEXT("target_current_pose_name"), Retargeter.GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target).ToString());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("source_target_ik_rigs"), TEXT("chain_mapping"), TEXT("current_retarget_pose_metadata") };
	Entity.Completeness.Omitted = { TEXT("runtime_retarget_evaluation"), TEXT("retarget_profile_overrides") };
	AddAnimEvidence(Entity, RetargeterPath, TEXT("UIKRetargeter structural metadata read through public IKRig API."));
	OutEntities.Add(MoveTemp(Entity));
	return RetargeterId;
}

FString IKRetargetChainMapKey(const UIKRetargeter& Retargeter, const URetargetChainSettings& ChainSettings, int32 ChainIndex)
{
	return FString::Printf(TEXT("%s:ik_retarget_chain_map:%d:%s:%s"),
		*Retargeter.GetPathName(),
		ChainIndex,
		*ChainSettings.SourceChain.ToString(),
		*ChainSettings.TargetChain.ToString());
}

FString AddIKRetargetChainMapEntity(
	const FString& ProjectId,
	const UIKRetargeter& Retargeter,
	const URetargetChainSettings& ChainSettings,
	int32 ChainIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = IKRetargetChainMapKey(Retargeter, ChainSettings, ChainIndex);
	const FString ChainMapId = MakeStableId(ProjectId, TEXT("ik_retarget_chain_map"), CanonicalKey);
	if (FindEntity(OutEntities, ChainMapId))
	{
		return ChainMapId;
	}

	const FTargetChainSettings& Settings = ChainSettings.Settings;
	FEntityRecord Entity;
	Entity.Id = ChainMapId;
	Entity.Kind = TEXT("ik_retarget_chain_map");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = FString::Printf(TEXT("%s -> %s"), *ChainSettings.SourceChain.ToString(), *ChainSettings.TargetChain.ToString());
	Entity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
	Entity.Attributes.Add(TEXT("ik_retargeter_path"), Retargeter.GetPathName());
	Entity.Attributes.Add(TEXT("chain_map_index"), FString::FromInt(ChainIndex));
	Entity.Attributes.Add(TEXT("source_chain"), ChainSettings.SourceChain.ToString());
	Entity.Attributes.Add(TEXT("target_chain"), ChainSettings.TargetChain.ToString());
	Entity.Attributes.Add(TEXT("fk_enabled"), AnimBool(Settings.FK.EnableFK));
	Entity.Attributes.Add(TEXT("fk_rotation_mode"), AnimEnumValue(StaticEnum<ERetargetRotationMode>(), static_cast<int64>(Settings.FK.RotationMode)));
	Entity.Attributes.Add(TEXT("fk_translation_mode"), AnimEnumValue(StaticEnum<ERetargetTranslationMode>(), static_cast<int64>(Settings.FK.TranslationMode)));
	Entity.Attributes.Add(TEXT("ik_enabled"), AnimBool(Settings.IK.EnableIK));
	Entity.Attributes.Add(TEXT("speed_planting_enabled"), AnimBool(Settings.SpeedPlanting.EnableSpeedPlanting));
	Entity.Attributes.Add(TEXT("speed_curve_name"), Settings.SpeedPlanting.SpeedCurveName.ToString());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("chain_mapping"), TEXT("fk_settings"), TEXT("ik_settings"), TEXT("speed_planting_settings") };
	Entity.Completeness.Omitted = { TEXT("runtime_chain_pose") };
	AddAnimEvidence(Entity, Retargeter.GetPathName(), TEXT("IK Retargeter chain mapping read from URetargetChainSettings."));
	OutEntities.Add(MoveTemp(Entity));
	return ChainMapId;
}

TSharedRef<FJsonObject> IKRetargetRootSettingsJson(const URetargetRootSettings* RootSettings)
{
	const FTargetRootSettings Settings = RootSettings ? RootSettings->Settings : FTargetRootSettings();
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
	Object->SetNumberField(TEXT("translation_alpha"), Settings.TranslationAlpha);
	Object->SetNumberField(TEXT("blend_to_source"), Settings.BlendToSource);
	Object->SetObjectField(TEXT("blend_to_source_weights"), AnimVectorObject(Settings.BlendToSourceWeights));
	Object->SetNumberField(TEXT("scale_horizontal"), Settings.ScaleHorizontal);
	Object->SetNumberField(TEXT("scale_vertical"), Settings.ScaleVertical);
	Object->SetObjectField(TEXT("translation_offset"), AnimVectorObject(Settings.TranslationOffset));
	Object->SetNumberField(TEXT("affect_ik_horizontal"), Settings.AffectIKHorizontal);
	Object->SetNumberField(TEXT("affect_ik_vertical"), Settings.AffectIKVertical);
	return Object;
}

TSharedRef<FJsonObject> IKRetargetGlobalSettingsJson(const UIKRetargetGlobalSettings* GlobalSettings)
{
	const FRetargetGlobalSettings Settings = GlobalSettings ? GlobalSettings->Settings : FRetargetGlobalSettings();
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("enable_root"), Settings.bEnableRoot);
	Object->SetBoolField(TEXT("enable_fk"), Settings.bEnableFK);
	Object->SetBoolField(TEXT("enable_ik"), Settings.bEnableIK);
	Object->SetBoolField(TEXT("enable_warping"), Settings.bWarping);
	Object->SetStringField(TEXT("direction_source"), AnimEnumValue(StaticEnum<EWarpingDirectionSource>(), static_cast<int64>(Settings.DirectionSource)));
	Object->SetStringField(TEXT("forward_direction"), AnimEnumValue(StaticEnum<EBasicAxis>(), static_cast<int64>(Settings.ForwardDirection)));
	Object->SetStringField(TEXT("direction_chain"), Settings.DirectionChain.ToString());
	Object->SetNumberField(TEXT("warp_forwards"), Settings.WarpForwards);
	Object->SetNumberField(TEXT("sideways_offset"), Settings.SidewaysOffset);
	Object->SetNumberField(TEXT("warp_splay"), Settings.WarpSplay);
	return Object;
}

TSharedRef<FJsonObject> IKRetargetPoseJson(const FIKRetargetPose* Pose, const FName& PoseName)
{
	TArray<TSharedPtr<FJsonValue>> BoneOffsetValues;
	if (Pose)
	{
		TArray<FName> BoneNames;
		for (const TPair<FName, FQuat>& Pair : Pose->GetAllDeltaRotations())
		{
			BoneNames.Add(Pair.Key);
		}
		BoneNames.Sort(FNameLexicalLess());
		for (const FName& BoneName : BoneNames)
		{
			const FQuat* RotationOffset = Pose->GetAllDeltaRotations().Find(BoneName);
			if (!RotationOffset)
			{
				continue;
			}
			TSharedRef<FJsonObject> OffsetObject = MakeShared<FJsonObject>();
			OffsetObject->SetStringField(TEXT("bone_name"), BoneName.ToString());
			OffsetObject->SetObjectField(TEXT("rotation_offset"), AnimQuatObject(*RotationOffset));
			BoneOffsetValues.Add(MakeShared<FJsonValueObject>(OffsetObject));
		}
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("name"), PoseName.ToString());
	Object->SetObjectField(TEXT("root_translation_offset"), AnimVectorObject(Pose ? Pose->GetRootTranslationDelta() : FVector::ZeroVector));
	Object->SetNumberField(TEXT("bone_rotation_offset_count"), BoneOffsetValues.Num());
	Object->SetArrayField(TEXT("bone_rotation_offsets"), BoneOffsetValues);
	return Object;
}

TSharedRef<FJsonObject> IKRetargetChainMapJson(const URetargetChainSettings& ChainSettings, int32 ChainIndex, const FString& ChainMapId)
{
	const FTargetChainSettings& Settings = ChainSettings.Settings;
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), ChainMapId);
	Object->SetNumberField(TEXT("index"), ChainIndex);
	Object->SetStringField(TEXT("source_chain"), ChainSettings.SourceChain.ToString());
	Object->SetStringField(TEXT("target_chain"), ChainSettings.TargetChain.ToString());
	Object->SetBoolField(TEXT("fk_enabled"), Settings.FK.EnableFK);
	Object->SetStringField(TEXT("fk_rotation_mode"), AnimEnumValue(StaticEnum<ERetargetRotationMode>(), static_cast<int64>(Settings.FK.RotationMode)));
	Object->SetNumberField(TEXT("fk_rotation_alpha"), Settings.FK.RotationAlpha);
	Object->SetStringField(TEXT("fk_translation_mode"), AnimEnumValue(StaticEnum<ERetargetTranslationMode>(), static_cast<int64>(Settings.FK.TranslationMode)));
	Object->SetNumberField(TEXT("fk_translation_alpha"), Settings.FK.TranslationAlpha);
	Object->SetBoolField(TEXT("ik_enabled"), Settings.IK.EnableIK);
	Object->SetNumberField(TEXT("ik_blend_to_source"), Settings.IK.BlendToSource);
	Object->SetObjectField(TEXT("ik_blend_to_source_weights"), AnimVectorObject(Settings.IK.BlendToSourceWeights));
	Object->SetNumberField(TEXT("ik_extension"), Settings.IK.Extension);
	Object->SetBoolField(TEXT("ik_affected_by_warping"), Settings.IK.bAffectedByIKWarping);
	Object->SetBoolField(TEXT("speed_planting_enabled"), Settings.SpeedPlanting.EnableSpeedPlanting);
	Object->SetStringField(TEXT("speed_curve_name"), Settings.SpeedPlanting.SpeedCurveName.ToString());
	Object->SetNumberField(TEXT("speed_threshold"), Settings.SpeedPlanting.SpeedThreshold);
	return Object;
}

TSharedRef<FJsonObject> IKRetargeterSnapshot(
	const FString& ProjectId,
	const UIKRetargeter& Retargeter,
	const FString& RetargeterId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> ChainMapValues;
	const TArray<TObjectPtr<URetargetChainSettings>>& ChainSettingsArray = Retargeter.GetAllChainSettings();
	for (int32 ChainIndex = 0; ChainIndex < ChainSettingsArray.Num(); ++ChainIndex)
	{
		const URetargetChainSettings* ChainSettings = ChainSettingsArray[ChainIndex].Get();
		if (!ChainSettings)
		{
			continue;
		}
		const FString ChainMapId = AddIKRetargetChainMapEntity(ProjectId, Retargeter, *ChainSettings, ChainIndex, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_ik_retarget_chain_map"), RetargeterId, ChainMapId, Retargeter.GetPathName(), TEXT("IK Retargeter contains this chain map."), OutRelations);
		ChainMapValues.Add(MakeShared<FJsonValueObject>(IKRetargetChainMapJson(*ChainSettings, ChainIndex, ChainMapId)));
	}

	const UIKRigDefinition* SourceIKRig = Retargeter.GetSourceIKRig();
	const UIKRigDefinition* TargetIKRig = Retargeter.GetTargetIKRig();
	const FName SourcePoseName = Retargeter.GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Source);
	const FName TargetPoseName = Retargeter.GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.ik_retargeter.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::AnimationDataModel));
	Object->SetStringField(TEXT("ik_retargeter_path"), Retargeter.GetPathName());
	Object->SetStringField(TEXT("source_ik_rig_path"), SourceIKRig ? SourceIKRig->GetPathName() : FString());
	Object->SetStringField(TEXT("target_ik_rig_path"), TargetIKRig ? TargetIKRig->GetPathName() : FString());
	Object->SetNumberField(TEXT("chain_map_count"), ChainMapValues.Num());
	Object->SetStringField(TEXT("source_current_pose_name"), SourcePoseName.ToString());
	Object->SetStringField(TEXT("target_current_pose_name"), TargetPoseName.ToString());
	Object->SetObjectField(TEXT("source_current_pose"), IKRetargetPoseJson(Retargeter.GetCurrentRetargetPose(ERetargetSourceOrTarget::Source), SourcePoseName));
	Object->SetObjectField(TEXT("target_current_pose"), IKRetargetPoseJson(Retargeter.GetCurrentRetargetPose(ERetargetSourceOrTarget::Target), TargetPoseName));
	Object->SetObjectField(TEXT("root_settings"), IKRetargetRootSettingsJson(Retargeter.GetRootSettingsUObject()));
	Object->SetObjectField(TEXT("global_settings"), IKRetargetGlobalSettingsJson(Retargeter.GetGlobalSettingsUObject()));
	Object->SetArrayField(TEXT("chain_maps"), ChainMapValues);
	return Object;
}

#endif

TSharedRef<FJsonObject> AnimSegmentJson(
	const FAnimSegment& Segment,
	int32 SegmentIndex)
{
	const UAnimSequenceBase* AnimReference = Segment.GetAnimReference().Get();
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), SegmentIndex);
	Object->SetStringField(TEXT("animation_path"), AnimReference ? AnimReference->GetPathName() : FString());
	Object->SetStringField(TEXT("animation_class"), AnimReference ? AnimReference->GetClass()->GetPathName() : FString());
	Object->SetNumberField(TEXT("track_start_time"), Segment.StartPos);
	Object->SetNumberField(TEXT("track_end_time"), Segment.GetEndPos());
	Object->SetNumberField(TEXT("length"), Segment.GetLength());
	Object->SetNumberField(TEXT("source_start_time"), Segment.AnimStartTime);
	Object->SetNumberField(TEXT("source_end_time"), Segment.AnimEndTime);
	Object->SetNumberField(TEXT("play_rate"), Segment.AnimPlayRate);
	Object->SetNumberField(TEXT("valid_play_rate"), Segment.GetValidPlayRate());
	Object->SetNumberField(TEXT("looping_count"), Segment.LoopingCount);
	Object->SetBoolField(TEXT("valid"), Segment.IsValid());
	return Object;
}

TArray<TSharedPtr<FJsonValue>> AnimTrackSegmentsJson(
	const FString& ProjectId,
	const FString& OwnerId,
	const FString& OwnerPath,
	const FAnimTrack& Track,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> SegmentValues;
	for (int32 SegmentIndex = 0; SegmentIndex < Track.AnimSegments.Num(); ++SegmentIndex)
	{
		const FAnimSegment& Segment = Track.AnimSegments[SegmentIndex];
		if (const UAnimSequenceBase* AnimReference = Segment.GetAnimReference().Get())
		{
			const FString ReferenceId = AddAnimSequenceBaseReferenceEntity(ProjectId, *AnimReference, OutEntities);
			AddAnimRelation(ProjectId, TEXT("uses_animation"), OwnerId, ReferenceId, OwnerPath, TEXT("Animation segment references this animation asset."), OutRelations);
		}
		SegmentValues.Add(MakeShared<FJsonValueObject>(AnimSegmentJson(Segment, SegmentIndex)));
	}
	return SegmentValues;
}

TSharedRef<FJsonObject> MontageSectionJson(const FCompositeSection& Section, int32 SectionIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), SectionIndex);
	Object->SetStringField(TEXT("name"), Section.SectionName.ToString());
	Object->SetNumberField(TEXT("start_time"), Section.GetTime());
	Object->SetStringField(TEXT("next_section"), Section.NextSectionName.ToString());
	Object->SetNumberField(TEXT("slot_index"), Section.GetSlotIndex());
	Object->SetNumberField(TEXT("segment_index"), Section.GetSegmentIndex());
	Object->SetStringField(TEXT("link_method"), AnimEnumValue(StaticEnum<EAnimLinkMethod::Type>(), static_cast<int64>(Section.GetLinkMethod())));
	Object->SetNumberField(TEXT("metadata_count"), Section.GetMetaData().Num());
	return Object;
}

TSharedRef<FJsonObject> MontageSlotJson(
	const FString& ProjectId,
	const FString& MontageId,
	const FString& MontagePath,
	const FSlotAnimationTrack& SlotTrack,
	int32 SlotIndex,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> SegmentValues = AnimTrackSegmentsJson(ProjectId, MontageId, MontagePath, SlotTrack.AnimTrack, OutEntities, OutRelations);
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), SlotIndex);
	Object->SetStringField(TEXT("slot_name"), SlotTrack.SlotName.ToString());
	Object->SetNumberField(TEXT("segment_count"), SegmentValues.Num());
	Object->SetArrayField(TEXT("segments"), MoveTemp(SegmentValues));
	return Object;
}

TSharedRef<FJsonObject> AnimationCompositeSnapshot(
	const FString& ProjectId,
	const UAnimComposite& Composite,
	const FString& CompositeId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> SegmentValues = AnimTrackSegmentsJson(ProjectId, CompositeId, Composite.GetPathName(), Composite.AnimationTrack, OutEntities, OutRelations);
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.anim_composite.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::AnimationDataModel));
	Object->SetStringField(TEXT("composite_path"), Composite.GetPathName());
	Object->SetStringField(TEXT("skeleton_path"), Composite.GetSkeleton() ? Composite.GetSkeleton()->GetPathName() : FString());
	Object->SetNumberField(TEXT("play_length_seconds"), Composite.GetPlayLength());
	Object->SetStringField(TEXT("sampling_frame_rate"), AnimFrameRateString(Composite.GetSamplingFrameRate()));
	Object->SetNumberField(TEXT("rate_scale"), Composite.RateScale);
	Object->SetStringField(TEXT("additive_anim_type"), AnimEnumValue(StaticEnum<EAdditiveAnimationType>(), static_cast<int64>(Composite.GetAdditiveAnimType())));
	Object->SetBoolField(TEXT("valid_additive"), Composite.IsValidAdditive());
	Object->SetBoolField(TEXT("root_motion"), Composite.HasRootMotion());
	Object->SetNumberField(TEXT("segment_count"), SegmentValues.Num());
	Object->SetArrayField(TEXT("segments"), MoveTemp(SegmentValues));
	return Object;
}

TSharedRef<FJsonObject> AnimationMontageSnapshot(
	const FString& ProjectId,
	const UAnimMontage& Montage,
	const FString& MontageId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> SectionValues;
	for (int32 SectionIndex = 0; SectionIndex < Montage.CompositeSections.Num(); ++SectionIndex)
	{
		SectionValues.Add(MakeShared<FJsonValueObject>(MontageSectionJson(Montage.CompositeSections[SectionIndex], SectionIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> SlotValues;
	for (int32 SlotIndex = 0; SlotIndex < Montage.SlotAnimTracks.Num(); ++SlotIndex)
	{
		SlotValues.Add(MakeShared<FJsonValueObject>(MontageSlotJson(ProjectId, MontageId, Montage.GetPathName(), Montage.SlotAnimTracks[SlotIndex], SlotIndex, OutEntities, OutRelations)));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.anim_montage.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::AnimationDataModel));
	Object->SetStringField(TEXT("montage_path"), Montage.GetPathName());
	Object->SetStringField(TEXT("skeleton_path"), Montage.GetSkeleton() ? Montage.GetSkeleton()->GetPathName() : FString());
	Object->SetNumberField(TEXT("play_length_seconds"), Montage.GetPlayLength());
	Object->SetStringField(TEXT("sampling_frame_rate"), AnimFrameRateString(Montage.GetSamplingFrameRate()));
	Object->SetNumberField(TEXT("rate_scale"), Montage.RateScale);
	Object->SetNumberField(TEXT("blend_in_time"), Montage.GetDefaultBlendInTime());
	Object->SetNumberField(TEXT("blend_out_time"), Montage.GetDefaultBlendOutTime());
	Object->SetNumberField(TEXT("blend_out_trigger_time"), Montage.BlendOutTriggerTime);
	Object->SetStringField(TEXT("sync_group"), Montage.SyncGroup.ToString());
	Object->SetNumberField(TEXT("sync_slot_index"), Montage.SyncSlotIndex);
	Object->SetBoolField(TEXT("auto_blend_out"), Montage.bEnableAutoBlendOut);
	Object->SetStringField(TEXT("root_motion_root_lock"), AnimEnumValue(StaticEnum<ERootMotionRootLock::Type>(), static_cast<int64>(Montage.RootMotionRootLock.GetValue())));
	Object->SetNumberField(TEXT("section_count"), SectionValues.Num());
	Object->SetNumberField(TEXT("slot_count"), SlotValues.Num());
	Object->SetNumberField(TEXT("segment_count"), MontageSegmentCount(Montage));
	Object->SetArrayField(TEXT("sections"), MoveTemp(SectionValues));
	Object->SetArrayField(TEXT("slots"), MoveTemp(SlotValues));
	return Object;
}

TSharedRef<FJsonObject> AnimationSequenceSnapshot(
	const FString& ProjectId,
	const UAnimSequence& Sequence,
	const FString& SequenceId,
	const TMap<FName, FString>& BoneNameToId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const IAnimationDataModel* DataModel = Sequence.GetDataModel();
	TArray<FName> TrackNames;
	if (DataModel)
	{
		DataModel->GetBoneTrackNames(TrackNames);
		TrackNames.Sort(FNameLexicalLess());
	}
	const FFrameRate DataFrameRate = DataModel ? DataModel->GetFrameRate() : Sequence.GetSamplingFrameRate();
	const TArray<int32> SampleFrameNumbers = DataModel ? AnimSequenceSampleFrames(*DataModel) : TArray<int32>();

	TArray<TSharedPtr<FJsonValue>> TrackValues;
	TArray<FAnimTrackMotionMetrics> MotionMetricsValues;
	for (int32 TrackIndex = 0; TrackIndex < TrackNames.Num(); ++TrackIndex)
	{
		const FName TrackName = TrackNames[TrackIndex];
		TArray<FTransform> RawLocalTransforms;
		if (DataModel)
		{
			DataModel->GetBoneTrackTransforms(TrackName, RawLocalTransforms);
		}
		const FAnimTrackMotionMetrics MotionMetrics = CalculateTrackMotionMetrics(TrackName, TrackIndex, RawLocalTransforms);
		MotionMetricsValues.Add(MotionMetrics);

		const FString CanonicalKey = FString::Printf(TEXT("%s:track:%d:%s"), *Sequence.GetPathName(), TrackIndex, *TrackName.ToString());
		const FString TrackId = MakeStableId(ProjectId, TEXT("animation_track"), CanonicalKey);
		if (!FindEntity(OutEntities, TrackId))
		{
			FEntityRecord TrackEntity;
			TrackEntity.Id = TrackId;
			TrackEntity.Kind = TEXT("animation_track");
			TrackEntity.CanonicalKey = CanonicalKey;
			TrackEntity.DisplayName = TrackName.ToString();
			TrackEntity.SourceLayer = LexToString(ESourceLayer::AnimationDataModel);
			TrackEntity.Attributes.Add(TEXT("sequence_path"), Sequence.GetPathName());
			TrackEntity.Attributes.Add(TEXT("track_index"), FString::FromInt(TrackIndex));
			TrackEntity.Attributes.Add(TEXT("bone_name"), TrackName.ToString());
			TrackEntity.Attributes.Add(TEXT("raw_local_key_count"), FString::FromInt(RawLocalTransforms.Num()));
			TrackEntity.Attributes.Add(TEXT("motion_changes"), AnimBool(MotionMetrics.Changes()));
			TrackEntity.Attributes.Add(TEXT("translation_changes"), AnimBool(MotionMetrics.bTranslationChanges));
			TrackEntity.Attributes.Add(TEXT("rotation_changes"), AnimBool(MotionMetrics.bRotationChanges));
			TrackEntity.Attributes.Add(TEXT("scale_changes"), AnimBool(MotionMetrics.bScaleChanges));
			TrackEntity.Attributes.Add(TEXT("translation_range"), AnimNumber(MotionMetrics.TranslationRange));
			TrackEntity.Attributes.Add(TEXT("translation_max_delta"), AnimNumber(MotionMetrics.TranslationMaxDelta));
			TrackEntity.Attributes.Add(TEXT("rotation_range_degrees"), AnimNumber(MotionMetrics.RotationRangeDegrees));
			TrackEntity.Attributes.Add(TEXT("scale_range"), AnimNumber(MotionMetrics.ScaleRange));
			TrackEntity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
			TrackEntity.Completeness.State = ECompletenessState::Partial;
			TrackEntity.Completeness.Covered = { TEXT("track_metadata"), TEXT("raw_local_key_count"), TEXT("raw_local_samples"), TEXT("motion_summary") };
			TrackEntity.Completeness.Omitted = { TEXT("full_raw_local_track_artifact") };
			TSharedRef<FJsonObject> TrackSnapshot = MakeShared<FJsonObject>();
			TrackSnapshot->SetStringField(TEXT("schema_version"), TEXT("uepi.anim_track.v1"));
			TrackSnapshot->SetStringField(TEXT("sequence_path"), Sequence.GetPathName());
			TrackSnapshot->SetStringField(TEXT("bone_name"), TrackName.ToString());
			TrackSnapshot->SetObjectField(TEXT("motion"), AnimTrackMotionObject(MotionMetrics));
			TrackEntity.Snapshot = TrackSnapshot;
			AddAnimEvidence(TrackEntity, Sequence.GetPathName(), TEXT("Bone animation track read from Animation Data Model."));
			OutEntities.Add(MoveTemp(TrackEntity));
		}

		AddAnimRelation(ProjectId, TEXT("contains_track"), SequenceId, TrackId, Sequence.GetPathName(), TEXT("Animation sequence contains a bone track."), OutRelations);
		if (const FString* BoneId = BoneNameToId.Find(TrackName))
		{
			AddAnimRelation(ProjectId, TEXT("animates_bone"), TrackId, *BoneId, Sequence.GetPathName(), TEXT("Bone track targets this skeleton bone."), OutRelations);
		}

		TSharedRef<FJsonObject> TrackObject = MakeShared<FJsonObject>();
		TrackObject->SetStringField(TEXT("id"), TrackId);
		TrackObject->SetNumberField(TEXT("index"), TrackIndex);
		TrackObject->SetStringField(TEXT("bone_name"), TrackName.ToString());
		TrackObject->SetNumberField(TEXT("raw_local_key_count"), RawLocalTransforms.Num());
		TrackObject->SetObjectField(TEXT("motion"), AnimTrackMotionObject(MotionMetrics));
		if (RawLocalTransforms.Num() > 0)
		{
			TrackObject->SetObjectField(TEXT("raw_local_first"), AnimTransformObject(RawLocalTransforms[0]));
			TrackObject->SetObjectField(TEXT("raw_local_last"), AnimTransformObject(RawLocalTransforms.Last()));
		}

		TArray<TSharedPtr<FJsonValue>> RawLocalSampleValues;
		if (DataModel)
		{
			for (const int32 FrameNumber : SampleFrameNumbers)
			{
				const FTransform SampleTransform = DataModel->GetBoneTrackTransform(TrackName, FFrameNumber(FrameNumber));
				RawLocalSampleValues.Add(MakeShared<FJsonValueObject>(AnimFrameTransformJson(FrameNumber, AnimFrameSeconds(DataFrameRate, FrameNumber), SampleTransform)));
			}
		}
		TrackObject->SetArrayField(TEXT("raw_local_samples"), MoveTemp(RawLocalSampleValues));
		TrackValues.Add(MakeShared<FJsonValueObject>(TrackObject));
	}
	const TSharedRef<FJsonObject> MotionSummaryObject = AnimationMotionSummaryObject(Sequence, MotionMetricsValues);
	const TSharedRef<FJsonObject> BoneMotionProfileManifest = WriteAnimationBoneMotionProfileArtifact(ProjectId, Sequence, TrackNames, MotionMetricsValues);
	if (FEntityRecord* SequenceEntity = FindEntity(OutEntities, SequenceId))
	{
		int32 ChangingBoneCount = 0;
		for (const FAnimTrackMotionMetrics& Metrics : MotionMetricsValues)
		{
			if (Metrics.Changes())
			{
				++ChangingBoneCount;
			}
		}
		SequenceEntity->Attributes.Add(TEXT("motion_changing_bone_count"), FString::FromInt(ChangingBoneCount));
		SequenceEntity->Attributes.Add(TEXT("motion_static_bone_count"), FString::FromInt(FMath::Max(MotionMetricsValues.Num() - ChangingBoneCount, 0)));
		SequenceEntity->Attributes.Add(TEXT("bone_motion_profile_artifact_uri"), BoneMotionProfileManifest->GetStringField(TEXT("artifact_uri")));
		SequenceEntity->Attributes.Add(TEXT("bone_motion_profile_changed_bone_count"), FString::FromInt(static_cast<int32>(BoneMotionProfileManifest->GetNumberField(TEXT("changed_bone_count")))));
		SequenceEntity->Attributes.Add(TEXT("bone_motion_profile_position_changed_bone_count"), FString::FromInt(static_cast<int32>(BoneMotionProfileManifest->GetNumberField(TEXT("position_changed_bone_count")))));
		SequenceEntity->Attributes.Add(TEXT("bone_motion_profile_driver_bone_count"), FString::FromInt(static_cast<int32>(BoneMotionProfileManifest->GetNumberField(TEXT("driver_bone_count")))));
		SequenceEntity->Attributes.Add(TEXT("bone_motion_profile_inherited_motion_bone_count"), FString::FromInt(static_cast<int32>(BoneMotionProfileManifest->GetNumberField(TEXT("inherited_motion_bone_count")))));
		SequenceEntity->Completeness.Covered.AddUnique(TEXT("motion_summary"));
		SequenceEntity->Completeness.Covered.AddUnique(TEXT("bone_motion_profile_artifact"));
	}

	TArray<TSharedPtr<FJsonValue>> NotifyValues;
	for (int32 NotifyIndex = 0; NotifyIndex < Sequence.Notifies.Num(); ++NotifyIndex)
	{
		const FAnimNotifyEvent& Notify = Sequence.Notifies[NotifyIndex];
		const FString NotifyId = AddNotifyEntity(ProjectId, Sequence, Notify, NotifyIndex, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_notify"), SequenceId, NotifyId, Sequence.GetPathName(), TEXT("Animation sequence contains a notify event."), OutRelations);
		NotifyValues.Add(MakeShared<FJsonValueObject>(NotifyJson(Notify, NotifyIndex, NotifyId)));
	}

	TArray<TSharedPtr<FJsonValue>> PoseSampleValues;
	if (DataModel)
	{
		const USkeleton* Skeleton = Sequence.GetSkeleton();
		const FReferenceSkeleton* RefSkeleton = Skeleton ? &Skeleton->GetReferenceSkeleton() : nullptr;
		for (const int32 FrameNumber : SampleFrameNumbers)
		{
			TArray<FTransform> TrackTransforms;
			if (TrackNames.Num() > 0)
			{
				DataModel->GetBoneTracksTransform(TrackNames, FFrameNumber(FrameNumber), TrackTransforms);
			}

			TArray<TSharedPtr<FJsonValue>> LocalTrackValues;
			for (int32 TrackIndex = 0; TrackIndex < TrackNames.Num(); ++TrackIndex)
			{
				if (TrackTransforms.IsValidIndex(TrackIndex))
				{
					LocalTrackValues.Add(MakeShared<FJsonValueObject>(AnimTransformEntryJson(TrackIndex, TrackNames[TrackIndex], TrackTransforms[TrackIndex])));
				}
			}

			TArray<TSharedPtr<FJsonValue>> ComponentBoneValues;
			if (RefSkeleton)
			{
				TArray<FTransform> LocalBonePoses = RefSkeleton->GetRefBonePose();
				const int32 ExistingPoseCount = LocalBonePoses.Num();
				LocalBonePoses.SetNum(RefSkeleton->GetNum());
				for (int32 BoneIndex = ExistingPoseCount; BoneIndex < LocalBonePoses.Num(); ++BoneIndex)
				{
					LocalBonePoses[BoneIndex] = FTransform::Identity;
				}

				for (int32 TrackIndex = 0; TrackIndex < TrackNames.Num(); ++TrackIndex)
				{
					const int32 BoneIndex = RefSkeleton->FindBoneIndex(TrackNames[TrackIndex]);
					if (RefSkeleton->IsValidIndex(BoneIndex) && TrackTransforms.IsValidIndex(TrackIndex))
					{
						LocalBonePoses[BoneIndex] = TrackTransforms[TrackIndex];
					}
				}

				const TArray<FTransform> ComponentPoses = BuildComponentSpacePose(*RefSkeleton, LocalBonePoses);
				for (int32 BoneIndex = 0; BoneIndex < RefSkeleton->GetNum(); ++BoneIndex)
				{
					ComponentBoneValues.Add(MakeShared<FJsonValueObject>(AnimTransformEntryJson(BoneIndex, RefSkeleton->GetBoneName(BoneIndex), ComponentPoses.IsValidIndex(BoneIndex) ? ComponentPoses[BoneIndex] : FTransform::Identity)));
				}
			}

			TSharedRef<FJsonObject> PoseSampleObject = MakeShared<FJsonObject>();
			PoseSampleObject->SetNumberField(TEXT("frame_number"), FrameNumber);
			PoseSampleObject->SetNumberField(TEXT("time_seconds"), AnimFrameSeconds(DataFrameRate, FrameNumber));
			PoseSampleObject->SetNumberField(TEXT("local_track_count"), LocalTrackValues.Num());
			PoseSampleObject->SetNumberField(TEXT("component_bone_count"), ComponentBoneValues.Num());
			PoseSampleObject->SetArrayField(TEXT("local_tracks"), MoveTemp(LocalTrackValues));
			PoseSampleObject->SetArrayField(TEXT("component_bones"), MoveTemp(ComponentBoneValues));
			PoseSampleValues.Add(MakeShared<FJsonValueObject>(PoseSampleObject));
		}
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.anim_sequence.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::AnimationDataModel));
	Object->SetStringField(TEXT("sequence_path"), Sequence.GetPathName());
	Object->SetStringField(TEXT("skeleton_path"), Sequence.GetSkeleton() ? Sequence.GetSkeleton()->GetPathName() : FString());
	Object->SetNumberField(TEXT("play_length_seconds"), Sequence.GetPlayLength());
	Object->SetNumberField(TEXT("sample_key_count"), Sequence.GetNumberOfSampledKeys());
	Object->SetStringField(TEXT("sampling_frame_rate"), AnimFrameRateString(Sequence.GetSamplingFrameRate()));
	Object->SetNumberField(TEXT("frame_count"), DataModel ? DataModel->GetNumberOfFrames() : 0);
	Object->SetNumberField(TEXT("data_model_key_count"), DataModel ? DataModel->GetNumberOfKeys() : 0);
	Object->SetNumberField(TEXT("rate_scale"), Sequence.RateScale);
	Object->SetBoolField(TEXT("loop"), Sequence.bLoop);
	Object->SetStringField(TEXT("additive_anim_type"), AnimEnumValue(StaticEnum<EAdditiveAnimationType>(), static_cast<int64>(Sequence.AdditiveAnimType.GetValue())));
	Object->SetStringField(TEXT("additive_base_pose_type"), AnimEnumValue(StaticEnum<EAdditiveBasePoseType>(), static_cast<int64>(Sequence.RefPoseType.GetValue())));
	Object->SetNumberField(TEXT("additive_ref_frame_index"), Sequence.RefFrameIndex);
	Object->SetStringField(TEXT("additive_ref_pose_sequence_path"), Sequence.RefPoseSeq ? Sequence.RefPoseSeq->GetPathName() : FString());
	Object->SetBoolField(TEXT("valid_additive"), Sequence.IsValidAdditive());
	Object->SetBoolField(TEXT("root_motion_enabled"), Sequence.HasRootMotion());
	Object->SetStringField(TEXT("root_motion_root_lock"), AnimEnumValue(StaticEnum<ERootMotionRootLock::Type>(), static_cast<int64>(Sequence.RootMotionRootLock.GetValue())));
	Object->SetBoolField(TEXT("force_root_lock"), Sequence.bForceRootLock);
	Object->SetBoolField(TEXT("use_normalized_root_motion_scale"), Sequence.bUseNormalizedRootMotionScale);
	Object->SetBoolField(TEXT("root_motion_settings_copied_from_montage"), Sequence.bRootMotionSettingsCopiedFromMontage);
	Object->SetObjectField(TEXT("root_motion_full_range"), AnimTransformObject(Sequence.ExtractRootMotionFromRange(0.0f, Sequence.GetPlayLength())));
	Object->SetNumberField(TEXT("bone_track_count"), TrackValues.Num());
	Object->SetNumberField(TEXT("float_curve_count"), DataModel ? DataModel->GetNumberOfFloatCurves() : 0);
	Object->SetNumberField(TEXT("transform_curve_count"), DataModel ? DataModel->GetNumberOfTransformCurves() : 0);
	Object->SetNumberField(TEXT("attribute_count"), DataModel ? DataModel->GetNumberOfAttributes() : 0);
	Object->SetNumberField(TEXT("notify_count"), NotifyValues.Num());
	Object->SetNumberField(TEXT("sampled_pose_count"), PoseSampleValues.Num());
	Object->SetArrayField(TEXT("tracks"), TrackValues);
	Object->SetObjectField(TEXT("motion_summary"), MotionSummaryObject);
	Object->SetObjectField(TEXT("bone_motion_profile"), BoneMotionProfileManifest);
	Object->SetArrayField(TEXT("notifies"), NotifyValues);
	Object->SetArrayField(TEXT("pose_samples"), MoveTemp(PoseSampleValues));
	return Object;
}
}

bool FAnimationReader::AppendAnimationAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (USkeleton* Skeleton = Cast<USkeleton>(&Asset))
	{
		const FString SkeletonId = AddSkeletonEntity(ProjectId, Skeleton, OutEntities, OutRelations);
		AddAnimRelation(ProjectId, TEXT("contains_skeleton"), AssetEntity.Id, SkeletonId, Skeleton->GetPathName(), TEXT("Skeleton asset contains the extracted skeleton record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("skeleton"), SkeletonSnapshot(ProjectId, *Skeleton, OutEntities, OutRelations, SkeletonId, nullptr));
		AssetEntity.Attributes.Add(TEXT("skeleton_bone_count"), FString::FromInt(Skeleton->GetReferenceSkeleton().GetNum()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("reference_skeleton"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("bone_hierarchy"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("retarget_sources"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::AnimationDataModel),
			Skeleton->GetPathName(),
			TEXT("USkeleton structural data extracted through public Engine API.")
		});
		return true;
	}

	if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(&Asset))
	{
		const FString MeshId = AddSkeletalMeshEntity(ProjectId, *Mesh, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_skeletal_mesh"), AssetEntity.Id, MeshId, Mesh->GetPathName(), TEXT("Skeletal mesh asset contains the extracted skeletal mesh record."), OutRelations);

		TMap<FName, FString> BoneNameToId;
		const FString SkeletonId = AddSkeletonEntity(ProjectId, Mesh->GetSkeleton(), OutEntities, OutRelations, &BoneNameToId);
		if (!SkeletonId.IsEmpty())
		{
			AddAnimRelation(ProjectId, TEXT("uses_skeleton"), MeshId, SkeletonId, Mesh->GetPathName(), TEXT("Skeletal mesh references this skeleton asset."), OutRelations);
		}

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("skeletal_mesh"), SkeletalMeshSnapshot(ProjectId, *Mesh, MeshId, OutEntities, OutRelations, &BoneNameToId));
		AssetEntity.Attributes.Add(TEXT("skeletal_mesh_bone_count"), FString::FromInt(Mesh->GetRefSkeleton().GetNum()));
		AssetEntity.Attributes.Add(TEXT("skeletal_mesh_lod_count"), FString::FromInt(Mesh->GetLODNum()));
		AssetEntity.Attributes.Add(TEXT("skeletal_mesh_material_count"), FString::FromInt(Mesh->GetMaterials().Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("skeletal_mesh_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("skeleton_reference"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("skin_weight_buffers"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("lod_section_geometry"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::AnimationDataModel),
			Mesh->GetPathName(),
			TEXT("USkeletalMesh structural metadata extracted through public Engine API.")
		});
		return true;
	}

	if (UAnimMontage* Montage = Cast<UAnimMontage>(&Asset))
	{
		const FString MontageId = AddAnimationMontageEntity(ProjectId, *Montage, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_montage"), AssetEntity.Id, MontageId, Montage->GetPathName(), TEXT("AnimMontage asset contains the extracted montage record."), OutRelations);

		const FString SkeletonId = AddSkeletonEntity(ProjectId, Montage->GetSkeleton(), OutEntities, OutRelations);
		if (!SkeletonId.IsEmpty())
		{
			AddAnimRelation(ProjectId, TEXT("uses_skeleton"), MontageId, SkeletonId, Montage->GetPathName(), TEXT("Animation montage references this skeleton asset."), OutRelations);
		}

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("anim_montage"), AnimationMontageSnapshot(ProjectId, *Montage, MontageId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("animation_montage_section_count"), FString::FromInt(Montage->CompositeSections.Num()));
		AssetEntity.Attributes.Add(TEXT("animation_montage_slot_count"), FString::FromInt(Montage->SlotAnimTracks.Num()));
		AssetEntity.Attributes.Add(TEXT("animation_montage_segment_count"), FString::FromInt(MontageSegmentCount(*Montage)));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("montage_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("sections"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("slot_tracks"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("segment_timeline"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_montage_state"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::AnimationDataModel),
			Montage->GetPathName(),
			TEXT("UAnimMontage structural metadata extracted through public Engine API.")
		});
		return true;
	}

	if (UAnimComposite* Composite = Cast<UAnimComposite>(&Asset))
	{
		const FString CompositeId = AddAnimationCompositeEntity(ProjectId, *Composite, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_composite"), AssetEntity.Id, CompositeId, Composite->GetPathName(), TEXT("AnimComposite asset contains the extracted composite record."), OutRelations);

		const FString SkeletonId = AddSkeletonEntity(ProjectId, Composite->GetSkeleton(), OutEntities, OutRelations);
		if (!SkeletonId.IsEmpty())
		{
			AddAnimRelation(ProjectId, TEXT("uses_skeleton"), CompositeId, SkeletonId, Composite->GetPathName(), TEXT("Animation composite references this skeleton asset."), OutRelations);
		}

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("anim_composite"), AnimationCompositeSnapshot(ProjectId, *Composite, CompositeId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("animation_composite_segment_count"), FString::FromInt(AnimTrackSegmentCount(Composite->AnimationTrack)));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("composite_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("segment_timeline"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_pose_evaluation"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::AnimationDataModel),
			Composite->GetPathName(),
			TEXT("UAnimComposite structural metadata extracted through public Engine API.")
		});
		return true;
	}

	if (UAnimSequence* Sequence = Cast<UAnimSequence>(&Asset))
	{
		TMap<FName, FString> BoneNameToId;
		const FString SkeletonId = AddSkeletonEntity(ProjectId, Sequence->GetSkeleton(), OutEntities, OutRelations, &BoneNameToId);
		const FString SequenceId = AddAnimationSequenceEntity(ProjectId, *Sequence, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_animation_sequence"), AssetEntity.Id, SequenceId, Sequence->GetPathName(), TEXT("AnimSequence asset contains the extracted animation sequence record."), OutRelations);
		if (!SkeletonId.IsEmpty())
		{
			AddAnimRelation(ProjectId, TEXT("uses_skeleton"), SequenceId, SkeletonId, Sequence->GetPathName(), TEXT("Animation sequence references this skeleton asset."), OutRelations);
		}

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("animation_sequence"), AnimationSequenceSnapshot(ProjectId, *Sequence, SequenceId, BoneNameToId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("animation_play_length_seconds"), AnimNumber(Sequence->GetPlayLength()));
		AssetEntity.Attributes.Add(TEXT("animation_sample_key_count"), FString::FromInt(Sequence->GetNumberOfSampledKeys()));
		AssetEntity.Attributes.Add(TEXT("animation_bone_track_count"), Sequence->GetDataModel() ? FString::FromInt(Sequence->GetDataModel()->GetNumBoneTracks()) : TEXT("0"));
		AssetEntity.Attributes.Add(TEXT("animation_notify_count"), FString::FromInt(Sequence->Notifies.Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("animation_sequence_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("animation_bone_tracks"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("animation_motion_summary"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("animation_notifies"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("per_frame_pose_samples"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_pose_evaluation"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::AnimationDataModel),
			Sequence->GetPathName(),
			TEXT("UAnimSequence structural metadata extracted through Animation Data Model.")
		});
		return true;
	}

	if (UPoseAsset* PoseAsset = Cast<UPoseAsset>(&Asset))
	{
		const FString SkeletonId = AddSkeletonEntity(ProjectId, PoseAsset->GetSkeleton(), OutEntities, OutRelations);
		const FString PoseAssetId = AddPoseAssetEntity(ProjectId, *PoseAsset, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_pose_asset"), AssetEntity.Id, PoseAssetId, PoseAsset->GetPathName(), TEXT("PoseAsset asset contains the extracted pose asset record."), OutRelations);
		if (!SkeletonId.IsEmpty())
		{
			AddAnimRelation(ProjectId, TEXT("uses_skeleton"), PoseAssetId, SkeletonId, PoseAsset->GetPathName(), TEXT("PoseAsset references this skeleton asset."), OutRelations);
		}
#if WITH_EDITORONLY_DATA
		if (PoseAsset->SourceAnimation)
		{
			const FString AnimationId = AddAnimationSequenceEntity(ProjectId, *PoseAsset->SourceAnimation, OutEntities);
			AddAnimRelation(ProjectId, TEXT("uses_animation"), PoseAssetId, AnimationId, PoseAsset->GetPathName(), TEXT("PoseAsset source animation reference."), OutRelations);
		}
#endif

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("pose_asset"), PoseAssetSnapshot(ProjectId, *PoseAsset, PoseAssetId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("pose_asset_pose_count"), FString::FromInt(PoseAsset->GetNumPoses()));
		AssetEntity.Attributes.Add(TEXT("pose_asset_curve_count"), FString::FromInt(PoseAsset->GetNumCurves()));
		AssetEntity.Attributes.Add(TEXT("pose_asset_track_count"), FString::FromInt(PoseAsset->GetNumTracks()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("pose_asset_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("pose_names"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("curve_values"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("source_animation_reference"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_pose_evaluation"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("component_space_pose_samples"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::AnimationDataModel),
			PoseAsset->GetPathName(),
			TEXT("UPoseAsset structural metadata extracted through public Engine API.")
		});
		return true;
	}

	if (UPhysicsAsset* PhysicsAsset = Cast<UPhysicsAsset>(&Asset))
	{
		TMap<FName, FString> BoneNameToId;
		USkeletalMesh* PreviewMesh = PhysicsAsset->GetPreviewMesh();
		const FString PhysicsAssetId = AddPhysicsAssetEntity(ProjectId, *PhysicsAsset, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_physics_asset"), AssetEntity.Id, PhysicsAssetId, PhysicsAsset->GetPathName(), TEXT("PhysicsAsset asset contains the extracted physics asset record."), OutRelations);

		if (PreviewMesh)
		{
			const FString PreviewMeshId = AddSkeletalMeshEntity(ProjectId, *PreviewMesh, OutEntities);
			AddAnimRelation(ProjectId, TEXT("uses_skeletal_mesh"), PhysicsAssetId, PreviewMeshId, PhysicsAsset->GetPathName(), TEXT("PhysicsAsset preview mesh reference."), OutRelations);

			const FString SkeletonId = AddSkeletonEntity(ProjectId, PreviewMesh->GetSkeleton(), OutEntities, OutRelations, &BoneNameToId);
			if (!SkeletonId.IsEmpty())
			{
				AddAnimRelation(ProjectId, TEXT("uses_skeleton"), PhysicsAssetId, SkeletonId, PhysicsAsset->GetPathName(), TEXT("PhysicsAsset preview mesh references this skeleton asset."), OutRelations);
			}
		}

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("physics_asset"), PhysicsAssetSnapshot(ProjectId, *PhysicsAsset, PhysicsAssetId, BoneNameToId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("physics_asset_body_count"), FString::FromInt(PhysicsAsset->SkeletalBodySetups.Num()));
		AssetEntity.Attributes.Add(TEXT("physics_asset_constraint_count"), FString::FromInt(PhysicsAsset->ConstraintSetup.Num()));
		AssetEntity.Attributes.Add(TEXT("physics_asset_shape_count"), FString::FromInt(PhysicsAssetShapeCount(*PhysicsAsset)));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("physics_asset_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("body_setups"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("collision_shapes"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("constraints"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("cooked_chaos_meshes"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_simulation_state"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::AnimationDataModel),
			PhysicsAsset->GetPathName(),
			TEXT("UPhysicsAsset structural metadata extracted through public Engine API.")
		});
		return true;
	}

#if UEPI_WITH_IK_RIG
	if (UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(&Asset))
	{
		const FString IKRigId = AddIKRigEntity(ProjectId, *IKRig, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_ik_rig"), AssetEntity.Id, IKRigId, IKRig->GetPathName(), TEXT("IK Rig asset contains the extracted IK Rig record."), OutRelations);

		if (USkeletalMesh* PreviewMesh = IKRig->GetPreviewMesh())
		{
			const FString PreviewMeshId = AddSkeletalMeshEntity(ProjectId, *PreviewMesh, OutEntities);
			AddAnimRelation(ProjectId, TEXT("uses_skeletal_mesh"), IKRigId, PreviewMeshId, IKRig->GetPathName(), TEXT("IK Rig preview mesh reference."), OutRelations);
		}

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("ik_rig"), IKRigSnapshot(ProjectId, *IKRig, IKRigId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("ik_rig_chain_count"), FString::FromInt(IKRig->GetRetargetChains().Num()));
		AssetEntity.Attributes.Add(TEXT("ik_rig_goal_count"), FString::FromInt(IKRig->GetGoalArray().Num()));
		AssetEntity.Attributes.Add(TEXT("ik_rig_solver_count"), FString::FromInt(IKRig->GetSolverArray().Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("ik_rig_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("retarget_chains"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("goals"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("solvers"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_ik_solve"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("evaluated_pose"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::AnimationDataModel),
			IKRig->GetPathName(),
			TEXT("UIKRigDefinition structural metadata extracted through public IKRig API.")
		});
		return true;
	}

	if (UIKRetargeter* Retargeter = Cast<UIKRetargeter>(&Asset))
	{
		const FString RetargeterId = AddIKRetargeterEntity(ProjectId, *Retargeter, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_ik_retargeter"), AssetEntity.Id, RetargeterId, Retargeter->GetPathName(), TEXT("IK Retargeter asset contains the extracted retargeter record."), OutRelations);

		if (const UIKRigDefinition* SourceIKRig = Retargeter->GetSourceIKRig())
		{
			const FString SourceIKRigId = AddIKRigEntity(ProjectId, *SourceIKRig, OutEntities);
			AddAnimRelation(ProjectId, TEXT("uses_source_ik_rig"), RetargeterId, SourceIKRigId, Retargeter->GetPathName(), TEXT("IK Retargeter source IK Rig reference."), OutRelations);
		}
		if (const UIKRigDefinition* TargetIKRig = Retargeter->GetTargetIKRig())
		{
			const FString TargetIKRigId = AddIKRigEntity(ProjectId, *TargetIKRig, OutEntities);
			AddAnimRelation(ProjectId, TEXT("uses_target_ik_rig"), RetargeterId, TargetIKRigId, Retargeter->GetPathName(), TEXT("IK Retargeter target IK Rig reference."), OutRelations);
		}

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("ik_retargeter"), IKRetargeterSnapshot(ProjectId, *Retargeter, RetargeterId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("ik_retargeter_chain_map_count"), FString::FromInt(Retargeter->GetAllChainSettings().Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("ik_retargeter_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("source_target_ik_rigs"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("chain_mapping"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("current_retarget_pose_metadata"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_retarget_evaluation"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("retarget_profile_overrides"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::AnimationDataModel),
			Retargeter->GetPathName(),
			TEXT("UIKRetargeter structural metadata extracted through public IKRig API.")
		});
		return true;
	}
#endif

	if (UBlendSpace* BlendSpace = Cast<UBlendSpace>(&Asset))
	{
		const FString SkeletonId = AddSkeletonEntity(ProjectId, BlendSpace->GetSkeleton(), OutEntities, OutRelations);
		const FString BlendSpaceId = AddBlendSpaceEntity(ProjectId, *BlendSpace, OutEntities);
		AddAnimRelation(ProjectId, TEXT("contains_blend_space"), AssetEntity.Id, BlendSpaceId, BlendSpace->GetPathName(), TEXT("BlendSpace asset contains the extracted blend space record."), OutRelations);
		if (!SkeletonId.IsEmpty())
		{
			AddAnimRelation(ProjectId, TEXT("uses_skeleton"), BlendSpaceId, SkeletonId, BlendSpace->GetPathName(), TEXT("BlendSpace references this skeleton asset."), OutRelations);
		}

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("blend_space"), BlendSpaceSnapshot(ProjectId, *BlendSpace, BlendSpaceId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("blend_space_dimension_count"), FString::FromInt(BlendSpaceDimensionCount(*BlendSpace)));
		AssetEntity.Attributes.Add(TEXT("blend_space_sample_count"), FString::FromInt(BlendSpace->GetBlendSamples().Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("blend_space_axes"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("blend_space_samples"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("sample_animation_references"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_sample_weights"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("evaluated_pose"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::AnimationDataModel),
			BlendSpace->GetPathName(),
			TEXT("UBlendSpace structural metadata extracted through public Engine API.")
		});
		return true;
	}

	return false;
}
}
