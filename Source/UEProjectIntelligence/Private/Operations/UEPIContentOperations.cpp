#include "Operations/UEPIContentOperations.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataAsset.h"
#include "Factories/DataAssetFactory.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Reflection/UEPIPropertyCodec.h"
#include "UEPISettings.h"

namespace UE::ProjectIntelligence
{
	namespace ContentOperationsPrivate
	{
		FString JsonString(const FJsonObject& Object, const TCHAR* Field, const FString& DefaultValue = FString())
		{
			FString Value; return Object.TryGetStringField(Field, Value) ? Value : DefaultValue;
		}

		bool JsonBool(const FJsonObject& Object, const TCHAR* Field, bool bDefault = false)
		{
			bool Value = false; return Object.TryGetBoolField(Field, Value) ? Value : bDefault;
		}

		FUEPIEditResult Failure(const FString& Code, const FString& Message)
		{
			FUEPIEditResult Result; Result.ErrorCode = Code; Result.Message = Message; return Result;
		}

		FUEPIEditResult Success(const FString& Message, const TSharedPtr<FJsonObject>& Detail = nullptr)
		{
			FUEPIEditResult Result; Result.bOk = true; Result.Message = Message; Result.Result = Detail; return Result;
		}

		TSharedRef<FJsonObject> AssetJson(UObject* Object)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (Object) { Result->SetStringField(TEXT("name"), Object->GetName()); Result->SetStringField(TEXT("path"), Object->GetPathName()); Result->SetStringField(TEXT("class"), Object->GetClass()->GetPathName()); Result->SetStringField(TEXT("package"), Object->GetOutermost()->GetName()); }
			return Result;
		}

		FString NormalizeContentPath(FString Path)
		{
			Path = Path.TrimStartAndEnd(); Path.ReplaceInline(TEXT("\\"), TEXT("/")); while (Path.EndsWith(TEXT("/")) && Path.Len() > 5) Path.LeftChopInline(1); return Path;
		}

		bool ValidateContentPath(const FString& Path, FString& OutError)
		{
			if (Path.IsEmpty() || (!Path.Equals(TEXT("/Game"), ESearchCase::IgnoreCase) && !Path.StartsWith(TEXT("/Game/"))) || Path.Contains(TEXT("..")) || Path.Contains(TEXT("\\")))
			{
				OutError = FString::Printf(TEXT("Content path must be under /Game without traversal: %s"), *Path); return false;
			}
			return true;
		}

		bool SplitDestination(const FJsonObject& Params, const FString& DefaultName, FString& OutPath, FString& OutName, FString& OutError)
		{
			FString Destination = NormalizeContentPath(JsonString(Params, TEXT("destination"), JsonString(Params, TEXT("destination_asset"))));
			if (!Destination.IsEmpty()) { FString Package = Destination; int32 Dot = INDEX_NONE; if (Package.FindChar(TEXT('.'), Dot)) Package = Package.Left(Dot); OutPath = FPackageName::GetLongPackagePath(Package); OutName = FPackageName::GetLongPackageAssetName(Package); }
			OutPath = NormalizeContentPath(JsonString(Params, TEXT("destination_path"), JsonString(Params, TEXT("folder"), OutPath))); OutName = JsonString(Params, TEXT("name"), JsonString(Params, TEXT("asset_name"), OutName.IsEmpty() ? DefaultName : OutName)).TrimStartAndEnd();
			if (!ValidateContentPath(OutPath, OutError) || OutName.IsEmpty() || OutName.Contains(TEXT("/")) || OutName.Contains(TEXT(".")) || OutName.Contains(TEXT("\\"))) { if (OutError.IsEmpty()) OutError = TEXT("Asset name contains invalid path characters."); return false; }
			return true;
		}

		FString ReferenceId(const FJsonObject& Params, const TCHAR* Field)
		{
			const TSharedPtr<FJsonObject>* Reference = nullptr; if (!Params.TryGetObjectField(Field, Reference) || !Reference || !Reference->IsValid()) return FString();
			FString Ref = JsonString(**Reference, TEXT("$ref")); int32 Hash = INDEX_NONE; if (Ref.FindChar(TEXT('#'), Hash)) Ref = Ref.Left(Hash); return Ref;
		}

