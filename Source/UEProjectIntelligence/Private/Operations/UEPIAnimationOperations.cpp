#include "Operations/UEPIAnimationOperations.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimMontageFactory.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"

namespace UE::ProjectIntelligence
{
	namespace AnimationOperationsPrivate
	{
		FString JsonString(const FJsonObject& Object,const TCHAR* Field,const FString& Default=FString()){FString Value;return Object.TryGetStringField(Field,Value)?Value:Default;}
		int32 JsonInt(const FJsonObject& Object,const TCHAR* Field,int32 Default=0){int32 Value=0;return Object.TryGetNumberField(Field,Value)?Value:Default;}
		FUEPIEditResult Failure(const FString& Code,const FString& Message){FUEPIEditResult Result;Result.ErrorCode=Code;Result.Message=Message;return Result;}
		FUEPIEditResult Success(const FString& Message,const TSharedPtr<FJsonObject>& Detail=nullptr){FUEPIEditResult Result;Result.bOk=true;Result.Message=Message;Result.Result=Detail;return Result;}

		FString ReferenceId(const FJsonObject& Params,const TCHAR* Field){const TSharedPtr<FJsonObject>* Ref=nullptr;if(!Params.TryGetObjectField(Field,Ref)||!Ref||!Ref->IsValid())return FString();FString Value=JsonString(**Ref,TEXT("$ref"));int32 Hash=INDEX_NONE;if(Value.FindChar(TEXT('#'),Hash))Value=Value.Left(Hash);return Value;}
		FString ResolveAsset(const FUEPIEditContext& Context,const FJsonObject& Params,const TCHAR* Field){FString Direct=JsonString(Params,Field);if(!Direct.IsEmpty())return Direct;const FString Ref=ReferenceId(Params,Field);if(const FString* Value=Context.ResolvedAssets.Find(Ref))return *Value;return FString();}

		bool SplitDestination(const FJsonObject& Params,FString& OutPath,FString& OutName,FString& OutError)
		{
			FString Destination=JsonString(Params,TEXT("destination"),JsonString(Params,TEXT("destination_asset")));Destination.ReplaceInline(TEXT("\\"),TEXT("/"));if(!Destination.IsEmpty()){FString Package=Destination;int32 Dot=INDEX_NONE;if(Package.FindChar(TEXT('.'),Dot))Package=Package.Left(Dot);OutPath=FPackageName::GetLongPackagePath(Package);OutName=FPackageName::GetLongPackageAssetName(Package);}OutPath=JsonString(Params,TEXT("destination_path"),JsonString(Params,TEXT("folder"),OutPath));OutPath.ReplaceInline(TEXT("\\"),TEXT("/"));while(OutPath.EndsWith(TEXT("/")))OutPath.LeftChopInline(1);OutName=JsonString(Params,TEXT("name"),JsonString(Params,TEXT("asset_name"),OutName.IsEmpty()?TEXT("AM_UEPIMontage"):OutName));if((!OutPath.Equals(TEXT("/Game"))&&!OutPath.StartsWith(TEXT("/Game/")))||OutPath.Contains(TEXT(".."))||OutName.IsEmpty()||OutName.Contains(TEXT("/"))||OutName.Contains(TEXT("."))){OutError=TEXT("Montage destination must be a valid asset name under /Game.");return false;}return true;
		}

		bool SlotAvailable(const FUEPIEditContext& Context,USkeleton* Skeleton,FName Slot)
		{
			if(!Skeleton||Slot.IsNone())return false;if(Skeleton->ContainsSlotName(Slot))return true;if(const TSet<FName>* Slots=Context.PlannedSkeletonSlots.Find(Skeleton->GetPathName()))return Slots->Contains(Slot);return false;
		}

		USkeleton* PlannedMontageSkeleton(const FUEPIEditContext& Context,const FJsonObject& Params)
		{
			const FString Path=ResolveAsset(Context,Params,TEXT("asset"));if(UAnimMontage* Montage=LoadObject<UAnimMontage>(nullptr,*Path))return Montage->GetSkeleton();const FString Ref=ReferenceId(Params,TEXT("asset"));if(const FString* SkeletonPath=Context.PlannedAssetSkeletons.Find(Ref))return LoadObject<USkeleton>(nullptr,**SkeletonPath);return nullptr;
		}

