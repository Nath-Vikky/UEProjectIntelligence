#include "Operations/UEPIMaterialOperations.h"

#include "AssetToolsModule.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/Texture.h"
#include "EngineUtils.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "GameFramework/Actor.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialTypes.h"
#include "Misc/PackageName.h"
#include "UEPISettings.h"

namespace UE::ProjectIntelligence
{
	namespace MaterialOperationsPrivate
	{
		FString JsonString(const FJsonObject& Object, const TCHAR* Field, const FString& DefaultValue = FString())
		{
			FString Value; return Object.TryGetStringField(Field, Value) ? Value : DefaultValue;
		}

		int32 JsonInt(const FJsonObject& Object, const TCHAR* Field, int32 DefaultValue = 0)
		{
			int32 Value = 0; return Object.TryGetNumberField(Field, Value) ? Value : DefaultValue;
		}

		FUEPIEditResult Failure(const FString& Code, const FString& Message)
		{
			FUEPIEditResult Result; Result.ErrorCode = Code; Result.Message = Message; return Result;
		}

		FUEPIEditResult Success(const FString& Message, const TSharedPtr<FJsonObject>& Detail = nullptr)
		{
			FUEPIEditResult Result; Result.bOk = true; Result.Message = Message; Result.Result = Detail; return Result;
		}

		FString ResolveAsset(const FUEPIEditContext& Context, const FJsonObject& Params, const TCHAR* Field, const TCHAR* Alternate = nullptr)
		{
			FString Direct = JsonString(Params, Field, Alternate ? JsonString(Params, Alternate) : FString());
			if (!Direct.IsEmpty()) return Direct;
			const TSharedPtr<FJsonObject>* Reference = nullptr;
			if (!Params.TryGetObjectField(Field, Reference) || !Reference || !Reference->IsValid()) return FString();
			FString Ref = JsonString(**Reference, TEXT("$ref")); int32 Hash = INDEX_NONE; if (Ref.FindChar(TEXT('#'), Hash)) Ref = Ref.Left(Hash);
			if (const FString* Resolved = Context.ResolvedAssets.Find(Ref)) return *Resolved;
			return FString();
		}

