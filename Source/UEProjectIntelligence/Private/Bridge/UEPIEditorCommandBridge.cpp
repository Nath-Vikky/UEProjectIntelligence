#include "Bridge/UEPIEditorCommandBridge.h"

#include "Bridge/UEPIBridgeProtocol.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Components/ActorComponent.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ContentWidget.h"
#include "Components/PanelWidget.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextBlock.h"
#include "Common/TcpListener.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/DataAssetFactory.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "ImageUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_InputKey.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "Edit/UEPIEditOperationRegistry.h"
#include "Edit/UEPIBackupService.h"
#include "Edit/UEPIPackageSaveService.h"
#include "Edit/UEPITransactionJournal.h"
#include "Logging/TokenizedMessage.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_Slot.h"
#include "Factories/AnimMontageFactory.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/DataAsset.h"
#include "MaterialTypes.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "ScopedTransaction.h"
#include "PackageTools.h"
#include "PlayInEditorDataTypes.h"
#include "Reflection/UEPIPropertyCodec.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UEPISettings.h"
#include "UEPISnapshotStore.h"
#include "Validation/UEPIValidatorRegistry.h"
#include "UnrealClient.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#if UEPI_WITH_ENHANCED_INPUT
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputEditorModule.h"
#include "InputMappingContext.h"
#endif

namespace UE::ProjectIntelligence
{
	namespace
	{
		FString UEPICanonicalProjectFile()
		{
			FString ProjectFile = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
			FPaths::NormalizeFilename(ProjectFile);
			FPaths::CollapseRelativeDirectories(ProjectFile);
#if PLATFORM_WINDOWS
			ProjectFile = ProjectFile.ToLower();
#endif
			ProjectFile.RemoveFromEnd(TEXT("/"));
			return ProjectFile;
		}

		FString UEPIProjectBindingId()
		{
			const FString CanonicalProjectFile = UEPICanonicalProjectFile();
			FTCHARToUTF8 Utf8(*CanonicalProjectFile);
			FSHA256Signature Signature;
			if (!FPlatformMisc::GetSHA256Signature(Utf8.Get(), static_cast<uint32>(Utf8.Length()), Signature))
			{
				return FString();
			}
			return FString::Printf(TEXT("sha256:%s"), *Signature.ToString().ToLower());
		}

		FString UEPIFileSha256(const FString& Filename)
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
			{
				return FString();
			}
			FSHA256Signature Signature;
			if (!FPlatformMisc::GetSHA256Signature(Bytes.GetData(), static_cast<uint32>(Bytes.Num()), Signature))
			{
				return FString();
			}
			return FString::Printf(TEXT("sha256:%s"), *Signature.ToString().ToLower());
		}