		class FUEPIAnimationOperation final:public IUEPIEditOperation
		{
		public:
			explicit FUEPIAnimationOperation(FUEPIEditOperationDescriptor InDescriptor):Descriptor(MoveTemp(InDescriptor)){}
			virtual FString GetOperationType()const override{return Descriptor.Name;}
			virtual FUEPIEditOperationDescriptor GetDescriptor()const override{return Descriptor;}
			virtual FUEPIEditResult Validate(const FUEPIEditContext& Context,const FJsonObject& Params)override{return Preview(Context,Params);}
			virtual FUEPIEditResult Preview(const FUEPIEditContext& Context,const FJsonObject& Params)override
			{
				const FString Type=Descriptor.Name==TEXT("animation.register_slot")?TEXT("animation.create_slot_group"):Descriptor.Name;
				if(Type==TEXT("animation.create_slot_group")){USkeleton* Skeleton=LoadObject<USkeleton>(nullptr,*JsonString(Params,TEXT("skeleton")));const FName Group(*JsonString(Params,TEXT("group_name"),TEXT("DefaultGroup")));const FName Slot(*JsonString(Params,TEXT("slot_name"),TEXT("DefaultSlot")));if(!Skeleton||Group.IsNone()||Slot.IsNone())return Failure(TEXT("UEPI_EDIT_ANIMATION_SLOT_INVALID"),TEXT("Valid skeleton, group_name, and slot_name are required."));TSharedRef<FJsonObject> Detail=MakeShared<FJsonObject>();Detail->SetStringField(TEXT("skeleton"),Skeleton->GetPathName());Detail->SetStringField(TEXT("slot_name"),Slot.ToString());return Success(TEXT("Skeleton slot preflight passed."),Detail);}
				if(Type==TEXT("animation.create_montage_from_sequence")){UAnimSequence* Sequence=LoadObject<UAnimSequence>(nullptr,*JsonString(Params,TEXT("sequence")));const FName Slot(*JsonString(Params,TEXT("slot_name"),TEXT("DefaultSlot")));FString Path,Name,Error;if(!Sequence||!Sequence->GetSkeleton()||!SplitDestination(Params,Path,Name,Error))return Failure(TEXT("UEPI_EDIT_MONTAGE_PREFLIGHT_FAILED"),Error.IsEmpty()?TEXT("Valid sequence, skeleton, and destination are required."):Error);if(!SlotAvailable(Context,Sequence->GetSkeleton(),Slot))return Failure(TEXT("UEPI_EDIT_ANIMATION_SLOT_MISSING"),FString::Printf(TEXT("Montage slot is not registered: %s"),*Slot.ToString()));const FString ObjectPath=FString::Printf(TEXT("%s/%s.%s"),*Path,*Name,*Name);if(LoadObject<UObject>(nullptr,*ObjectPath))return Failure(TEXT("UEPI_EDIT_TARGET_ALREADY_EXISTS"),FString::Printf(TEXT("Montage already exists: %s"),*ObjectPath));TSharedRef<FJsonObject> Detail=MakeShared<FJsonObject>();Detail->SetStringField(TEXT("asset_path"),ObjectPath);Detail->SetStringField(TEXT("skeleton"),Sequence->GetSkeleton()->GetPathName());return Success(TEXT("Montage creation preflight passed."),Detail);}
				USkeleton* Skeleton=PlannedMontageSkeleton(Context,Params);if(!Skeleton)return Failure(TEXT("UEPI_EDIT_MONTAGE_PREFLIGHT_FAILED"),TEXT("Montage target or prior Montage reference was not found."));UAnimMontage* Montage=LoadObject<UAnimMontage>(nullptr,*ResolveAsset(Context,Params,TEXT("asset")));
				if(Type==TEXT("animation.add_montage_slot_track")){const FName Slot(*JsonString(Params,TEXT("slot_name"),TEXT("DefaultSlot")));if(!SlotAvailable(Context,Skeleton,Slot))return Failure(TEXT("UEPI_EDIT_ANIMATION_SLOT_MISSING"),TEXT("Requested Montage slot is not registered."));}
				else if(Type==TEXT("animation.add_montage_segment")){UAnimSequence* Sequence=LoadObject<UAnimSequence>(nullptr,*JsonString(Params,TEXT("sequence")));if(!Sequence||Sequence->GetSkeleton()!=Skeleton)return Failure(TEXT("UEPI_EDIT_ANIMATION_SKELETON_MISMATCH"),TEXT("Montage segment sequence has an incompatible skeleton."));if(Montage){const FName Slot(*JsonString(Params,TEXT("slot_name"),TEXT("DefaultSlot")));if(!Montage->SlotAnimTracks.ContainsByPredicate([Slot](const FSlotAnimationTrack& Track){return Track.SlotName==Slot;}))return Failure(TEXT("UEPI_EDIT_ANIMATION_SLOT_MISSING"),TEXT("Montage slot track does not exist."));}}
				else if(Type==TEXT("animation.add_montage_section")){const FString Name=JsonString(Params,TEXT("section_name"),JsonString(Params,TEXT("name")));if(Name.IsEmpty()||(Montage&&Montage->GetSectionIndex(FName(*Name))!=INDEX_NONE))return Failure(TEXT("UEPI_EDIT_MONTAGE_SECTION_INVALID"),TEXT("Montage section name is empty or already exists."));}
				else if(Type==TEXT("animation.set_preview_mesh")){USkeletalMesh* Mesh=LoadObject<USkeletalMesh>(nullptr,*JsonString(Params,TEXT("preview_mesh")));if(!Mesh||Mesh->GetSkeleton()!=Skeleton)return Failure(TEXT("UEPI_EDIT_ANIMATION_SKELETON_MISMATCH"),TEXT("Preview mesh has an incompatible skeleton."));}
				return Success(TEXT("Montage operation preflight passed."));
			}

