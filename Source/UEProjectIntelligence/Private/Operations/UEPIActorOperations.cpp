#include "Operations/UEPIActorOperations.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Reflection/UEPIPropertyCodec.h"
#include "UEPISettings.h"

namespace UE::ProjectIntelligence
{
	namespace
	{
		FString JsonString(const FJsonObject& Object, const TCHAR* Field, const FString& DefaultValue = FString())
		{
			FString Value;
			return Object.TryGetStringField(Field, Value) ? Value : DefaultValue;
		}

		TSharedPtr<FJsonObject> JsonObject(const FJsonObject& Object, const TCHAR* Field)
		{
			const TSharedPtr<FJsonObject>* Value = nullptr;
			return Object.TryGetObjectField(Field, Value) && Value ? *Value : nullptr;
		}

		bool JsonVector(const TSharedPtr<FJsonObject>& Object, FVector& OutValue)
		{
			if (!Object.IsValid()) return false;
			double X = 0.0, Y = 0.0, Z = 0.0;
			if (!Object->TryGetNumberField(TEXT("x"), X) || !Object->TryGetNumberField(TEXT("y"), Y) || !Object->TryGetNumberField(TEXT("z"), Z)) return false;
			OutValue = FVector(X, Y, Z);
			return true;
		}

		bool JsonRotator(const TSharedPtr<FJsonObject>& Object, FRotator& OutValue)
		{
			if (!Object.IsValid()) return false;
			double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
			if (!Object->TryGetNumberField(TEXT("pitch"), Pitch) || !Object->TryGetNumberField(TEXT("yaw"), Yaw) || !Object->TryGetNumberField(TEXT("roll"), Roll)) return false;
			OutValue = FRotator(Pitch, Yaw, Roll);
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values) Result.Add(MakeShared<FJsonValueString>(Value));
			return Result;
		}

		UWorld* EditorWorld()
		{
			return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		}

		TArray<AActor*> ResolveActors(const FJsonObject& Params)
		{
			TArray<FString> Paths;
			if (const TSharedPtr<FJsonObject> Targets = JsonObject(Params, TEXT("targets")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (Targets->TryGetArrayField(TEXT("paths"), Values) && Values)
				{
					for (const TSharedPtr<FJsonValue>& Value : *Values)
					{
						FString Path;
						if (Value.IsValid() && Value->TryGetString(Path) && !Path.IsEmpty()) Paths.AddUnique(Path);
					}
				}
			}
			const FString Direct = JsonString(Params, TEXT("actor"), JsonString(Params, TEXT("path")));
			if (!Direct.IsEmpty()) Paths.AddUnique(Direct);

			TArray<AActor*> Result;
			UWorld* World = EditorWorld();
			if (!World) return Result;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (Paths.Contains(It->GetPathName()) || Paths.Contains(It->GetName()) || Paths.Contains(It->GetActorLabel())) Result.AddUnique(*It);
			}
			return Result;
		}

		FString NormalizeBlueprintPath(const FString& RawPath)
		{
			if (RawPath.Contains(TEXT("."))) return RawPath;
			const FString Name = FPackageName::GetLongPackageAssetName(RawPath);
			return Name.IsEmpty() ? RawPath : RawPath + TEXT(".") + Name;
		}