		FString UEPISessionsDirectory()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("sessions")));
		}

		bool SaveJsonObject(const TSharedRef<FJsonObject>& Object, const FString& Path);

		FString UEPIBridgeSessionPath()
		{
			return FPaths::Combine(UEPISessionsDirectory(), TEXT("editor-bridge.json"));
		}

		FString UEPIBridgeTokenPath()
		{
			return FPaths::Combine(UEPISessionsDirectory(), TEXT("editor-bridge-token.txt"));
		}

		FString UEPIStoreRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence")));
		}

		FString UEPIGlobalSessionsDirectory()
		{
			FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
			if (LocalAppData.IsEmpty())
			{
				LocalAppData = FPaths::ProjectSavedDir();
			}
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(LocalAppData, TEXT("UEProjectIntelligence"), TEXT("sessions")));
		}

		FString UEPIGlobalBridgeSessionPath()
		{
			const FString ProjectHash = UEPIProjectBindingId().Replace(TEXT("sha256:"), TEXT("")).Left(12);
			const FString ProjectName(FApp::GetProjectName());
			const FString FileName = FPaths::MakeValidFileName(FString::Printf(TEXT("%s-%s.json"), *ProjectName, *ProjectHash));
			return FPaths::Combine(UEPIGlobalSessionsDirectory(), FileName);
		}

		FString UEPIRequestsDirectory()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("requests")));
		}

		FString JsonObjectToString(const TSharedRef<FJsonObject>& Object)
		{
			FString Output;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
			FJsonSerializer::Serialize(Object, Writer);
			return Output;
		}

		TArray<TSharedPtr<FJsonValue>> StringArrayToJsonValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			JsonValues.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Value));
			}
			return JsonValues;
		}

		TArray<TSharedPtr<FJsonValue>> EmptyJsonArray()
		{
			return TArray<TSharedPtr<FJsonValue>>();
		}

		TSharedRef<FJsonObject> SuccessResponse(const FString& RequestId, const TSharedRef<FJsonObject>& Result, TArray<TSharedPtr<FJsonValue>> Diagnostics = EmptyJsonArray())
		{
			TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("id"), RequestId);
			Response->SetBoolField(TEXT("ok"), true);
			Response->SetObjectField(TEXT("result"), Result);
			Response->SetArrayField(TEXT("diagnostics"), Diagnostics);
			return Response;
		}

		TSharedRef<FJsonObject> DiagnosticObject(const FString& Code, const FString& Severity, const FString& Message)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("code"), Code);
			Object->SetStringField(TEXT("severity"), Severity);
			Object->SetStringField(TEXT("message"), Message);
			return Object;
		}

		TArray<TSharedPtr<FJsonValue>> DiagnosticsArray(const FString& Code, const FString& Severity, const FString& Message)
		{
			return { MakeShared<FJsonValueObject>(DiagnosticObject(Code, Severity, Message)) };
		}

		FString JsonString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& DefaultValue = FString())
		{
			if (!Object.IsValid())
			{
				return DefaultValue;
			}
			FString Value;
			return Object->TryGetStringField(Field, Value) ? Value : DefaultValue;
		}

		bool JsonBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool bDefaultValue = false)
		{
			if (!Object.IsValid())
			{
				return bDefaultValue;
			}
			bool bValue = false;
			return Object->TryGetBoolField(Field, bValue) ? bValue : bDefaultValue;
		}

		int32 JsonInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 DefaultValue = 0)
		{
			if (!Object.IsValid())
			{
				return DefaultValue;
			}
			int32 Value = 0;
			return Object->TryGetNumberField(Field, Value) ? Value : DefaultValue;
		}

		const TArray<TSharedPtr<FJsonValue>>* JsonArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
		{
			if (!Object.IsValid())
			{
				return nullptr;
			}
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			return Object->TryGetArrayField(Field, Values) ? Values : nullptr;
		}

		TArray<FString> JsonStringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
		{
			TArray<FString> Result;
			if (const TArray<TSharedPtr<FJsonValue>>* Values = JsonArray(Object, Field))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					FString Text;
					if (Value.IsValid() && Value->TryGetString(Text) && !Text.IsEmpty())
					{
						Result.AddUnique(Text);
					}
				}
			}
			return Result;
		}

		TSharedPtr<FJsonObject> JsonObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
		{
			if (!Object.IsValid())
			{
				return nullptr;
			}
			const TSharedPtr<FJsonObject>* Value = nullptr;
			return Object->TryGetObjectField(Field, Value) && Value ? *Value : nullptr;
		}

		FString OperationId(const TSharedPtr<FJsonObject>& Operation)
		{
			return JsonString(Operation, TEXT("operation_id"), JsonString(Operation, TEXT("id"), JsonString(Operation, TEXT("ref"))));
		}

		FString ResolveOperationAsset(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const TMap<FString, FString>& OperationAssets)
		{
			const FString Direct = JsonString(Params, Field);
			if (!Direct.IsEmpty())
			{
				return Direct;
			}
			const TSharedPtr<FJsonObject> Reference = JsonObjectField(Params, Field);
			FString Ref = JsonString(Reference, TEXT("$ref"));
			int32 FragmentIndex = INDEX_NONE;
			if (Ref.FindChar(TEXT('#'), FragmentIndex))
			{
				Ref = Ref.Left(FragmentIndex);
			}
			if (const FString* Resolved = OperationAssets.Find(Ref))
			{
				return *Resolved;
			}
			return FString();
		}

		TSharedPtr<FJsonObject> PropertyWritesObject(const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			if (const TSharedPtr<FJsonObject> Properties = JsonObjectField(Params, TEXT("properties")))
			{
				return Properties;
			}
			const TArray<TSharedPtr<FJsonValue>>* Writes = JsonArray(Params, TEXT("writes"));
			if (!Writes)
			{
				OutError = TEXT("asset.set_properties requires properties or writes.");
				return nullptr;
			}
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			for (const TSharedPtr<FJsonValue>& Value : *Writes)
			{
				const TSharedPtr<FJsonObject> Write = Value.IsValid() ? Value->AsObject() : nullptr;
				const FString Path = JsonString(Write, TEXT("path"));
				const FString Mode = JsonString(Write, TEXT("mode"), TEXT("replace"));
				const TSharedPtr<FJsonValue> TypedValue = Write.IsValid() ? Write->TryGetField(TEXT("value")) : nullptr;
				if (!Write.IsValid() || Path.IsEmpty() || !TypedValue.IsValid())
				{
					OutError = TEXT("Each property write requires path, mode, and value.");
					return nullptr;
				}
				TSharedRef<FJsonObject> EncodedWrite = MakeShared<FJsonObject>();
				EncodedWrite->SetStringField(TEXT("__uepi_write_mode"), Mode);
				EncodedWrite->SetField(TEXT("__uepi_write_value"), TypedValue);
				Properties->SetObjectField(Path, EncodedWrite);
			}
			return Properties;
		}

		FString PropertyWriteMode(const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject> Object = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			return JsonString(Object, TEXT("__uepi_write_mode"), TEXT("replace"));
		}

		TSharedPtr<FJsonValue> PropertyWriteValue(const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject> Object = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			const TSharedPtr<FJsonValue> Encoded = Object.IsValid() ? Object->TryGetField(TEXT("__uepi_write_value")) : nullptr;
			return Encoded.IsValid() ? Encoded : Value;
		}

		FString BlueprintStatusString(const UBlueprint* Blueprint)
		{
			if (!Blueprint)
			{
				return TEXT("unknown");
			}
			if (const UEnum* Enum = StaticEnum<EBlueprintStatus>())
			{
				return Enum->GetNameStringByValue(static_cast<int64>(Blueprint->Status));
			}
			return FString::FromInt(static_cast<int32>(Blueprint->Status));
		}

		FString MessageSeverityString(EMessageSeverity::Type Severity)
		{
			switch (Severity)
			{
			case EMessageSeverity::Error:
				return TEXT("error");
			case EMessageSeverity::PerformanceWarning:
			case EMessageSeverity::Warning:
				return TEXT("warning");
			case EMessageSeverity::Info:
				return TEXT("info");
			default:
				return TEXT("message");
			}
		}

		TSharedRef<FJsonObject> CompileBlueprintToJson(UBlueprint* Blueprint)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Blueprint)
			{
				Result->SetBoolField(TEXT("ok"), false);
				Result->SetStringField(TEXT("error"), TEXT("Blueprint was null."));
				return Result;
			}

			FCompilerResultsLog Log;
			Log.SetSourcePath(Blueprint->GetPathName());
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Log);

			TArray<TSharedPtr<FJsonValue>> Messages;
			const int32 MaxMessages = FMath::Min(Log.Messages.Num(), 40);
			for (int32 Index = 0; Index < MaxMessages; ++Index)
			{
				const TSharedRef<FTokenizedMessage>& Message = Log.Messages[Index];
				TSharedRef<FJsonObject> MessageObject = MakeShared<FJsonObject>();
				MessageObject->SetStringField(TEXT("severity"), MessageSeverityString(Message->GetSeverity()));
				MessageObject->SetStringField(TEXT("message"), Message->ToText().ToString());
				Messages.Add(MakeShared<FJsonValueObject>(MessageObject));
			}

			Result->SetBoolField(TEXT("ok"), Log.NumErrors == 0);
			Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
			Result->SetStringField(TEXT("status"), BlueprintStatusString(Blueprint));
			Result->SetNumberField(TEXT("error_count"), Log.NumErrors);
			Result->SetNumberField(TEXT("warning_count"), Log.NumWarnings);
			Result->SetNumberField(TEXT("message_count"), Log.Messages.Num());
			Result->SetArrayField(TEXT("messages"), Messages);
			return Result;
		}

		bool BuildPinType(const FString& RequestedType, FEdGraphPinType& OutPinType, FString& OutError)
		{
			const FString Type = RequestedType.TrimStartAndEnd().ToLower();
			OutPinType = FEdGraphPinType();
			if (Type == TEXT("bool") || Type == TEXT("boolean"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			}
			else if (Type == TEXT("byte"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			}
			else if (Type == TEXT("int") || Type == TEXT("integer"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			}
			else if (Type == TEXT("int64") || Type == TEXT("long"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
			}
			else if (Type == TEXT("float") || Type == TEXT("real"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Float;
			}
			else if (Type == TEXT("double"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Double;
			}
			else if (Type == TEXT("string"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
			}
			else if (Type == TEXT("name"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
			}
			else if (Type == TEXT("text"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
			}
			else
			{
				OutError = FString::Printf(TEXT("Unsupported simple Blueprint variable type: %s"), *RequestedType);
				return false;
			}
			return true;
		}

		FString NormalizeBlueprintObjectPath(const FString& RawPath)
		{
			FString Path = RawPath.TrimStartAndEnd();
			if (Path.IsEmpty() || Path.Contains(TEXT(".")))
			{
				return Path;
			}
			FString Package;
			FString AssetName;
			if (Path.Split(TEXT("/"), &Package, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !AssetName.IsEmpty())
			{
				return Path + TEXT(".") + AssetName;
			}
			return Path;
		}

		UBlueprint* LoadBlueprintForEdit(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
		{
			OutAssetPath = NormalizeBlueprintObjectPath(JsonString(Params, TEXT("asset")));
			if (OutAssetPath.IsEmpty())
			{
				OutAssetPath = NormalizeBlueprintObjectPath(JsonString(Params, TEXT("blueprint")));
			}
			if (OutAssetPath.IsEmpty())
			{
				OutError = TEXT("Blueprint operation requires params.asset or params.blueprint.");
				return nullptr;
			}
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *OutAssetPath);
			if (!Blueprint)
			{
				OutError = FString::Printf(TEXT("Failed to load Blueprint asset: %s"), *OutAssetPath);
				return nullptr;
			}
			return Blueprint;
		}

		UClass* ResolveComponentClass(const FString& RequestedClass)
		{
			const FString ClassName = RequestedClass.TrimStartAndEnd();
			if (ClassName.IsEmpty() || ClassName == TEXT("SceneComponent") || ClassName == TEXT("USceneComponent"))
			{
				return USceneComponent::StaticClass();
			}
			if (ClassName == TEXT("StaticMeshComponent") || ClassName == TEXT("UStaticMeshComponent"))
			{
				return UStaticMeshComponent::StaticClass();
			}
			if (ClassName == TEXT("ActorComponent") || ClassName == TEXT("UActorComponent"))
			{
				return UActorComponent::StaticClass();
			}
			return StaticLoadClass(UActorComponent::StaticClass(), nullptr, *ClassName);
		}

		bool SetSimplePropertyValue(UObject* Object, const FString& PropertyName, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			if (!Object)
			{
				OutError = TEXT("Property target object is null.");
				return false;
			}
			FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
			if (!Property)
			{
				OutError = FString::Printf(TEXT("Property was not found on %s: %s"), *Object->GetClass()->GetName(), *PropertyName);
				return false;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
			{
				BoolProperty->SetPropertyValue(ValuePtr, JsonBool(Params, TEXT("value")));
				return true;
			}
			if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
			{
				IntProperty->SetPropertyValue(ValuePtr, JsonInt(Params, TEXT("value")));
				return true;
			}
			double NumberValue = 0.0;
			const bool bHasNumber = Params.IsValid() && Params->TryGetNumberField(TEXT("value"), NumberValue);
			if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
			{
				if (!bHasNumber)
				{
					OutError = TEXT("Float property requires a numeric value.");
					return false;
				}
				FloatProperty->SetPropertyValue(ValuePtr, static_cast<float>(NumberValue));
				return true;
			}
			if (FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
			{
				if (!bHasNumber)
				{
					OutError = TEXT("Double property requires a numeric value.");
					return false;
				}
				DoubleProperty->SetPropertyValue(ValuePtr, NumberValue);
				return true;
			}
			FString StringValue = JsonString(Params, TEXT("value"));
			if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
			{
				StringProperty->SetPropertyValue(ValuePtr, StringValue);
				return true;
			}
			if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
			{
				NameProperty->SetPropertyValue(ValuePtr, FName(*StringValue));
				return true;
			}
			OutError = FString::Printf(TEXT("Property type is not supported by write alpha: %s"), *Property->GetClass()->GetName());
			return false;
		}

		bool JsonVector(const TSharedPtr<FJsonObject>& Object, FVector& OutVector)
		{
			if (!Object.IsValid())
			{
				return false;
			}
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!Object->TryGetNumberField(TEXT("x"), X) || !Object->TryGetNumberField(TEXT("y"), Y) || !Object->TryGetNumberField(TEXT("z"), Z))
			{
				return false;
			}
			OutVector = FVector(X, Y, Z);
			return true;
		}

		bool JsonRotator(const TSharedPtr<FJsonObject>& Object, FRotator& OutRotator)
		{
			if (!Object.IsValid())
			{
				return false;
			}
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			Object->TryGetNumberField(TEXT("pitch"), Pitch);
			Object->TryGetNumberField(TEXT("yaw"), Yaw);
			Object->TryGetNumberField(TEXT("roll"), Roll);
			OutRotator = FRotator(Pitch, Yaw, Roll);
			return true;
		}

		bool JsonColor(const TSharedPtr<FJsonObject>& Object, FLinearColor& OutColor)
		{
			if (!Object.IsValid())
			{
				return false;
			}
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			double A = 1.0;
			if (!Object->TryGetNumberField(TEXT("r"), R) || !Object->TryGetNumberField(TEXT("g"), G) || !Object->TryGetNumberField(TEXT("b"), B))
			{
				return false;
			}
			Object->TryGetNumberField(TEXT("a"), A);
			OutColor = FLinearColor(R, G, B, A);
			return true;
		}

		FVector2D NodePositionFromParams(const TSharedPtr<FJsonObject>& Params, int32 Index)
		{
			double X = static_cast<double>(Index * 260);
			double Y = static_cast<double>(Index * 120);
			bool bHasX = false;
			bool bHasY = false;

			if (const TSharedPtr<FJsonObject> Position = JsonObjectField(Params, TEXT("position")))
			{
				bHasX = Position->TryGetNumberField(TEXT("x"), X);
				bHasY = Position->TryGetNumberField(TEXT("y"), Y);
			}
			if (!bHasX && Params.IsValid())
			{
				bHasX = Params->TryGetNumberField(TEXT("x"), X) || Params->TryGetNumberField(TEXT("node_pos_x"), X);
			}
			if (!bHasY && Params.IsValid())
			{
				bHasY = Params->TryGetNumberField(TEXT("y"), Y) || Params->TryGetNumberField(TEXT("node_pos_y"), Y);
			}
			return FVector2D(X, Y);
		}

		UEdGraph* FindBlueprintGraph(UBlueprint* Blueprint, const FString& RequestedGraph)
		{
			if (!Blueprint)
			{
				return nullptr;
			}

			const FString GraphName = RequestedGraph.TrimStartAndEnd();
			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			if (!GraphName.IsEmpty())
			{
				for (UEdGraph* Graph : Graphs)
				{
					if (Graph && (Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase) || Graph->GetFName() == FName(*GraphName)))
					{
						return Graph;
					}
				}
			}

			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				if (Graph && (GraphName.IsEmpty() || Graph->GetName().Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase)))
				{
					return Graph;
				}
			}
			return Graphs.Num() > 0 ? Graphs[0] : nullptr;
		}

		UEdGraph* ResolveGraphForEdit(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FString& OutGraphName, FString& OutError)
		{
			OutGraphName = JsonString(Params, TEXT("graph"), JsonString(Params, TEXT("graph_name")));
			UEdGraph* Graph = FindBlueprintGraph(Blueprint, OutGraphName);
			if (!Graph)
			{
				OutError = OutGraphName.IsEmpty()
					? TEXT("Blueprint graph operation could not find a default graph.")
					: FString::Printf(TEXT("Blueprint graph not found: %s"), *OutGraphName);
				return nullptr;
			}
			OutGraphName = Graph->GetName();
			return Graph;
		}

		FString GraphNodeGuidString(const UEdGraphNode* Node)
		{
			return Node ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString();
		}

		TSharedRef<FJsonObject> PinToJson(const UEdGraphPin* Pin)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			if (!Pin)
			{
				return Object;
			}
			Object->SetStringField(TEXT("pin_id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
			Object->SetStringField(TEXT("name"), Pin->PinName.ToString());
			Object->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			Object->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
			Object->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
			Object->SetStringField(TEXT("default_value"), Pin->DefaultValue);

			TArray<TSharedPtr<FJsonValue>> Links;
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin)
				{
					continue;
				}
				TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
				Link->SetStringField(TEXT("node_guid"), GraphNodeGuidString(LinkedPin->GetOwningNode()));
				Link->SetStringField(TEXT("pin_id"), LinkedPin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
				Link->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
				Links.Add(MakeShared<FJsonValueObject>(Link));
			}
			Object->SetArrayField(TEXT("linked_to"), Links);
			return Object;
		}

		TSharedRef<FJsonObject> NodeToJson(const UEdGraphNode* Node, const UEdGraph* Graph)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			if (!Node)
			{
				return Object;
			}
			Object->SetStringField(TEXT("node_guid"), GraphNodeGuidString(Node));
			Object->SetStringField(TEXT("node_id"), GraphNodeGuidString(Node));
			Object->SetStringField(TEXT("graph"), Graph ? Graph->GetName() : FString());
			Object->SetStringField(TEXT("class"), Node->GetClass() ? Node->GetClass()->GetName() : FString());
			Object->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			Object->SetNumberField(TEXT("x"), Node->NodePosX);
			Object->SetNumberField(TEXT("y"), Node->NodePosY);

			TArray<TSharedPtr<FJsonValue>> Pins;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
			}
			Object->SetArrayField(TEXT("pins"), Pins);
			return Object;
		}

		FString PinDefaultString(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid())
			{
				return FString();
			}
			FString StringValue;
			if (Value->TryGetString(StringValue))
			{
				return StringValue;
			}
			double NumberValue = 0.0;
			if (Value->TryGetNumber(NumberValue))
			{
				return FString::SanitizeFloat(NumberValue);
			}
			bool BoolValue = false;
			if (Value->TryGetBool(BoolValue))
			{
				return BoolValue ? TEXT("true") : TEXT("false");
			}
			return FString();
		}

		FString NormalizePinAlias(const FString& PinName)
		{
			const FString Lower = PinName.TrimStartAndEnd().ToLower();
			if (Lower == TEXT("exec") || Lower == TEXT("execute"))
			{
				return UEdGraphSchema_K2::PN_Execute.ToString();
			}
			if (Lower == TEXT("then") || Lower == TEXT("true"))
			{
				return UEdGraphSchema_K2::PN_Then.ToString();
			}
			if (Lower == TEXT("else") || Lower == TEXT("false"))
			{
				return UEdGraphSchema_K2::PN_Else.ToString();
			}
			if (Lower == TEXT("condition") || Lower == TEXT("cond"))
			{
				return UEdGraphSchema_K2::PN_Condition.ToString();
			}
			if (Lower == TEXT("return") || Lower == TEXT("return_value") || Lower == TEXT("returnvalue"))
			{
				return UEdGraphSchema_K2::PN_ReturnValue.ToString();
			}
			if (Lower == TEXT("self") || Lower == TEXT("target"))
			{
				return UEdGraphSchema_K2::PN_Self.ToString();
			}
			return PinName.TrimStartAndEnd();
		}

		bool PinDirectionMatches(const UEdGraphPin* Pin, const FString& Direction)
		{
			if (!Pin || Direction.IsEmpty())
			{
				return true;
			}
			const FString Lower = Direction.ToLower();
			if (Lower == TEXT("input") || Lower == TEXT("in"))
			{
				return Pin->Direction == EGPD_Input;
			}
			if (Lower == TEXT("output") || Lower == TEXT("out"))
			{
				return Pin->Direction == EGPD_Output;
			}
			return true;
		}

		UEdGraphPin* FindPinFlexible(UEdGraphNode* Node, const FString& PinText, const FString& Direction = FString())
		{
			if (!Node || PinText.TrimStartAndEnd().IsEmpty())
			{
				return nullptr;
			}
			FGuid PinGuid;
			if (FGuid::Parse(PinText, PinGuid))
			{
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->PinId == PinGuid && PinDirectionMatches(Pin, Direction))
					{
						return Pin;
					}
				}
			}

			const FString NormalizedName = NormalizePinAlias(PinText);
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || !PinDirectionMatches(Pin, Direction))
				{
					continue;
				}
				if (Pin->PinName.ToString().Equals(NormalizedName, ESearchCase::IgnoreCase) ||
					Pin->GetDisplayName().ToString().Equals(NormalizedName, ESearchCase::IgnoreCase))
				{
					return Pin;
				}
			}
			return nullptr;
		}

		void ApplyPinDefaults(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Params)
		{
			if (!Node || !Params.IsValid())
			{
				return;
			}
			const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
			if (const TSharedPtr<FJsonObject> Defaults = JsonObjectField(Params, TEXT("defaults")))
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Defaults->Values)
				{
					if (UEdGraphPin* Pin = FindPinFlexible(Node, Pair.Key))
					{
						K2Schema->TrySetDefaultValue(*Pin, PinDefaultString(Pair.Value), true);
					}
				}
			}
		}

		UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& GuidText)
		{
			if (!Graph || GuidText.TrimStartAndEnd().IsEmpty())
			{
				return nullptr;
			}
			FGuid Guid;
			const bool bParsed = FGuid::Parse(GuidText, Guid);
			const FString Digits = GuidText.Replace(TEXT("-"), TEXT(""));
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if ((bParsed && Node->NodeGuid == Guid) || Node->NodeGuid.ToString(EGuidFormats::Digits).Equals(Digits, ESearchCase::IgnoreCase))
				{
					return Node;
				}
			}
			return nullptr;
		}

		void AddNodeReference(TMap<FString, UEdGraphNode*>& NodeRefs, const FString& Reference, UEdGraphNode* Node)
		{
			const FString CleanReference = Reference.TrimStartAndEnd();
			if (!CleanReference.IsEmpty() && Node)
			{
				NodeRefs.Add(CleanReference, Node);
			}
		}

		void RegisterNodeReferences(TMap<FString, UEdGraphNode*>& NodeRefs, const TSharedPtr<FJsonObject>& Operation, const TSharedPtr<FJsonObject>& Params, UEdGraphNode* Node)
		{
			if (!Node)
			{
				return;
			}
			AddNodeReference(NodeRefs, JsonString(Operation, TEXT("ref")), Node);
			AddNodeReference(NodeRefs, JsonString(Operation, TEXT("node_ref")), Node);
			AddNodeReference(NodeRefs, JsonString(Operation, TEXT("result_ref")), Node);
			AddNodeReference(NodeRefs, OperationId(Operation), Node);
			AddNodeReference(NodeRefs, JsonString(Params, TEXT("ref")), Node);
			AddNodeReference(NodeRefs, JsonString(Params, TEXT("node_ref")), Node);
			AddNodeReference(NodeRefs, JsonString(Params, TEXT("result_ref")), Node);
		}

		UEdGraphNode* ResolveNodeReference(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params, const TMap<FString, UEdGraphNode*>& NodeRefs, FString& OutError)
		{
			const FString NodeRef = JsonString(Params, TEXT("node_ref"), JsonString(Params, TEXT("ref")));
			UEdGraphNode* Node = nullptr;
			if (!NodeRef.IsEmpty())
			{
				if (UEdGraphNode* const* Found = NodeRefs.Find(NodeRef)) Node = *Found;
			}
			else
			{
				Node = FindNodeByGuid(Graph, JsonString(Params, TEXT("node_guid"), JsonString(Params, TEXT("node_id"))));
			}
			if (!Node || Node->GetGraph() != Graph)
			{
				OutError = TEXT("Node reference was not found in the requested graph.");
				return nullptr;
			}
			return Node;
		}

		UEdGraphPin* ResolveEndpointPin(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params, const TCHAR* Prefix, const FString& DefaultDirection, const TMap<FString, UEdGraphNode*>& NodeRefs, UEdGraphNode*& OutNode, FString& OutError)
		{
			OutNode = nullptr;
			const TSharedPtr<FJsonObject> Endpoint = JsonObjectField(Params, Prefix);
			const FString PrefixString(Prefix);
			const FString NodeGuidKey = PrefixString + TEXT("_node_guid");
			const FString NodeIdKey = PrefixString + TEXT("_node_id");
			const FString NodeRefKey = PrefixString + TEXT("_node_ref");
			const FString RefKey = PrefixString + TEXT("_ref");
			const FString PinNameKey = PrefixString + TEXT("_pin_name");
			const FString PinKey = PrefixString + TEXT("_pin");
			const FString PinIdKey = PrefixString + TEXT("_pin_id");

			FString NodeGuid = JsonString(Endpoint, TEXT("node_guid"), JsonString(Endpoint, TEXT("node_id")));
			const TSharedPtr<FJsonObject> NestedNode = JsonObjectField(Endpoint, TEXT("node"));
			if (NodeGuid.IsEmpty()) NodeGuid = JsonString(NestedNode, TEXT("node_guid"), JsonString(NestedNode, TEXT("node_id")));
			if (NodeGuid.IsEmpty())
			{
				NodeGuid = JsonString(Params, *NodeGuidKey, JsonString(Params, *NodeIdKey));
			}
			FString NodeRef = JsonString(Endpoint, TEXT("node_ref"), JsonString(Endpoint, TEXT("ref")));
			if (NodeRef.IsEmpty())
			{
				NodeRef = JsonString(NestedNode, TEXT("node_ref"), JsonString(NestedNode, TEXT("ref"), JsonString(NestedNode, TEXT("$ref"))));
				int32 Fragment = INDEX_NONE;
				if (NodeRef.FindChar(TEXT('#'), Fragment)) NodeRef = NodeRef.Left(Fragment);
			}
			if (NodeRef.IsEmpty())
			{
				NodeRef = JsonString(Params, *NodeRefKey, JsonString(Params, *RefKey));
			}
			FString PinText = JsonString(Endpoint, TEXT("pin_name"), JsonString(Endpoint, TEXT("pin"), JsonString(Endpoint, TEXT("pin_id"))));
			if (PinText.IsEmpty())
			{
				const TSharedPtr<FJsonObject> NestedPin = JsonObjectField(Endpoint, TEXT("pin"));
				PinText = JsonString(NestedPin, TEXT("pin_id"), JsonString(NestedPin, TEXT("name_internal"), JsonString(NestedPin, TEXT("name"))));
			}
			if (PinText.IsEmpty())
			{
				PinText = JsonString(Params, *PinNameKey, JsonString(Params, *PinKey, JsonString(Params, *PinIdKey)));
			}
			const FString Direction = JsonString(Endpoint, TEXT("direction"), DefaultDirection);

			if ((NodeGuid.IsEmpty() && NodeRef.IsEmpty()) || PinText.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s endpoint requires node_guid/node_id/node_ref and pin_name/pin/pin_id."), Prefix);
				return nullptr;
			}
			if (!NodeRef.IsEmpty())
			{
				UEdGraphNode* const* RefNode = NodeRefs.Find(NodeRef);
				OutNode = RefNode ? *RefNode : nullptr;
				if (!OutNode)
				{
					OutError = FString::Printf(TEXT("%s node_ref was not found in this transaction: %s"), Prefix, *NodeRef);
					return nullptr;
				}
				if (OutNode->GetGraph() != Graph)
				{
					OutError = FString::Printf(TEXT("%s node_ref belongs to a different graph: %s"), Prefix, *NodeRef);
					return nullptr;
				}
			}
			else
			{
				OutNode = FindNodeByGuid(Graph, NodeGuid);
			}
			if (!OutNode)
			{
				OutError = FString::Printf(TEXT("%s node was not found in graph %s: %s"), Prefix, Graph ? *Graph->GetName() : TEXT("<null>"), *NodeGuid);
				return nullptr;
			}
			UEdGraphPin* Pin = FindPinFlexible(OutNode, PinText, Direction);
			if (!Pin)
			{
				OutError = FString::Printf(TEXT("%s pin was not found on node %s: %s"), Prefix, *NodeGuid, *PinText);
				return nullptr;
			}
			return Pin;
		}

		bool BlueprintHasVariableNamed(const UBlueprint* Blueprint, const FName VariableName)
		{
			if (!Blueprint || VariableName.IsNone())
			{
				return false;
			}
			if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName) != INDEX_NONE)
			{
				return true;
			}
			if (Blueprint->SkeletonGeneratedClass && Blueprint->SkeletonGeneratedClass->FindPropertyByName(VariableName))
			{
				return true;
			}
			if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->FindPropertyByName(VariableName))
			{
				return true;
			}
			return false;
		}

		UClass* ResolveClassByPathOrName(const FString& RequestedClass)
		{
			const FString ClassText = RequestedClass.TrimStartAndEnd();
			if (ClassText.IsEmpty())
			{
				return nullptr;
			}
			if (ClassText == TEXT("KismetSystemLibrary") || ClassText == TEXT("UKismetSystemLibrary") || ClassText == TEXT("/Script/Engine.KismetSystemLibrary"))
			{
				return UKismetSystemLibrary::StaticClass();
			}
			if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassText))
			{
				return LoadedClass;
			}
			if (UClass* LoadedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassText))
			{
				return LoadedClass;
			}
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Class = *It;
				if (Class && (Class->GetPathName().Equals(ClassText, ESearchCase::IgnoreCase) || Class->GetName().Equals(ClassText, ESearchCase::IgnoreCase)))
				{
					return Class;
				}
			}
			return nullptr;
		}

		UFunction* ResolveFunctionForCall(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FName& OutSelfMemberName, FString& OutError)
		{
			OutSelfMemberName = NAME_None;
			FString FunctionName = JsonString(Params, TEXT("function_name"), JsonString(Params, TEXT("name")));
			FString FunctionClass = JsonString(Params, TEXT("function_class"), JsonString(Params, TEXT("class")));
			const FString FunctionPath = JsonString(Params, TEXT("function_path"), JsonString(Params, TEXT("function")));

			if (!FunctionPath.IsEmpty())
			{
				if (UFunction* DirectFunction = FindObject<UFunction>(nullptr, *FunctionPath))
				{
					return DirectFunction;
				}
				FString ParsedClass;
				FString ParsedFunction;
				if (FunctionPath.Split(TEXT(":"), &ParsedClass, &ParsedFunction, ESearchCase::CaseSensitive, ESearchDir::FromEnd) ||
					FunctionPath.Split(TEXT("."), &ParsedClass, &ParsedFunction, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
				{
					if (FunctionClass.IsEmpty())
					{
						FunctionClass = ParsedClass;
					}
					if (FunctionName.IsEmpty())
					{
						FunctionName = ParsedFunction;
					}
				}
				else if (FunctionName.IsEmpty())
				{
					FunctionName = FunctionPath;
				}
			}

			if (FunctionName.IsEmpty())
			{
				OutError = TEXT("Function call operation requires function_name or function_path.");
				return nullptr;
			}

			if (!FunctionClass.IsEmpty())
			{
				UClass* Class = ResolveClassByPathOrName(FunctionClass);
				if (!Class)
				{
					OutError = FString::Printf(TEXT("Function class was not found: %s"), *FunctionClass);
					return nullptr;
				}
				if (UFunction* Function = Class->FindFunctionByName(FName(*FunctionName)))
				{
					return Function;
				}
				OutError = FString::Printf(TEXT("Function %s was not found on class %s."), *FunctionName, *Class->GetPathName());
				return nullptr;
			}

			if (Blueprint && Blueprint->SkeletonGeneratedClass)
			{
				if (UFunction* Function = Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*FunctionName)))
				{
					return Function;
				}
			}
			if (Blueprint && Blueprint->GeneratedClass)
			{
				if (UFunction* Function = Blueprint->GeneratedClass->FindFunctionByName(FName(*FunctionName)))
				{
					return Function;
				}
			}

			OutSelfMemberName = FName(*FunctionName);
			return nullptr;
		}

		TArray<AActor*> ResolveActorTargets(const TSharedPtr<FJsonObject>& Params)
		{
			TArray<FString> Paths;
			if (const TSharedPtr<FJsonObject> Targets = JsonObjectField(Params, TEXT("targets")))
			{
				if (const TArray<TSharedPtr<FJsonValue>>* Values = JsonArray(Targets, TEXT("paths")))
				{
					for (const TSharedPtr<FJsonValue>& Value : *Values)
					{
						FString Path;
						if (Value.IsValid() && Value->TryGetString(Path) && !Path.IsEmpty())
						{
							Paths.Add(Path);
						}
					}
				}
			}
			const FString DirectPath = JsonString(Params, TEXT("actor"), JsonString(Params, TEXT("path")));
			if (!DirectPath.IsEmpty())
			{
				Paths.Add(DirectPath);
			}

			TArray<AActor*> Actors;
			for (const FString& Path : Paths)
			{
				AActor* Actor = FindObject<AActor>(nullptr, *Path);
				if (!Actor)
				{
					for (TObjectIterator<AActor> It; It; ++It)
					{
						if (It->GetPathName() == Path || It->GetName() == Path)
						{
							Actor = *It;
							break;
						}
					}
				}
				if (Actor)
				{
					Actors.AddUnique(Actor);
				}
			}
			return Actors;
		}

		UMaterialInstanceConstant* LoadMaterialInstanceForEdit(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
		{
			OutAssetPath = JsonString(Params, TEXT("asset"), JsonString(Params, TEXT("material")));
			if (OutAssetPath.IsEmpty())
			{
				OutError = TEXT("Material operation requires params.asset or params.material.");
				return nullptr;
			}
			UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *OutAssetPath);
			if (!Instance)
			{
				OutError = FString::Printf(TEXT("Failed to load MaterialInstanceConstant: %s"), *OutAssetPath);
				return nullptr;
			}
			return Instance;
		}

		UMaterialInterface* LoadMaterialInterfaceForEdit(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
		{
			OutAssetPath = JsonString(Params, TEXT("asset"), JsonString(Params, TEXT("material")));
			if (OutAssetPath.IsEmpty())
			{
				OutError = TEXT("Material operation requires params.asset or params.material.");
				return nullptr;
			}
			UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *OutAssetPath);
			if (!Material)
			{
				OutError = FString::Printf(TEXT("Failed to load material interface: %s"), *OutAssetPath);
				return nullptr;
			}
			return Material;
		}

		TSharedRef<FJsonObject> ObjectToJson(UObject* Object)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Object)
			{
				return Result;
			}
			Result->SetStringField(TEXT("name"), Object->GetName());
			Result->SetStringField(TEXT("path"), Object->GetPathName());
			Result->SetStringField(TEXT("class"), Object->GetClass() ? Object->GetClass()->GetPathName() : FString());
			Result->SetStringField(TEXT("package"), Object->GetOutermost() ? Object->GetOutermost()->GetName() : FString());
			return Result;
		}

		FString NormalizedContentPath(const FString& RawPath)
		{
			FString Path = RawPath.TrimStartAndEnd();
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			while (Path.EndsWith(TEXT("/")) && Path.Len() > 5)
			{
				Path.LeftChopInline(1);
			}
			return Path;
		}

		bool ValidateGameContentPath(const FString& Path, FString& OutError)
		{
			if (Path.IsEmpty() || (!Path.Equals(TEXT("/Game"), ESearchCase::IgnoreCase) && !Path.StartsWith(TEXT("/Game/"))))
			{
				OutError = FString::Printf(TEXT("Write alpha content paths must be under /Game: %s"), *Path);
				return false;
			}
			if (Path.Contains(TEXT("..")) || Path.Contains(TEXT("\\")))
			{
				OutError = FString::Printf(TEXT("Content path contains unsupported traversal or separator characters: %s"), *Path);
				return false;
			}
			return true;
		}

		bool SplitDestinationPath(const TSharedPtr<FJsonObject>& Params, const FString& DefaultAssetName, FString& OutPackagePath, FString& OutAssetName, FString& OutError)
		{
			FString Destination = JsonString(Params, TEXT("destination"), JsonString(Params, TEXT("destination_asset")));
			if (!Destination.IsEmpty())
			{
				Destination = NormalizedContentPath(Destination);
				FString PackageName = Destination;
				int32 DotIndex = INDEX_NONE;
				if (Destination.FindChar(TEXT('.'), DotIndex))
				{
					PackageName = Destination.Left(DotIndex);
				}
				OutPackagePath = FPackageName::GetLongPackagePath(PackageName);
				OutAssetName = FPackageName::GetLongPackageAssetName(PackageName);
			}

			OutPackagePath = NormalizedContentPath(JsonString(Params, TEXT("destination_path"), JsonString(Params, TEXT("folder"), OutPackagePath)));
			OutAssetName = JsonString(Params, TEXT("name"), JsonString(Params, TEXT("asset_name"), OutAssetName.IsEmpty() ? DefaultAssetName : OutAssetName)).TrimStartAndEnd();
			if (OutPackagePath.IsEmpty() || OutAssetName.IsEmpty())
			{
				OutError = TEXT("Destination requires destination/destination_path plus name.");
				return false;
			}
			if (!ValidateGameContentPath(OutPackagePath, OutError))
			{
				return false;
			}
			if (OutAssetName.Contains(TEXT("/")) || OutAssetName.Contains(TEXT(".")) || OutAssetName.Contains(TEXT("\\")))
			{
				OutError = FString::Printf(TEXT("Asset name must not contain path separators or dots: %s"), *OutAssetName);
				return false;
			}
			return true;
		}

		UObject* LoadContentObject(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
		{
			OutAssetPath = JsonString(Params, TEXT("asset"), JsonString(Params, TEXT("source")));
			if (OutAssetPath.IsEmpty())
			{
				OutError = TEXT("Content operation requires params.asset or params.source.");
				return nullptr;
			}
			UObject* Object = LoadObject<UObject>(nullptr, *OutAssetPath);
			if (!Object)
			{
				OutError = FString::Printf(TEXT("Failed to load asset: %s"), *OutAssetPath);
				return nullptr;
			}
			return Object;
		}

		UWorld* EditorWorld()
		{
			return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		}

		UPrimitiveComponent* FindPrimitiveComponentForMaterial(AActor* Actor, const FString& ComponentName)
		{
			if (!Actor)
			{
				return nullptr;
			}
			TArray<UPrimitiveComponent*> Components;
			Actor->GetComponents<UPrimitiveComponent>(Components);
			for (UPrimitiveComponent* Component : Components)
			{
				if (!Component)
				{
					continue;
				}
				if (ComponentName.IsEmpty() || Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
				{
					return Component;
				}
			}
			return nullptr;
		}

		bool JsonVector2D(const TSharedPtr<FJsonObject>& Object, FVector2D& OutVector)
		{
			if (!Object.IsValid())
			{
				return false;
			}
			double X = 0.0;
			double Y = 0.0;
			if (!Object->TryGetNumberField(TEXT("x"), X) || !Object->TryGetNumberField(TEXT("y"), Y))
			{
				return false;
			}
			OutVector = FVector2D(X, Y);
			return true;
		}

		bool JsonMargin(const TSharedPtr<FJsonObject>& Object, FMargin& OutMargin)
		{
			if (!Object.IsValid())
			{
				return false;
			}
			double Left = 0.0;
			double Top = 0.0;
			double Right = 0.0;
			double Bottom = 0.0;
			if (!Object->TryGetNumberField(TEXT("left"), Left) || !Object->TryGetNumberField(TEXT("top"), Top) ||
				!Object->TryGetNumberField(TEXT("right"), Right) || !Object->TryGetNumberField(TEXT("bottom"), Bottom))
			{
				return false;
			}
			OutMargin = FMargin(Left, Top, Right, Bottom);
			return true;
		}

		UWidgetBlueprint* LoadWidgetBlueprintForEdit(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
		{
			OutAssetPath = NormalizeBlueprintObjectPath(JsonString(Params, TEXT("asset"), JsonString(Params, TEXT("widget"))));
			if (OutAssetPath.IsEmpty())
			{
				OutError = TEXT("Widget operation requires params.asset or params.widget.");
				return nullptr;
			}
			UWidgetBlueprint* WidgetBlueprint = LoadObject<UWidgetBlueprint>(nullptr, *OutAssetPath);
			if (!WidgetBlueprint)
			{
				OutError = FString::Printf(TEXT("Failed to load Widget Blueprint asset: %s"), *OutAssetPath);
				return nullptr;
			}
			return WidgetBlueprint;
		}

		UCanvasPanel* EnsureCanvasRoot(UWidgetBlueprint* WidgetBlueprint)
		{
			if (!WidgetBlueprint)
			{
				return nullptr;
			}
			if (!WidgetBlueprint->WidgetTree)
			{
				WidgetBlueprint->WidgetTree = NewObject<UWidgetTree>(WidgetBlueprint, UWidgetTree::StaticClass(), TEXT("WidgetTree"), RF_Transactional);
			}
			if (!WidgetBlueprint->WidgetTree)
			{
				return nullptr;
			}
			if (UCanvasPanel* ExistingCanvas = Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget))
			{
				return ExistingCanvas;
			}
			if (WidgetBlueprint->WidgetTree->RootWidget)
			{
				return nullptr;
			}
			UCanvasPanel* Canvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
			WidgetBlueprint->WidgetTree->RootWidget = Canvas;
			return Canvas;
		}

		UPanelWidget* ResolveWidgetParent(UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>& Params, FString& OutError)
		{
			if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
			{
				OutError = TEXT("Widget Blueprint has no WidgetTree.");
				return nullptr;
			}
			const FString ParentName = JsonString(Params, TEXT("parent"), JsonString(Params, TEXT("parent_widget")));
			if (!ParentName.IsEmpty())
			{
				UPanelWidget* Parent = WidgetBlueprint->WidgetTree->FindWidget<UPanelWidget>(FName(*ParentName));
				if (!Parent)
				{
					OutError = FString::Printf(TEXT("Widget parent panel was not found: %s"), *ParentName);
					return nullptr;
				}
				return Parent;
			}
			if (UPanelWidget* RootPanel = Cast<UPanelWidget>(WidgetBlueprint->WidgetTree->RootWidget))
			{
				return RootPanel;
			}
			OutError = TEXT("Widget Blueprint root is not a panel widget.");
			return nullptr;
		}

		TSharedRef<FJsonObject> WidgetToJson(UWidget* Widget)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Widget)
			{
				return Result;
			}
			Result->SetStringField(TEXT("name"), Widget->GetName());
			Result->SetStringField(TEXT("path"), Widget->GetPathName());
			Result->SetStringField(TEXT("class"), Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString());
			if (UPanelSlot* Slot = Widget->Slot)
			{
				Result->SetStringField(TEXT("slot_class"), Slot->GetClass() ? Slot->GetClass()->GetPathName() : FString());
				if (UWidget* ParentWidget = Slot->Parent)
				{
					Result->SetStringField(TEXT("parent"), ParentWidget->GetName());
				}
				if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
				{
					TSharedRef<FJsonObject> SlotObject = MakeShared<FJsonObject>();
					const FVector2D Position = CanvasSlot->GetPosition();
					const FVector2D Size = CanvasSlot->GetSize();
					const FVector2D Alignment = CanvasSlot->GetAlignment();
					TSharedRef<FJsonObject> PositionObject = MakeShared<FJsonObject>();
					PositionObject->SetNumberField(TEXT("x"), Position.X);
					PositionObject->SetNumberField(TEXT("y"), Position.Y);
					TSharedRef<FJsonObject> SizeObject = MakeShared<FJsonObject>();
					SizeObject->SetNumberField(TEXT("x"), Size.X);
					SizeObject->SetNumberField(TEXT("y"), Size.Y);
					TSharedRef<FJsonObject> AlignmentObject = MakeShared<FJsonObject>();
					AlignmentObject->SetNumberField(TEXT("x"), Alignment.X);
					AlignmentObject->SetNumberField(TEXT("y"), Alignment.Y);
					SlotObject->SetObjectField(TEXT("position"), PositionObject);
					SlotObject->SetObjectField(TEXT("size"), SizeObject);
					SlotObject->SetObjectField(TEXT("alignment"), AlignmentObject);
					SlotObject->SetBoolField(TEXT("auto_size"), CanvasSlot->GetAutoSize());
					SlotObject->SetNumberField(TEXT("z_order"), CanvasSlot->GetZOrder());
					Result->SetObjectField(TEXT("slot"), SlotObject);
				}
			}
			return Result;
		}

		UPanelSlot* AddWidgetToParent(UPanelWidget* Parent, UWidget* Widget)
		{
			if (!Parent || !Widget)
			{
				return nullptr;
			}
			if (UCanvasPanel* Canvas = Cast<UCanvasPanel>(Parent))
			{
				return Canvas->AddChildToCanvas(Widget);
			}
			return Parent->AddChild(Widget);
		}

		void ApplyCanvasSlotParams(UWidget* Widget, const TSharedPtr<FJsonObject>& Params)
		{
			if (!Widget || !Params.IsValid())
			{
				return;
			}
			UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot);
			if (!CanvasSlot)
			{
				return;
			}
			FVector2D VectorValue;
			if (JsonVector2D(JsonObjectField(Params, TEXT("position")), VectorValue))
			{
				CanvasSlot->SetPosition(VectorValue);
			}
			if (JsonVector2D(JsonObjectField(Params, TEXT("size")), VectorValue))
			{
				CanvasSlot->SetSize(VectorValue);
			}
			if (JsonVector2D(JsonObjectField(Params, TEXT("alignment")), VectorValue))
			{
				CanvasSlot->SetAlignment(VectorValue);
			}
			if (const TSharedPtr<FJsonObject> AnchorsObject = JsonObjectField(Params, TEXT("anchors")))
			{
				FVector2D Minimum;
				FVector2D Maximum;
				if (JsonVector2D(JsonObjectField(AnchorsObject, TEXT("minimum")), Minimum) || JsonVector2D(JsonObjectField(AnchorsObject, TEXT("min")), Minimum))
				{
					if (!JsonVector2D(JsonObjectField(AnchorsObject, TEXT("maximum")), Maximum) && !JsonVector2D(JsonObjectField(AnchorsObject, TEXT("max")), Maximum))
					{
						Maximum = Minimum;
					}
					CanvasSlot->SetAnchors(FAnchors(Minimum.X, Minimum.Y, Maximum.X, Maximum.Y));
				}
			}
			FMargin MarginValue;
			if (JsonMargin(JsonObjectField(Params, TEXT("offsets")), MarginValue))
			{
				CanvasSlot->SetOffsets(MarginValue);
			}
			if (Params->HasField(TEXT("auto_size")))
			{
				CanvasSlot->SetAutoSize(JsonBool(Params, TEXT("auto_size"), false));
			}
			if (Params->HasField(TEXT("z_order")))
			{
				CanvasSlot->SetZOrder(JsonInt(Params, TEXT("z_order"), 0));
			}
		}

		FObjectProperty* FindWidgetObjectProperty(UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
		{
			if (!WidgetBlueprint || !Widget)
			{
				return nullptr;
			}
			const FName WidgetName = Widget->GetFName();
			UClass* ClassesToCheck[] = {
				WidgetBlueprint->GeneratedClass,
				WidgetBlueprint->SkeletonGeneratedClass
			};
			for (UClass* Class : ClassesToCheck)
			{
				if (!Class)
				{
					continue;
				}
				FObjectProperty* Property = FindFProperty<FObjectProperty>(Class, WidgetName);
				if (Property && Property->PropertyClass && Property->PropertyClass->IsChildOf(Widget->GetClass()))
				{
					return Property;
				}
			}
			return nullptr;
		}

#if UEPI_WITH_ENHANCED_INPUT
		bool ParseInputActionValueType(const FString& RawType, EInputActionValueType& OutValueType, FString& OutError)
		{
			const FString Type = RawType.TrimStartAndEnd().ToLower();
			if (Type.IsEmpty() || Type == TEXT("bool") || Type == TEXT("boolean") || Type == TEXT("digital"))
			{
				OutValueType = EInputActionValueType::Boolean;
				return true;
			}
			if (Type == TEXT("axis1d") || Type == TEXT("float") || Type == TEXT("1d"))
			{
				OutValueType = EInputActionValueType::Axis1D;
				return true;
			}
			if (Type == TEXT("axis2d") || Type == TEXT("vector2d") || Type == TEXT("2d"))
			{
				OutValueType = EInputActionValueType::Axis2D;
				return true;
			}
			if (Type == TEXT("axis3d") || Type == TEXT("vector") || Type == TEXT("vector3d") || Type == TEXT("3d"))
			{
				OutValueType = EInputActionValueType::Axis3D;
				return true;
			}
			OutError = FString::Printf(TEXT("Unsupported Enhanced Input action value type: %s"), *RawType);
			return false;
		}

		UInputAction* LoadInputActionForEdit(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
		{
			OutAssetPath = JsonString(Params, TEXT("action"), JsonString(Params, TEXT("input_action")));
			if (OutAssetPath.IsEmpty())
			{
				OutError = TEXT("Enhanced Input operation requires params.action or params.input_action.");
				return nullptr;
			}
			UInputAction* Action = LoadObject<UInputAction>(nullptr, *OutAssetPath);
			if (!Action)
			{
				OutError = FString::Printf(TEXT("Failed to load InputAction: %s"), *OutAssetPath);
				return nullptr;
			}
			return Action;
		}

		UInputMappingContext* LoadInputMappingContextForEdit(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
		{
			OutAssetPath = JsonString(Params, TEXT("context"), JsonString(Params, TEXT("mapping_context"), JsonString(Params, TEXT("asset"))));
			if (OutAssetPath.IsEmpty())
			{
				OutError = TEXT("Enhanced Input operation requires params.context, params.mapping_context, or params.asset.");
				return nullptr;
			}
			UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *OutAssetPath);
			if (!Context)
			{
				OutError = FString::Printf(TEXT("Failed to load InputMappingContext: %s"), *OutAssetPath);
				return nullptr;
			}
			return Context;
		}

		TSharedRef<FJsonObject> InputMappingToJson(const FEnhancedActionKeyMapping& Mapping)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("action"), Mapping.Action ? Mapping.Action->GetPathName() : FString());
			Result->SetStringField(TEXT("key"), Mapping.Key.ToString());
			Result->SetNumberField(TEXT("trigger_count"), Mapping.Triggers.Num());
			Result->SetNumberField(TEXT("modifier_count"), Mapping.Modifiers.Num());
			return Result;
		}