		FString ResolveAsset(const FUEPIEditContext& Context, const FJsonObject& Params, const TCHAR* Field, const TCHAR* Alternate = nullptr)
		{
			FString Direct = JsonString(Params, Field, Alternate ? JsonString(Params, Alternate) : FString()); if (!Direct.IsEmpty()) return Direct;
			const FString Ref = ReferenceId(Params, Field); if (const FString* Resolved = Context.ResolvedAssets.Find(Ref)) return *Resolved; return FString();
		}

		TSharedPtr<FJsonObject> PropertyWrites(const FJsonObject& Params, FString& OutError)
		{
			const TSharedPtr<FJsonObject>* Direct = nullptr; if (Params.TryGetObjectField(TEXT("properties"), Direct) && Direct) return *Direct;
			const TArray<TSharedPtr<FJsonValue>>* Writes = nullptr; if (!Params.TryGetArrayField(TEXT("writes"), Writes) || !Writes) { OutError = TEXT("asset.set_properties requires properties or writes."); return nullptr; }
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			for (const TSharedPtr<FJsonValue>& Item : *Writes)
			{
				const TSharedPtr<FJsonObject> Write = Item.IsValid() ? Item->AsObject() : nullptr; const FString Path = Write.IsValid() ? JsonString(*Write, TEXT("path")) : FString(); const TSharedPtr<FJsonValue> Value = Write.IsValid() ? Write->TryGetField(TEXT("value")) : nullptr;
				if (Path.IsEmpty() || !Value.IsValid()) { OutError = TEXT("Each property write requires path, mode, and value."); return nullptr; }
				TSharedRef<FJsonObject> Encoded = MakeShared<FJsonObject>(); Encoded->SetStringField(TEXT("mode"), JsonString(*Write, TEXT("mode"), TEXT("replace"))); Encoded->SetField(TEXT("value"), Value); Properties->SetObjectField(Path, Encoded);
			}
			return Properties;
		}

		void DecodeWrite(const TSharedPtr<FJsonValue>& Encoded, FString& OutMode, TSharedPtr<FJsonValue>& OutValue)
		{
			const TSharedPtr<FJsonObject> Object = Encoded.IsValid() && Encoded->Type == EJson::Object ? Encoded->AsObject() : nullptr; OutMode = Object.IsValid() ? JsonString(*Object, TEXT("mode"), TEXT("replace")) : TEXT("replace"); const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(TEXT("value")) : nullptr; OutValue = Value.IsValid() ? Value : Encoded;
		}

		TArray<FString> StringArray(const FJsonObject& Params, const TCHAR* Field)
		{
			TArray<FString> Result; const TArray<TSharedPtr<FJsonValue>>* Values = nullptr; if (Params.TryGetArrayField(Field, Values) && Values) for (const TSharedPtr<FJsonValue>& Value : *Values) { FString Text; if (Value.IsValid() && Value->TryGetString(Text) && !Text.IsEmpty()) Result.AddUnique(Text); } return Result;
		}

		class FUEPIContentOperation final : public IUEPIEditOperation
		{
		public:
			explicit FUEPIContentOperation(FUEPIEditOperationDescriptor InDescriptor) : Descriptor(MoveTemp(InDescriptor)) {}
			virtual FString GetOperationType() const override { return Descriptor.Name; }
			virtual FUEPIEditOperationDescriptor GetDescriptor() const override { return Descriptor; }
			virtual FUEPIEditResult Validate(const FUEPIEditContext& Context, const FJsonObject& Params) override { return Preview(Context, Params); }

