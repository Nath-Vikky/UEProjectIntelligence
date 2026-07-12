#include "Operations/UEPIInputOperations.h"

#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "UEPISettings.h"

#if UEPI_WITH_ENHANCED_INPUT
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputEditorModule.h"
#include "InputMappingContext.h"
#endif

namespace UE::ProjectIntelligence
{
	namespace InputOperationsPrivate
	{
		FString JsonString(const FJsonObject& Object, const TCHAR* Field, const FString& DefaultValue = FString())
		{
			FString Value;
			return Object.TryGetStringField(Field, Value) ? Value : DefaultValue;
		}

		FUEPIEditResult Failure(const FString& Code, const FString& Message)
		{
			FUEPIEditResult Result; Result.ErrorCode = Code; Result.Message = Message; return Result;
		}

		FUEPIEditResult Success(const FString& Message, const TSharedPtr<FJsonObject>& Detail = nullptr)
		{
			FUEPIEditResult Result; Result.bOk = true; Result.Message = Message; Result.Result = Detail; return Result;
		}

		bool SplitDestination(const FJsonObject& Params, const FString& DefaultName, FString& OutPath, FString& OutName, FString& OutError)
		{
			FString Destination = JsonString(Params, TEXT("destination"), JsonString(Params, TEXT("destination_asset")));
			Destination.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (!Destination.IsEmpty())
			{
				FString Package = Destination;
				int32 Dot = INDEX_NONE;
				if (Package.FindChar(TEXT('.'), Dot)) Package = Package.Left(Dot);
				OutPath = FPackageName::GetLongPackagePath(Package);
				OutName = FPackageName::GetLongPackageAssetName(Package);
			}
			OutPath = JsonString(Params, TEXT("destination_path"), JsonString(Params, TEXT("folder"), OutPath));
			OutPath.ReplaceInline(TEXT("\\"), TEXT("/"));
			while (OutPath.EndsWith(TEXT("/"))) OutPath.LeftChopInline(1);
			OutName = JsonString(Params, TEXT("name"), JsonString(Params, TEXT("asset_name"), OutName.IsEmpty() ? DefaultName : OutName)).TrimStartAndEnd();
			if ((!OutPath.Equals(TEXT("/Game")) && !OutPath.StartsWith(TEXT("/Game/"))) || OutPath.Contains(TEXT("..")) || OutName.IsEmpty() || OutName.Contains(TEXT("/")) || OutName.Contains(TEXT(".")))
			{
				OutError = TEXT("Enhanced Input destination must be a valid asset name under /Game.");
				return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> AssetJson(UObject* Asset)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (Asset)
			{
				Result->SetStringField(TEXT("name"), Asset->GetName()); Result->SetStringField(TEXT("path"), Asset->GetPathName()); Result->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
			}
			return Result;
		}

#if UEPI_WITH_ENHANCED_INPUT
		bool ParseValueType(const FString& Text, EInputActionValueType& OutType)
		{
			const FString Type = Text.ToLower();
			if (Type.IsEmpty() || Type == TEXT("bool") || Type == TEXT("boolean") || Type == TEXT("digital")) OutType = EInputActionValueType::Boolean;
			else if (Type == TEXT("axis1d") || Type == TEXT("float") || Type == TEXT("1d")) OutType = EInputActionValueType::Axis1D;
			else if (Type == TEXT("axis2d") || Type == TEXT("vector2d") || Type == TEXT("2d")) OutType = EInputActionValueType::Axis2D;
			else if (Type == TEXT("axis3d") || Type == TEXT("vector") || Type == TEXT("3d")) OutType = EInputActionValueType::Axis3D;
			else return false;
			return true;
		}

		UInputMappingContext* LoadContext(const FJsonObject& Params)
		{
			const FString Path = JsonString(Params, TEXT("context"), JsonString(Params, TEXT("mapping_context"), JsonString(Params, TEXT("asset"))));
			return Path.IsEmpty() ? nullptr : LoadObject<UInputMappingContext>(nullptr, *Path);
		}

		UInputAction* LoadAction(const FJsonObject& Params)
		{
			const FString Path = JsonString(Params, TEXT("action"), JsonString(Params, TEXT("input_action")));
			return Path.IsEmpty() ? nullptr : LoadObject<UInputAction>(nullptr, *Path);
		}

		TSharedRef<FJsonObject> MappingJson(const FEnhancedActionKeyMapping& Mapping)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("action"), Mapping.Action ? Mapping.Action->GetPathName() : FString()); Result->SetStringField(TEXT("key"), Mapping.Key.ToString());
			return Result;
		}
#endif