		UClass* ResolveActorClass(const FJsonObject& Params, FString& OutError)
		{
			const FString ClassPath = JsonString(Params, TEXT("actor_class"), JsonString(Params, TEXT("class"), JsonString(Params, TEXT("asset"))));
			if (ClassPath.IsEmpty())
			{
				OutError = TEXT("actor.spawn requires actor_class, class, or asset.");
				return nullptr;
			}
			if (UClass* Class = LoadObject<UClass>(nullptr, *ClassPath))
			{
				if (Class->IsChildOf(AActor::StaticClass())) return Class;
			}
			if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *NormalizeBlueprintPath(ClassPath)))
			{
				UClass* Class = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->SkeletonGeneratedClass;
				if (Class && Class->IsChildOf(AActor::StaticClass())) return Class;
			}
			OutError = FString::Printf(TEXT("Failed to resolve actor class for spawn: %s"), *ClassPath);
			return nullptr;
		}

		TSharedPtr<FJsonObject> PropertyWrites(const FJsonObject& Params, FString& OutError)
		{
			if (const TSharedPtr<FJsonObject> Properties = JsonObject(Params, TEXT("properties"))) return Properties;
			const TArray<TSharedPtr<FJsonValue>>* Writes = nullptr;
			if (!Params.TryGetArrayField(TEXT("writes"), Writes) || !Writes)
			{
				OutError = TEXT("actor.set_properties requires properties or writes.");
				return nullptr;
			}
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			for (const TSharedPtr<FJsonValue>& Value : *Writes)
			{
				const TSharedPtr<FJsonObject> Write = Value.IsValid() ? Value->AsObject() : nullptr;
				const FString Path = Write.IsValid() ? JsonString(*Write, TEXT("path")) : FString();
				const TSharedPtr<FJsonValue> TypedValue = Write.IsValid() ? Write->TryGetField(TEXT("value")) : nullptr;
				if (Path.IsEmpty() || !TypedValue.IsValid())
				{
					OutError = TEXT("Each actor property write requires path, mode, and value.");
					return nullptr;
				}
				TSharedRef<FJsonObject> Encoded = MakeShared<FJsonObject>();
				Encoded->SetStringField(TEXT("mode"), JsonString(*Write, TEXT("mode"), TEXT("replace")));
				Encoded->SetField(TEXT("value"), TypedValue);
				Properties->SetObjectField(Path, Encoded);
			}
			return Properties;
		}

		void DecodeWrite(const TSharedPtr<FJsonValue>& Encoded, FString& OutMode, TSharedPtr<FJsonValue>& OutValue)
		{
			const TSharedPtr<FJsonObject> Object = Encoded.IsValid() && Encoded->Type == EJson::Object ? Encoded->AsObject() : nullptr;
			OutMode = Object.IsValid() ? JsonString(*Object, TEXT("mode"), TEXT("replace")) : TEXT("replace");
			const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(TEXT("value")) : nullptr;
			OutValue = Value.IsValid() ? Value : Encoded;
		}

		TSharedRef<FJsonObject> ActorJson(AActor* Actor)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Actor) return Result;
			Result->SetStringField(TEXT("name"), Actor->GetName());
			Result->SetStringField(TEXT("path"), Actor->GetPathName());
			Result->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
			Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Result->SetStringField(TEXT("level"), Actor->GetLevel() ? Actor->GetLevel()->GetPathName() : FString());
			return Result;
		}

		FUEPIEditResult Failure(const FString& Code, const FString& Message)
		{
			FUEPIEditResult Result;
			Result.ErrorCode = Code;
			Result.Message = Message;
			return Result;
		}

		FUEPIEditResult Success(const FString& Message, const TSharedPtr<FJsonObject>& Detail = nullptr)
		{
			FUEPIEditResult Result;
			Result.bOk = true;
			Result.Message = Message;
			Result.Result = Detail;
			return Result;
		}

		class FUEPIActorOperation final : public IUEPIEditOperation
		{
		public:
			explicit FUEPIActorOperation(FUEPIEditOperationDescriptor InDescriptor) : Descriptor(MoveTemp(InDescriptor)) {}

			virtual FString GetOperationType() const override { return Descriptor.Name; }
			virtual FUEPIEditOperationDescriptor GetDescriptor() const override { return Descriptor; }

			virtual FUEPIEditResult Validate(const FUEPIEditContext& Context, const FJsonObject& Params) override
			{
				return Preview(Context, Params);
			}

			virtual FUEPIEditResult Preview(const FUEPIEditContext&, const FJsonObject& Params) override
			{
				const UUEPISettings* Settings = GetDefault<UUEPISettings>();
				if (!Settings || !Settings->bAllowActorEdits) return Failure(TEXT("UEPI_EDIT_CAPABILITY_DISABLED"), TEXT("Actor edits are disabled in UEPI Project Settings."));
				if (!EditorWorld()) return Failure(TEXT("UEPI_EDIT_EDITOR_WORLD_UNAVAILABLE"), TEXT("No editor world is available."));

				if (Descriptor.Name == TEXT("actor.spawn"))
				{
					FString Error;
					return ResolveActorClass(Params, Error) ? Success(TEXT("Actor spawn preflight passed.")) : Failure(TEXT("UEPI_EDIT_ACTOR_CLASS_INVALID"), Error);
				}
				TArray<AActor*> Actors = ResolveActors(Params);
				if (Actors.Num() == 0) return Failure(TEXT("UEPI_EDIT_ACTOR_NOT_FOUND"), TEXT("Actor operation did not resolve any target actors in the editor world."));
				if (Descriptor.Name == TEXT("actor.set_transform")) return Success(TEXT("Actor transform preflight passed."));

				FString Error;
				TSharedPtr<FJsonObject> Properties;
				if (Descriptor.Name == TEXT("actor.set_property"))
				{
					const FString Path = JsonString(Params, TEXT("property"));
					const TSharedPtr<FJsonValue> Value = Params.TryGetField(TEXT("value"));
					if (Path.IsEmpty() || !Value.IsValid()) return Failure(TEXT("UEPI_EDIT_PROPERTY_SCHEMA_INVALID"), TEXT("actor.set_property requires property and value."));
					Properties = MakeShared<FJsonObject>();
					Properties->SetField(Path, Value);
				}
				else
				{
					Properties = PropertyWrites(Params, Error);
				}
				if (!Properties.IsValid()) return Failure(TEXT("UEPI_EDIT_PROPERTY_SCHEMA_INVALID"), Error);
				for (AActor* Actor : Actors)
				{
					AActor* Probe = DuplicateObject<AActor>(Actor, GetTransientPackage());
					if (!Probe) return Failure(TEXT("UEPI_EDIT_PREFLIGHT_FAILED"), TEXT("Failed to create an actor preflight probe."));
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
					{
						FString Mode; TSharedPtr<FJsonValue> Value; DecodeWrite(Pair.Value, Mode, Value);
						TSharedPtr<FJsonValue> Before; TSharedPtr<FJsonValue> After;
						if (!FUEPIPropertyCodec::SetPropertyPath(Probe, Pair.Key, Value, Before, After, Error, Mode)) return Failure(TEXT("UEPI_EDIT_PROPERTY_TYPE_MISMATCH"), Error);
					}
				}
				return Success(TEXT("Actor property preflight passed."));
			}

			virtual FUEPIEditResult Apply(const FUEPIEditContext& Context, const FJsonObject& Params) override
			{
				FUEPIEditResult Preflight = Preview(Context, Params);
				if (!Preflight.bOk) return Preflight;
				if (Descriptor.Name == TEXT("actor.spawn"))
				{
					FString Error;
					UClass* Class = ResolveActorClass(Params, Error);
					FVector Location = FVector::ZeroVector, Scale = FVector::OneVector; FRotator Rotation = FRotator::ZeroRotator;
					const TSharedPtr<FJsonObject> Transform = JsonObject(Params, TEXT("transform"));
					const FJsonObject& Source = Transform.IsValid() ? *Transform : Params;
					JsonVector(JsonObject(Source, TEXT("location")), Location); JsonRotator(JsonObject(Source, TEXT("rotation")), Rotation); JsonVector(JsonObject(Source, TEXT("scale")), Scale);
					FActorSpawnParameters SpawnParams;
					SpawnParams.Name = FName(*JsonString(Params, TEXT("name")));
					SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					AActor* Actor = EditorWorld()->SpawnActor<AActor>(Class, FTransform(Rotation, Location, Scale), SpawnParams);
					if (!Actor) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("Failed to spawn actor."));
					const FString Label = JsonString(Params, TEXT("label"));
					if (!Label.IsEmpty()) Actor->SetActorLabel(Label);
					Actor->Modify();
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetObjectField(TEXT("actor"), ActorJson(Actor)); Detail->SetStringField(TEXT("actor_class"), Class->GetPathName());
					return Success(TEXT("Actor spawned in the editor world."), Detail);
				}

				TArray<AActor*> Actors = ResolveActors(Params);
				if (Descriptor.Name == TEXT("actor.set_transform"))
				{
					TSharedPtr<FJsonObject> Set = JsonObject(Params, TEXT("set"));
					if (!Set.IsValid()) Set = JsonObject(Params, TEXT("operation"));
					if (Set.IsValid() && JsonObject(*Set, TEXT("set")).IsValid()) Set = JsonObject(*Set, TEXT("set"));
					const FJsonObject& Source = Set.IsValid() ? *Set : Params;
					TArray<FString> Paths;
					for (AActor* Actor : Actors)
					{
						Actor->Modify(); FTransform Value = Actor->GetActorTransform(); FVector Vector; FRotator Rotator;
						if (JsonVector(JsonObject(Source, TEXT("location")), Vector)) Value.SetLocation(Vector);
						if (JsonRotator(JsonObject(Source, TEXT("rotation")), Rotator)) Value.SetRotation(Rotator.Quaternion());
						if (JsonVector(JsonObject(Source, TEXT("scale")), Vector)) Value.SetScale3D(Vector);
						Actor->SetActorTransform(Value, false, nullptr, ETeleportType::TeleportPhysics); Paths.Add(Actor->GetPathName());
					}
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetArrayField(TEXT("actors"), Strings(Paths));
					return Success(TEXT("Actor transform updated."), Detail);
				}

				FString Error; TSharedPtr<FJsonObject> Properties;
				if (Descriptor.Name == TEXT("actor.set_property"))
				{
					Properties = MakeShared<FJsonObject>(); Properties->SetField(JsonString(Params, TEXT("property")), Params.TryGetField(TEXT("value")));
				}
				else Properties = PropertyWrites(Params, Error);
				TArray<TSharedPtr<FJsonValue>> ActorDiffs;
				for (AActor* Actor : Actors)
				{
					Actor->Modify(); TArray<TSharedPtr<FJsonValue>> PropertyDiffs;
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
					{
						FString Mode; TSharedPtr<FJsonValue> Value; DecodeWrite(Pair.Value, Mode, Value); TSharedPtr<FJsonValue> Before; TSharedPtr<FJsonValue> After;
						if (!FUEPIPropertyCodec::SetPropertyPath(Actor, Pair.Key, Value, Before, After, Error, Mode)) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), Error);
						TSharedRef<FJsonObject> Diff = MakeShared<FJsonObject>(); Diff->SetStringField(TEXT("property_path"), Pair.Key); Diff->SetField(TEXT("before"), Before); Diff->SetField(TEXT("after"), After); PropertyDiffs.Add(MakeShared<FJsonValueObject>(Diff));
					}
					Actor->PostEditChange(); TSharedRef<FJsonObject> ActorDiff = MakeShared<FJsonObject>(); ActorDiff->SetStringField(TEXT("actor"), Actor->GetPathName()); ActorDiff->SetArrayField(TEXT("property_diff"), PropertyDiffs); ActorDiffs.Add(MakeShared<FJsonValueObject>(ActorDiff));
				}
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetArrayField(TEXT("actors"), ActorDiffs);
				return Success(Descriptor.Name == TEXT("actor.set_property") ? TEXT("Actor property updated.") : TEXT("Actor typed properties updated."), Detail);
			}

		private:
			FUEPIEditOperationDescriptor Descriptor;
		};
	}

	TSharedRef<IUEPIEditOperation> MakeUEPIActorOperation(const FUEPIEditOperationDescriptor& Descriptor)
	{
		return MakeShared<FUEPIActorOperation>(Descriptor);
	}
}