			virtual FUEPIEditResult Apply(const FUEPIEditContext& Context,const FJsonObject& Params)override
			{
				FUEPIEditResult Check=Preview(Context,Params);if(!Check.bOk)return Check;const FString Type=Descriptor.Name==TEXT("animation.register_slot")?TEXT("animation.create_slot_group"):Descriptor.Name;
				if(Type==TEXT("animation.create_slot_group")){USkeleton* Skeleton=LoadObject<USkeleton>(nullptr,*JsonString(Params,TEXT("skeleton")));const FName Group(*JsonString(Params,TEXT("group_name"),TEXT("DefaultGroup")));const FName Slot(*JsonString(Params,TEXT("slot_name"),TEXT("DefaultSlot")));Skeleton->Modify();Skeleton->AddSlotGroupName(Group);Skeleton->RegisterSlotNode(Slot);Skeleton->SetSlotGroupName(Slot,Group);Skeleton->PostEditChange();Skeleton->MarkPackageDirty();TSharedRef<FJsonObject> Detail=MakeShared<FJsonObject>();Detail->SetStringField(TEXT("skeleton"),Skeleton->GetPathName());Detail->SetStringField(TEXT("group_name"),Group.ToString());Detail->SetStringField(TEXT("slot_name"),Slot.ToString());return Success(TEXT("Skeleton slot group registered."),Detail);}
				if(Type==TEXT("animation.create_montage_from_sequence")){UAnimSequence* Sequence=LoadObject<UAnimSequence>(nullptr,*JsonString(Params,TEXT("sequence")));FString Path,Name,Error;SplitDestination(Params,Path,Name,Error);UAnimMontageFactory* Factory=NewObject<UAnimMontageFactory>();Factory->TargetSkeleton=Sequence->GetSkeleton();Factory->SourceAnimation=Sequence;UAnimMontage* Montage=Cast<UAnimMontage>(FAssetToolsModule::GetModule().Get().CreateAsset(Name,Path,UAnimMontage::StaticClass(),Factory,TEXT("UEPI")));if(!Montage)return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"),TEXT("AnimMontage creation failed."));const FName Slot(*JsonString(Params,TEXT("slot_name"),TEXT("DefaultSlot")));Montage->Modify();if(Montage->SlotAnimTracks.Num()==0)Montage->AddSlot(Slot);else Montage->SlotAnimTracks[0].SlotName=Slot;Montage->PostEditChange();Montage->MarkPackageDirty();TSharedRef<FJsonObject> Detail=MakeShared<FJsonObject>();Detail->SetStringField(TEXT("asset_path"),Montage->GetPathName());Detail->SetStringField(TEXT("sequence"),Sequence->GetPathName());Detail->SetStringField(TEXT("slot_name"),Slot.ToString());return Success(TEXT("AnimMontage created from sequence."),Detail);}
				UAnimMontage* Montage=LoadObject<UAnimMontage>(nullptr,*ResolveAsset(Context,Params,TEXT("asset")));Montage->Modify();TSharedRef<FJsonObject> Detail=MakeShared<FJsonObject>();Detail->SetStringField(TEXT("asset"),Montage->GetPathName());
				if(Type==TEXT("animation.add_montage_slot_track")){const FName Slot(*JsonString(Params,TEXT("slot_name"),TEXT("DefaultSlot")));const bool bExists=Montage->SlotAnimTracks.ContainsByPredicate([Slot](const FSlotAnimationTrack& Track){return Track.SlotName==Slot;});if(!bExists)Montage->AddSlot(Slot);Detail->SetStringField(TEXT("slot_name"),Slot.ToString());Detail->SetBoolField(TEXT("already_exists"),bExists);}
				else if(Type==TEXT("animation.add_montage_segment")){UAnimSequence* Sequence=LoadObject<UAnimSequence>(nullptr,*JsonString(Params,TEXT("sequence")));const FName Slot(*JsonString(Params,TEXT("slot_name"),TEXT("DefaultSlot")));FSlotAnimationTrack* Track=Montage->SlotAnimTracks.FindByPredicate([Slot](const FSlotAnimationTrack& Candidate){return Candidate.SlotName==Slot;});FAnimSegment Segment;Segment.SetAnimReference(Sequence,true);Segment.StartPos=static_cast<float>(JsonInt(Params,TEXT("start_position_ms"),0))/1000.0f;double Number=0;if(Params.TryGetNumberField(TEXT("start_position"),Number))Segment.StartPos=Number;if(Params.TryGetNumberField(TEXT("anim_start_time"),Number))Segment.AnimStartTime=Number;if(Params.TryGetNumberField(TEXT("anim_end_time"),Number))Segment.AnimEndTime=Number;if(Params.TryGetNumberField(TEXT("play_rate"),Number))Segment.AnimPlayRate=Number;Segment.LoopingCount=FMath::Max(1,JsonInt(Params,TEXT("loop_count"),1));Track->AnimTrack.AnimSegments.Add(Segment);Montage->SetCompositeLength(FMath::Max(Montage->GetPlayLength(),Segment.GetEndPos()));Detail->SetStringField(TEXT("sequence"),Sequence->GetPathName());Detail->SetStringField(TEXT("slot_name"),Slot.ToString());}
				else if(Type==TEXT("animation.add_montage_section")){const FName Name(*JsonString(Params,TEXT("section_name"),JsonString(Params,TEXT("name"))));double Start=0;Params.TryGetNumberField(TEXT("start_time"),Start);Montage->AddAnimCompositeSection(Name,Start);Detail->SetStringField(TEXT("section_name"),Name.ToString());Detail->SetNumberField(TEXT("start_time"),Start);}
				else if(Type==TEXT("animation.set_montage_blend")){double Number=0;if(Params.TryGetNumberField(TEXT("blend_in_time"),Number))Montage->BlendIn.SetBlendTime(Number);if(Params.TryGetNumberField(TEXT("blend_out_time"),Number))Montage->BlendOut.SetBlendTime(Number);if(Params.TryGetNumberField(TEXT("blend_out_trigger_time"),Number))Montage->BlendOutTriggerTime=Number;Detail->SetNumberField(TEXT("blend_in_time"),Montage->GetDefaultBlendInTime());Detail->SetNumberField(TEXT("blend_out_time"),Montage->GetDefaultBlendOutTime());}
				else{USkeletalMesh* Mesh=LoadObject<USkeletalMesh>(nullptr,*JsonString(Params,TEXT("preview_mesh")));Montage->SetPreviewMesh(Mesh);Detail->SetStringField(TEXT("preview_mesh"),Mesh->GetPathName());}
				Montage->PostEditChange();Montage->MarkPackageDirty();return Success(TEXT("AnimMontage updated."),Detail);
			}
		private:FUEPIEditOperationDescriptor Descriptor;
		};
	}
	TSharedRef<IUEPIEditOperation> MakeUEPIAnimationOperation(const FUEPIEditOperationDescriptor& Descriptor){return MakeShared<AnimationOperationsPrivate::FUEPIAnimationOperation>(Descriptor);}
}
