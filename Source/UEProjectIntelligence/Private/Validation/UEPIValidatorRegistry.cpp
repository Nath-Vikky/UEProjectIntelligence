#include "Validation/UEPIValidatorRegistry.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimationAsset.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInstance.h"

namespace UE::ProjectIntelligence
{
	namespace
	{
		FUEPIValidationResult ResultFor(const UObject* Object, const FString& Validator, bool bOk, const FString& Message)
		{
			FUEPIValidationResult Result;
			Result.bOk = bOk;
			Result.Asset = Object ? Object->GetPathName() : FString();
			Result.ClassPath = Object && Object->GetClass() ? Object->GetClass()->GetPathName() : FString();
			Result.Validator = Validator;
			Result.Message = Message;
			return Result;
		}

		class FUEPIBlueprintValidator final : public IUEPIAssetValidator
		{
		public:
			bool Supports(const UObject* Object) const override { return Object && Object->IsA<UBlueprint>(); }
			FUEPIValidationResult Validate(UObject* Object) const override
			{
				UBlueprint* Blueprint = CastChecked<UBlueprint>(Object);
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
				const bool bOk = Blueprint->Status != BS_Error && Blueprint->Status != BS_Unknown;
				return ResultFor(Blueprint, Blueprint->IsA<UAnimBlueprint>() ? TEXT("anim_blueprint") : TEXT("blueprint"), bOk, bOk ? TEXT("Blueprint compile passed.") : TEXT("Blueprint compile status is error/unknown."));
			}
		};

		class FUEPIAnimationValidator final : public IUEPIAssetValidator
		{
		public:
			bool Supports(const UObject* Object) const override { return Object && Object->IsA<UAnimationAsset>(); }
			FUEPIValidationResult Validate(UObject* Object) const override
			{
				UAnimationAsset* Asset = CastChecked<UAnimationAsset>(Object);
				bool bOk = Asset->GetSkeleton() != nullptr;
				FString Message = bOk ? TEXT("Animation asset has a valid skeleton.") : TEXT("Animation asset has no skeleton.");
				if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
				{
					for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
					{
						if (!Montage->GetSkeleton()->ContainsSlotName(Track.SlotName)) { bOk = false; Message = FString::Printf(TEXT("Montage slot is not registered: %s"), *Track.SlotName.ToString()); break; }
						for (const FAnimSegment& Segment : Track.AnimTrack.AnimSegments)
						{
							const UAnimSequenceBase* Sequence = Segment.GetAnimReference();
							if (!Sequence || Sequence->GetSkeleton() != Montage->GetSkeleton()) { bOk = false; Message = TEXT("Montage contains a missing or skeleton-incompatible segment."); break; }
						}
						if (!bOk) break;
					}
				}
				return ResultFor(Asset, TEXT("animation"), bOk, Message);
			}
		};

		class FUEPIMaterialValidator final : public IUEPIAssetValidator
		{
		public:
			bool Supports(const UObject* Object) const override { return Object && Object->IsA<UMaterialInstance>(); }
			FUEPIValidationResult Validate(UObject* Object) const override
			{
				const UMaterialInstance* Instance = CastChecked<UMaterialInstance>(Object);
				return ResultFor(Instance, TEXT("material"), Instance->Parent != nullptr, Instance->Parent ? TEXT("Material Instance has a parent.") : TEXT("Material Instance has no parent."));
			}
		};

		class FUEPIDataAssetValidator final : public IUEPIAssetValidator
		{
		public:
			bool Supports(const UObject* Object) const override { return Object && Object->IsA<UDataAsset>(); }
			FUEPIValidationResult Validate(UObject* Object) const override { return ResultFor(Object, TEXT("data_asset"), IsValid(Object) && !Object->GetClass()->HasAnyClassFlags(CLASS_Abstract), TEXT("DataAsset object and class are valid.")); }
		};

		class FUEPIGenericValidator final : public IUEPIAssetValidator
		{
		public:
			bool Supports(const UObject*) const override { return true; }
			FUEPIValidationResult Validate(UObject* Object) const override { return ResultFor(Object, TEXT("generic_uobject"), IsValid(Object), IsValid(Object) ? TEXT("UObject is valid.") : TEXT("UObject is invalid.")); }
		};
	}

	FUEPIValidatorRegistry& FUEPIValidatorRegistry::Get()
	{
		static FUEPIValidatorRegistry Registry;
		return Registry;
	}

	FUEPIValidatorRegistry::FUEPIValidatorRegistry()
	{
		Validators = { MakeShared<FUEPIBlueprintValidator>(), MakeShared<FUEPIAnimationValidator>(), MakeShared<FUEPIMaterialValidator>(), MakeShared<FUEPIDataAssetValidator>(), MakeShared<FUEPIGenericValidator>() };
	}

	FUEPIValidationResult FUEPIValidatorRegistry::Validate(UObject* Object) const
	{
		for (const TSharedRef<IUEPIAssetValidator>& Validator : Validators) if (Validator->Supports(Object)) return Validator->Validate(Object);
		return ResultFor(Object, TEXT("none"), false, TEXT("No validator accepted the object."));
	}
}