			virtual FUEPIEditResult Preview(const FUEPIEditContext& Context, const FJsonObject& Params) override
			{
				const UUEPISettings* Settings = GetDefault<UUEPISettings>(); if (!Settings || !Settings->bAllowContentEdits) return Failure(TEXT("UEPI_EDIT_CAPABILITY_DISABLED"), TEXT("Content edits are disabled in UEPI Project Settings."));
				if (Descriptor.Name == TEXT("content.save_assets")) { for (const FString& Path : StringArray(Params, TEXT("assets"))) if (!LoadObject<UObject>(nullptr, *Path) && !Context.ResolvedAssets.FindKey(Path)) return Failure(TEXT("UEPI_EDIT_ASSET_NOT_FOUND"), FString::Printf(TEXT("Save target was not found: %s"), *Path)); return Success(TEXT("Explicit save target preflight passed.")); }
				if (Descriptor.Name == TEXT("content.create_folder")) { FString Error; const FString Folder = NormalizeContentPath(JsonString(Params, TEXT("path"), JsonString(Params, TEXT("folder")))); return ValidateContentPath(Folder, Error) ? Success(TEXT("Content folder preflight passed.")) : Failure(TEXT("UEPI_EDIT_PATH_NOT_ALLOWED"), Error); }
				if (Descriptor.Name == TEXT("content.import"))
				{
					const FString File = JsonString(Params, TEXT("file"), JsonString(Params, TEXT("filename"))); const FString Extension = FPaths::GetExtension(File).ToLower(); FString Error; const FString Destination = NormalizeContentPath(JsonString(Params, TEXT("destination_path"), JsonString(Params, TEXT("folder"))));
					if (File.IsEmpty() || !FPaths::FileExists(File)) return Failure(TEXT("UEPI_EDIT_IMPORT_FILE_MISSING"), FString::Printf(TEXT("Import file does not exist: %s"), *File));
					if (!(Extension == TEXT("fbx") || Extension == TEXT("obj") || Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg") || Extension == TEXT("wav") || Extension == TEXT("uasset"))) return Failure(TEXT("UEPI_EDIT_IMPORT_TYPE_BLOCKED"), FString::Printf(TEXT("Import extension is not allowlisted: %s"), *Extension));
					return ValidateContentPath(Destination, Error) ? Success(TEXT("Content import preflight passed.")) : Failure(TEXT("UEPI_EDIT_PATH_NOT_ALLOWED"), Error);
				}
				if (Descriptor.Name == TEXT("content.create_asset"))
				{
					const FString ClassPath = JsonString(Params, TEXT("asset_class"), JsonString(Params, TEXT("class_path"))); UClass* Class = LoadObject<UClass>(nullptr, *ClassPath); if (!Class || !Class->IsChildOf(UDataAsset::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract)) return Failure(TEXT("UEPI_EDIT_ASSET_CLASS_UNSUPPORTED"), FString::Printf(TEXT("DataAsset class is invalid: %s"), *ClassPath));
					FString Path, Name, Error; if (!SplitDestination(Params, TEXT("DA_UEPIAsset"), Path, Name, Error)) return Failure(TEXT("UEPI_EDIT_DESTINATION_INVALID"), Error); const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *Path, *Name, *Name); if (LoadObject<UObject>(nullptr, *ObjectPath)) return Failure(TEXT("UEPI_EDIT_TARGET_ALREADY_EXISTS"), FString::Printf(TEXT("Asset already exists: %s"), *ObjectPath)); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("asset_path"), ObjectPath); Detail->SetStringField(TEXT("asset_class"), Class->GetPathName()); return Success(TEXT("DataAsset creation preflight passed."), Detail);
				}
				if (Descriptor.Name == TEXT("asset.set_properties"))
				{
					const FString Path = ResolveAsset(Context, Params, TEXT("asset")); UObject* Source = LoadObject<UObject>(nullptr, *Path); UObject* Probe = Source ? DuplicateObject(Source, GetTransientPackage()) : nullptr;
					if (!Probe) { const FString Ref = ReferenceId(Params, TEXT("asset")); if (UClass* const* Class = Context.ResolvedAssetClasses.Find(Ref)) Probe = NewObject<UObject>(GetTransientPackage(), *Class); }
					FString Error; const TSharedPtr<FJsonObject> Properties = PropertyWrites(Params, Error); if (!Probe || !Properties.IsValid()) return Failure(TEXT("UEPI_EDIT_PROPERTY_PREFLIGHT_FAILED"), Error.IsEmpty() ? TEXT("Property target or writes are invalid.") : Error);
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values) { FString Mode; TSharedPtr<FJsonValue> Value; DecodeWrite(Pair.Value, Mode, Value); TSharedPtr<FJsonValue> Before, After; if (!FUEPIPropertyCodec::SetPropertyPath(Probe, Pair.Key, Value, Before, After, Error, Mode)) return Failure(TEXT("UEPI_EDIT_PROPERTY_TYPE_MISMATCH"), Error); }
					return Success(TEXT("Typed property preflight passed."));
				}
				const FString SourcePath = ResolveAsset(Context, Params, TEXT("source"), TEXT("asset")); UObject* Source = LoadObject<UObject>(nullptr, *SourcePath); if (!Source) return Failure(TEXT("UEPI_EDIT_ASSET_NOT_FOUND"), FString::Printf(TEXT("Source asset was not found: %s"), *SourcePath));
				FString Path, Name, Error; if (!SplitDestination(Params, Descriptor.Name == TEXT("content.duplicate_asset") ? Source->GetName() + TEXT("_Copy") : Source->GetName(), Path, Name, Error)) return Failure(TEXT("UEPI_EDIT_DESTINATION_INVALID"), Error); const FString Destination = FString::Printf(TEXT("%s/%s.%s"), *Path, *Name, *Name); if (LoadObject<UObject>(nullptr, *Destination)) return Failure(TEXT("UEPI_EDIT_TARGET_ALREADY_EXISTS"), FString::Printf(TEXT("Destination asset exists: %s"), *Destination)); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("asset_path"), Destination); return Success(TEXT("Content asset move/copy preflight passed."), Detail);
			}

			virtual FUEPIEditResult Apply(const FUEPIEditContext& Context, const FJsonObject& Params) override
			{
				FUEPIEditResult Check = Preview(Context, Params); if (!Check.bOk) return Check;
				if (Descriptor.Name == TEXT("content.save_assets")) { TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); TArray<TSharedPtr<FJsonValue>> Values; for (const FString& Path : StringArray(Params, TEXT("assets"))) Values.Add(MakeShared<FJsonValueString>(Path)); Detail->SetArrayField(TEXT("assets"), Values); return Success(TEXT("Touched assets are scheduled for save after validation."), Detail); }
				if (Descriptor.Name == TEXT("content.create_folder")) { const FString Folder = NormalizeContentPath(JsonString(Params, TEXT("path"), JsonString(Params, TEXT("folder")))); const FString Directory = FPackageName::LongPackageNameToFilename(Folder); if (!IFileManager::Get().MakeDirectory(*Directory, true)) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("Failed to create content folder.")); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("folder"), Folder); Detail->SetStringField(TEXT("directory"), Directory); return Success(TEXT("Content folder created."), Detail); }
				if (Descriptor.Name == TEXT("content.import"))
				{
					UAssetImportTask* Task = NewObject<UAssetImportTask>(); Task->Filename = JsonString(Params, TEXT("file"), JsonString(Params, TEXT("filename"))); Task->DestinationPath = NormalizeContentPath(JsonString(Params, TEXT("destination_path"), JsonString(Params, TEXT("folder")))); Task->DestinationName = JsonString(Params, TEXT("name"), JsonString(Params, TEXT("asset_name"))); Task->bAutomated = true; Task->bAsync = false; Task->bSave = false; Task->bReplaceExisting = JsonBool(Params, TEXT("replace_existing"), false); TArray<UAssetImportTask*> Tasks = { Task }; FAssetToolsModule::GetModule().Get().ImportAssetTasks(Tasks);
					TArray<TSharedPtr<FJsonValue>> Assets; for (UObject* Object : Task->GetObjects()) if (Object) Assets.Add(MakeShared<FJsonValueObject>(AssetJson(Object))); if (Assets.Num() == 0) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("Import produced no assets.")); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("file"), Task->Filename); Detail->SetStringField(TEXT("destination_path"), Task->DestinationPath); Detail->SetArrayField(TEXT("assets"), Assets); return Success(TEXT("Asset imported."), Detail);
				}
				if (Descriptor.Name == TEXT("content.create_asset"))
				{
					UClass* Class = LoadObject<UClass>(nullptr, *JsonString(Params, TEXT("asset_class"), JsonString(Params, TEXT("class_path")))); FString Path, Name, Error; SplitDestination(Params, TEXT("DA_UEPIAsset"), Path, Name, Error); UDataAssetFactory* Factory = NewObject<UDataAssetFactory>(); Factory->DataAssetClass = Class; UObject* Asset = FAssetToolsModule::GetModule().Get().CreateAsset(Name, Path, Class, Factory, TEXT("UEPI")); if (!Asset) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("DataAsset creation failed.")); Asset->MarkPackageDirty(); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetObjectField(TEXT("asset"), AssetJson(Asset)); Detail->SetStringField(TEXT("asset_path"), Asset->GetPathName()); return Success(TEXT("DataAsset created."), Detail);
				}
				if (Descriptor.Name == TEXT("asset.set_properties"))
				{
					UObject* Object = LoadObject<UObject>(nullptr, *ResolveAsset(Context, Params, TEXT("asset"))); FString Error; const TSharedPtr<FJsonObject> Properties = PropertyWrites(Params, Error); Object->Modify(); TArray<TSharedPtr<FJsonValue>> Diffs;
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values) { FString Mode; TSharedPtr<FJsonValue> Value, Before, After; DecodeWrite(Pair.Value, Mode, Value); if (!FUEPIPropertyCodec::SetPropertyPath(Object, Pair.Key, Value, Before, After, Error, Mode)) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), Error); TSharedRef<FJsonObject> Diff = MakeShared<FJsonObject>(); Diff->SetStringField(TEXT("property_path"), Pair.Key); Diff->SetField(TEXT("before"), Before); Diff->SetField(TEXT("after"), After); Diffs.Add(MakeShared<FJsonValueObject>(Diff)); }
					Object->PostEditChange(); Object->MarkPackageDirty(); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("asset"), Object->GetPathName()); Detail->SetArrayField(TEXT("property_diff"), Diffs); return Success(TEXT("Typed properties updated."), Detail);
				}
				const FString SourcePath = ResolveAsset(Context, Params, TEXT("source"), TEXT("asset")); UObject* Source = LoadObject<UObject>(nullptr, *SourcePath); FString Path, Name, Error; SplitDestination(Params, Descriptor.Name == TEXT("content.duplicate_asset") ? Source->GetName() + TEXT("_Copy") : Source->GetName(), Path, Name, Error);
				if (Descriptor.Name == TEXT("content.duplicate_asset")) { UObject* Asset = FAssetToolsModule::GetModule().Get().DuplicateAsset(Name, Path, Source); if (!Asset) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("Asset duplication failed.")); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetObjectField(TEXT("source"), AssetJson(Source)); Detail->SetObjectField(TEXT("asset"), AssetJson(Asset)); return Success(TEXT("Asset duplicated."), Detail); }
				TArray<FAssetRenameData> RenameData = { FAssetRenameData(Source, Path, Name) }; if (!FAssetToolsModule::GetModule().Get().RenameAssets(RenameData)) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("Asset rename failed.")); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("old_asset"), SourcePath); Detail->SetStringField(TEXT("new_asset"), FString::Printf(TEXT("%s/%s.%s"), *Path, *Name, *Name)); return Success(TEXT("Asset renamed."), Detail);
			}

		private:
			FUEPIEditOperationDescriptor Descriptor;
		};
	}

	TSharedRef<IUEPIEditOperation> MakeUEPIContentOperation(const FUEPIEditOperationDescriptor& Descriptor)
	{
		return MakeShared<ContentOperationsPrivate::FUEPIContentOperation>(Descriptor);
	}
}