#endif

		void AddOperationResult(TArray<TSharedPtr<FJsonValue>>& OperationResults, int32 Index, const FString& Type, bool bOk, const FString& Message, const TSharedPtr<FJsonObject>& Detail = nullptr)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("index"), Index);
			Object->SetStringField(TEXT("type"), Type);
			Object->SetBoolField(TEXT("ok"), bOk);
			Object->SetStringField(TEXT("message"), Message);
			if (Detail.IsValid())
			{
				Object->SetObjectField(TEXT("detail"), Detail);
			}
			OperationResults.Add(MakeShared<FJsonValueObject>(Object));
		}

		bool WriteRefreshRequest(const TArray<FString>& Targets, const FString& DataMode, FString& OutRequestPath, FString& OutError)
		{
			const FString RequestIdPart = FString::Printf(TEXT("uepi-refresh:%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			const FString CreatedAt = FDateTime::UtcNow().ToIso8601();
			const FString SafeRequestId = RequestIdPart.Replace(TEXT(":"), TEXT("-"));
			const FString RequestPath = FPaths::Combine(UEPIRequestsDirectory(), FString::Printf(TEXT("%s.queued.json"), *SafeRequestId));

			TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
			Request->SetStringField(TEXT("schema_version"), TEXT("uepi.refresh-request.v2"));
			Request->SetStringField(TEXT("request_id"), RequestIdPart);
			Request->SetStringField(TEXT("project_binding_id"), UEPIProjectBindingId());
			Request->SetStringField(TEXT("status"), TEXT("queued"));
			Request->SetStringField(TEXT("created_at"), CreatedAt);
			Request->SetStringField(TEXT("expires_at"), (FDateTime::UtcNow() + FTimespan::FromMinutes(5.0)).ToIso8601());
			Request->SetStringField(TEXT("data_mode"), DataMode);
			Request->SetArrayField(TEXT("targets"), StringArrayToJsonValues(Targets));
			Request->SetArrayField(TEXT("target_object_paths"), StringArrayToJsonValues(Targets));
			Request->SetArrayField(TEXT("domains"), EmptyJsonArray());
			Request->SetArrayField(TEXT("artifacts"), EmptyJsonArray());
			Request->SetStringField(TEXT("reason"), TEXT("bridge_edit_apply"));
			Request->SetStringField(TEXT("tool_name"), TEXT("uepi_bridge"));

			if (!SaveJsonObject(Request, RequestPath))
			{
				OutError = FString::Printf(TEXT("Failed to write refresh request: %s"), *RequestPath);
				return false;
			}
			OutRequestPath = RequestPath;
			OutError.Reset();
			return true;
		}


		FString AbsoluteLogFilename()
		{
			const FString Candidate = FGenericPlatformOutputDevices::GetAbsoluteLogFilename();
			return FPaths::ConvertRelativePathToFull(Candidate);
		}

		TArray<FString> TailLines(const FString& Path, int32 LineLimit)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				return {};
			}
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			const int32 Start = FMath::Max(0, Lines.Num() - FMath::Clamp(LineLimit, 1, 500));
			TArray<FString> Tail;
			for (int32 Index = Start; Index < Lines.Num(); ++Index)
			{
				Tail.Add(Lines[Index]);
			}
			return Tail;
		}

		bool SaveJsonObject(const TSharedRef<FJsonObject>& Object, const FString& Path)
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			return FFileHelper::SaveStringToFile(JsonObjectToString(Object), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		TSharedRef<FJsonObject> ActorObject(AActor* Actor)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			if (!Actor)
			{
				return Object;
			}
			Object->SetStringField(TEXT("name"), Actor->GetName());
			Object->SetStringField(TEXT("path"), Actor->GetPathName());
			Object->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
			Object->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Object->SetStringField(TEXT("folder"), Actor->GetFolderPath().ToString());
			Object->SetStringField(TEXT("level"), Actor->GetLevel() ? Actor->GetLevel()->GetPathName() : FString());
			TArray<FString> Tags;
			for (const FName& Tag : Actor->Tags)
			{
				Tags.Add(Tag.ToString());
			}
			Object->SetArrayField(TEXT("tags"), StringArrayToJsonValues(Tags));
			const FTransform Transform = Actor->GetActorTransform();
			TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
			Location->SetNumberField(TEXT("x"), Transform.GetLocation().X);
			Location->SetNumberField(TEXT("y"), Transform.GetLocation().Y);
			Location->SetNumberField(TEXT("z"), Transform.GetLocation().Z);
			Object->SetObjectField(TEXT("location"), Location);
			TSharedRef<FJsonObject> Rotation = MakeShared<FJsonObject>();
			Rotation->SetNumberField(TEXT("pitch"), Transform.Rotator().Pitch);
			Rotation->SetNumberField(TEXT("yaw"), Transform.Rotator().Yaw);
			Rotation->SetNumberField(TEXT("roll"), Transform.Rotator().Roll);
			Object->SetObjectField(TEXT("rotation"), Rotation);
			TSharedRef<FJsonObject> Scale = MakeShared<FJsonObject>();
			Scale->SetNumberField(TEXT("x"), Transform.GetScale3D().X);
			Scale->SetNumberField(TEXT("y"), Transform.GetScale3D().Y);
			Scale->SetNumberField(TEXT("z"), Transform.GetScale3D().Z);
			Object->SetObjectField(TEXT("scale"), Scale);
			Object->SetNumberField(TEXT("component_count"), Actor->GetComponents().Num());
			return Object;
		}
	}

	bool FUEPIEditorCommandBridge::Start(const FString& InSessionId, int32 InRequestedPort, FString& OutError)
	{
		SessionId = InSessionId;
		SessionPath = UEPIBridgeSessionPath();
		TokenPath = UEPIBridgeTokenPath();
		Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		bActive = true;

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(TokenPath), true);
		if (!FFileHelper::SaveStringToFile(Token, *TokenPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			bActive = false;
			OutError = FString::Printf(TEXT("Failed to write UEPI bridge token file: %s"), *TokenPath);
			return false;
		}

		const int32 FirstPort = InRequestedPort > 0 ? FMath::Clamp(InRequestedPort, 1, 65535) : 48735;
		const int32 MaxAttempts = InRequestedPort > 0 ? 1 : 64;
		for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
		{
			const int32 CandidatePort = FirstPort + Attempt;
			if (CandidatePort > 65535)
			{
				break;
			}
			const FIPv4Endpoint Endpoint(FIPv4Address(127, 0, 0, 1), static_cast<uint16>(CandidatePort));
			Listener = MakeUnique<FTcpListener>(Endpoint, FTimespan::FromMilliseconds(100), true);
			if (Listener.IsValid() && Listener->IsActive())
			{
				Port = CandidatePort;
				break;
			}
			Listener.Reset();
		}
		if (!Listener.IsValid() || !Listener->IsActive())
		{
			bActive = false;
			OutError = InRequestedPort > 0
				? FString::Printf(TEXT("Failed to start UEPI bridge on requested port %d."), InRequestedPort)
				: TEXT("Failed to start UEPI bridge on any default localhost port.");
			return false;
		}
		Listener->OnConnectionAccepted().BindRaw(this, &FUEPIEditorCommandBridge::HandleConnectionAccepted);

		return WriteSessionObject(TEXT("active"), OutError);
	}

	void FUEPIEditorCommandBridge::Stop()
	{
		if (bOwnsPIESession && GEditor)
		{
			if (GEditor->PlayWorld) GEditor->RequestEndPlayMap(); else GEditor->CancelRequestPlaySession();
			bOwnsPIESession = false;
			OwnedRuntimeSessionId.Reset();
		}
		FSocket* PendingSocket = nullptr;
		while (PendingSockets.Dequeue(PendingSocket))
		{
			if (PendingSocket)
			{
				PendingSocket->Close();
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(PendingSocket);
			}
		}
		if (bActive)
		{
			FString Error;
			WriteSessionObject(TEXT("stopped"), Error);
		}
		bActive = false;
		Listener.Reset();
		Token.Reset();
	}

	void FUEPIEditorCommandBridge::TickHeartbeat()
	{
		if (!bActive)
		{
			return;
		}
		ProcessPendingSockets();
		FString Error;
		WriteSessionObject(TEXT("active"), Error);
	}

	bool FUEPIEditorCommandBridge::IsActive() const
	{
		return bActive;
	}

	const FString& FUEPIEditorCommandBridge::GetSessionPath() const
	{
		return SessionPath;
	}

	const FString& FUEPIEditorCommandBridge::GetTokenPath() const
	{
		return TokenPath;
	}

	int32 FUEPIEditorCommandBridge::GetPort() const
	{
		return Port;
	}

	bool FUEPIEditorCommandBridge::HandleConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& RemoteEndpoint)
	{
		if (!ClientSocket || RemoteEndpoint.Address != FIPv4Address(127, 0, 0, 1))
		{
			return false;
		}
		PendingSockets.Enqueue(ClientSocket);
		return true;
	}

	void FUEPIEditorCommandBridge::ProcessPendingSockets()
	{
		constexpr int32 MaxSocketsPerTick = 8;
		for (int32 Index = 0; Index < MaxSocketsPerTick; ++Index)
		{
			FSocket* ClientSocket = nullptr;
			if (!PendingSockets.Dequeue(ClientSocket))
			{
				return;
			}
			ProcessSocket(ClientSocket);
			if (ClientSocket)
			{
				ClientSocket->Close();
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
			}
		}
	}

	bool FUEPIEditorCommandBridge::ProcessSocket(FSocket* ClientSocket)
	{
		if (!ClientSocket)
		{
			return false;
		}
		FString JsonText;
		if (!ReadFrame(ClientSocket, JsonText))
		{
			WriteFrame(ClientSocket, ErrorResponse(TEXT(""), TEXT("UEPI_BRIDGE_READ_FAILED"), TEXT("Failed to read UEPI bridge request frame.")));
			return false;
		}

		TSharedPtr<FJsonObject> Request;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
		{
			WriteFrame(ClientSocket, ErrorResponse(TEXT(""), TEXT("UEPI_BRIDGE_BAD_JSON"), TEXT("UEPI bridge request was not a JSON object.")));
			return false;
		}

		return WriteFrame(ClientSocket, HandleRequest(Request));
	}

	bool FUEPIEditorCommandBridge::ReadFrame(FSocket* ClientSocket, FString& OutJsonText) const
	{
		uint8 Header[4] = { 0, 0, 0, 0 };
		int32 BytesRead = 0;
		for (int32 Offset = 0; Offset < 4;)
		{
			if (!ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(250)))
			{
				return false;
			}
			int32 ChunkRead = 0;
			if (!ClientSocket->Recv(Header + Offset, 4 - Offset, ChunkRead) || ChunkRead <= 0)
			{
				return false;
			}
			Offset += ChunkRead;
			BytesRead += ChunkRead;
		}

		const uint32 PayloadSize =
			(static_cast<uint32>(Header[0]) << 24) |
			(static_cast<uint32>(Header[1]) << 16) |
			(static_cast<uint32>(Header[2]) << 8) |
			static_cast<uint32>(Header[3]);
		if (PayloadSize == 0 || PayloadSize > 1024 * 1024)
		{
			return false;
		}

		TArray<uint8> Payload;
		Payload.SetNumUninitialized(static_cast<int32>(PayloadSize));
		for (int32 Offset = 0; Offset < static_cast<int32>(PayloadSize);)
		{
			if (!ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(250)))
			{
				return false;
			}
			int32 ChunkRead = 0;
			if (!ClientSocket->Recv(Payload.GetData() + Offset, static_cast<int32>(PayloadSize) - Offset, ChunkRead) || ChunkRead <= 0)
			{
				return false;
			}
			Offset += ChunkRead;
		}

		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Payload.GetData()), Payload.Num());
		OutJsonText = FString(Converter.Length(), Converter.Get());
		return true;
	}

	bool FUEPIEditorCommandBridge::WriteFrame(FSocket* ClientSocket, const TSharedRef<FJsonObject>& Response) const
	{
		const FString ResponseText = JsonObjectToString(Response);
		FTCHARToUTF8 Converter(*ResponseText);
		const uint32 PayloadSize = static_cast<uint32>(Converter.Length());
		uint8 Header[4] = {
			static_cast<uint8>((PayloadSize >> 24) & 0xff),
			static_cast<uint8>((PayloadSize >> 16) & 0xff),
			static_cast<uint8>((PayloadSize >> 8) & 0xff),
			static_cast<uint8>(PayloadSize & 0xff),
		};
		int32 BytesSent = 0;
		if (!ClientSocket->Send(Header, 4, BytesSent) || BytesSent != 4)
		{
			return false;
		}
		int32 PayloadSent = 0;
		const uint8* PayloadData = reinterpret_cast<const uint8*>(Converter.Get());
		while (PayloadSent < Converter.Length())
		{
			int32 ChunkSent = 0;
			if (!ClientSocket->Send(PayloadData + PayloadSent, Converter.Length() - PayloadSent, ChunkSent) || ChunkSent <= 0)
			{
				return false;
			}
			PayloadSent += ChunkSent;
		}
		return true;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::HandleRequest(const TSharedPtr<FJsonObject>& Request)
	{
		FString RequestId;
		Request->TryGetStringField(TEXT("id"), RequestId);
		FString RequestToken;
		if (!Request->TryGetStringField(TEXT("token"), RequestToken) || RequestToken != Token)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_BRIDGE_UNAUTHORIZED"), TEXT("UEPI bridge token is missing or invalid."));
		}

		FString Command;
		if (!Request->TryGetStringField(TEXT("command"), Command))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_BRIDGE_COMMAND_MISSING"), TEXT("UEPI bridge request did not include a command."));
		}

		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		TSharedPtr<FJsonObject> Params;
		if (Request->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
		{
			Params = *ParamsPtr;
		}
		const FString ExpectedProjectBindingId = JsonString(Params, TEXT("expected_project_binding_id"));
		if (!ExpectedProjectBindingId.IsEmpty() && ExpectedProjectBindingId != UEPIProjectBindingId())
		{
			return ErrorResponse(RequestId, TEXT("UEPI_PROJECT_BINDING_MISMATCH"), TEXT("Bridge request project binding does not match this Editor project."));
		}
		const FString ExpectedEditorSessionId = JsonString(Params, TEXT("expected_editor_session_id"));
		if (!ExpectedEditorSessionId.IsEmpty() && ExpectedEditorSessionId != SessionId)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDITOR_SESSION_MISMATCH"), TEXT("Bridge request session does not match this Editor session."));
		}

		if (Command == TEXT("editor.get_status"))
		{
			return StatusResult(RequestId);
		}
		if (Command == TEXT("editor.get_selection"))
		{
			return SelectionResult(RequestId);
		}
		if (Command == TEXT("editor.read_output_log"))
		{
			return OutputLogResult(RequestId, Params);
		}
		if (Command == TEXT("editor.read_world"))
		{
			return WorldReadResult(RequestId, Params);
		}
		if (Command == TEXT("schema.get"))
		{
			return SchemaResult(RequestId, Params);
		}
		if (Command == TEXT("runtime.control"))
		{
			return RuntimeResult(RequestId, Params);
		}
		if (Command == TEXT("asset.refresh_now"))
		{
			TArray<FString> Targets;
			FString DataMode = TEXT("live");
			if (Params.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* TargetValues = nullptr;
				if (Params->TryGetArrayField(TEXT("target_object_paths"), TargetValues) && TargetValues)
				{
					for (const TSharedPtr<FJsonValue>& Value : *TargetValues)
					{
						FString Target;
						if (Value.IsValid() && Value->TryGetString(Target) && !Target.IsEmpty())
						{
							Targets.AddUnique(Target);
						}
					}
				}
				Params->TryGetStringField(TEXT("data_mode"), DataMode);
			}
			return RefreshNowResult(RequestId, Targets, DataMode.IsEmpty() ? TEXT("live") : DataMode);
		}
		if (Command == TEXT("editor.capture_viewport"))
		{
			return ViewportCaptureUnsupported(RequestId);
		}
		if (Command == TEXT("edit.discover"))
		{
			return EditDiscoverResult(RequestId);
		}
		if (Command == TEXT("edit.apply"))
		{
			return EditApplyResult(RequestId, Params);
		}
		if (Command == TEXT("edit.validate"))
		{
			return EditValidateResult(RequestId, Params);
		}
		if (Command == TEXT("edit.rollback"))
		{
			return EditRollbackResult(RequestId, Params);
		}
		return ErrorResponse(RequestId, TEXT("UEPI_BRIDGE_UNKNOWN_COMMAND"), FString::Printf(TEXT("Unknown UEPI bridge command: %s"), *Command));
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::ErrorResponse(const FString& RequestId, const FString& Code, const FString& Message) const
	{
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), false);
		Response->SetArrayField(TEXT("diagnostics"), DiagnosticsArray(Code, TEXT("error"), Message));
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::StatusResult(const FString& RequestId) const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("session_id"), SessionId);
		Result->SetStringField(TEXT("editor_session_id"), SessionId);
		Result->SetStringField(TEXT("project_binding_id"), UEPIProjectBindingId());
		Result->SetStringField(TEXT("project_name"), FApp::GetProjectName());
		Result->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		Result->SetStringField(TEXT("session_path"), SessionPath);
		Result->SetNumberField(TEXT("port"), Port);
		Result->SetBoolField(TEXT("transport_ready"), true);
		TArray<FString> Capabilities = FUEPIBridgeProtocol::ReadCapabilities();
		Capabilities.Append(FUEPIBridgeProtocol::WriteCapabilities());
		Result->SetArrayField(TEXT("capabilities"), StringArrayToJsonValues(Capabilities));

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), true);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::SelectionResult(const FString& RequestId) const
	{
		TArray<TSharedPtr<FJsonValue>> ActorValues;
		if (GEditor && GEditor->GetSelectedActors())
		{
			TArray<AActor*> Actors;
			GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Actors);
			for (AActor* Actor : Actors)
			{
				ActorValues.Add(MakeShared<FJsonValueObject>(ActorObject(Actor)));
			}
		}

		TArray<TSharedPtr<FJsonValue>> ObjectValues;
		if (GEditor && GEditor->GetSelectedObjects())
		{
			TArray<UObject*> Objects;
			GEditor->GetSelectedObjects()->GetSelectedObjects(Objects);
			for (UObject* Object : Objects)
			{
				if (!Object)
				{
					continue;
				}
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Object->GetName());
				Item->SetStringField(TEXT("path"), Object->GetPathName());
				Item->SetStringField(TEXT("class"), Object->GetClass() ? Object->GetClass()->GetPathName() : FString());
				ObjectValues.Add(MakeShared<FJsonValueObject>(Item));
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("actor_count"), ActorValues.Num());
		Result->SetArrayField(TEXT("actors"), ActorValues);
		Result->SetNumberField(TEXT("object_count"), ObjectValues.Num());
		Result->SetArrayField(TEXT("objects"), ObjectValues);

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), true);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::OutputLogResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params) const
	{
		const FString OutputLogPath = AbsoluteLogFilename();
		const int32 LineLimit = FMath::Clamp(JsonInt(Params, TEXT("line_limit"), 100), 1, 2000);
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *OutputLogPath))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDITOR_LOG_READ_FAILED"), TEXT("The active output log file could not be read."));
		}
		int64 ByteOffset = FMath::Max<int64>(0, Bytes.Num() - 1024 * 1024);
		if (const TSharedPtr<FJsonObject> Cursor = JsonObjectField(Params, TEXT("cursor")))
		{
			double RequestedOffset = 0.0;
			if (Cursor->TryGetNumberField(TEXT("byte_offset"), RequestedOffset))
			{
				ByteOffset = FMath::Clamp<int64>(static_cast<int64>(RequestedOffset), 0, Bytes.Num());
			}
		}
		const int32 RemainingBytes = Bytes.Num() - static_cast<int32>(ByteOffset);
		FString LogText;
		if (RemainingBytes > 0)
		{
			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + ByteOffset), RemainingBytes);
			LogText = FString(Converter.Length(), Converter.Get());
		}
		TArray<FString> ParsedLines;
		LogText.ParseIntoArrayLines(ParsedLines, false);
		TArray<TSharedPtr<FJsonValue>> Lines;
		const int32 FirstLine = FMath::Max(0, ParsedLines.Num() - LineLimit);
		for (int32 Index = FirstLine; Index < ParsedLines.Num(); ++Index)
		{
			Lines.Add(MakeShared<FJsonValueString>(ParsedLines[Index]));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("log_path"), OutputLogPath);
		Result->SetNumberField(TEXT("line_count"), Lines.Num());
		Result->SetArrayField(TEXT("lines"), Lines);
		TSharedRef<FJsonObject> Cursor = MakeShared<FJsonObject>();
		Cursor->SetStringField(TEXT("file_identity"), FMD5::HashAnsiString(*FString::Printf(TEXT("%s:%d"), *OutputLogPath, Bytes.Num())));
		Cursor->SetNumberField(TEXT("byte_offset"), Bytes.Num());
		Cursor->SetNumberField(TEXT("file_size"), Bytes.Num());
		Result->SetObjectField(TEXT("cursor"), Cursor);

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), true);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), TArray<TSharedPtr<FJsonValue>>());
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::WorldReadResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params) const
	{
		const FString WorldKind = JsonString(Params, TEXT("world"), TEXT("editor"));
		UWorld* World = nullptr;
		if (GEditor)
		{
			if (WorldKind.Equals(TEXT("pie"), ESearchCase::IgnoreCase))
			{
				for (const FWorldContext& Context : GEditor->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE)
					{
						World = Context.World();
						break;
					}
				}
			}
			else
			{
				World = GEditor->GetEditorWorldContext().World();
			}
		}
		if (!World)
		{
			return ErrorResponse(RequestId, WorldKind.Equals(TEXT("pie"), ESearchCase::IgnoreCase) ? TEXT("UEPI_RUNTIME_SESSION_REQUIRED") : TEXT("UEPI_EDITOR_WORLD_UNAVAILABLE"), TEXT("The requested Unreal world is not available."));
		}

		const TSharedPtr<FJsonObject> Filters = JsonObjectField(Params, TEXT("filters"));
		const TArray<FString> ClassPaths = Filters.IsValid() ? JsonStringArray(Filters, TEXT("class_paths")) : TArray<FString>();
		const TArray<FString> Labels = Filters.IsValid() ? JsonStringArray(Filters, TEXT("labels")) : TArray<FString>();
		const TArray<FString> ObjectPaths = Filters.IsValid() ? JsonStringArray(Filters, TEXT("object_paths")) : TArray<FString>();
		TArray<TSharedPtr<FJsonValue>> ActorValues;
		for (TActorIterator<AActor> It(World); It && ActorValues.Num() < 5000; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			const FString ClassPath = Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString();
			if (ClassPaths.Num() > 0 && !ClassPaths.Contains(ClassPath))
			{
				continue;
			}
			if (Labels.Num() > 0 && !Labels.Contains(Actor->GetActorLabel()))
			{
				continue;
			}
			if (ObjectPaths.Num() > 0 && !ObjectPaths.Contains(Actor->GetPathName()))
			{
				continue;
			}
			TSharedRef<FJsonObject> ActorJson = ActorObject(Actor);
			TArray<TSharedPtr<FJsonValue>> Components;
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (!Component)
				{
					continue;
				}
				TSharedRef<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
				ComponentJson->SetStringField(TEXT("name"), Component->GetName());
				ComponentJson->SetStringField(TEXT("path"), Component->GetPathName());
				ComponentJson->SetStringField(TEXT("class"), Component->GetClass() ? Component->GetClass()->GetPathName() : FString());
				Components.Add(MakeShared<FJsonValueObject>(ComponentJson));
			}
			ActorJson->SetArrayField(TEXT("components"), Components);
			ActorValues.Add(MakeShared<FJsonValueObject>(ActorJson));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("world"), WorldKind.ToLower());
		Result->SetStringField(TEXT("map"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
		Result->SetNumberField(TEXT("actor_count"), ActorValues.Num());
		Result->SetArrayField(TEXT("actors"), ActorValues);
		return SuccessResponse(RequestId, Result);
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::SchemaResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params) const
	{
		const FString Action = JsonString(Params, TEXT("action"), TEXT("class_property"));
		const int32 MaxDepth = FMath::Clamp(JsonInt(Params, TEXT("max_depth"), 8), 1, 16);
		if (Action == TEXT("class_property"))
		{
			const FString ClassPath = JsonString(Params, TEXT("class_path"));
			UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
			if (!Class)
			{
				return ErrorResponse(RequestId, TEXT("UEPI_SCHEMA_CLASS_NOT_FOUND"), FString::Printf(TEXT("Class was not found: %s"), *ClassPath));
			}
			return SuccessResponse(RequestId, FUEPIPropertyCodec::BuildSchema(Class, MaxDepth));
		}
		if (Action == TEXT("asset_property"))
		{
			const FString AssetPath = JsonString(Params, TEXT("asset"));
			UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
			if (!Object)
			{
				return ErrorResponse(RequestId, TEXT("UEPI_SCHEMA_ASSET_NOT_FOUND"), FString::Printf(TEXT("Asset was not found: %s"), *AssetPath));
			}
			TSharedRef<FJsonObject> Result = FUEPIPropertyCodec::BuildObjectSchema(Object, MaxDepth);
			Result->SetStringField(TEXT("asset"), Object->GetPathName());
			return SuccessResponse(RequestId, Result);
		}
		if (Action == TEXT("blueprint_node"))
		{
			const FString Kind = JsonString(Params, TEXT("kind"));
			const TSet<FString> SupportedKinds = { TEXT("custom_event"), TEXT("input_key"), TEXT("function_call"), TEXT("make_struct"), TEXT("variable_get"), TEXT("variable_set"), TEXT("branch"), TEXT("print_string"), TEXT("animgraph_slot") };
			if (!SupportedKinds.Contains(Kind))
			{
				return ErrorResponse(RequestId, TEXT("UEPI_SCHEMA_BLUEPRINT_NODE_KIND_UNSUPPORTED"), FString::Printf(TEXT("Blueprint node kind is not registered: %s"), *Kind));
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema_version"), TEXT("uepi.blueprint-node-schema.v1"));
			Result->SetStringField(TEXT("kind"), Kind);
			Result->SetStringField(TEXT("graph_schema"), JsonString(Params, TEXT("graph_schema"), Kind == TEXT("animgraph_slot") ? TEXT("AnimGraph") : TEXT("K2")));
			Result->SetBoolField(TEXT("returns_real_pins_after_create"), true);
			Result->SetStringField(TEXT("connection_rule"), TEXT("Use returned pin_id/name and Graph Schema; never guess pins."));
			if (Kind == TEXT("make_struct"))
			{
				UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *JsonString(Params, TEXT("struct_path")));
				if (!Struct || !UK2Node_MakeStruct::CanBeMade(Struct))
				{
					return ErrorResponse(RequestId, TEXT("UEPI_SCHEMA_STRUCT_NOT_MAKEABLE"), TEXT("The requested struct is unavailable or cannot be exposed as a Make Struct node."));
				}
				Result->SetObjectField(TEXT("struct_schema"), FUEPIPropertyCodec::BuildSchema(Struct, MaxDepth));
			}
			return SuccessResponse(RequestId, Result);
		}
		return ErrorResponse(RequestId, TEXT("UEPI_SCHEMA_ACTION_UNSUPPORTED"), FString::Printf(TEXT("Schema action is not implemented by the Editor Bridge: %s"), *Action));
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::RuntimeResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params)
	{
		const UUEPISettings* Settings = GetDefault<UUEPISettings>();
		const FString Action = JsonString(Params, TEXT("action"), TEXT("status"));
		UWorld* PIEWorld = GEditor ? GEditor->PlayWorld : nullptr;
		if (Action == TEXT("status"))
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("runtime_session_id"), OwnedRuntimeSessionId);
			Result->SetStringField(TEXT("state"), PIEWorld ? TEXT("running") : (bOwnsPIESession ? TEXT("starting") : TEXT("stopped")));
			Result->SetBoolField(TEXT("owned_by_uepi"), bOwnsPIESession);
			Result->SetStringField(TEXT("map"), PIEWorld && PIEWorld->GetOutermost() ? PIEWorld->GetOutermost()->GetName() : FString());
			return SuccessResponse(RequestId, Result);
		}
		if (!Settings || !Settings->bAllowPIEControl)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_CAPABILITY_DISABLED"), TEXT("Controlled PIE is disabled in UEPI Project Settings."));
		}
		if (Action == TEXT("start"))
		{
			if (PIEWorld || bOwnsPIESession)
			{
				return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_ALREADY_RUNNING"), TEXT("A PIE session is already active or starting."));
			}
			FRequestPlaySessionParams SessionParams;
			SessionParams.SessionDestination = EPlaySessionDestinationType::InProcess;
			SessionParams.WorldType = EPlaySessionWorldType::PlayInEditor;
			SessionParams.bAllowOnlineSubsystem = false;
			SessionParams.GlobalMapOverride = JsonString(Params, TEXT("map"));
			OwnedRuntimeSessionId = FString::Printf(TEXT("uepi-runtime:%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			bOwnsPIESession = true;
			GEditor->RequestPlaySession(SessionParams);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("runtime_session_id"), OwnedRuntimeSessionId); Result->SetStringField(TEXT("state"), TEXT("starting")); return SuccessResponse(RequestId, Result);
		}
		if (Action == TEXT("stop"))
		{
			if (!bOwnsPIESession)
			{
				return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_NOT_OWNER"), TEXT("UEPI will not stop a PIE session it did not start."));
			}
			if (PIEWorld) GEditor->RequestEndPlayMap(); else GEditor->CancelRequestPlaySession();
			const FString StoppedSession = OwnedRuntimeSessionId; OwnedRuntimeSessionId.Reset(); bOwnsPIESession = false;
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("runtime_session_id"), StoppedSession); Result->SetStringField(TEXT("state"), TEXT("stopping")); return SuccessResponse(RequestId, Result);
		}
		if (!PIEWorld || !bOwnsPIESession)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_SESSION_REQUIRED"), TEXT("A UEPI-owned PIE session is required."));
		}
		if (Action == TEXT("input"))
		{
			APlayerController* Controller = PIEWorld->GetFirstPlayerController();
			const FKey Key(FName(*JsonString(Params, TEXT("key"))));
			const FString Event = JsonString(Params, TEXT("event"), TEXT("pressed"));
			if (!Controller || !Key.IsValid()) return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_INPUT_INVALID"), TEXT("PIE PlayerController or input key is invalid."));
			const bool bHandled = Controller->InputKey(FInputKeyParams(Key, Event == TEXT("released") ? IE_Released : IE_Pressed, 1.0, false));
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetBoolField(TEXT("handled"), bHandled); Result->SetStringField(TEXT("key"), Key.ToString()); return SuccessResponse(RequestId, Result);
		}
		const FString ObjectPath = JsonString(Params, TEXT("object_path"));
		UObject* Object = FindObject<UObject>(nullptr, *ObjectPath);
		if (!Object || Object->GetWorld() != PIEWorld)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_OBJECT_NOT_FOUND"), TEXT("Runtime object was not found in the owned PIE world."));
		}
		if (Action == TEXT("read"))
		{
			const FString PropertyName = JsonString(Params, TEXT("property"));
			FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *PropertyName);
			if (!Property) return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_PROPERTY_NOT_FOUND"), TEXT("Runtime property was not found."));
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("object_path"), Object->GetPathName()); Result->SetStringField(TEXT("property"), PropertyName); Result->SetField(TEXT("value"), FUEPIPropertyCodec::ReadValue(Property, Property->ContainerPtrToValuePtr<void>(Object))); return SuccessResponse(RequestId, Result);
		}
		if (Action == TEXT("invoke"))
		{
			if (!Settings->bAllowRuntimeInvoke) return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_INVOKE_DISABLED"), TEXT("Runtime invoke is disabled."));
			const FString FunctionName = JsonString(Params, TEXT("function"));
			UFunction* Function = Object->FindFunction(FName(*FunctionName));
			if (!Function || !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable) || Function->HasAnyFunctionFlags(FUNC_Exec) || Function->HasMetaData(TEXT("Latent")) || Function->ParmsSize > 0)
			{
				return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_FUNCTION_NOT_ALLOWED"), TEXT("Runtime function must be allowlisted by BlueprintCallable semantics, non-Exec, non-Latent, and parameterless in P0."));
			}
			const FString FunctionKey = FString::Printf(TEXT("%s:%s"), Function->GetOwnerClass() ? *Function->GetOwnerClass()->GetPathName() : *Object->GetClass()->GetPathName(), *FunctionName);
			if (!Settings->AllowedRuntimeFunctions.Contains(FunctionKey)) return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_FUNCTION_NOT_ALLOWLISTED"), FString::Printf(TEXT("Runtime function is not in Project Settings allowlist: %s"), *FunctionKey));
			Object->ProcessEvent(Function, nullptr);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("object_path"), Object->GetPathName()); Result->SetStringField(TEXT("function"), Function->GetName()); Result->SetBoolField(TEXT("invoked"), true); return SuccessResponse(RequestId, Result);
		}
		return ErrorResponse(RequestId, TEXT("UEPI_RUNTIME_ACTION_UNSUPPORTED"), FString::Printf(TEXT("Runtime action is unsupported: %s"), *Action));
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::RefreshNowResult(const FString& RequestId, const TArray<FString>& Targets, const FString& DataMode) const
	{
		FString RequestPath;
		FString Error;
		if (!WriteRefreshRequest(Targets, DataMode, RequestPath, Error))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_BRIDGE_REFRESH_REQUEST_FAILED"), Error);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("request_path"), RequestPath);
		Result->SetStringField(TEXT("data_mode"), DataMode);
		Result->SetArrayField(TEXT("target_object_paths"), StringArrayToJsonValues(Targets));
		return SuccessResponse(RequestId, Result);
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::ViewportCaptureUnsupported(const FString& RequestId) const
	{
		if (!GEditor || !GEditor->GetActiveViewport())
		{
			return ErrorResponse(RequestId, TEXT("UEPI_VIEWPORT_CAPTURE_NO_ACTIVE_VIEWPORT"), TEXT("No active editor viewport is available for capture."));
		}

		FViewport* Viewport = GEditor->GetActiveViewport();
		const FIntPoint Size = Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_VIEWPORT_CAPTURE_BAD_SIZE"), TEXT("Active viewport has no readable size."));
		}

		TArray<FColor> Pixels;
		if (!Viewport->ReadPixels(Pixels) || Pixels.Num() != Size.X * Size.Y)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_VIEWPORT_CAPTURE_READ_FAILED"), TEXT("Failed to read pixels from the active editor viewport."));
		}
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}

		TArray64<uint8> PngData;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, MakeArrayView(Pixels), PngData);
		if (PngData.Num() <= 0)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_VIEWPORT_CAPTURE_ENCODE_FAILED"), TEXT("Failed to encode active viewport pixels as PNG."));
		}

		const FString ArtifactDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("artifacts"), TEXT("screenshots"));
		IFileManager::Get().MakeDirectory(*ArtifactDirectory, true);
		const FString CreatedAt = FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ"));
		const FString ArtifactPath = FPaths::Combine(ArtifactDirectory, FString::Printf(TEXT("viewport-%s.png"), *CreatedAt));
		if (!FFileHelper::SaveArrayToFile(PngData, *ArtifactPath))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_VIEWPORT_CAPTURE_SAVE_FAILED"), FString::Printf(TEXT("Failed to save viewport artifact: %s"), *ArtifactPath));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.viewport-capture.v1"));
		Result->SetStringField(TEXT("artifact_uri"), FString::Printf(TEXT("uepi://artifact/screenshots/%s"), *FPaths::GetCleanFilename(ArtifactPath)));
		Result->SetStringField(TEXT("artifact_path"), ArtifactPath);
		Result->SetStringField(TEXT("artifact_directory"), ArtifactDirectory);
		Result->SetStringField(TEXT("format"), TEXT("png"));
		Result->SetNumberField(TEXT("width"), Size.X);
		Result->SetNumberField(TEXT("height"), Size.Y);
		Result->SetNumberField(TEXT("byte_count"), static_cast<double>(PngData.Num()));
		return SuccessResponse(RequestId, Result);
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::EditDiscoverResult(const FString& RequestId) const
	{
		const UUEPISettings* Settings = GetDefault<UUEPISettings>();
		const bool bWriteEnabled = Settings && Settings->bEnableWriteTools;
		const bool bBlueprintApplyEnabled = bWriteEnabled && Settings->bAllowBlueprintEdits;
		const bool bActorApplyEnabled = bWriteEnabled && Settings->bAllowActorEdits;
		const bool bContentApplyEnabled = bWriteEnabled && Settings->bAllowContentEdits;
		const bool bMaterialApplyEnabled = bWriteEnabled && Settings->bAllowMaterialEdits;
		const bool bMaterialBlueprintApplyEnabled = bMaterialApplyEnabled && Settings->bAllowBlueprintEdits;
		const bool bUMGApplyEnabled = bWriteEnabled && Settings->bAllowUMGEdits;
#if UEPI_WITH_ENHANCED_INPUT
		const bool bInputApplyEnabled = bWriteEnabled && Settings->bAllowInputEdits;
#else
		const bool bInputApplyEnabled = false;
#endif

		FUEPIEditOperationRegistry& Registry = FUEPIEditOperationRegistry::Get();
		Registry.EnsureBuiltinsRegistered();
		TArray<TSharedPtr<FJsonValue>> Operations;
		for (const FUEPIEditOperationDescriptor& Descriptor : Registry.GetDescriptors())
		{
			const bool bDomainEnabled =
				(Descriptor.Domain == TEXT("blueprint") && bBlueprintApplyEnabled) ||
				(Descriptor.Domain == TEXT("actor") && bActorApplyEnabled) ||
				(Descriptor.Domain == TEXT("content") && bContentApplyEnabled) ||
				(Descriptor.Domain == TEXT("asset") && bContentApplyEnabled) ||
				(Descriptor.Domain == TEXT("material") && (Descriptor.Name == TEXT("material.apply_to_blueprint_component") ? bMaterialBlueprintApplyEnabled : bMaterialApplyEnabled)) ||
				(Descriptor.Domain == TEXT("umg") && bUMGApplyEnabled) ||
				(Descriptor.Domain == TEXT("input") && bInputApplyEnabled);
			const bool bApplySupported = bDomainEnabled || ((Descriptor.Domain == TEXT("animgraph") || Descriptor.Domain == TEXT("animation")) && bBlueprintApplyEnabled);
			TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
			Operation->SetStringField(TEXT("name"), Descriptor.Name);
			Operation->SetNumberField(TEXT("version"), Descriptor.Version);
			Operation->SetStringField(TEXT("domain"), Descriptor.Domain);
			Operation->SetStringField(TEXT("summary"), Descriptor.Summary);
			Operation->SetStringField(TEXT("risk"), Descriptor.Risk);
			Operation->SetStringField(TEXT("rollback_mode"), Descriptor.RollbackMode);
			Operation->SetStringField(TEXT("validation_mode"), Descriptor.ValidationMode);
			Operation->SetStringField(TEXT("validation_behavior"), Descriptor.ValidationMode);
			Operation->SetStringField(TEXT("rollback_behavior"), Descriptor.RollbackMode);
			Operation->SetStringField(TEXT("save_behavior"), Descriptor.SaveBehavior);
			Operation->SetStringField(TEXT("idempotency_behavior"), Descriptor.IdempotencyBehavior);
			Operation->SetStringField(TEXT("required_plugin"), Descriptor.RequiredPlugin);
			Operation->SetBoolField(TEXT("requires_save"), Descriptor.bRequiresSave);
			Operation->SetBoolField(TEXT("atomic_supported"), Descriptor.bAtomicSupported);
			Operation->SetBoolField(TEXT("preview_supported"), true);
			Operation->SetBoolField(TEXT("apply_supported"), bApplySupported);
			Operation->SetArrayField(TEXT("target_fields"), StringArrayToJsonValues(Descriptor.TargetFields));
			Operation->SetArrayField(TEXT("required_capabilities"), StringArrayToJsonValues(Descriptor.RequiredCapabilities));
			Operation->SetArrayField(TEXT("supported_engine_versions"), StringArrayToJsonValues(Descriptor.SupportedEngineVersions));
			Operation->SetArrayField(TEXT("supported_asset_classes"), StringArrayToJsonValues(Descriptor.SupportedAssetClasses));
			Operation->SetArrayField(TEXT("supported_graph_schemas"), StringArrayToJsonValues(Descriptor.SupportedGraphSchemas));
			TSharedRef<FJsonObject> InputSchema = MakeShared<FJsonObject>();
			InputSchema->SetStringField(TEXT("type"), TEXT("object"));
			TSharedRef<FJsonObject> InputProperties = MakeShared<FJsonObject>();
			for (const FString& TargetField : Descriptor.TargetFields)
			{
				TSharedRef<FJsonObject> TargetSchema = MakeShared<FJsonObject>();
				TargetSchema->SetArrayField(TEXT("type"), { MakeShared<FJsonValueString>(TEXT("string")), MakeShared<FJsonValueString>(TEXT("object")), MakeShared<FJsonValueString>(TEXT("array")) });
				InputProperties->SetObjectField(TargetField, TargetSchema);
			}
			InputSchema->SetObjectField(TEXT("properties"), InputProperties);
			InputSchema->SetBoolField(TEXT("additionalProperties"), true);
			Operation->SetObjectField(TEXT("input_schema"), InputSchema);
			TSharedRef<FJsonObject> OutputSchema = MakeShared<FJsonObject>();
			OutputSchema->SetStringField(TEXT("type"), TEXT("object"));
			Operation->SetObjectField(TEXT("output_schema"), OutputSchema);
			Operation->SetArrayField(TEXT("examples"), EmptyJsonArray());
			TSharedRef<FJsonObject> Availability = MakeShared<FJsonObject>();
			Availability->SetBoolField(TEXT("preview"), true);
			Availability->SetBoolField(TEXT("apply"), bApplySupported);
			Availability->SetStringField(TEXT("reason"), bApplySupported ? TEXT("available") : TEXT("disabled_by_project_capability"));
			Operation->SetObjectField(TEXT("availability"), Availability);
			Operations.Add(MakeShared<FJsonValueObject>(Operation));
		}

		TSharedRef<FJsonObject> SettingsObject = MakeShared<FJsonObject>();
		SettingsObject->SetBoolField(TEXT("write_tools_enabled"), bWriteEnabled);
		SettingsObject->SetBoolField(TEXT("blueprint_edits_enabled"), Settings && Settings->bAllowBlueprintEdits);
		SettingsObject->SetBoolField(TEXT("actor_edits_enabled"), Settings && Settings->bAllowActorEdits);
		SettingsObject->SetBoolField(TEXT("content_edits_enabled"), Settings && Settings->bAllowContentEdits);
		SettingsObject->SetBoolField(TEXT("material_edits_enabled"), Settings && Settings->bAllowMaterialEdits);
		SettingsObject->SetBoolField(TEXT("umg_edits_enabled"), Settings && Settings->bAllowUMGEdits);
		SettingsObject->SetBoolField(TEXT("input_edits_enabled"), Settings && Settings->bAllowInputEdits);
		SettingsObject->SetBoolField(TEXT("enhanced_input_compiled"), UEPI_WITH_ENHANCED_INPUT != 0);
		SettingsObject->SetBoolField(TEXT("saving_enabled"), Settings && Settings->bAllowSavingPackages);
		SettingsObject->SetNumberField(TEXT("max_operations_per_transaction"), Settings ? Settings->MaxWriteOperationsPerTransaction : 0);
		SettingsObject->SetNumberField(TEXT("max_assets_per_transaction"), Settings ? Settings->MaxWriteAssetsPerTransaction : 0);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.bridge-edit-discover.v2"));
		Result->SetStringField(TEXT("catalog_version"), TEXT("2.0.0"));
		Result->SetStringField(TEXT("catalog_hash"), Registry.GetCatalogHash());
		Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Result->SetStringField(TEXT("plugin_build_id"), TEXT("uepi-vnext"));
		Result->SetObjectField(TEXT("settings"), SettingsObject);
		Result->SetArrayField(TEXT("operations"), Operations);
		Result->SetArrayField(TEXT("capabilities"), StringArrayToJsonValues(FUEPIBridgeProtocol::WriteCapabilities()));
		return SuccessResponse(RequestId, Result);
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::EditApplyResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params)
	{
		const UUEPISettings* Settings = GetDefault<UUEPISettings>();
		if (!Settings || !Settings->bEnableWriteTools)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_WRITE_DISABLED"), TEXT("UEPI write tools are disabled in Project Settings."));
		}
		if (!GEditor)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_EDITOR_UNAVAILABLE"), TEXT("GEditor is not available."));
		}
		if (GEditor->PlayWorld)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_BLOCKED_DURING_PIE"), TEXT("UEPI write tools do not run while PIE is active."));
		}
		if (!JsonBool(Params, TEXT("approved"), false))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_APPROVAL_REQUIRED"), TEXT("edit.apply requires approved=true after user review."));
		}
		if (JsonBool(Params, TEXT("save"), false) && !Settings->bAllowSavingPackages)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_SAVE_DISABLED"), TEXT("Saving packages is disabled for write alpha."));
		}

		const FString TransactionId = JsonString(Params, TEXT("transaction_id"));
		const TSharedPtr<FJsonObject> Plan = JsonObjectField(Params, TEXT("plan"));
		if (!Plan.IsValid() || JsonString(Plan, TEXT("schema_version")) != TEXT("uepi.edit_plan.v2"))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_PLAN_VERSION_UNSUPPORTED"), TEXT("Apply requires an immutable uepi.edit_plan.v2 generated by the current Preview."));
		}
		if (JsonString(Params, TEXT("plan_hash")) != JsonString(Plan, TEXT("plan_hash")) || JsonString(Params, TEXT("approval_nonce")) != JsonString(Plan, TEXT("approval_nonce")))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_APPROVAL_MISMATCH"), TEXT("Apply approval identity does not match the immutable Preview plan."));
		}
		if (JsonString(Plan, TEXT("project_binding_id")) != UEPIProjectBindingId())
		{
			return ErrorResponse(RequestId, TEXT("UEPI_PROJECT_BINDING_MISMATCH"), TEXT("Edit plan belongs to a different project binding."));
		}
		if (JsonString(Plan, TEXT("editor_session_id")) != SessionId)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDITOR_SESSION_MISMATCH"), TEXT("Edit plan belongs to a different Editor session."));
		}
		FDateTime ExpiresAt;
		if (!FDateTime::ParseIso8601(*JsonString(Plan, TEXT("expires_at")), ExpiresAt) || ExpiresAt < FDateTime::UtcNow())
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_PLAN_EXPIRED"), TEXT("Edit plan approval window has expired; run Preview again."));
		}
		FUEPIEditOperationRegistry& Registry = FUEPIEditOperationRegistry::Get();
		Registry.EnsureBuiltinsRegistered();
		if (JsonString(Plan, TEXT("catalog_hash")) != Registry.GetCatalogHash())
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_CATALOG_STALE"), TEXT("Operation catalog changed after Preview; run Preview again."));
		}
		const TArray<TSharedPtr<FJsonValue>>* Operations = Plan.IsValid() ? JsonArray(Plan, TEXT("operations")) : JsonArray(Params, TEXT("operations"));
		if (!Operations)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_PLAN_MISSING"), TEXT("edit.apply requires a plan with operations."));
		}
		if (Operations->Num() <= 0)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_PLAN_EMPTY"), TEXT("edit.apply received an empty operation plan."));
		}
		if (Operations->Num() > Settings->MaxWriteOperationsPerTransaction)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_TOO_MANY_OPERATIONS"), FString::Printf(TEXT("Operation count %d exceeds MaxWriteOperationsPerTransaction=%d."), Operations->Num(), Settings->MaxWriteOperationsPerTransaction));
		}

		TArray<FString> AffectedAssets;
		if (Plan.IsValid())
		{
			if (const TArray<TSharedPtr<FJsonValue>>* AssetValues = JsonArray(Plan, TEXT("affected_assets")))
			{
				for (const TSharedPtr<FJsonValue>& Value : *AssetValues)
				{
					FString AssetPath;
					if (Value.IsValid() && Value->TryGetString(AssetPath) && !AssetPath.IsEmpty())
					{
						AffectedAssets.AddUnique(NormalizeBlueprintObjectPath(AssetPath));
					}
				}
			}
		}
		if (AffectedAssets.Num() > Settings->MaxWriteAssetsPerTransaction)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_TOO_MANY_ASSETS"), FString::Printf(TEXT("Affected asset count %d exceeds MaxWriteAssetsPerTransaction=%d before modification."), AffectedAssets.Num(), Settings->MaxWriteAssetsPerTransaction));
		}
		TMap<FString, UClass*> PlannedAssetClasses;
		TMap<FString, FString> PlannedAssetPaths;
		TMap<FString, USkeleton*> PlannedMontageSkeletons;
		TMap<USkeleton*, TSet<FName>> PlannedSkeletonSlots;
		for (int32 Index = 0; Index < Operations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Operation = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
			const FString Type = JsonString(Operation, TEXT("type"), JsonString(Operation, TEXT("operation")));
			if (!Operation.IsValid() || Type.IsEmpty() || !Registry.FindOperation(Type).IsValid())
			{
				return ErrorResponse(RequestId, TEXT("UEPI_EDIT_OPERATION_UNSUPPORTED"), FString::Printf(TEXT("Operation %d is not present in the active Operation Registry: %s"), Index, *Type));
			}
			TSharedPtr<FJsonObject> OpParams = JsonObjectField(Operation, TEXT("params"));
			if (!OpParams.IsValid()) OpParams = Operation;
			if (Type.StartsWith(TEXT("actor.")))
			{
				FUEPIEditContext Context;
				Context.TransactionId = TransactionId;
				Context.ProjectId = UEPIProjectBindingId();
				Context.AssetAllowList = AffectedAssets;
				Context.bDryRun = true;
				const FUEPIEditResult Preflight = Registry.FindOperation(Type)->Preview(Context, *OpParams);
				if (!Preflight.bOk)
				{
					return ErrorResponse(RequestId, Preflight.ErrorCode.IsEmpty() ? TEXT("UEPI_EDIT_PRECHECK_FAILED") : Preflight.ErrorCode, Preflight.Message);
				}
				continue;
			}
			if (Type == TEXT("asset.set_properties"))
			{
				const FString AssetPath = ResolveOperationAsset(OpParams, TEXT("asset"), PlannedAssetPaths);
				UObject* Object = AssetPath.IsEmpty() ? nullptr : StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
				UObject* Probe = Object ? DuplicateObject(Object, GetTransientPackage()) : nullptr;
				if (!Probe)
				{
					const TSharedPtr<FJsonObject> Reference = JsonObjectField(OpParams, TEXT("asset"));
					FString Ref = JsonString(Reference, TEXT("$ref"));
					int32 Fragment = INDEX_NONE;
					if (Ref.FindChar(TEXT('#'), Fragment)) Ref = Ref.Left(Fragment);
					if (UClass* const* PlannedClass = PlannedAssetClasses.Find(Ref))
					{
						Probe = NewObject<UObject>(GetTransientPackage(), *PlannedClass);
					}
				}
				FString WriteError;
				const TSharedPtr<FJsonObject> Properties = PropertyWritesObject(OpParams, WriteError);
				if (!Probe || !Properties.IsValid())
				{
					return ErrorResponse(RequestId, TEXT("UEPI_EDIT_PROPERTY_PREFLIGHT_FAILED"), WriteError.IsEmpty() ? TEXT("asset.set_properties requires a valid asset/reference and property writes.") : WriteError);
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
				{
					TSharedPtr<FJsonValue> Before;
					TSharedPtr<FJsonValue> After;
					FString Error;
					if (!FUEPIPropertyCodec::SetPropertyPath(Probe, Pair.Key, PropertyWriteValue(Pair.Value), Before, After, Error, PropertyWriteMode(Pair.Value)))
					{
						return ErrorResponse(RequestId, TEXT("UEPI_EDIT_PROPERTY_TYPE_MISMATCH"), FString::Printf(TEXT("Property preflight failed for %s: %s"), *Pair.Key, *Error));
					}
				}
			}
			else if (Type == TEXT("content.create_asset"))
			{
				const FString ClassPath = JsonString(OpParams, TEXT("asset_class"), JsonString(OpParams, TEXT("class_path")));
				UClass* AssetClass = LoadObject<UClass>(nullptr, *ClassPath);
				if (!AssetClass || !AssetClass->IsChildOf(UDataAsset::StaticClass()) || AssetClass->HasAnyClassFlags(CLASS_Abstract))
				{
					return ErrorResponse(RequestId, TEXT("UEPI_EDIT_ASSET_CLASS_UNSUPPORTED"), FString::Printf(TEXT("P0 content.create_asset requires a UDataAsset/UPrimaryDataAsset class: %s"), *ClassPath));
				}
				FString PackagePath;
				FString AssetName;
				FString DestinationError;
				if (!SplitDestinationPath(OpParams, TEXT("DA_UEPIAsset"), PackagePath, AssetName, DestinationError))
				{
					return ErrorResponse(RequestId, TEXT("UEPI_EDIT_DESTINATION_INVALID"), DestinationError);
				}
				const FString OpId = OperationId(Operation);
				if (!OpId.IsEmpty())
				{
					PlannedAssetClasses.Add(OpId, AssetClass);
					PlannedAssetPaths.Add(OpId, FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName));
				}
			}
			else if (Type == TEXT("animation.register_slot") || Type == TEXT("animation.create_slot_group"))
			{
				USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *JsonString(OpParams, TEXT("skeleton")));
				const FName SlotName(*JsonString(OpParams, TEXT("slot_name"), TEXT("DefaultSlot")));
				if (!Skeleton || SlotName.IsNone()) return ErrorResponse(RequestId, TEXT("UEPI_EDIT_ANIMATION_SLOT_INVALID"), TEXT("A valid skeleton and slot_name are required."));
				PlannedSkeletonSlots.FindOrAdd(Skeleton).Add(SlotName);
			}
			else if (Type == TEXT("animation.create_montage_from_sequence"))
			{
				UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *JsonString(OpParams, TEXT("sequence")));
				FString PackagePath; FString AssetName; FString DestinationError;
				const FName SlotName(*JsonString(OpParams, TEXT("slot_name"), TEXT("DefaultSlot")));
				if (!Sequence || !Sequence->GetSkeleton() || SlotName.IsNone() || !SplitDestinationPath(OpParams, TEXT("AM_UEPIMontage"), PackagePath, AssetName, DestinationError))
				{
					return ErrorResponse(RequestId, TEXT("UEPI_EDIT_MONTAGE_PREFLIGHT_FAILED"), DestinationError.IsEmpty() ? TEXT("A valid sequence, skeleton, slot, and destination are required.") : DestinationError);
				}
				const bool bSlotExists = Sequence->GetSkeleton()->ContainsSlotName(SlotName) || PlannedSkeletonSlots.FindOrAdd(Sequence->GetSkeleton()).Contains(SlotName);
				if (!bSlotExists) return ErrorResponse(RequestId, TEXT("UEPI_EDIT_ANIMATION_SLOT_MISSING"), FString::Printf(TEXT("Montage slot is not registered: %s"), *SlotName.ToString()));
				const FString OpId = OperationId(Operation);
				if (!OpId.IsEmpty())
				{
					PlannedAssetClasses.Add(OpId, UAnimMontage::StaticClass());
					PlannedAssetPaths.Add(OpId, FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName));
					PlannedMontageSkeletons.Add(OpId, Sequence->GetSkeleton());
				}
			}
			else if (Type == TEXT("animation.add_montage_slot_track") || Type == TEXT("animation.add_montage_segment") || Type == TEXT("animation.add_montage_section") || Type == TEXT("animation.set_montage_blend") || Type == TEXT("animation.set_preview_mesh"))
			{
				const FString MontagePath = ResolveOperationAsset(OpParams, TEXT("asset"), PlannedAssetPaths);
				UAnimMontage* Montage = MontagePath.IsEmpty() ? nullptr : LoadObject<UAnimMontage>(nullptr, *MontagePath);
				USkeleton* Skeleton = Montage ? Montage->GetSkeleton() : nullptr;
				if (!Skeleton)
				{
					const TSharedPtr<FJsonObject> Reference = JsonObjectField(OpParams, TEXT("asset")); FString Ref = JsonString(Reference, TEXT("$ref")); int32 Fragment = INDEX_NONE; if (Ref.FindChar(TEXT('#'), Fragment)) Ref = Ref.Left(Fragment);
					if (USkeleton* const* PlannedSkeleton = PlannedMontageSkeletons.Find(Ref)) Skeleton = *PlannedSkeleton;
				}
				if (!Skeleton) return ErrorResponse(RequestId, TEXT("UEPI_EDIT_MONTAGE_PREFLIGHT_FAILED"), TEXT("Montage target or prior Montage reference was not found."));
				if (Type == TEXT("animation.add_montage_slot_track"))
				{
					const FName SlotName(*JsonString(OpParams, TEXT("slot_name"), TEXT("DefaultSlot")));
					if (!Skeleton->ContainsSlotName(SlotName) && !PlannedSkeletonSlots.FindOrAdd(Skeleton).Contains(SlotName)) return ErrorResponse(RequestId, TEXT("UEPI_EDIT_ANIMATION_SLOT_MISSING"), TEXT("Requested Montage slot is not registered."));
				}
				else if (Type == TEXT("animation.add_montage_segment"))
				{
					UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *JsonString(OpParams, TEXT("sequence")));
					if (!Sequence || Sequence->GetSkeleton() != Skeleton) return ErrorResponse(RequestId, TEXT("UEPI_EDIT_ANIMATION_SKELETON_MISMATCH"), TEXT("Montage segment sequence has an incompatible skeleton."));
				}
				else if (Type == TEXT("animation.add_montage_section") && JsonString(OpParams, TEXT("section_name"), JsonString(OpParams, TEXT("name"))).IsEmpty())
				{
					return ErrorResponse(RequestId, TEXT("UEPI_EDIT_MONTAGE_SECTION_INVALID"), TEXT("Montage section requires a name."));
				}
				else if (Type == TEXT("animation.set_preview_mesh"))
				{
					USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *JsonString(OpParams, TEXT("preview_mesh")));
					if (!Mesh || Mesh->GetSkeleton() != Skeleton) return ErrorResponse(RequestId, TEXT("UEPI_EDIT_ANIMATION_SKELETON_MISMATCH"), TEXT("Preview mesh has an incompatible skeleton."));
				}
			}
		}
		for (const FString& AssetPath : AffectedAssets)
		{
			if (!AssetPath.StartsWith(TEXT("/")))
			{
				continue;
			}
			const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
			if (UPackage* ExistingPackage = FindPackage(nullptr, *PackageName))
			{
				if (ExistingPackage->IsDirty())
				{
					return ErrorResponse(RequestId, TEXT("UEPI_EDIT_TARGET_DIRTY"), FString::Printf(TEXT("Target package already has user changes: %s"), *PackageName));
				}
			}
			const FString PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
			if (IFileManager::Get().FileExists(*PackageFile) && IFileManager::Get().IsReadOnly(*PackageFile))
			{
				return ErrorResponse(RequestId, TEXT("UEPI_EDIT_TARGET_READ_ONLY"), FString::Printf(TEXT("Target package file is read-only: %s"), *PackageFile));
			}
		}
		if (const TArray<TSharedPtr<FJsonValue>>* BeforeFingerprints = JsonArray(Plan, TEXT("before_fingerprints")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *BeforeFingerprints)
			{
				const TSharedPtr<FJsonObject> Fingerprint = Value.IsValid() ? Value->AsObject() : nullptr;
				const FString AssetPath = JsonString(Fingerprint, TEXT("asset"));
				if (!Fingerprint.IsValid() || AssetPath.IsEmpty() || !AssetPath.StartsWith(TEXT("/")))
				{
					continue;
				}
				const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
				const FString PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
				const bool bExpectedExists = JsonBool(Fingerprint, TEXT("exists"), false);
				const bool bExists = IFileManager::Get().FileExists(*PackageFile);
				const FString ExpectedSha256 = JsonString(Fingerprint, TEXT("sha256"));
				if (bExists != bExpectedExists || (bExists && !ExpectedSha256.IsEmpty() && UEPIFileSha256(PackageFile) != ExpectedSha256))
				{
					return ErrorResponse(RequestId, TEXT("UEPI_EDIT_BEFORE_FINGERPRINT_CHANGED"), FString::Printf(TEXT("Target package changed after Preview: %s"), *AssetPath));
				}
			}
		}
		TMap<FString, FString> TransactionBackupFiles;
		FString BackupDirectory;
		FString ServiceError;
		if (!FUEPIBackupService::Create(TransactionId, AffectedAssets, TransactionBackupFiles, BackupDirectory, ServiceError))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_BACKUP_FAILED"), ServiceError);
		}
		FString JournalPath;
		if (!FUEPITransactionJournal::Write(TransactionId, TEXT("prepared"), AffectedAssets, TransactionBackupFiles, false, TEXT("Preflight passed and package backups were created."), JournalPath, ServiceError))
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_JOURNAL_FAILED"), ServiceError);
		}

		TArray<TSharedPtr<FJsonValue>> OperationResults;
		TArray<TSharedPtr<FJsonValue>> CompileResults;
		bool bAllOk = true;
		bool bValidationOk = true;
		bool bMutated = false;
		FString FailureMessage;
		TSet<UBlueprint*> TouchedBlueprints;
		TMap<FString, UEdGraphNode*> NodeRefs;
		TMap<FString, FString> OperationAssets;

		const FText TransactionText = FText::FromString(TransactionId.IsEmpty() ? FString(TEXT("UEPI edit transaction")) : FString::Printf(TEXT("UEPI edit %s"), *TransactionId));
		FScopedTransaction Transaction(TEXT("UEProjectIntelligence"), TransactionText, nullptr, true);
		for (int32 Index = 0; Index < Operations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Operation = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
			FString Type = JsonString(Operation, TEXT("type"), JsonString(Operation, TEXT("operation")));
			TSharedPtr<FJsonObject> OpParams = JsonObjectField(Operation, TEXT("params"));
			if (!OpParams.IsValid())
			{
				OpParams = Operation;
			}
			if (Type.StartsWith(TEXT("actor.")))
			{
				FUEPIEditContext Context;
				Context.TransactionId = TransactionId;
				Context.ProjectId = UEPIProjectBindingId();
				Context.AssetAllowList = AffectedAssets;
				Context.bDryRun = false;
				Context.bAllowSave = JsonBool(Params, TEXT("save"), false);
				const FUEPIEditResult OperationResult = Registry.FindOperation(Type)->Apply(Context, *OpParams);
				AddOperationResult(OperationResults, Index, Type, OperationResult.bOk, OperationResult.Message, OperationResult.Result);
				if (!OperationResult.bOk)
				{
					bAllOk = false;
					FailureMessage = OperationResult.Message;
					break;
				}
				bMutated = true;
				continue;
			}
			if (Type == TEXT("blueprint.add_node"))
			{
				const FString Kind = JsonString(OpParams, TEXT("kind"));
				if (Kind == TEXT("custom_event")) { Type = TEXT("blueprint.add_event_node"); OpParams->SetStringField(TEXT("event_kind"), TEXT("custom_event")); }
				else if (Kind == TEXT("function_call")) Type = TEXT("blueprint.add_function_call_node");
				else if (Kind == TEXT("variable_get")) Type = TEXT("blueprint.add_variable_get_node");
				else if (Kind == TEXT("variable_set")) Type = TEXT("blueprint.add_variable_set_node");
				else if (Kind == TEXT("branch")) Type = TEXT("blueprint.add_branch_node");
				else if (Kind == TEXT("print_string")) Type = TEXT("blueprint.add_print_string_node");
				else if (Kind != TEXT("make_struct") && Kind != TEXT("input_key"))
				{
					bAllOk = false; FailureMessage = FString::Printf(TEXT("Unsupported blueprint.add_node kind: %s"), *Kind); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break;
				}
			}
			else if (Type == TEXT("animation.register_slot"))
			{
				Type = TEXT("animation.create_slot_group");
			}
			else if (Type == TEXT("animgraph.add_slot"))
			{
				Type = TEXT("animgraph.add_slot_node");
			}
			else if (Type == TEXT("animgraph.connect_pose") || Type == TEXT("animgraph.connect_pose_pins"))
			{
				if (const TSharedPtr<FJsonObject> From = JsonObjectField(OpParams, TEXT("from"))) OpParams->SetObjectField(TEXT("source"), From.ToSharedRef());
				if (const TSharedPtr<FJsonObject> To = JsonObjectField(OpParams, TEXT("to"))) OpParams->SetObjectField(TEXT("target"), To.ToSharedRef());
				Type = TEXT("blueprint.connect_pins");
			}
			else if (Type == TEXT("animgraph.disconnect_pose_pins")) Type = TEXT("blueprint.disconnect_pins");
			else if (Type == TEXT("animgraph.remove_node")) Type = TEXT("blueprint.remove_node");
			else if (Type == TEXT("animgraph.compile")) Type = TEXT("blueprint.compile");
			else if (Type == TEXT("widget.add_widget"))
			{
				const FString WidgetClass = JsonString(OpParams, TEXT("widget_class"), JsonString(OpParams, TEXT("kind")));
				if (WidgetClass.Contains(TEXT("TextBlock"), ESearchCase::IgnoreCase) || WidgetClass.Equals(TEXT("text"), ESearchCase::IgnoreCase)) Type = TEXT("widget.add_text");
				else if (WidgetClass.Contains(TEXT("Button"), ESearchCase::IgnoreCase) || WidgetClass.Equals(TEXT("button"), ESearchCase::IgnoreCase)) Type = TEXT("widget.add_button");
			}

			if (!Operation.IsValid() || Type.IsEmpty())
			{
				bAllOk = false;
				FailureMessage = TEXT("Operation is not a JSON object with a type.");
				AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
				break;
			}

			FString AssetPath;
			FString Error;
			UBlueprint* Blueprint = nullptr;
			if (Type == TEXT("content.create_asset"))
			{
				if (!Settings->bAllowContentEdits)
				{
					bAllOk = false; FailureMessage = TEXT("Content edits are disabled."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break;
				}
				FString PackagePath;
				FString AssetName;
				if (!SplitDestinationPath(OpParams, TEXT("DA_UEPIAsset"), PackagePath, AssetName, Error))
				{
					bAllOk = false; FailureMessage = Error; AddOperationResult(OperationResults, Index, Type, false, Error); break;
				}
				UClass* AssetClass = LoadObject<UClass>(nullptr, *JsonString(OpParams, TEXT("asset_class"), JsonString(OpParams, TEXT("class_path"))));
				UDataAssetFactory* Factory = NewObject<UDataAssetFactory>();
				Factory->DataAssetClass = AssetClass;
				UObject* NewAsset = FAssetToolsModule::GetModule().Get().CreateAsset(AssetName, PackagePath, AssetClass, Factory, TEXT("UEPI"));
				if (!NewAsset)
				{
					bAllOk = false; FailureMessage = TEXT("DataAsset creation failed."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break;
				}
				NewAsset->MarkPackageDirty();
				AffectedAssets.AddUnique(NewAsset->GetPathName());
				if (const FString OpId = OperationId(Operation); !OpId.IsEmpty()) OperationAssets.Add(OpId, NewAsset->GetPathName());
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetObjectField(TEXT("asset"), ObjectToJson(NewAsset));
				Detail->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
				AddOperationResult(OperationResults, Index, Type, true, TEXT("DataAsset created."), Detail);
				bMutated = true;
				continue;
			}
			if (Type == TEXT("content.save_assets"))
			{
				for (const FString& ExplicitAsset : JsonStringArray(OpParams, TEXT("assets"))) AffectedAssets.AddUnique(ExplicitAsset);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Touched assets are scheduled for save after validation."));
				continue;
			}
			if (Type == TEXT("asset.set_properties"))
			{
				if (!Settings->bAllowContentEdits)
				{
					bAllOk = false; FailureMessage = TEXT("Content edits are disabled."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break;
				}
				AssetPath = ResolveOperationAsset(OpParams, TEXT("asset"), OperationAssets);
				UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
				const TSharedPtr<FJsonObject> Properties = PropertyWritesObject(OpParams, Error);
				if (!Object || !Properties.IsValid())
				{
					bAllOk = false; FailureMessage = TEXT("Asset or properties object is invalid."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break;
				}
				Object->Modify();
				TArray<TSharedPtr<FJsonValue>> PropertyDiffs;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
				{
					TSharedPtr<FJsonValue> Before;
					TSharedPtr<FJsonValue> After;
					if (!FUEPIPropertyCodec::SetPropertyPath(Object, Pair.Key, PropertyWriteValue(Pair.Value), Before, After, Error, PropertyWriteMode(Pair.Value)))
					{
						bAllOk = false; FailureMessage = Error; AddOperationResult(OperationResults, Index, Type, false, Error); break;
					}
					TSharedRef<FJsonObject> Diff = MakeShared<FJsonObject>(); Diff->SetStringField(TEXT("property_path"), Pair.Key); Diff->SetField(TEXT("before"), Before); Diff->SetField(TEXT("after"), After); PropertyDiffs.Add(MakeShared<FJsonValueObject>(Diff));
				}
				if (!bAllOk) break;
				Object->PostEditChange(); Object->MarkPackageDirty(); AffectedAssets.AddUnique(Object->GetPathName());
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("asset"), Object->GetPathName()); Detail->SetArrayField(TEXT("property_diff"), PropertyDiffs);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Typed properties updated."), Detail); bMutated = true; continue;
			}
			if (Type == TEXT("animation.create_slot_group"))
			{
				USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *JsonString(OpParams, TEXT("skeleton")));
				const FName GroupName(*JsonString(OpParams, TEXT("group_name"), TEXT("DefaultGroup")));
				const FName SlotName(*JsonString(OpParams, TEXT("slot_name"), TEXT("DefaultSlot")));
				if (!Skeleton || SlotName.IsNone() || GroupName.IsNone())
				{
					bAllOk = false; FailureMessage = TEXT("Valid skeleton, group_name, and slot_name are required."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break;
				}
				Skeleton->Modify(); Skeleton->AddSlotGroupName(GroupName); Skeleton->RegisterSlotNode(SlotName); Skeleton->SetSlotGroupName(SlotName, GroupName); Skeleton->PostEditChange(); Skeleton->MarkPackageDirty();
				AffectedAssets.AddUnique(Skeleton->GetPathName());
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("skeleton"), Skeleton->GetPathName()); Detail->SetStringField(TEXT("group_name"), GroupName.ToString()); Detail->SetStringField(TEXT("slot_name"), SlotName.ToString());
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Skeleton slot group registered."), Detail); bMutated = true; continue;
			}
			if (Type == TEXT("animation.create_montage_from_sequence"))
			{
				UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *JsonString(OpParams, TEXT("sequence")));
				FString PackagePath; FString AssetName;
				if (!Sequence || !Sequence->GetSkeleton() || !SplitDestinationPath(OpParams, TEXT("AM_UEPIMontage"), PackagePath, AssetName, Error))
				{
					bAllOk = false; FailureMessage = Error.IsEmpty() ? TEXT("A valid sequence, skeleton, destination_path, and name are required.") : Error; AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break;
				}
				const FName SlotName(*JsonString(OpParams, TEXT("slot_name"), TEXT("DefaultSlot")));
				if (!Sequence->GetSkeleton()->ContainsSlotName(SlotName))
				{
					bAllOk = false; FailureMessage = FString::Printf(TEXT("Sequence skeleton does not contain slot: %s"), *SlotName.ToString()); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break;
				}
				UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>(); Factory->TargetSkeleton = Sequence->GetSkeleton(); Factory->SourceAnimation = Sequence;
				UAnimMontage* Montage = Cast<UAnimMontage>(FAssetToolsModule::GetModule().Get().CreateAsset(AssetName, PackagePath, UAnimMontage::StaticClass(), Factory, TEXT("UEPI")));
				if (!Montage) { bAllOk = false; FailureMessage = TEXT("AnimMontage creation failed."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break; }
				Montage->Modify();
				if (Montage->SlotAnimTracks.Num() == 0) Montage->AddSlot(SlotName); else Montage->SlotAnimTracks[0].SlotName = SlotName;
				Montage->PostEditChange(); Montage->MarkPackageDirty(); AffectedAssets.AddUnique(Montage->GetPathName());
				if (const FString OpId = OperationId(Operation); !OpId.IsEmpty()) OperationAssets.Add(OpId, Montage->GetPathName());
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("asset_path"), Montage->GetPathName()); Detail->SetStringField(TEXT("sequence"), Sequence->GetPathName()); Detail->SetStringField(TEXT("slot_name"), SlotName.ToString());
				AddOperationResult(OperationResults, Index, Type, true, TEXT("AnimMontage created from sequence."), Detail); bMutated = true; continue;
			}
			if (Type == TEXT("animation.add_montage_slot_track") || Type == TEXT("animation.add_montage_segment") || Type == TEXT("animation.add_montage_section") || Type == TEXT("animation.set_montage_blend") || Type == TEXT("animation.set_preview_mesh"))
			{
				const FString MontagePath = ResolveOperationAsset(OpParams, TEXT("asset"), OperationAssets);
				UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *MontagePath);
				if (!Montage) { bAllOk = false; FailureMessage = FString::Printf(TEXT("AnimMontage was not found: %s"), *MontagePath); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break; }
				Montage->Modify();
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("asset"), Montage->GetPathName());
				if (Type == TEXT("animation.add_montage_slot_track"))
				{
					const FName SlotName(*JsonString(OpParams, TEXT("slot_name"), TEXT("DefaultSlot")));
					if (!Montage->GetSkeleton() || !Montage->GetSkeleton()->ContainsSlotName(SlotName)) { bAllOk = false; FailureMessage = TEXT("Montage skeleton does not contain the requested slot."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break; }
					const bool bExists = Montage->SlotAnimTracks.ContainsByPredicate([SlotName](const FSlotAnimationTrack& Track) { return Track.SlotName == SlotName; });
					if (!bExists) Montage->AddSlot(SlotName);
					Detail->SetStringField(TEXT("slot_name"), SlotName.ToString()); Detail->SetBoolField(TEXT("already_exists"), bExists);
				}
				else if (Type == TEXT("animation.add_montage_segment"))
				{
					UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *JsonString(OpParams, TEXT("sequence")));
					const FName SlotName(*JsonString(OpParams, TEXT("slot_name"), TEXT("DefaultSlot")));
					FSlotAnimationTrack* Track = Montage->SlotAnimTracks.FindByPredicate([SlotName](const FSlotAnimationTrack& Candidate) { return Candidate.SlotName == SlotName; });
					if (!Sequence || Sequence->GetSkeleton() != Montage->GetSkeleton() || !Track) { bAllOk = false; FailureMessage = TEXT("Sequence skeleton or Montage slot track is incompatible."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break; }
					FAnimSegment Segment; Segment.SetAnimReference(Sequence, true); Segment.StartPos = static_cast<float>(JsonInt(OpParams, TEXT("start_position_ms"), 0)) / 1000.0f;
					double Number = 0.0; if (OpParams->TryGetNumberField(TEXT("start_position"), Number)) Segment.StartPos = static_cast<float>(Number);
					if (OpParams->TryGetNumberField(TEXT("anim_start_time"), Number)) Segment.AnimStartTime = static_cast<float>(Number);
					if (OpParams->TryGetNumberField(TEXT("anim_end_time"), Number)) Segment.AnimEndTime = static_cast<float>(Number);
					if (OpParams->TryGetNumberField(TEXT("play_rate"), Number)) Segment.AnimPlayRate = static_cast<float>(Number);
					Segment.LoopingCount = FMath::Max(1, JsonInt(OpParams, TEXT("loop_count"), 1)); Track->AnimTrack.AnimSegments.Add(Segment);
					Montage->SetCompositeLength(FMath::Max(Montage->GetPlayLength(), Segment.GetEndPos())); Detail->SetStringField(TEXT("sequence"), Sequence->GetPathName()); Detail->SetStringField(TEXT("slot_name"), SlotName.ToString());
				}
				else if (Type == TEXT("animation.add_montage_section"))
				{
					const FName SectionName(*JsonString(OpParams, TEXT("section_name"), JsonString(OpParams, TEXT("name")))); double StartTime = 0.0; OpParams->TryGetNumberField(TEXT("start_time"), StartTime);
					if (SectionName.IsNone() || Montage->GetSectionIndex(SectionName) != INDEX_NONE) { bAllOk = false; FailureMessage = TEXT("Montage section name is empty or already exists."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break; }
					Montage->AddAnimCompositeSection(SectionName, static_cast<float>(StartTime)); Detail->SetStringField(TEXT("section_name"), SectionName.ToString()); Detail->SetNumberField(TEXT("start_time"), StartTime);
				}
				else if (Type == TEXT("animation.set_montage_blend"))
				{
					double Number = 0.0; if (OpParams->TryGetNumberField(TEXT("blend_in_time"), Number)) Montage->BlendIn.SetBlendTime(static_cast<float>(Number)); if (OpParams->TryGetNumberField(TEXT("blend_out_time"), Number)) Montage->BlendOut.SetBlendTime(static_cast<float>(Number)); if (OpParams->TryGetNumberField(TEXT("blend_out_trigger_time"), Number)) Montage->BlendOutTriggerTime = static_cast<float>(Number);
					Detail->SetNumberField(TEXT("blend_in_time"), Montage->GetDefaultBlendInTime()); Detail->SetNumberField(TEXT("blend_out_time"), Montage->GetDefaultBlendOutTime());
				}
				else
				{
					USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *JsonString(OpParams, TEXT("preview_mesh")));
					if (!Mesh || Mesh->GetSkeleton() != Montage->GetSkeleton()) { bAllOk = false; FailureMessage = TEXT("Preview mesh is missing or skeleton-incompatible."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break; }
					Montage->SetPreviewMesh(Mesh); Detail->SetStringField(TEXT("preview_mesh"), Mesh->GetPathName());
				}
				Montage->PostEditChange(); Montage->MarkPackageDirty(); AffectedAssets.AddUnique(Montage->GetPathName()); AddOperationResult(OperationResults, Index, Type, true, TEXT("AnimMontage updated."), Detail); bMutated = true; continue;
			}
			if (Type.StartsWith(TEXT("blueprint.")) || Type.StartsWith(TEXT("animgraph.")))
			{
				if (!Settings->bAllowBlueprintEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("Blueprint write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				Blueprint = LoadBlueprintForEdit(OpParams, AssetPath, Error);
				if (!Blueprint)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				AffectedAssets.AddUnique(AssetPath);
			}

			if (Type == TEXT("animgraph.set_node_property"))
			{
				FString GraphName;
				UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error);
				UEdGraphNode* Node = Graph ? ResolveNodeReference(Graph, OpParams, NodeRefs, Error) : nullptr;
				const FString PropertyPath = JsonString(OpParams, TEXT("property_path"), JsonString(OpParams, TEXT("property")));
				const TSharedPtr<FJsonValue> Value = OpParams->TryGetField(TEXT("value"));
				TSharedPtr<FJsonValue> Before;
				TSharedPtr<FJsonValue> After;
				if (Node) Node->Modify();
				if (!Node || PropertyPath.IsEmpty() || !Value.IsValid() || !FUEPIPropertyCodec::SetPropertyPath(Node, PropertyPath, Value, Before, After, Error))
				{
					bAllOk = false; FailureMessage = Error.IsEmpty() ? TEXT("AnimGraph node property request is invalid.") : Error; AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break;
				}
				Node->ReconstructNode();
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				TouchedBlueprints.Add(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("property_path"), PropertyPath); Detail->SetField(TEXT("before"), Before); Detail->SetField(TEXT("after"), After);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("AnimGraph node property updated."), Detail); bMutated = true; continue;
			}

			if (Type == TEXT("blueprint.add_node"))
			{
				FString GraphName;
				UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error);
				const FString Kind = JsonString(OpParams, TEXT("kind"));
				UEdGraphNode* Node = nullptr;
				if (!Graph)
				{
					bAllOk = false; FailureMessage = Error; AddOperationResult(OperationResults, Index, Type, false, Error); break;
				}
				if (Kind == TEXT("make_struct"))
				{
					UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *JsonString(OpParams, TEXT("struct_path")));
					if (!Struct || !UK2Node_MakeStruct::CanBeMade(Struct))
					{
						bAllOk = false; FailureMessage = TEXT("Struct cannot be used by a Make Struct node."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break;
					}
					Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_MakeStruct>(Graph, NodePositionFromParams(OpParams, Index), EK2NewNodeFlags::None, [Struct](UK2Node_MakeStruct* NewNode) { NewNode->StructType = Struct; });
				}
				else if (Kind == TEXT("input_key"))
				{
					const FKey Key(FName(*JsonString(OpParams, TEXT("key"))));
					if (!Key.IsValid()) { bAllOk = false; FailureMessage = TEXT("Input key is invalid."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break; }
					Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_InputKey>(Graph, NodePositionFromParams(OpParams, Index), EK2NewNodeFlags::None, [Key](UK2Node_InputKey* NewNode) { NewNode->InputKey = Key; });
				}
				if (!Node) { bAllOk = false; FailureMessage = TEXT("Generic Blueprint node creation failed."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break; }
				ApplyPinDefaults(Node, OpParams); RegisterNodeReferences(NodeRefs, Operation, OpParams, Node); FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName()); Detail->SetObjectField(TEXT("node"), NodeToJson(Node, Graph));
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Generic Blueprint node added."), Detail); bMutated = true; TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("animgraph.add_slot_node"))
			{
				UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Blueprint);
				FString GraphName; UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error);
				const FName SlotName(*JsonString(OpParams, TEXT("slot_name"), TEXT("DefaultSlot")));
				if (!AnimBlueprint || !Graph || SlotName.IsNone()) { bAllOk = false; FailureMessage = TEXT("AnimBlueprint, AnimGraph, and slot_name are required."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break; }
				if (AnimBlueprint->TargetSkeleton && !AnimBlueprint->TargetSkeleton->ContainsSlotName(SlotName)) { bAllOk = false; FailureMessage = TEXT("Slot is not registered on the AnimBlueprint target Skeleton."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break; }
				UAnimGraphNode_Slot* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UAnimGraphNode_Slot>(Graph, NodePositionFromParams(OpParams, Index), EK2NewNodeFlags::None, [SlotName](UAnimGraphNode_Slot* NewNode) { NewNode->Node.SlotName = SlotName; });
				if (!Node) { bAllOk = false; FailureMessage = TEXT("AnimGraph Slot node creation failed."); AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break; }
				RegisterNodeReferences(NodeRefs, Operation, OpParams, Node); FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName()); Detail->SetObjectField(TEXT("node"), NodeToJson(Node, Graph)); Detail->SetStringField(TEXT("slot_name"), SlotName.ToString());
				AddOperationResult(OperationResults, Index, Type, true, TEXT("AnimGraph Slot node added."), Detail); bMutated = true; TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.add_variable"))
			{
				const FString VariableNameText = JsonString(OpParams, TEXT("name"));
				if (VariableNameText.IsEmpty())
				{
					bAllOk = false;
					FailureMessage = TEXT("blueprint.add_variable requires params.name.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				FEdGraphPinType PinType;
				if (!BuildPinType(JsonString(OpParams, TEXT("pin_type"), JsonString(OpParams, TEXT("type_name"), TEXT("float"))), PinType, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				const FName VariableName(*VariableNameText);
				if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName) != INDEX_NONE)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Blueprint variable already exists: %s"), *VariableNameText);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString DefaultValue = JsonString(OpParams, TEXT("default_value"), JsonString(OpParams, TEXT("default")));
				const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, PinType, DefaultValue);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("variable"), VariableNameText);
				Detail->SetStringField(TEXT("pin_category"), PinType.PinCategory.ToString());
				Detail->SetStringField(TEXT("default_value"), DefaultValue);
				AddOperationResult(OperationResults, Index, Type, bAdded, bAdded ? TEXT("Variable added.") : TEXT("FBlueprintEditorUtils::AddMemberVariable failed."), Detail);
				bAllOk &= bAdded;
				if (!bAdded)
				{
					FailureMessage = TEXT("Failed to add Blueprint variable.");
					break;
				}
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.set_variable_default"))
			{
				const FString VariableNameText = JsonString(OpParams, TEXT("name"), JsonString(OpParams, TEXT("variable")));
				const FName VariableName(*VariableNameText);
				const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName);
				if (VariableIndex == INDEX_NONE)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Blueprint variable not found: %s"), *VariableNameText);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString DefaultValue = JsonString(OpParams, TEXT("default_value"), JsonString(OpParams, TEXT("value")));
				Blueprint->Modify();
				Blueprint->NewVariables[VariableIndex].DefaultValue = DefaultValue;
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("variable"), VariableNameText);
				Detail->SetStringField(TEXT("default_value"), DefaultValue);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Variable default updated."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.add_component"))
			{
				const FString ComponentNameText = JsonString(OpParams, TEXT("name"), TEXT("UEPIComponent"));
				UClass* ComponentClass = ResolveComponentClass(JsonString(OpParams, TEXT("component_class")));
				if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
				{
					bAllOk = false;
					FailureMessage = TEXT("blueprint.add_component requires a component_class derived from UActorComponent.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				if (!Blueprint->SimpleConstructionScript)
				{
					bAllOk = false;
					FailureMessage = TEXT("Blueprint has no SimpleConstructionScript.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FName ComponentName(*ComponentNameText);
				if (Blueprint->SimpleConstructionScript->FindSCSNode(ComponentName))
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Component already exists: %s"), *ComponentNameText);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				Blueprint->Modify();
				Blueprint->SimpleConstructionScript->Modify();
				USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(ComponentClass, ComponentName);
				Blueprint->SimpleConstructionScript->AddNode(Node);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("component"), ComponentNameText);
				Detail->SetStringField(TEXT("component_class"), ComponentClass->GetPathName());
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Component added."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.set_component_property"))
			{
				const FString ComponentNameText = JsonString(OpParams, TEXT("component"), JsonString(OpParams, TEXT("component_name")));
				const FString PropertyName = JsonString(OpParams, TEXT("property"));
				if (!Blueprint->SimpleConstructionScript || ComponentNameText.IsEmpty() || PropertyName.IsEmpty())
				{
					bAllOk = false;
					FailureMessage = TEXT("blueprint.set_component_property requires component and property.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(FName(*ComponentNameText));
				UObject* Template = Node ? Node->ComponentTemplate : nullptr;
				if (!Template)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Component template not found: %s"), *ComponentNameText);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				Template->Modify();
				if (!SetSimplePropertyValue(Template, PropertyName, OpParams, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("component"), ComponentNameText);
				Detail->SetStringField(TEXT("property"), PropertyName);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Component property updated."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.set_component_properties"))
			{
				const FString ComponentNameText = JsonString(OpParams, TEXT("component"), JsonString(OpParams, TEXT("component_name")));
				USCS_Node* Node = Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->FindSCSNode(FName(*ComponentNameText)) : nullptr;
				UObject* Template = Node ? Node->ComponentTemplate : nullptr;
				const TSharedPtr<FJsonObject> Properties = PropertyWritesObject(OpParams, Error);
				if (!Template || !Properties.IsValid()) { bAllOk = false; FailureMessage = Error.IsEmpty() ? TEXT("Component template or typed writes are invalid.") : Error; AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break; }
				Template->Modify();
				TArray<TSharedPtr<FJsonValue>> PropertyDiffs;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
				{
					TSharedPtr<FJsonValue> Before; TSharedPtr<FJsonValue> After;
					if (!FUEPIPropertyCodec::SetPropertyPath(Template, Pair.Key, PropertyWriteValue(Pair.Value), Before, After, Error, PropertyWriteMode(Pair.Value))) { bAllOk = false; FailureMessage = Error; AddOperationResult(OperationResults, Index, Type, false, Error); break; }
					TSharedRef<FJsonObject> Diff = MakeShared<FJsonObject>(); Diff->SetStringField(TEXT("property_path"), Pair.Key); Diff->SetField(TEXT("before"), Before); Diff->SetField(TEXT("after"), After); PropertyDiffs.Add(MakeShared<FJsonValueObject>(Diff));
				}
				if (!bAllOk) break;
				Template->PostEditChange(); FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("component"), ComponentNameText); Detail->SetArrayField(TEXT("property_diff"), PropertyDiffs);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Component typed properties updated."), Detail); bMutated = true; TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.create_function"))
			{
				const FString FunctionNameText = JsonString(OpParams, TEXT("name"), JsonString(OpParams, TEXT("function_name")));
				if (FunctionNameText.IsEmpty())
				{
					bAllOk = false;
					FailureMessage = TEXT("blueprint.create_function requires params.name or params.function_name.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				if (FindBlueprintGraph(Blueprint, FunctionNameText))
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Blueprint graph or function already exists: %s"), *FunctionNameText);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}

				Blueprint->Modify();
				UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*FunctionNameText), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				if (!NewGraph)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Failed to create function graph: %s"), *FunctionNameText);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				NewGraph->Modify();
				FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, true, nullptr);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				TArray<TSharedPtr<FJsonValue>> Nodes;
				for (UEdGraphNode* Node : NewGraph->Nodes)
				{
					Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node, NewGraph)));
				}
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("function"), FunctionNameText);
				Detail->SetStringField(TEXT("graph"), NewGraph->GetName());
				Detail->SetArrayField(TEXT("nodes"), Nodes);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Function graph created."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.add_event_node"))
			{
				const FString EventKind = JsonString(OpParams, TEXT("event_kind"), JsonString(OpParams, TEXT("kind"), TEXT("custom_event")));
				if (!EventKind.Equals(TEXT("custom"), ESearchCase::IgnoreCase) && !EventKind.Equals(TEXT("custom_event"), ESearchCase::IgnoreCase))
				{
					bAllOk = false;
					FailureMessage = TEXT("blueprint.add_event_node currently supports custom_event only.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString EventNameText = JsonString(OpParams, TEXT("event_name"), JsonString(OpParams, TEXT("name")));
				if (EventNameText.IsEmpty())
				{
					bAllOk = false;
					FailureMessage = TEXT("blueprint.add_event_node requires params.event_name or params.name.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				if (FBlueprintEditorUtils::FindCustomEventNode(Blueprint, FName(*EventNameText)))
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Custom event already exists: %s"), *EventNameText);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}

				FString GraphName;
				UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error);
				if (!Graph)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				Graph->Modify();
				UK2Node_CustomEvent* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CustomEvent>(
					Graph,
					NodePositionFromParams(OpParams, Index),
					EK2NewNodeFlags::None,
					[EventNameText](UK2Node_CustomEvent* NewNode)
					{
						NewNode->CustomFunctionName = FName(*EventNameText);
					});
				if (!Node)
				{
					bAllOk = false;
					FailureMessage = TEXT("Failed to spawn custom event node.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("event"), EventNameText);
				Detail->SetObjectField(TEXT("node"), NodeToJson(Node, Graph));
				RegisterNodeReferences(NodeRefs, Operation, OpParams, Node);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Custom event node added."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.add_function_call_node"))
			{
				FString GraphName;
				UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error);
				if (!Graph)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				FName SelfMemberName;
				UFunction* Function = ResolveFunctionForCall(Blueprint, OpParams, SelfMemberName, Error);
				if (!Function && SelfMemberName.IsNone())
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				Graph->Modify();
				UK2Node_CallFunction* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CallFunction>(
					Graph,
					NodePositionFromParams(OpParams, Index),
					EK2NewNodeFlags::None,
					[Function, SelfMemberName](UK2Node_CallFunction* NewNode)
					{
						if (Function)
						{
							NewNode->SetFromFunction(Function);
						}
						else
						{
							NewNode->FunctionReference.SetSelfMember(SelfMemberName);
						}
					});
				if (!Node)
				{
					bAllOk = false;
					FailureMessage = TEXT("Failed to spawn function call node.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				ApplyPinDefaults(Node, OpParams);
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("function"), Function ? Function->GetPathName() : SelfMemberName.ToString());
				Detail->SetObjectField(TEXT("node"), NodeToJson(Node, Graph));
				RegisterNodeReferences(NodeRefs, Operation, OpParams, Node);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Function call node added."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.add_variable_get_node") || Type == TEXT("blueprint.add_variable_set_node"))
			{
				const FString VariableNameText = JsonString(OpParams, TEXT("variable"), JsonString(OpParams, TEXT("name")));
				const FName VariableName(*VariableNameText);
				if (!BlueprintHasVariableNamed(Blueprint, VariableName))
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Blueprint variable was not found: %s"), *VariableNameText);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}

				FString GraphName;
				UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error);
				if (!Graph)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				Graph->Modify();
				UEdGraphNode* Node = nullptr;
				if (Type == TEXT("blueprint.add_variable_get_node"))
				{
					Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_VariableGet>(
						Graph,
						NodePositionFromParams(OpParams, Index),
						EK2NewNodeFlags::None,
						[VariableName](UK2Node_VariableGet* NewNode)
						{
							NewNode->VariableReference.SetSelfMember(VariableName);
						});
				}
				else
				{
					UK2Node_VariableSet* SetNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_VariableSet>(
						Graph,
						NodePositionFromParams(OpParams, Index),
						EK2NewNodeFlags::None,
						[VariableName](UK2Node_VariableSet* NewNode)
						{
							NewNode->VariableReference.SetSelfMember(VariableName);
						});
					Node = SetNode;
					const TSharedPtr<FJsonValue>* DefaultValue = OpParams->Values.Find(TEXT("value"));
					if (!DefaultValue)
					{
						DefaultValue = OpParams->Values.Find(TEXT("default_value"));
					}
					if (SetNode && DefaultValue)
					{
						if (UEdGraphPin* ValuePin = FindPinFlexible(SetNode, VariableNameText, TEXT("input")))
						{
							GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*ValuePin, PinDefaultString(*DefaultValue), true);
						}
					}
				}
				if (!Node)
				{
					bAllOk = false;
					FailureMessage = TEXT("Failed to spawn Blueprint variable node.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				ApplyPinDefaults(Node, OpParams);
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("variable"), VariableNameText);
				Detail->SetObjectField(TEXT("node"), NodeToJson(Node, Graph));
				RegisterNodeReferences(NodeRefs, Operation, OpParams, Node);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Blueprint variable node added."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.add_branch_node"))
			{
				FString GraphName;
				UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error);
				if (!Graph)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				Graph->Modify();
				UK2Node_IfThenElse* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_IfThenElse>(Graph, NodePositionFromParams(OpParams, Index), EK2NewNodeFlags::None);
				if (!Node)
				{
					bAllOk = false;
					FailureMessage = TEXT("Failed to spawn branch node.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				if (OpParams->HasField(TEXT("condition_default")))
				{
					GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*Node->GetConditionPin(), JsonBool(OpParams, TEXT("condition_default"), true) ? TEXT("true") : TEXT("false"), true);
				}
				ApplyPinDefaults(Node, OpParams);
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetObjectField(TEXT("node"), NodeToJson(Node, Graph));
				RegisterNodeReferences(NodeRefs, Operation, OpParams, Node);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Branch node added."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.add_print_string_node"))
			{
				FString GraphName;
				UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error);
				if (!Graph)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				UFunction* PrintStringFunction = UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
				if (!PrintStringFunction)
				{
					bAllOk = false;
					FailureMessage = TEXT("UKismetSystemLibrary::PrintString was not found.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				Graph->Modify();
				UK2Node_CallFunction* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CallFunction>(
					Graph,
					NodePositionFromParams(OpParams, Index),
					EK2NewNodeFlags::None,
					[PrintStringFunction](UK2Node_CallFunction* NewNode)
					{
						NewNode->SetFromFunction(PrintStringFunction);
					});
				if (!Node)
				{
					bAllOk = false;
					FailureMessage = TEXT("Failed to spawn PrintString node.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString InString = JsonString(OpParams, TEXT("in_string"), JsonString(OpParams, TEXT("text"), JsonString(OpParams, TEXT("message"))));
				if (!InString.IsEmpty())
				{
					if (UEdGraphPin* InStringPin = FindPinFlexible(Node, TEXT("InString"), TEXT("input")))
					{
						GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*InStringPin, InString, true);
					}
				}
				ApplyPinDefaults(Node, OpParams);
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetObjectField(TEXT("node"), NodeToJson(Node, Graph));
				RegisterNodeReferences(NodeRefs, Operation, OpParams, Node);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("PrintString node added."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.connect_pins"))
			{
				FString GraphName;
				UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error);
				if (!Graph)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				UEdGraphNode* SourceNode = nullptr;
				UEdGraphNode* TargetNode = nullptr;
				UEdGraphPin* SourcePin = ResolveEndpointPin(Graph, OpParams, TEXT("source"), TEXT("output"), NodeRefs, SourceNode, Error);
				if (!SourcePin)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				UEdGraphPin* TargetPin = ResolveEndpointPin(Graph, OpParams, TEXT("target"), TEXT("input"), NodeRefs, TargetNode, Error);
				if (!TargetPin)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				Graph->Modify();
				SourceNode->Modify();
				TargetNode->Modify();
				const UEdGraphSchema* Schema = Graph->GetSchema();
				const bool bConnected = Schema && Schema->TryCreateConnection(SourcePin, TargetPin);
				if (!bConnected)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Failed to connect %s.%s to %s.%s."), *GraphNodeGuidString(SourceNode), *SourcePin->PinName.ToString(), *GraphNodeGuidString(TargetNode), *TargetPin->PinName.ToString());
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());
				Detail->SetStringField(TEXT("graph"), Graph->GetName());
				Detail->SetObjectField(TEXT("source_node"), NodeToJson(SourceNode, Graph));
				Detail->SetObjectField(TEXT("target_node"), NodeToJson(TargetNode, Graph));
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Blueprint pins connected."), Detail);
				bMutated = true;
				TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.set_pin_default"))
			{
				FString GraphName; UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error); UEdGraphNode* Node = nullptr;
				UEdGraphPin* Pin = Graph ? ResolveEndpointPin(Graph, OpParams, TEXT("target"), TEXT("input"), NodeRefs, Node, Error) : nullptr;
				const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
				const TSharedPtr<FJsonValue> Value = OpParams->TryGetField(TEXT("value"));
				if (!Pin || !Schema || !Value.IsValid())
				{
					bAllOk = false; FailureMessage = Error.IsEmpty() ? TEXT("Pin default request is incomplete.") : Error; AddOperationResult(OperationResults, Index, Type, false, FailureMessage); break;
				}
				Schema->TrySetDefaultValue(*Pin, PinDefaultString(Value), true);
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint); TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetObjectField(TEXT("node"), NodeToJson(Node, Graph)); Detail->SetStringField(TEXT("pin"), Pin->PinName.ToString()); AddOperationResult(OperationResults, Index, Type, true, TEXT("Pin default updated."), Detail); bMutated = true; TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.disconnect_pins"))
			{
				FString GraphName; UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error); UEdGraphNode* Node = nullptr;
				UEdGraphPin* Pin = Graph ? ResolveEndpointPin(Graph, OpParams, TEXT("target"), TEXT("input"), NodeRefs, Node, Error) : nullptr;
				if (!Pin) { bAllOk = false; FailureMessage = Error; AddOperationResult(OperationResults, Index, Type, false, Error); break; }
				Pin->Modify(); Pin->BreakAllPinLinks(true); FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint); AddOperationResult(OperationResults, Index, Type, true, TEXT("Pin links disconnected.")); bMutated = true; TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.break_all_links"))
			{
				FString GraphName; UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error); UEdGraphNode* Node = Graph ? ResolveNodeReference(Graph, OpParams, NodeRefs, Error) : nullptr;
				if (!Node) { bAllOk = false; FailureMessage = Error; AddOperationResult(OperationResults, Index, Type, false, Error); break; }
				Node->Modify(); for (UEdGraphPin* Pin : Node->Pins) if (Pin) { Pin->Modify(); Pin->BreakAllPinLinks(true); }
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint); AddOperationResult(OperationResults, Index, Type, true, TEXT("All node pin links disconnected.")); bMutated = true; TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.remove_node") || Type == TEXT("blueprint.move_node") || Type == TEXT("blueprint.set_node_comment"))
			{
				FString GraphName; UEdGraph* Graph = ResolveGraphForEdit(Blueprint, OpParams, GraphName, Error); UEdGraphNode* Node = Graph ? ResolveNodeReference(Graph, OpParams, NodeRefs, Error) : nullptr;
				if (!Node) { bAllOk = false; FailureMessage = Error; AddOperationResult(OperationResults, Index, Type, false, Error); break; }
				Graph->Modify(); Node->Modify();
				if (Type == TEXT("blueprint.remove_node")) { Graph->RemoveNode(Node); }
				else if (Type == TEXT("blueprint.move_node")) { const FVector2D Position = NodePositionFromParams(OpParams, Index); Node->NodePosX = FMath::RoundToInt(Position.X); Node->NodePosY = FMath::RoundToInt(Position.Y); }
				else { Node->NodeComment = JsonString(OpParams, TEXT("comment")); Node->bCommentBubbleVisible = JsonBool(OpParams, TEXT("comment_visible"), false); }
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint); AddOperationResult(OperationResults, Index, Type, true, TEXT("Blueprint node updated.")); bMutated = true; TouchedBlueprints.Add(Blueprint);
			}
			else if (Type == TEXT("blueprint.compile"))
			{
				TSharedRef<FJsonObject> CompileResult = CompileBlueprintToJson(Blueprint);
				CompileResults.Add(MakeShared<FJsonValueObject>(CompileResult));
				const bool bCompileOk = CompileResult->GetBoolField(TEXT("ok"));
				AddOperationResult(OperationResults, Index, Type, bCompileOk, bCompileOk ? TEXT("Blueprint compiled.") : TEXT("Blueprint compile returned errors."), CompileResult);
				bValidationOk &= bCompileOk;
				if (!bCompileOk)
				{
					FailureMessage = TEXT("Blueprint compile returned errors.");
					break;
				}
			}
			else if (Type == TEXT("material.create_instance"))
			{
				if (!Settings->bAllowMaterialEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("Material write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString ParentPath = JsonString(OpParams, TEXT("parent"), JsonString(OpParams, TEXT("parent_material")));
				UMaterialInterface* ParentMaterial = ParentPath.IsEmpty() ? nullptr : LoadObject<UMaterialInterface>(nullptr, *ParentPath);
				if (!ParentMaterial)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("material.create_instance requires a valid parent material: %s"), *ParentPath);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				FString PackagePath;
				FString AssetName;
				if (!SplitDestinationPath(OpParams, ParentMaterial->GetName() + TEXT("_Inst"), PackagePath, AssetName, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
				Factory->InitialParent = ParentMaterial;
				UObject* NewAsset = FAssetToolsModule::GetModule().Get().CreateAsset(AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory, TEXT("UEPI"));
				UMaterialInstanceConstant* NewInstance = Cast<UMaterialInstanceConstant>(NewAsset);
				if (!NewInstance)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Failed to create MaterialInstanceConstant %s/%s."), *PackagePath, *AssetName);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				AffectedAssets.AddUnique(NewInstance->GetPathName());
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetObjectField(TEXT("asset"), ObjectToJson(NewInstance));
				Detail->SetStringField(TEXT("parent"), ParentMaterial->GetPathName());
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Material instance created."), Detail);
				bMutated = true;
			}
			else if (Type == TEXT("material.apply_to_actor"))
			{
				if (!Settings->bAllowMaterialEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("Material write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				UMaterialInterface* Material = LoadMaterialInterfaceForEdit(OpParams, AssetPath, Error);
				if (!Material)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				TArray<AActor*> Actors = ResolveActorTargets(OpParams);
				if (Actors.Num() == 0)
				{
					bAllOk = false;
					FailureMessage = TEXT("material.apply_to_actor did not resolve any target actors.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString ComponentName = JsonString(OpParams, TEXT("component"), JsonString(OpParams, TEXT("component_name")));
				const int32 MaterialIndex = JsonInt(OpParams, TEXT("material_index"), 0);
				TArray<TSharedPtr<FJsonValue>> Applied;
				for (AActor* Actor : Actors)
				{
					UPrimitiveComponent* Component = FindPrimitiveComponentForMaterial(Actor, ComponentName);
					if (!Component)
					{
						bAllOk = false;
						FailureMessage = FString::Printf(TEXT("Primitive component not found on actor %s."), Actor ? *Actor->GetPathName() : TEXT("<null>"));
						break;
					}
					Actor->Modify();
					Component->Modify();
					Component->SetMaterial(MaterialIndex, Material);
					TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("actor"), Actor->GetPathName());
					Item->SetStringField(TEXT("component"), Component->GetName());
					Item->SetNumberField(TEXT("material_index"), MaterialIndex);
					Applied.Add(MakeShared<FJsonValueObject>(Item));
				}
				if (!bAllOk)
				{
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("material"), Material->GetPathName());
				Detail->SetArrayField(TEXT("applied"), Applied);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Material applied to actor component(s)."), Detail);
				bMutated = true;
			}
			else if (Type == TEXT("material.apply_to_blueprint_component"))
			{
				if (!Settings->bAllowMaterialEdits || !Settings->bAllowBlueprintEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("Material and Blueprint write alpha must both be enabled for material.apply_to_blueprint_component.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				UMaterialInterface* Material = LoadMaterialInterfaceForEdit(OpParams, AssetPath, Error);
				if (!Material)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				FString BlueprintAssetPath;
				UBlueprint* TargetBlueprint = LoadBlueprintForEdit(OpParams, BlueprintAssetPath, Error);
				if (!TargetBlueprint)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				const FString ComponentName = JsonString(OpParams, TEXT("component"), JsonString(OpParams, TEXT("component_name")));
				const int32 MaterialIndex = JsonInt(OpParams, TEXT("material_index"), 0);
				USCS_Node* Node = TargetBlueprint->SimpleConstructionScript ? TargetBlueprint->SimpleConstructionScript->FindSCSNode(FName(*ComponentName)) : nullptr;
				UPrimitiveComponent* Template = Node ? Cast<UPrimitiveComponent>(Node->ComponentTemplate) : nullptr;
				if (!Template)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Primitive component template not found on Blueprint: %s"), *ComponentName);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				Template->Modify();
				Template->SetMaterial(MaterialIndex, Material);
				FBlueprintEditorUtils::MarkBlueprintAsModified(TargetBlueprint);
				AffectedAssets.AddUnique(BlueprintAssetPath);
				TouchedBlueprints.Add(TargetBlueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("blueprint"), TargetBlueprint->GetPathName());
				Detail->SetStringField(TEXT("component"), ComponentName);
				Detail->SetStringField(TEXT("material"), Material->GetPathName());
				Detail->SetNumberField(TEXT("material_index"), MaterialIndex);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Material applied to Blueprint component template."), Detail);
				bMutated = true;
			}
			else if (Type == TEXT("material.set_scalar_parameter") || Type == TEXT("material.set_vector_parameter") || Type == TEXT("material.set_texture_parameter"))
			{
				if (!Settings->bAllowMaterialEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("Material write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				UMaterialInstanceConstant* Instance = LoadMaterialInstanceForEdit(OpParams, AssetPath, Error);
				if (!Instance)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				const FString ParameterName = JsonString(OpParams, TEXT("parameter"), JsonString(OpParams, TEXT("name")));
				if (ParameterName.IsEmpty())
				{
					bAllOk = false;
					FailureMessage = TEXT("Material parameter operation requires parameter or name.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				Instance->Modify();
				if (Type == TEXT("material.set_scalar_parameter"))
				{
					double NumberValue = 0.0;
					if (!OpParams->TryGetNumberField(TEXT("value"), NumberValue))
					{
						bAllOk = false;
						FailureMessage = TEXT("material.set_scalar_parameter requires numeric value.");
						AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
						break;
					}
					Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(FName(*ParameterName)), static_cast<float>(NumberValue));
				}
				else if (Type == TEXT("material.set_vector_parameter"))
				{
					FLinearColor ColorValue;
					TSharedPtr<FJsonObject> ColorObject = JsonObjectField(OpParams, TEXT("value"));
					if (!ColorObject.IsValid())
					{
						ColorObject = OpParams;
					}
					if (!JsonColor(ColorObject, ColorValue))
					{
						bAllOk = false;
						FailureMessage = TEXT("material.set_vector_parameter requires value {r,g,b,a?}.");
						AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
						break;
					}
					Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(FName(*ParameterName)), ColorValue);
				}
				else
				{
					const FString TexturePath = JsonString(OpParams, TEXT("texture"), JsonString(OpParams, TEXT("value")));
					UTexture* Texture = TexturePath.IsEmpty() ? nullptr : LoadObject<UTexture>(nullptr, *TexturePath);
					if (!Texture)
					{
						bAllOk = false;
						FailureMessage = FString::Printf(TEXT("Failed to load texture parameter value: %s"), *TexturePath);
						AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
						break;
					}
					Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(*ParameterName)), Texture);
				}
				Instance->PostEditChange();
				Instance->MarkPackageDirty();
				AffectedAssets.AddUnique(AssetPath);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), Instance->GetPathName());
				Detail->SetStringField(TEXT("parameter"), ParameterName);
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Material instance parameter updated."), Detail);
				bMutated = true;
			}
			else if (Type == TEXT("content.create_folder"))
			{
				if (!Settings->bAllowContentEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("Content write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString FolderPath = NormalizedContentPath(JsonString(OpParams, TEXT("path"), JsonString(OpParams, TEXT("folder"))));
				if (!ValidateGameContentPath(FolderPath, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				const FString Directory = FPackageName::LongPackageNameToFilename(FolderPath);
				const bool bCreated = IFileManager::Get().MakeDirectory(*Directory, true);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("folder"), FolderPath);
				Detail->SetStringField(TEXT("directory"), Directory);
				AddOperationResult(OperationResults, Index, Type, bCreated, bCreated ? TEXT("Content folder created.") : TEXT("Failed to create content folder."), Detail);
				bAllOk &= bCreated;
				if (!bCreated)
				{
					FailureMessage = TEXT("Failed to create content folder.");
					break;
				}
				bMutated = true;
			}
			else if (Type == TEXT("content.duplicate_asset"))
			{
				if (!Settings->bAllowContentEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("Content write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				UObject* SourceObject = LoadContentObject(OpParams, AssetPath, Error);
				if (!SourceObject)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				FString PackagePath;
				FString AssetName;
				if (!SplitDestinationPath(OpParams, SourceObject->GetName() + TEXT("_Copy"), PackagePath, AssetName, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				UObject* NewObject = FAssetToolsModule::GetModule().Get().DuplicateAsset(AssetName, PackagePath, SourceObject);
				if (!NewObject)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Failed to duplicate asset to %s/%s."), *PackagePath, *AssetName);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				AffectedAssets.AddUnique(NewObject->GetPathName());
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetObjectField(TEXT("source"), ObjectToJson(SourceObject));
				Detail->SetObjectField(TEXT("asset"), ObjectToJson(NewObject));
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Asset duplicated."), Detail);
				bMutated = true;
			}
			else if (Type == TEXT("content.rename_asset"))
			{
				if (!Settings->bAllowContentEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("Content write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				UObject* SourceObject = LoadContentObject(OpParams, AssetPath, Error);
				if (!SourceObject)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				FString PackagePath;
				FString AssetName;
				if (!SplitDestinationPath(OpParams, SourceObject->GetName(), PackagePath, AssetName, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				TArray<FAssetRenameData> RenameData;
				RenameData.Add(FAssetRenameData(SourceObject, PackagePath, AssetName));
				const bool bRenamed = FAssetToolsModule::GetModule().Get().RenameAssets(RenameData);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("old_asset"), AssetPath);
				Detail->SetStringField(TEXT("new_asset"), PackagePath / AssetName + TEXT(".") + AssetName);
				AddOperationResult(OperationResults, Index, Type, bRenamed, bRenamed ? TEXT("Asset renamed.") : TEXT("Asset rename failed."), Detail);
				bAllOk &= bRenamed;
				if (!bRenamed)
				{
					FailureMessage = TEXT("Asset rename failed.");
					break;
				}
				AffectedAssets.AddUnique(PackagePath / AssetName + TEXT(".") + AssetName);
				bMutated = true;
			}
			else if (Type == TEXT("content.import"))
			{
				if (!Settings->bAllowContentEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("Content write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString Filename = JsonString(OpParams, TEXT("file"), JsonString(OpParams, TEXT("filename")));
				if (Filename.IsEmpty() || !FPaths::FileExists(Filename))
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Import file does not exist: %s"), *Filename);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString Extension = FPaths::GetExtension(Filename).ToLower();
				if (!(Extension == TEXT("fbx") || Extension == TEXT("obj") || Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg") || Extension == TEXT("wav") || Extension == TEXT("uasset")))
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Import extension is not allowlisted for write alpha: %s"), *Extension);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				FString DestinationPath = NormalizedContentPath(JsonString(OpParams, TEXT("destination_path"), JsonString(OpParams, TEXT("folder"))));
				if (!ValidateGameContentPath(DestinationPath, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				UAssetImportTask* Task = NewObject<UAssetImportTask>();
				Task->Filename = Filename;
				Task->DestinationPath = DestinationPath;
				Task->DestinationName = JsonString(OpParams, TEXT("name"), JsonString(OpParams, TEXT("asset_name")));
				Task->bAutomated = true;
				Task->bAsync = false;
				Task->bSave = false;
				Task->bReplaceExisting = JsonBool(OpParams, TEXT("replace_existing"), false);
				TArray<UAssetImportTask*> Tasks;
				Tasks.Add(Task);
				FAssetToolsModule::GetModule().Get().ImportAssetTasks(Tasks);
				const TArray<UObject*>& ImportedObjects = Task->GetObjects();
				TArray<TSharedPtr<FJsonValue>> Imported;
				for (UObject* ImportedObject : ImportedObjects)
				{
					if (ImportedObject)
					{
						Imported.Add(MakeShared<FJsonValueObject>(ObjectToJson(ImportedObject)));
						AffectedAssets.AddUnique(ImportedObject->GetPathName());
					}
				}
				const bool bImported = ImportedObjects.Num() > 0;
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("file"), Filename);
				Detail->SetStringField(TEXT("destination_path"), DestinationPath);
				Detail->SetArrayField(TEXT("assets"), Imported);
				AddOperationResult(OperationResults, Index, Type, bImported, bImported ? TEXT("Asset imported.") : TEXT("Import produced no assets."), Detail);
				bAllOk &= bImported;
				if (!bImported)
				{
					FailureMessage = TEXT("Import produced no assets.");
					break;
				}
				bMutated = true;
			}
			else if (Type == TEXT("widget.create"))
			{
				if (!Settings->bAllowUMGEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("UMG write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				FString PackagePath;
				FString AssetName;
				if (!SplitDestinationPath(OpParams, TEXT("WBP_UEPIWidget"), PackagePath, AssetName, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
				Factory->BlueprintType = BPTYPE_Normal;
				Factory->ParentClass = UUserWidget::StaticClass();
				UObject* NewAsset = FAssetToolsModule::GetModule().Get().CreateAsset(AssetName, PackagePath, UWidgetBlueprint::StaticClass(), Factory, TEXT("UEPI"));
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(NewAsset);
				if (!WidgetBlueprint)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Failed to create Widget Blueprint %s/%s."), *PackagePath, *AssetName);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				UCanvasPanel* RootCanvas = EnsureCanvasRoot(WidgetBlueprint);
				if (!RootCanvas)
				{
					bAllOk = false;
					FailureMessage = TEXT("Widget Blueprint was created but a CanvasPanel root could not be initialized.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				AffectedAssets.AddUnique(WidgetBlueprint->GetPathName());
				TouchedBlueprints.Add(WidgetBlueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetObjectField(TEXT("asset"), ObjectToJson(WidgetBlueprint));
				Detail->SetObjectField(TEXT("root_widget"), WidgetToJson(RootCanvas));
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Widget Blueprint created with a CanvasPanel root."), Detail);
				bMutated = true;
			}
			else if (Type == TEXT("widget.add_text") || Type == TEXT("widget.add_button"))
			{
				if (!Settings->bAllowUMGEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("UMG write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintForEdit(OpParams, AssetPath, Error);
				if (!WidgetBlueprint)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				if (!WidgetBlueprint->WidgetTree || !WidgetBlueprint->WidgetTree->RootWidget)
				{
					if (!EnsureCanvasRoot(WidgetBlueprint))
					{
						bAllOk = false;
						FailureMessage = TEXT("Widget Blueprint has no root widget and a CanvasPanel root could not be initialized.");
						AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
						break;
					}
				}
				UPanelWidget* Parent = ResolveWidgetParent(WidgetBlueprint, OpParams, Error);
				if (!Parent)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				const FString WidgetName = JsonString(OpParams, TEXT("name"), Type == TEXT("widget.add_text") ? TEXT("UEPIText") : TEXT("UEPIButton"));
				if (WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName)))
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Widget already exists in WidgetTree: %s"), *WidgetName);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}

				WidgetBlueprint->Modify();
				WidgetBlueprint->WidgetTree->Modify();
				Parent->Modify();
				if (Type == TEXT("widget.add_text"))
				{
					UTextBlock* TextBlock = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*WidgetName));
					if (!TextBlock)
					{
						bAllOk = false;
						FailureMessage = TEXT("Failed to construct UTextBlock.");
						AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
						break;
					}
					TextBlock->SetText(FText::FromString(JsonString(OpParams, TEXT("text"), TEXT("Text"))));
					UPanelSlot* Slot = AddWidgetToParent(Parent, TextBlock);
					if (!Slot)
					{
						bAllOk = false;
						FailureMessage = TEXT("Failed to add TextBlock to parent panel.");
						AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
						break;
					}
					ApplyCanvasSlotParams(TextBlock, OpParams);
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
					AffectedAssets.AddUnique(AssetPath);
					TouchedBlueprints.Add(WidgetBlueprint);
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
					Detail->SetStringField(TEXT("asset"), WidgetBlueprint->GetPathName());
					Detail->SetObjectField(TEXT("widget"), WidgetToJson(TextBlock));
					AddOperationResult(OperationResults, Index, Type, true, TEXT("TextBlock widget added."), Detail);
				}
				else
				{
					UButton* Button = WidgetBlueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), FName(*WidgetName));
					if (!Button)
					{
						bAllOk = false;
						FailureMessage = TEXT("Failed to construct UButton.");
						AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
						break;
					}
					UPanelSlot* Slot = AddWidgetToParent(Parent, Button);
					if (!Slot)
					{
						bAllOk = false;
						FailureMessage = TEXT("Failed to add Button to parent panel.");
						AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
						break;
					}
					UTextBlock* LabelBlock = nullptr;
					const FString LabelText = JsonString(OpParams, TEXT("text"), JsonString(OpParams, TEXT("label")));
					if (!LabelText.IsEmpty())
					{
						const FString LabelName = WidgetName + TEXT("_Label");
						LabelBlock = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*LabelName));
						if (LabelBlock)
						{
							LabelBlock->SetText(FText::FromString(LabelText));
							Button->SetContent(LabelBlock);
						}
					}
					ApplyCanvasSlotParams(Button, OpParams);
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
					AffectedAssets.AddUnique(AssetPath);
					TouchedBlueprints.Add(WidgetBlueprint);
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
					Detail->SetStringField(TEXT("asset"), WidgetBlueprint->GetPathName());
					Detail->SetObjectField(TEXT("widget"), WidgetToJson(Button));
					if (LabelBlock)
					{
						Detail->SetObjectField(TEXT("label_widget"), WidgetToJson(LabelBlock));
					}
					AddOperationResult(OperationResults, Index, Type, true, TEXT("Button widget added."), Detail);
				}
				bMutated = true;
			}
			else if (Type == TEXT("widget.set_slot"))
			{
				if (!Settings->bAllowUMGEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("UMG write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintForEdit(OpParams, AssetPath, Error);
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					bAllOk = false;
					FailureMessage = WidgetBlueprint ? TEXT("Widget Blueprint has no WidgetTree.") : Error;
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString WidgetName = JsonString(OpParams, TEXT("widget_name"), JsonString(OpParams, TEXT("name")));
				UWidget* Widget = WidgetName.IsEmpty() ? nullptr : WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName));
				if (!Widget)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Widget was not found in WidgetTree: %s"), *WidgetName);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				if (!Cast<UCanvasPanelSlot>(Widget->Slot))
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("widget.set_slot currently supports CanvasPanelSlot only: %s"), *WidgetName);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				WidgetBlueprint->Modify();
				Widget->Modify();
				Widget->Slot->Modify();
				ApplyCanvasSlotParams(Widget, OpParams);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				AffectedAssets.AddUnique(AssetPath);
				TouchedBlueprints.Add(WidgetBlueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), WidgetBlueprint->GetPathName());
				Detail->SetObjectField(TEXT("widget"), WidgetToJson(Widget));
				AddOperationResult(OperationResults, Index, Type, true, TEXT("Widget CanvasPanelSlot updated."), Detail);
				bMutated = true;
			}
			else if (Type == TEXT("widget.bind_button_to_custom_event"))
			{
				if (!Settings->bAllowUMGEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("UMG write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintForEdit(OpParams, AssetPath, Error);
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					bAllOk = false;
					FailureMessage = WidgetBlueprint ? TEXT("Widget Blueprint has no WidgetTree.") : Error;
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FString ButtonName = JsonString(OpParams, TEXT("button"), JsonString(OpParams, TEXT("button_name"), JsonString(OpParams, TEXT("widget_name"), JsonString(OpParams, TEXT("name")))));
				UButton* Button = ButtonName.IsEmpty() ? nullptr : WidgetBlueprint->WidgetTree->FindWidget<UButton>(FName(*ButtonName));
				if (!Button)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Button widget was not found in WidgetTree: %s"), *ButtonName);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const FName DelegateName(*JsonString(OpParams, TEXT("delegate"), JsonString(OpParams, TEXT("event"), TEXT("OnClicked"))));
				if (DelegateName.IsNone() || !FindFProperty<FMulticastDelegateProperty>(Button->GetClass(), DelegateName))
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Button delegate was not found: %s"), *DelegateName.ToString());
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}

				WidgetBlueprint->Modify();
				Button->Modify();
				Button->bIsVariable = true;
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

				FObjectProperty* ComponentProperty = FindWidgetObjectProperty(WidgetBlueprint, Button);
				if (!ComponentProperty)
				{
					TSharedRef<FJsonObject> CompileResult = CompileBlueprintToJson(WidgetBlueprint);
					CompileResults.Add(MakeShared<FJsonValueObject>(CompileResult));
					if (!CompileResult->GetBoolField(TEXT("ok")))
					{
						bAllOk = false;
						FailureMessage = FString::Printf(TEXT("Widget Blueprint compile failed before binding %s.%s."), *ButtonName, *DelegateName.ToString());
						AddOperationResult(OperationResults, Index, Type, false, FailureMessage, CompileResult);
						break;
					}
					ComponentProperty = FindWidgetObjectProperty(WidgetBlueprint, Button);
				}
				if (!ComponentProperty)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Generated widget variable property was not found for button: %s"), *ButtonName);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}

				const UK2Node_ComponentBoundEvent* ExistingNode = FKismetEditorUtilities::FindBoundEventForComponent(WidgetBlueprint, DelegateName, ComponentProperty->GetFName());
				const bool bAlreadyBound = ExistingNode != nullptr;
				if (!ExistingNode)
				{
					FKismetEditorUtilities::CreateNewBoundEventForClass(Button->GetClass(), DelegateName, WidgetBlueprint, ComponentProperty);
					ExistingNode = FKismetEditorUtilities::FindBoundEventForComponent(WidgetBlueprint, DelegateName, ComponentProperty->GetFName());
				}
				if (!ExistingNode)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Failed to create bound event node for %s.%s."), *ButtonName, *DelegateName.ToString());
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				AffectedAssets.AddUnique(AssetPath);
				TouchedBlueprints.Add(WidgetBlueprint);
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("asset"), WidgetBlueprint->GetPathName());
				Detail->SetStringField(TEXT("button"), ButtonName);
				Detail->SetStringField(TEXT("delegate"), DelegateName.ToString());
				Detail->SetBoolField(TEXT("already_bound"), bAlreadyBound);
				Detail->SetObjectField(TEXT("node"), NodeToJson(ExistingNode, ExistingNode->GetGraph()));
				AddOperationResult(OperationResults, Index, Type, true, bAlreadyBound ? TEXT("Button delegate was already bound.") : TEXT("Button delegate bound to a ComponentBoundEvent node."), Detail);
				bMutated = true;
			}
#if UEPI_WITH_ENHANCED_INPUT
			else if (Type == TEXT("input.create_action"))
			{
				if (!Settings->bAllowInputEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("Enhanced Input write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				FString PackagePath;
				FString AssetName;
				if (!SplitDestinationPath(OpParams, TEXT("IA_UEPIAction"), PackagePath, AssetName, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				UInputAction_Factory* Factory = NewObject<UInputAction_Factory>();
				Factory->InputActionClass = UInputAction::StaticClass();
				UObject* NewAsset = FAssetToolsModule::GetModule().Get().CreateAsset(AssetName, PackagePath, UInputAction::StaticClass(), Factory, TEXT("UEPI"));
				UInputAction* Action = Cast<UInputAction>(NewAsset);
				if (!Action)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Failed to create InputAction %s/%s."), *PackagePath, *AssetName);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				EInputActionValueType ValueType = EInputActionValueType::Boolean;
				if (!ParseInputActionValueType(JsonString(OpParams, TEXT("value_type"), TEXT("bool")), ValueType, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				Action->Modify();
				Action->ValueType = ValueType;
				const FString Description = JsonString(OpParams, TEXT("description"));
				if (!Description.IsEmpty())
				{
					Action->ActionDescription = FText::FromString(Description);
				}
				Action->PostEditChange();
				Action->MarkPackageDirty();
				AffectedAssets.AddUnique(Action->GetPathName());
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetObjectField(TEXT("asset"), ObjectToJson(Action));
				Detail->SetStringField(TEXT("value_type"), StaticEnum<EInputActionValueType>()->GetNameStringByValue(static_cast<int64>(Action->ValueType)));
				AddOperationResult(OperationResults, Index, Type, true, TEXT("InputAction created."), Detail);
				bMutated = true;
			}
			else if (Type == TEXT("input.create_mapping_context"))
			{
				if (!Settings->bAllowInputEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("Enhanced Input write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				FString PackagePath;
				FString AssetName;
				if (!SplitDestinationPath(OpParams, TEXT("IMC_UEPIContext"), PackagePath, AssetName, Error))
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				UInputMappingContext_Factory* Factory = NewObject<UInputMappingContext_Factory>();
				Factory->InputMappingContextClass = UInputMappingContext::StaticClass();
				UObject* NewAsset = FAssetToolsModule::GetModule().Get().CreateAsset(AssetName, PackagePath, UInputMappingContext::StaticClass(), Factory, TEXT("UEPI"));
				UInputMappingContext* Context = Cast<UInputMappingContext>(NewAsset);
				if (!Context)
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Failed to create InputMappingContext %s/%s."), *PackagePath, *AssetName);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				Context->Modify();
				const FString Description = JsonString(OpParams, TEXT("description"));
				if (!Description.IsEmpty())
				{
					Context->ContextDescription = FText::FromString(Description);
				}
				Context->PostEditChange();
				Context->MarkPackageDirty();
				AffectedAssets.AddUnique(Context->GetPathName());
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetObjectField(TEXT("asset"), ObjectToJson(Context));
				Detail->SetNumberField(TEXT("mapping_count"), Context->GetMappings().Num());
				AddOperationResult(OperationResults, Index, Type, true, TEXT("InputMappingContext created."), Detail);
				bMutated = true;
			}
			else if (Type == TEXT("input.add_key_mapping") || Type == TEXT("input.remove_key_mapping"))
			{
				if (!Settings->bAllowInputEdits)
				{
					bAllOk = false;
					FailureMessage = TEXT("Enhanced Input write alpha is disabled in UEPI Project Settings.");
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				FString ContextPath;
				UInputMappingContext* Context = LoadInputMappingContextForEdit(OpParams, ContextPath, Error);
				if (!Context)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				FString ActionPath;
				UInputAction* Action = LoadInputActionForEdit(OpParams, ActionPath, Error);
				if (!Action)
				{
					bAllOk = false;
					FailureMessage = Error;
					AddOperationResult(OperationResults, Index, Type, false, Error);
					break;
				}
				const FString KeyName = JsonString(OpParams, TEXT("key"));
				FKey Key(FName(*KeyName));
				if (KeyName.IsEmpty() || !Key.IsValid())
				{
					bAllOk = false;
					FailureMessage = FString::Printf(TEXT("Enhanced Input key is invalid: %s"), *KeyName);
					AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
					break;
				}
				const int32 BeforeCount = Context->GetMappings().Num();
				Context->Modify();
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("context"), Context->GetPathName());
				Detail->SetStringField(TEXT("action"), Action->GetPathName());
				Detail->SetStringField(TEXT("key"), Key.ToString());
				Detail->SetNumberField(TEXT("mapping_count_before"), BeforeCount);
				if (Type == TEXT("input.add_key_mapping"))
				{
					FEnhancedActionKeyMapping& Mapping = Context->MapKey(Action, Key);
					Detail->SetObjectField(TEXT("mapping"), InputMappingToJson(Mapping));
					Context->PostEditChange();
					Context->MarkPackageDirty();
					Detail->SetNumberField(TEXT("mapping_count_after"), Context->GetMappings().Num());
					AddOperationResult(OperationResults, Index, Type, true, TEXT("Enhanced Input key mapping added."), Detail);
					AffectedAssets.AddUnique(ContextPath);
					bMutated = true;
				}
				else
				{
					Context->UnmapKey(Action, Key);
					const int32 AfterCount = Context->GetMappings().Num();
					const bool bRemoved = AfterCount < BeforeCount;
					if (bRemoved)
					{
						Context->PostEditChange();
						Context->MarkPackageDirty();
						AffectedAssets.AddUnique(ContextPath);
						bMutated = true;
					}
					Detail->SetNumberField(TEXT("mapping_count_after"), AfterCount);
					AddOperationResult(OperationResults, Index, Type, bRemoved, bRemoved ? TEXT("Enhanced Input key mapping removed.") : TEXT("No matching Enhanced Input key mapping was removed."), Detail);
					if (!bRemoved)
					{
						bAllOk = false;
						FailureMessage = TEXT("No matching Enhanced Input key mapping was removed.");
						break;
					}
				}
			}
#else
			else if (Type.StartsWith(TEXT("input.")))
			{
				bAllOk = false;
				FailureMessage = TEXT("Enhanced Input is not enabled for this project build.");
				AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
				break;
			}
#endif
			else
			{
				bAllOk = false;
				FailureMessage = FString::Printf(TEXT("Operation is not implemented in write alpha: %s"), *Type);
				AddOperationResult(OperationResults, Index, Type, false, FailureMessage);
				break;
			}
		}

		if (!bAllOk)
		{
			Transaction.Cancel();
		}

		if (bAllOk)
		{
			for (UBlueprint* Blueprint : TouchedBlueprints)
			{
				TSharedRef<FJsonObject> CompileResult = CompileBlueprintToJson(Blueprint);
				CompileResults.Add(MakeShared<FJsonValueObject>(CompileResult));
				if (!CompileResult->GetBoolField(TEXT("ok")))
				{
					bValidationOk = false;
					FailureMessage = FString::Printf(TEXT("Blueprint compile returned errors for %s."), *Blueprint->GetPathName());
				}
			}
		}
		if (!bValidationOk)
		{
			bAllOk = false;
			Transaction.Cancel();
		}

		bool bSaved = false;
		TArray<TSharedPtr<FJsonValue>> SavedFileHashes;
		if (bAllOk && JsonBool(Params, TEXT("save"), false))
		{
			TArray<FUEPISavedFileHash> FileHashes;
			bSaved = FUEPIPackageSaveService::SaveTouched(AffectedAssets, FileHashes, ServiceError);
			if (!bSaved)
			{
				bAllOk = false;
				FailureMessage = ServiceError;
				Transaction.Cancel();
				FString RestoreError;
				FUEPIBackupService::Restore(TransactionBackupFiles, AffectedAssets, RestoreError);
			}
			else
			{
				for (const FUEPISavedFileHash& Hash : FileHashes)
				{
					TSharedRef<FJsonObject> FileHash = MakeShared<FJsonObject>();
					FileHash->SetStringField(TEXT("file"), Hash.File);
					FileHash->SetStringField(TEXT("md5"), Hash.Md5);
					SavedFileHashes.Add(MakeShared<FJsonValueObject>(FileHash));
				}
			}
		}
		FString JournalError;
		FUEPITransactionJournal::Write(TransactionId, bAllOk ? TEXT("applied") : TEXT("failed"), AffectedAssets, TransactionBackupFiles, bSaved, FailureMessage, JournalPath, JournalError);

		FString RefreshRequestPath;
		FString RefreshError;
		if (AffectedAssets.Num() > 0)
		{
			WriteRefreshRequest(AffectedAssets, TEXT("live"), RefreshRequestPath, RefreshError);
		}

		if (bMutated)
		{
			LastAppliedTransactionId = TransactionId;
			LastAppliedSummary = FString::Printf(TEXT("%d operation(s), %d asset(s)"), Operations->Num(), AffectedAssets.Num());
			LastAppliedBackupFiles = TransactionBackupFiles;
			LastAppliedAffectedAssets = AffectedAssets;
			bLastAppliedSaved = bSaved;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.bridge-edit-apply.v1"));
		Result->SetStringField(TEXT("transaction_id"), TransactionId);
		Result->SetBoolField(TEXT("applied"), bAllOk);
		Result->SetBoolField(TEXT("validation_ok"), bValidationOk);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetArrayField(TEXT("saved_file_hashes"), SavedFileHashes);
		Result->SetStringField(TEXT("backup_directory"), BackupDirectory);
		Result->SetStringField(TEXT("journal_path"), JournalPath);
		Result->SetStringField(TEXT("failure_message"), FailureMessage);
		Result->SetArrayField(TEXT("affected_assets"), StringArrayToJsonValues(AffectedAssets));
		Result->SetArrayField(TEXT("operations"), OperationResults);
		Result->SetArrayField(TEXT("compile"), CompileResults);
		Result->SetStringField(TEXT("refresh_request_path"), RefreshRequestPath);
		Result->SetStringField(TEXT("refresh_error"), RefreshError);
		Result->SetStringField(TEXT("rollback_strategy"), bSaved ? TEXT("file_backup_restore_and_package_reload") : TEXT("editor_transaction_undo"));

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), bAllOk);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), bAllOk ? (bValidationOk ? EmptyJsonArray() : DiagnosticsArray(TEXT("UEPI_EDIT_VALIDATE_FAILED"), TEXT("warning"), FailureMessage)) : DiagnosticsArray(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("error"), FailureMessage));
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::EditValidateResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params) const
	{
		const TSharedPtr<FJsonObject> Plan = JsonObjectField(Params, TEXT("plan"));
		TArray<FString> Assets;
		if (Plan.IsValid())
		{
			if (const TArray<TSharedPtr<FJsonValue>>* AssetValues = JsonArray(Plan, TEXT("affected_assets")))
			{
				for (const TSharedPtr<FJsonValue>& Value : *AssetValues)
				{
					FString AssetPath;
					if (Value.IsValid() && Value->TryGetString(AssetPath))
					{
						Assets.AddUnique(NormalizeBlueprintObjectPath(AssetPath));
					}
				}
			}
		}
		if (Assets.Num() == 0)
		{
			const FString AssetPath = NormalizeBlueprintObjectPath(JsonString(Params, TEXT("asset")));
			if (!AssetPath.IsEmpty())
			{
				Assets.Add(AssetPath);
			}
		}

		TArray<TSharedPtr<FJsonValue>> ValidationResults;
		bool bAllOk = true;
		for (const FString& AssetPath : Assets)
		{
			UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
			FUEPIValidationResult Validation = FUEPIValidatorRegistry::Get().Validate(Object);
			if (Validation.Asset.IsEmpty()) Validation.Asset = AssetPath;
			TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetBoolField(TEXT("ok"), Validation.bOk);
			Value->SetStringField(TEXT("asset"), Validation.Asset);
			Value->SetStringField(TEXT("class"), Validation.ClassPath);
			Value->SetStringField(TEXT("validator"), Validation.Validator);
			Value->SetStringField(TEXT("message"), Validation.Message);
			ValidationResults.Add(MakeShared<FJsonValueObject>(Value));
			bAllOk &= Validation.bOk;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.bridge-edit-validate.v2"));
		Result->SetStringField(TEXT("transaction_id"), JsonString(Params, TEXT("transaction_id")));
		Result->SetBoolField(TEXT("validated"), bAllOk);
		Result->SetArrayField(TEXT("assets"), StringArrayToJsonValues(Assets));
		Result->SetArrayField(TEXT("validations"), ValidationResults);

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), bAllOk);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), bAllOk ? EmptyJsonArray() : DiagnosticsArray(TEXT("UEPI_EDIT_VALIDATE_FAILED"), TEXT("error"), TEXT("One or more typed target validations failed.")));
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::EditRollbackResult(const FString& RequestId, const TSharedPtr<FJsonObject>& Params)
	{
		const FString TransactionId = JsonString(Params, TEXT("transaction_id"));
		if (TransactionId.IsEmpty() || TransactionId != LastAppliedTransactionId)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_ROLLBACK_NOT_MATCHING_LAST_TRANSACTION"), TEXT("Rollback is only allowed for the last UEPI-applied transaction in this editor session."));
		}
		if (!GEditor)
		{
			return ErrorResponse(RequestId, TEXT("UEPI_EDIT_EDITOR_UNAVAILABLE"), TEXT("GEditor is not available."));
		}
		const bool bUndone = GEditor->UndoTransaction(false);
		bool bFilesRestored = true;
		FString RestoreError;
		if (bLastAppliedSaved)
		{
			bFilesRestored = FUEPIBackupService::Restore(LastAppliedBackupFiles, LastAppliedAffectedAssets, RestoreError);
		}
		FString JournalPath;
		FString JournalError;
		FUEPITransactionJournal::Write(TransactionId, (bUndone && bFilesRestored) ? TEXT("rolled_back") : TEXT("rollback_failed"), LastAppliedAffectedAssets, LastAppliedBackupFiles, false, RestoreError, JournalPath, JournalError);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.bridge-edit-rollback.v1"));
		Result->SetStringField(TEXT("transaction_id"), TransactionId);
		Result->SetStringField(TEXT("summary"), LastAppliedSummary);
		Result->SetBoolField(TEXT("undone"), bUndone);
		Result->SetBoolField(TEXT("files_restored"), bFilesRestored);
		Result->SetStringField(TEXT("journal_path"), JournalPath);
		if (bUndone && bFilesRestored)
		{
			LastAppliedTransactionId.Reset();
			LastAppliedSummary.Reset();
			LastAppliedBackupFiles.Reset();
			LastAppliedAffectedAssets.Reset();
			bLastAppliedSaved = false;
		}
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), RequestId);
		Response->SetBoolField(TEXT("ok"), bUndone && bFilesRestored);
		Response->SetObjectField(TEXT("result"), Result);
		Response->SetArrayField(TEXT("diagnostics"), (bUndone && bFilesRestored) ? EmptyJsonArray() : DiagnosticsArray(TEXT("UEPI_EDIT_ROLLBACK_FAILED"), TEXT("error"), TEXT("Editor undo or persisted file restoration failed.")));
		return Response;
	}

	TSharedRef<FJsonObject> FUEPIEditorCommandBridge::MakeSessionObject(const FString& State) const
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema_version"), FUEPIBridgeProtocol::SessionSchemaVersion());
		Root->SetBoolField(TEXT("active"), State.Equals(TEXT("active"), ESearchCase::IgnoreCase));
		Root->SetStringField(TEXT("state"), State);
		Root->SetStringField(TEXT("session_id"), SessionId);
		Root->SetStringField(TEXT("editor_session_id"), SessionId);
		Root->SetStringField(TEXT("host"), TEXT("127.0.0.1"));
		Root->SetNumberField(TEXT("port"), Port);
		Root->SetStringField(TEXT("protocol"), FUEPIBridgeProtocol::ProtocolName());
		Root->SetBoolField(TEXT("transport_ready"), Listener.IsValid() && Listener->IsActive());
		Root->SetStringField(TEXT("implementation"), TEXT("tcp_length_prefixed_json"));
		Root->SetStringField(TEXT("project_name"), FApp::GetProjectName());
		Root->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		Root->SetStringField(TEXT("canonical_project_file"), UEPICanonicalProjectFile());
		Root->SetStringField(TEXT("project_binding_id"), UEPIProjectBindingId());
		Root->SetStringField(TEXT("project_root"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		Root->SetStringField(TEXT("store_root"), UEPIStoreRoot());
		Root->SetStringField(TEXT("session_path"), FPaths::ConvertRelativePathToFull(SessionPath));
		Root->SetStringField(TEXT("token_path"), FPaths::ConvertRelativePathToFull(TokenPath));
		Root->SetStringField(TEXT("token_hash"), FMD5::HashAnsiString(*Token));
		Root->SetNumberField(TEXT("pid"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
		Root->SetStringField(TEXT("started_at"), FDateTime::UtcNow().ToIso8601());
		Root->SetStringField(TEXT("last_heartbeat"), FDateTime::UtcNow().ToIso8601());
		Root->SetStringField(TEXT("heartbeat_at"), FDateTime::UtcNow().ToIso8601());
		const UUEPISettings* Settings = GetDefault<UUEPISettings>();
		Root->SetBoolField(TEXT("allow_save"), Settings && Settings->bAllowSavingPackages);
		Root->SetBoolField(TEXT("allow_pie"), Settings && Settings->bAllowPIEControl);
		Root->SetBoolField(TEXT("allow_runtime_invoke"), Settings && Settings->bAllowRuntimeInvoke);
		TSharedRef<FJsonObject> Bridge = MakeShared<FJsonObject>();
		Bridge->SetStringField(TEXT("host"), TEXT("127.0.0.1"));
		Bridge->SetNumberField(TEXT("port"), Port);
		Bridge->SetStringField(TEXT("protocol"), FUEPIBridgeProtocol::ProtocolName());
		Root->SetObjectField(TEXT("bridge"), Bridge);
		TArray<FString> Capabilities = FUEPIBridgeProtocol::ReadCapabilities();
		Capabilities.Append(FUEPIBridgeProtocol::WriteCapabilities());
		Root->SetArrayField(TEXT("capabilities"), StringArrayToJsonValues(Capabilities));
		return Root;
	}

	bool FUEPIEditorCommandBridge::WriteSessionObject(const FString& State, FString& OutError) const
	{
		if (SessionPath.IsEmpty())
		{
			OutError = TEXT("UEPI bridge session path is empty.");
			return false;
		}

		const TSharedRef<FJsonObject> SessionObject = MakeSessionObject(State);
		const FString SessionText = JsonObjectToString(SessionObject);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SessionPath), true);
		if (!FFileHelper::SaveStringToFile(SessionText, *SessionPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write UEPI bridge session file: %s"), *SessionPath);
			return false;
		}
		const FString GlobalSessionPath = UEPIGlobalBridgeSessionPath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(GlobalSessionPath), true);
		FFileHelper::SaveStringToFile(SessionText, *GlobalSessionPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		OutError.Reset();
		return true;
	}
}