		class FUEPIInputOperation final : public IUEPIEditOperation
		{
		public:
			explicit FUEPIInputOperation(FUEPIEditOperationDescriptor InDescriptor) : Descriptor(MoveTemp(InDescriptor)) {}
			virtual FString GetOperationType() const override { return Descriptor.Name; }
			virtual FUEPIEditOperationDescriptor GetDescriptor() const override { return Descriptor; }
			virtual FUEPIEditResult Validate(const FUEPIEditContext& Context, const FJsonObject& Params) override { return Preview(Context, Params); }

			virtual FUEPIEditResult Preview(const FUEPIEditContext&, const FJsonObject& Params) override
			{
#if !UEPI_WITH_ENHANCED_INPUT
				return Failure(TEXT("UEPI_EDIT_REQUIRED_PLUGIN_UNAVAILABLE"), TEXT("Enhanced Input is not enabled for this project build."));
#else
				const UUEPISettings* Settings = GetDefault<UUEPISettings>();
				if (!Settings || !Settings->bAllowInputEdits) return Failure(TEXT("UEPI_EDIT_CAPABILITY_DISABLED"), TEXT("Enhanced Input edits are disabled in UEPI Project Settings."));
				if (Descriptor.Name == TEXT("input.create_action") || Descriptor.Name == TEXT("input.create_mapping_context"))
				{
					FString Path, Name, Error;
					if (!SplitDestination(Params, Descriptor.Name == TEXT("input.create_action") ? TEXT("IA_UEPIAction") : TEXT("IMC_UEPIContext"), Path, Name, Error)) return Failure(TEXT("UEPI_EDIT_DESTINATION_INVALID"), Error);
					const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *Path, *Name, *Name);
					if (LoadObject<UObject>(nullptr, *ObjectPath)) return Failure(TEXT("UEPI_EDIT_TARGET_ALREADY_EXISTS"), FString::Printf(TEXT("Enhanced Input asset already exists: %s"), *ObjectPath));
					if (Descriptor.Name == TEXT("input.create_action"))
					{
						EInputActionValueType ValueType;
						if (!ParseValueType(JsonString(Params, TEXT("value_type"), TEXT("bool")), ValueType)) return Failure(TEXT("UEPI_EDIT_INPUT_VALUE_TYPE_INVALID"), TEXT("InputAction value_type is invalid."));
					}
					return Success(TEXT("Enhanced Input asset preflight passed."));
				}
				UInputMappingContext* Context = LoadContext(Params); UInputAction* Action = LoadAction(Params); const FKey Key(FName(*JsonString(Params, TEXT("key"))));
				if (!Context || !Action || !Key.IsValid()) return Failure(TEXT("UEPI_EDIT_INPUT_MAPPING_INVALID"), TEXT("A valid mapping context, input action, and key are required."));
				if (Descriptor.Name == TEXT("input.remove_key_mapping"))
				{
					const bool bExists = Context->GetMappings().ContainsByPredicate([Action, Key](const FEnhancedActionKeyMapping& Item) { return Item.Action == Action && Item.Key == Key; });
					if (!bExists) return Failure(TEXT("UEPI_EDIT_INPUT_MAPPING_NOT_FOUND"), TEXT("No matching Enhanced Input key mapping exists."));
				}
				return Success(TEXT("Enhanced Input mapping preflight passed."));
#endif
			}