		bool SplitDestination(const FJsonObject& Params, const FString& DefaultName, FString& OutPath, FString& OutName, FString& OutError)
		{
			FString Destination = JsonString(Params, TEXT("destination"), JsonString(Params, TEXT("destination_asset"))); Destination.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (!Destination.IsEmpty())
			{
				FString Package = Destination; int32 Dot = INDEX_NONE; if (Package.FindChar(TEXT('.'), Dot)) Package = Package.Left(Dot);
				OutPath = FPackageName::GetLongPackagePath(Package); OutName = FPackageName::GetLongPackageAssetName(Package);
			}
			OutPath = JsonString(Params, TEXT("destination_path"), JsonString(Params, TEXT("folder"), OutPath)); OutPath.ReplaceInline(TEXT("\\"), TEXT("/")); while (OutPath.EndsWith(TEXT("/"))) OutPath.LeftChopInline(1);
			OutName = JsonString(Params, TEXT("name"), JsonString(Params, TEXT("asset_name"), OutName.IsEmpty() ? DefaultName : OutName)).TrimStartAndEnd();
			if ((!OutPath.Equals(TEXT("/Game")) && !OutPath.StartsWith(TEXT("/Game/"))) || OutPath.Contains(TEXT("..")) || OutName.IsEmpty() || OutName.Contains(TEXT("/")) || OutName.Contains(TEXT(".")))
			{
				OutError = TEXT("Material destination must be a valid asset name under /Game."); return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> AssetJson(UObject* Object)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (Object) { Result->SetStringField(TEXT("name"), Object->GetName()); Result->SetStringField(TEXT("path"), Object->GetPathName()); Result->SetStringField(TEXT("class"), Object->GetClass()->GetPathName()); }
			return Result;
		}

		TArray<AActor*> ResolveActors(const FJsonObject& Params)
		{
			TArray<FString> Paths;
			const TSharedPtr<FJsonObject>* Targets = nullptr;
			if (Params.TryGetObjectField(TEXT("targets"), Targets) && Targets && Targets->IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if ((*Targets)->TryGetArrayField(TEXT("paths"), Values) && Values) for (const TSharedPtr<FJsonValue>& Value : *Values) { FString Path; if (Value.IsValid() && Value->TryGetString(Path)) Paths.AddUnique(Path); }
			}
			const FString Direct = JsonString(Params, TEXT("actor"), JsonString(Params, TEXT("path"))); if (!Direct.IsEmpty()) Paths.AddUnique(Direct);
			TArray<AActor*> Result; UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr; if (!World) return Result;
			for (TActorIterator<AActor> It(World); It; ++It) if (Paths.Contains(It->GetPathName()) || Paths.Contains(It->GetName()) || Paths.Contains(It->GetActorLabel())) Result.AddUnique(*It);
			return Result;
		}

		UPrimitiveComponent* ResolvePrimitive(AActor* Actor, const FString& Name)
		{
			if (!Actor) return nullptr; TArray<UPrimitiveComponent*> Components; Actor->GetComponents<UPrimitiveComponent>(Components);
			for (UPrimitiveComponent* Component : Components) if (Component && (Name.IsEmpty() || Component->GetName().Equals(Name, ESearchCase::IgnoreCase))) return Component;
			return nullptr;
		}

		FString NormalizeObjectPath(const FString& Raw)
		{
			if (Raw.Contains(TEXT("."))) return Raw; const FString Name = FPackageName::GetLongPackageAssetName(Raw); return Name.IsEmpty() ? Raw : Raw + TEXT(".") + Name;
		}

		bool JsonColor(const FJsonObject& Object, FLinearColor& OutColor)
		{
			double R = 0, G = 0, B = 0, A = 1; if (!Object.TryGetNumberField(TEXT("r"), R) || !Object.TryGetNumberField(TEXT("g"), G) || !Object.TryGetNumberField(TEXT("b"), B)) return false; Object.TryGetNumberField(TEXT("a"), A); OutColor = FLinearColor(R, G, B, A); return true;
		}

		class FUEPIMaterialOperation final : public IUEPIEditOperation
		{
		public:
			explicit FUEPIMaterialOperation(FUEPIEditOperationDescriptor InDescriptor) : Descriptor(MoveTemp(InDescriptor)) {}
			virtual FString GetOperationType() const override { return Descriptor.Name; }
			virtual FUEPIEditOperationDescriptor GetDescriptor() const override { return Descriptor; }
			virtual FUEPIEditResult Validate(const FUEPIEditContext& Context, const FJsonObject& Params) override { return Preview(Context, Params); }

			virtual FUEPIEditResult Preview(const FUEPIEditContext& Context, const FJsonObject& Params) override
			{
				const UUEPISettings* Settings = GetDefault<UUEPISettings>();
				if (!Settings || !Settings->bAllowMaterialEdits) return Failure(TEXT("UEPI_EDIT_CAPABILITY_DISABLED"), TEXT("Material edits are disabled in UEPI Project Settings."));
				if (Descriptor.Name == TEXT("material.apply_to_blueprint_component") && !Settings->bAllowBlueprintEdits) return Failure(TEXT("UEPI_EDIT_CAPABILITY_DISABLED"), TEXT("Blueprint edits are required for Blueprint component material assignment."));
				if (Descriptor.Name == TEXT("material.create_instance"))
				{
					const FString ParentPath = ResolveAsset(Context, Params, TEXT("parent"), TEXT("parent_material")); UMaterialInterface* Parent = ParentPath.IsEmpty() ? nullptr : LoadObject<UMaterialInterface>(nullptr, *ParentPath);
					if (!Parent) return Failure(TEXT("UEPI_EDIT_MATERIAL_PARENT_INVALID"), FString::Printf(TEXT("Material parent was not found: %s"), *ParentPath));
					FString Path, Name, Error; if (!SplitDestination(Params, Parent->GetName() + TEXT("_Inst"), Path, Name, Error)) return Failure(TEXT("UEPI_EDIT_DESTINATION_INVALID"), Error);
					const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *Path, *Name, *Name); if (LoadObject<UObject>(nullptr, *ObjectPath)) return Failure(TEXT("UEPI_EDIT_TARGET_ALREADY_EXISTS"), FString::Printf(TEXT("Material instance already exists: %s"), *ObjectPath));
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("asset_path"), ObjectPath); return Success(TEXT("Material instance preflight passed."), Detail);
				}
				const bool bAssignment = Descriptor.Name == TEXT("material.apply_to_actor") || Descriptor.Name == TEXT("material.apply_to_blueprint_component");
				FString MaterialPath = bAssignment ? ResolveAsset(Context, Params, TEXT("material")) : ResolveAsset(Context, Params, TEXT("asset"), TEXT("material"));
				if (Descriptor.Name.StartsWith(TEXT("material.set_")))
				{
					UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *MaterialPath); const bool bPlanned = Context.ResolvedAssets.FindKey(MaterialPath) != nullptr;
					if (!Instance && !bPlanned) return Failure(TEXT("UEPI_EDIT_MATERIAL_INSTANCE_INVALID"), FString::Printf(TEXT("Material instance was not found: %s"), *MaterialPath));
					if (JsonString(Params, TEXT("parameter"), JsonString(Params, TEXT("name"))).IsEmpty()) return Failure(TEXT("UEPI_EDIT_MATERIAL_PARAMETER_INVALID"), TEXT("Material parameter name is required."));
					if (Descriptor.Name == TEXT("material.set_scalar_parameter")) { double Value = 0; if (!Params.TryGetNumberField(TEXT("value"), Value)) return Failure(TEXT("UEPI_EDIT_MATERIAL_VALUE_INVALID"), TEXT("Scalar parameter requires a numeric value.")); }
					else if (Descriptor.Name == TEXT("material.set_vector_parameter")) { const TSharedPtr<FJsonObject>* Value = nullptr; const FJsonObject* Source = &Params; if (Params.TryGetObjectField(TEXT("value"), Value) && Value && Value->IsValid()) Source = Value->Get(); FLinearColor Color; if (!JsonColor(*Source, Color)) return Failure(TEXT("UEPI_EDIT_MATERIAL_VALUE_INVALID"), TEXT("Vector parameter requires r, g, b, and optional a.")); }
					else { const FString Texture = ResolveAsset(Context, Params, TEXT("texture"), TEXT("value")); if (!LoadObject<UTexture>(nullptr, *Texture)) return Failure(TEXT("UEPI_EDIT_MATERIAL_VALUE_INVALID"), FString::Printf(TEXT("Texture was not found: %s"), *Texture)); }
					return Success(TEXT("Material parameter preflight passed."));
				}
				UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath); const bool bPlanned = Context.ResolvedAssets.FindKey(MaterialPath) != nullptr;
				if (!Material && !bPlanned) return Failure(TEXT("UEPI_EDIT_MATERIAL_INVALID"), FString::Printf(TEXT("Material was not found: %s"), *MaterialPath));
				const FString ComponentName = JsonString(Params, TEXT("component"), JsonString(Params, TEXT("component_name")));
				if (Descriptor.Name == TEXT("material.apply_to_actor"))
				{
					const TArray<AActor*> Actors = ResolveActors(Params); if (Actors.Num() == 0) return Failure(TEXT("UEPI_EDIT_ACTOR_NOT_FOUND"), TEXT("No editor-world actor target was resolved."));
					for (AActor* Actor : Actors) if (!ResolvePrimitive(Actor, ComponentName)) return Failure(TEXT("UEPI_EDIT_COMPONENT_NOT_FOUND"), FString::Printf(TEXT("Primitive component was not found on %s."), *Actor->GetPathName()));
				}
				else
				{
					const FString BlueprintPath = NormalizeObjectPath(ResolveAsset(Context, Params, TEXT("asset"), TEXT("blueprint"))); UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
					USCS_Node* Node = Blueprint && Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->FindSCSNode(FName(*ComponentName)) : nullptr;
					if (!Blueprint || !Node || !Cast<UPrimitiveComponent>(Node->ComponentTemplate)) return Failure(TEXT("UEPI_EDIT_COMPONENT_NOT_FOUND"), TEXT("Blueprint primitive component template was not found."));
				}
				return Success(TEXT("Material assignment preflight passed."));
			}

			virtual FUEPIEditResult Apply(const FUEPIEditContext& Context, const FJsonObject& Params) override
			{
				FUEPIEditResult Check = Preview(Context, Params); if (!Check.bOk) return Check;
				if (Descriptor.Name == TEXT("material.create_instance"))
				{
					UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ResolveAsset(Context, Params, TEXT("parent"), TEXT("parent_material"))); FString Path, Name, Error; SplitDestination(Params, Parent->GetName() + TEXT("_Inst"), Path, Name, Error);
					UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(); Factory->InitialParent = Parent;
					UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, Path, UMaterialInstanceConstant::StaticClass(), Factory, TEXT("UEPI")));
					if (!Instance) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("Failed to create MaterialInstanceConstant."));
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetObjectField(TEXT("asset"), AssetJson(Instance)); Detail->SetStringField(TEXT("parent"), Parent->GetPathName()); return Success(TEXT("Material instance created."), Detail);
				}
				const bool bAssignment = Descriptor.Name == TEXT("material.apply_to_actor") || Descriptor.Name == TEXT("material.apply_to_blueprint_component");
				const FString MaterialPath = bAssignment ? ResolveAsset(Context, Params, TEXT("material")) : ResolveAsset(Context, Params, TEXT("asset"), TEXT("material"));
				if (Descriptor.Name.StartsWith(TEXT("material.set_")))
				{
					UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *MaterialPath); const FString Parameter = JsonString(Params, TEXT("parameter"), JsonString(Params, TEXT("name"))); Instance->Modify();
					if (Descriptor.Name == TEXT("material.set_scalar_parameter")) { double Value = 0; Params.TryGetNumberField(TEXT("value"), Value); Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(FName(*Parameter)), static_cast<float>(Value)); }
					else if (Descriptor.Name == TEXT("material.set_vector_parameter")) { const TSharedPtr<FJsonObject>* Value = nullptr; const FJsonObject* Source = &Params; if (Params.TryGetObjectField(TEXT("value"), Value) && Value && Value->IsValid()) Source = Value->Get(); FLinearColor Color; JsonColor(*Source, Color); Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(FName(*Parameter)), Color); }
					else { UTexture* Texture = LoadObject<UTexture>(nullptr, *ResolveAsset(Context, Params, TEXT("texture"), TEXT("value"))); Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(*Parameter)), Texture); }
					Instance->PostEditChange(); Instance->MarkPackageDirty(); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("asset"), Instance->GetPathName()); Detail->SetStringField(TEXT("parameter"), Parameter); return Success(TEXT("Material instance parameter updated."), Detail);
				}
				UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath); const FString ComponentName = JsonString(Params, TEXT("component"), JsonString(Params, TEXT("component_name"))); const int32 Index = JsonInt(Params, TEXT("material_index"), 0);
				if (Descriptor.Name == TEXT("material.apply_to_actor"))
				{
					TArray<TSharedPtr<FJsonValue>> Applied; for (AActor* Actor : ResolveActors(Params)) { UPrimitiveComponent* Component = ResolvePrimitive(Actor, ComponentName); Actor->Modify(); Component->Modify(); Component->SetMaterial(Index, Material); TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("actor"), Actor->GetPathName()); Item->SetStringField(TEXT("component"), Component->GetName()); Item->SetNumberField(TEXT("material_index"), Index); Applied.Add(MakeShared<FJsonValueObject>(Item)); }
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("material"), Material->GetPathName()); Detail->SetArrayField(TEXT("applied"), Applied); return Success(TEXT("Material applied to actor component(s)."), Detail);
				}
				const FString BlueprintPath = NormalizeObjectPath(ResolveAsset(Context, Params, TEXT("asset"), TEXT("blueprint"))); UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath); USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(FName(*ComponentName)); UPrimitiveComponent* Template = CastChecked<UPrimitiveComponent>(Node->ComponentTemplate);
				Template->Modify(); Template->SetMaterial(Index, Material); FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("blueprint"), Blueprint->GetPathName()); Detail->SetStringField(TEXT("component"), ComponentName); Detail->SetStringField(TEXT("material"), Material->GetPathName()); Detail->SetNumberField(TEXT("material_index"), Index); return Success(TEXT("Material applied to Blueprint component template."), Detail);
			}

		private:
			FUEPIEditOperationDescriptor Descriptor;
		};
	}

	TSharedRef<IUEPIEditOperation> MakeUEPIMaterialOperation(const FUEPIEditOperationDescriptor& Descriptor)
	{
		return MakeShared<MaterialOperationsPrivate::FUEPIMaterialOperation>(Descriptor);
	}
}