			virtual FUEPIEditResult Apply(const FUEPIEditContext& Context, const FJsonObject& Params) override
			{
				FUEPIEditResult Check = Preview(Context, Params); if (!Check.bOk) return Check;
#if !UEPI_WITH_ENHANCED_INPUT
				return Check;
#else
				if (Descriptor.Name == TEXT("input.create_action"))
				{
					FString Path, Name, Error; SplitDestination(Params, TEXT("IA_UEPIAction"), Path, Name, Error);
					UInputAction_Factory* Factory = NewObject<UInputAction_Factory>(); Factory->InputActionClass = UInputAction::StaticClass();
					UInputAction* Action = Cast<UInputAction>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, Path, UInputAction::StaticClass(), Factory, TEXT("UEPI")));
					if (!Action) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("Failed to create InputAction."));
					EInputActionValueType ValueType; ParseValueType(JsonString(Params, TEXT("value_type"), TEXT("bool")), ValueType); Action->Modify(); Action->ValueType = ValueType;
					const FString Description = JsonString(Params, TEXT("description")); if (!Description.IsEmpty()) Action->ActionDescription = FText::FromString(Description);
					Action->PostEditChange(); Action->MarkPackageDirty(); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetObjectField(TEXT("asset"), AssetJson(Action)); Detail->SetStringField(TEXT("value_type"), StaticEnum<EInputActionValueType>()->GetNameStringByValue(static_cast<int64>(Action->ValueType)));
					return Success(TEXT("InputAction created."), Detail);
				}
				if (Descriptor.Name == TEXT("input.create_mapping_context"))
				{
					FString Path, Name, Error; SplitDestination(Params, TEXT("IMC_UEPIContext"), Path, Name, Error);
					UInputMappingContext_Factory* Factory = NewObject<UInputMappingContext_Factory>(); Factory->InputMappingContextClass = UInputMappingContext::StaticClass();
					UInputMappingContext* MappingContext = Cast<UInputMappingContext>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, Path, UInputMappingContext::StaticClass(), Factory, TEXT("UEPI")));
					if (!MappingContext) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("Failed to create InputMappingContext."));
					MappingContext->Modify(); const FString Description = JsonString(Params, TEXT("description")); if (!Description.IsEmpty()) MappingContext->ContextDescription = FText::FromString(Description);
					MappingContext->PostEditChange(); MappingContext->MarkPackageDirty(); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetObjectField(TEXT("asset"), AssetJson(MappingContext)); Detail->SetNumberField(TEXT("mapping_count"), MappingContext->GetMappings().Num());
					return Success(TEXT("InputMappingContext created."), Detail);
				}
				UInputMappingContext* MappingContext = LoadContext(Params); UInputAction* Action = LoadAction(Params); const FKey Key(FName(*JsonString(Params, TEXT("key")))); const int32 Before = MappingContext->GetMappings().Num(); MappingContext->Modify();
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("context"), MappingContext->GetPathName()); Detail->SetStringField(TEXT("action"), Action->GetPathName()); Detail->SetStringField(TEXT("key"), Key.ToString()); Detail->SetNumberField(TEXT("mapping_count_before"), Before);
				if (Descriptor.Name == TEXT("input.add_key_mapping")) { FEnhancedActionKeyMapping& Mapping = MappingContext->MapKey(Action, Key); Detail->SetObjectField(TEXT("mapping"), MappingJson(Mapping)); }
				else MappingContext->UnmapKey(Action, Key);
				MappingContext->PostEditChange(); MappingContext->MarkPackageDirty(); Detail->SetNumberField(TEXT("mapping_count_after"), MappingContext->GetMappings().Num());
				return Success(Descriptor.Name == TEXT("input.add_key_mapping") ? TEXT("Enhanced Input key mapping added.") : TEXT("Enhanced Input key mapping removed."), Detail);
#endif
			}

		private:
			FUEPIEditOperationDescriptor Descriptor;
		};
	}

	TSharedRef<IUEPIEditOperation> MakeUEPIInputOperation(const FUEPIEditOperationDescriptor& Descriptor)
	{
		return MakeShared<InputOperationsPrivate::FUEPIInputOperation>(Descriptor);
	}
}
