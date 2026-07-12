#include "Operations/UEPIBlueprintOperations.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_Slot.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "InputCoreTypes.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_InputKey.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Reflection/UEPIPropertyCodec.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::ProjectIntelligence
{
	namespace BlueprintOperationsPrivate
	{
		FString JsonString(const FJsonObject& Object, const TCHAR* Field, const FString& Default = FString())
		{
			FString Value; return Object.TryGetStringField(Field, Value) ? Value : Default;
		}

		bool JsonBool(const FJsonObject& Object, const TCHAR* Field, bool bDefault = false)
		{
			bool Value = false; return Object.TryGetBoolField(Field, Value) ? Value : bDefault;
		}

		const TSharedPtr<FJsonObject> JsonObject(const FJsonObject& Object, const TCHAR* Field)
		{
			const TSharedPtr<FJsonObject>* Value = nullptr; return Object.TryGetObjectField(Field, Value) && Value ? *Value : nullptr;
		}

		FUEPIEditResult Failure(const FString& Code, const FString& Message)
		{
			FUEPIEditResult Result; Result.ErrorCode = Code; Result.Message = Message; return Result;
		}

		FUEPIEditResult Success(const FString& Message, const TSharedPtr<FJsonObject>& Detail = nullptr)
		{
			FUEPIEditResult Result; Result.bOk = true; Result.Message = Message; Result.Result = Detail; return Result;
		}

		FString NormalizeObjectPath(FString Path)
		{
			Path = Path.TrimStartAndEnd();
			if (Path.IsEmpty() || Path.Contains(TEXT("."))) return Path;
			FString Package, Name; return Path.Split(TEXT("/"), &Package, &Name, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !Name.IsEmpty() ? Path + TEXT(".") + Name : Path;
		}

		FString ReferenceId(const FJsonObject& Params, const TCHAR* Field)
		{
			const TSharedPtr<FJsonObject> Reference = JsonObject(Params, Field); if (!Reference.IsValid()) return FString();
			FString Ref = JsonString(*Reference, TEXT("$ref")); int32 Hash = INDEX_NONE; if (Ref.FindChar(TEXT('#'), Hash)) Ref = Ref.Left(Hash); return Ref;
		}

		FString ResolveAsset(const FUEPIEditContext& Context, const FJsonObject& Params)
		{
			FString Path = JsonString(Params, TEXT("asset"), JsonString(Params, TEXT("blueprint")));
			if (Path.IsEmpty()) { FString Ref = ReferenceId(Params, TEXT("asset")); if (const FString* Resolved = Context.ResolvedAssets.Find(Ref)) Path = *Resolved; }
			return NormalizeObjectPath(Path);
		}

		UBlueprint* ResolveBlueprint(const FUEPIEditContext& Context, const FJsonObject& Params)
		{
			if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ResolveAsset(Context, Params)))
			{
				return Blueprint;
			}
			const FString Ref = ReferenceId(Params, TEXT("asset"));
			if (!Ref.IsEmpty() && Context.ExecutionState)
			{
				if (UObject* const* Probe = Context.ExecutionState->ObjectRefs.Find(Ref))
				{
					return Cast<UBlueprint>(*Probe);
				}
			}
			return nullptr;
		}

		FString EffectiveType(const FString& RegisteredType, const FJsonObject& Params)
		{
			if (RegisteredType == TEXT("blueprint.add_node"))
			{
				const FString Kind = JsonString(Params, TEXT("kind")).ToLower();
				if (Kind == TEXT("custom_event")) return TEXT("blueprint.add_event_node");
				if (Kind == TEXT("function_call")) return TEXT("blueprint.add_function_call_node");
				if (Kind == TEXT("variable_get")) return TEXT("blueprint.add_variable_get_node");
				if (Kind == TEXT("variable_set")) return TEXT("blueprint.add_variable_set_node");
				if (Kind == TEXT("branch")) return TEXT("blueprint.add_branch_node");
				if (Kind == TEXT("print_string")) return TEXT("blueprint.add_print_string_node");
			}
			if (RegisteredType == TEXT("animgraph.add_slot")) return TEXT("animgraph.add_slot_node");
			if (RegisteredType == TEXT("animgraph.connect_pose") || RegisteredType == TEXT("animgraph.connect_pose_pins")) return TEXT("blueprint.connect_pins");
			if (RegisteredType == TEXT("animgraph.disconnect_pose_pins")) return TEXT("blueprint.disconnect_pins");
			if (RegisteredType == TEXT("animgraph.remove_node")) return TEXT("blueprint.remove_node");
			if (RegisteredType == TEXT("animgraph.compile")) return TEXT("blueprint.compile");
			return RegisteredType;
		}

		FVector2D NodePosition(const FJsonObject& Params, int32 Index)
		{
			double X = Index * 260.0, Y = Index * 120.0; bool bX = false, bY = false;
			if (const TSharedPtr<FJsonObject> Position = JsonObject(Params, TEXT("position"))) { bX = Position->TryGetNumberField(TEXT("x"), X); bY = Position->TryGetNumberField(TEXT("y"), Y); }
			if (!bX) bX = Params.TryGetNumberField(TEXT("x"), X) || Params.TryGetNumberField(TEXT("node_pos_x"), X);
			if (!bY) bY = Params.TryGetNumberField(TEXT("y"), Y) || Params.TryGetNumberField(TEXT("node_pos_y"), Y);
			return FVector2D(X, Y);
		}

		UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& Requested)
		{
			if (!Blueprint) return nullptr; TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
			for (UEdGraph* Graph : Graphs) if (Graph && !Requested.IsEmpty() && Graph->GetName().Equals(Requested, ESearchCase::IgnoreCase)) return Graph;
			for (UEdGraph* Graph : Blueprint->UbergraphPages) if (Graph && (Requested.IsEmpty() || Graph->GetName().Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))) return Graph;
			return Graphs.Num() > 0 ? Graphs[0] : nullptr;
		}

		UEdGraph* ResolveGraph(UBlueprint* Blueprint, const FJsonObject& Params, FString& OutError)
		{
			const FString Name = JsonString(Params, TEXT("graph"), JsonString(Params, TEXT("graph_name"))); UEdGraph* Graph = FindGraph(Blueprint, Name);
			if (!Graph) OutError = Name.IsEmpty() ? TEXT("Blueprint graph operation could not find a default graph.") : FString::Printf(TEXT("Blueprint graph not found: %s"), *Name); return Graph;
		}

		FString GuidString(const UEdGraphNode* Node) { return Node ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString(); }

		TSharedRef<FJsonObject> NodeJson(const UEdGraphNode* Node, const UEdGraph* Graph)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); if (!Node) return Result;
			Result->SetStringField(TEXT("node_guid"), GuidString(Node)); Result->SetStringField(TEXT("node_id"), GuidString(Node)); Result->SetStringField(TEXT("graph"), Graph ? Graph->GetName() : FString()); Result->SetStringField(TEXT("class"), Node->GetClass()->GetName()); Result->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString()); Result->SetNumberField(TEXT("x"), Node->NodePosX); Result->SetNumberField(TEXT("y"), Node->NodePosY);
			TArray<TSharedPtr<FJsonValue>> Pins; for (const UEdGraphPin* Pin : Node->Pins) if (Pin) { TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("pin_id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens)); Item->SetStringField(TEXT("name"), Pin->PinName.ToString()); Item->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output")); Item->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString()); Item->SetStringField(TEXT("default_value"), Pin->DefaultValue); Pins.Add(MakeShared<FJsonValueObject>(Item)); } Result->SetArrayField(TEXT("pins"), Pins); return Result;
		}

		FString PinValue(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return FString(); FString Text; if (Value->TryGetString(Text)) return Text; double Number = 0; if (Value->TryGetNumber(Number)) return FString::SanitizeFloat(Number); bool b = false; if (Value->TryGetBool(b)) return b ? TEXT("true") : TEXT("false"); return FString();
		}

		FString PinAlias(const FString& Name)
		{
			const FString Lower = Name.TrimStartAndEnd().ToLower(); if (Lower == TEXT("exec") || Lower == TEXT("execute")) return UEdGraphSchema_K2::PN_Execute.ToString(); if (Lower == TEXT("then") || Lower == TEXT("true")) return UEdGraphSchema_K2::PN_Then.ToString(); if (Lower == TEXT("else") || Lower == TEXT("false")) return UEdGraphSchema_K2::PN_Else.ToString(); if (Lower == TEXT("condition") || Lower == TEXT("cond")) return UEdGraphSchema_K2::PN_Condition.ToString(); if (Lower == TEXT("return") || Lower == TEXT("return_value") || Lower == TEXT("returnvalue")) return UEdGraphSchema_K2::PN_ReturnValue.ToString(); if (Lower == TEXT("self") || Lower == TEXT("target")) return UEdGraphSchema_K2::PN_Self.ToString(); return Name.TrimStartAndEnd();
		}

		bool DirectionMatches(const UEdGraphPin* Pin, const FString& Direction)
		{
			if (!Pin || Direction.IsEmpty()) return true; const FString Lower = Direction.ToLower(); if (Lower == TEXT("input") || Lower == TEXT("in")) return Pin->Direction == EGPD_Input; if (Lower == TEXT("output") || Lower == TEXT("out")) return Pin->Direction == EGPD_Output; return true;
		}

		UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& Text, const FString& Direction = FString())
		{
			if (!Node || Text.TrimStartAndEnd().IsEmpty()) return nullptr; FGuid Guid;
			if (FGuid::Parse(Text, Guid)) for (UEdGraphPin* Pin : Node->Pins) if (Pin && Pin->PinId == Guid && DirectionMatches(Pin, Direction)) return Pin;
			const FString Name = PinAlias(Text); for (UEdGraphPin* Pin : Node->Pins) if (Pin && DirectionMatches(Pin, Direction) && (Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase) || Pin->GetDisplayName().ToString().Equals(Name, ESearchCase::IgnoreCase))) return Pin; return nullptr;
		}

		void ApplyDefaults(UEdGraphNode* Node, const FJsonObject& Params)
		{
			const TSharedPtr<FJsonObject> Defaults = JsonObject(Params, TEXT("defaults")); if (!Node || !Defaults.IsValid()) return; const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>(); for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Defaults->Values) if (UEdGraphPin* Pin = FindPin(Node, Pair.Key)) Schema->TrySetDefaultValue(*Pin, PinValue(Pair.Value), true);
		}

		UEdGraphNode* FindNode(UEdGraph* Graph, const FString& GuidText)
		{
			if (!Graph || GuidText.IsEmpty()) return nullptr; FGuid Guid; const bool bGuid = FGuid::Parse(GuidText, Guid); const FString Digits = GuidText.Replace(TEXT("-"), TEXT("")); for (UEdGraphNode* Node : Graph->Nodes) if (Node && ((bGuid && Node->NodeGuid == Guid) || Node->NodeGuid.ToString(EGuidFormats::Digits).Equals(Digits, ESearchCase::IgnoreCase))) return Node; return nullptr;
		}

		void RegisterRefs(const FUEPIEditContext& Context, UObject* Object, UClass* PlannedClass = nullptr)
		{
			if (!Context.ExecutionState) return; for (const FString& Ref : Context.OperationReferences) if (!Ref.IsEmpty()) { Context.ExecutionState->PlannedObjectRefs.Add(Ref); if (Object) Context.ExecutionState->ObjectRefs.Add(Ref, Object); if (PlannedClass) Context.ExecutionState->PlannedObjectClasses.Add(Ref, PlannedClass); }
		}

		FString NodeRef(const FJsonObject& Params)
		{
			return JsonString(Params, TEXT("node_ref"), JsonString(Params, TEXT("ref")));
		}

		UEdGraphNode* ResolveNode(const FUEPIEditContext& Context, UEdGraph* Graph, const FJsonObject& Params, FString& OutError)
		{
			const FString Ref = NodeRef(Params); UEdGraphNode* Node = nullptr; if (!Ref.IsEmpty() && Context.ExecutionState) if (UObject* const* Found = Context.ExecutionState->ObjectRefs.Find(Ref)) Node = Cast<UEdGraphNode>(*Found); if (!Node) Node = FindNode(Graph, JsonString(Params, TEXT("node_guid"), JsonString(Params, TEXT("node_id")))); if (!Node || Node->GetGraph() != Graph) OutError = TEXT("Node reference was not found in the requested graph."); return Node;
		}

		bool IsPlannedRef(const FUEPIEditContext& Context, const FString& Ref)
		{
			return !Ref.IsEmpty() && Context.ExecutionState && Context.ExecutionState->PlannedObjectRefs.Contains(Ref);
		}

		UEdGraphPin* ResolveEndpoint(const FUEPIEditContext& Context, UEdGraph* Graph, const FJsonObject& Params, const TCHAR* Prefix, const FString& DefaultDirection, UEdGraphNode*& OutNode, FString& OutError)
		{
			OutNode = nullptr; const FString PrefixText(Prefix); TSharedPtr<FJsonObject> Endpoint = JsonObject(Params, Prefix); if (!Endpoint.IsValid()) Endpoint = JsonObject(Params, PrefixText == TEXT("source") ? TEXT("from") : TEXT("to"));
			FString Guid = Endpoint.IsValid() ? JsonString(*Endpoint, TEXT("node_guid"), JsonString(*Endpoint, TEXT("node_id"))) : FString(); FString Ref = Endpoint.IsValid() ? JsonString(*Endpoint, TEXT("node_ref"), JsonString(*Endpoint, TEXT("ref"))) : FString(); FString Pin = Endpoint.IsValid() ? JsonString(*Endpoint, TEXT("pin_name"), JsonString(*Endpoint, TEXT("pin"), JsonString(*Endpoint, TEXT("pin_id")))) : FString();
			if (Guid.IsEmpty()) Guid = JsonString(Params, *(PrefixText + TEXT("_node_guid")), JsonString(Params, *(PrefixText + TEXT("_node_id")))); if (Ref.IsEmpty()) Ref = JsonString(Params, *(PrefixText + TEXT("_node_ref")), JsonString(Params, *(PrefixText + TEXT("_ref")))); if (Pin.IsEmpty()) Pin = JsonString(Params, *(PrefixText + TEXT("_pin_name")), JsonString(Params, *(PrefixText + TEXT("_pin")), JsonString(Params, *(PrefixText + TEXT("_pin_id")))));
			if (Ref.IsEmpty() && Guid.IsEmpty() || Pin.IsEmpty()) { OutError = FString::Printf(TEXT("%s endpoint requires a node and pin."), Prefix); return nullptr; }
			if (!Ref.IsEmpty() && Context.ExecutionState) if (UObject* const* Found = Context.ExecutionState->ObjectRefs.Find(Ref)) OutNode = Cast<UEdGraphNode>(*Found); if (!OutNode) OutNode = FindNode(Graph, Guid); if (!OutNode || OutNode->GetGraph() != Graph) { OutError = FString::Printf(TEXT("%s node was not found in the requested graph."), Prefix); return nullptr; }
			const FString Direction = Endpoint.IsValid() ? JsonString(*Endpoint, TEXT("direction"), DefaultDirection) : DefaultDirection; UEdGraphPin* Result = FindPin(OutNode, Pin, Direction); if (!Result) OutError = FString::Printf(TEXT("%s pin was not found: %s"), Prefix, *Pin); return Result;
		}

		bool BuildPinType(const FString& Requested, FEdGraphPinType& OutType, FString& OutError)
		{
			const FString Type = Requested.TrimStartAndEnd().ToLower(); OutType = FEdGraphPinType();
			if (Type == TEXT("bool") || Type == TEXT("boolean")) OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			else if (Type == TEXT("byte")) OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			else if (Type == TEXT("int") || Type == TEXT("integer")) OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
			else if (Type == TEXT("int64") || Type == TEXT("long")) OutType.PinCategory = UEdGraphSchema_K2::PC_Int64;
			else if (Type == TEXT("float") || Type == TEXT("real")) OutType.PinCategory = UEdGraphSchema_K2::PC_Float;
			else if (Type == TEXT("double")) OutType.PinCategory = UEdGraphSchema_K2::PC_Double;
			else if (Type == TEXT("string")) OutType.PinCategory = UEdGraphSchema_K2::PC_String;
			else if (Type == TEXT("name")) OutType.PinCategory = UEdGraphSchema_K2::PC_Name;
			else if (Type == TEXT("text")) OutType.PinCategory = UEdGraphSchema_K2::PC_Text;
			else { OutError = FString::Printf(TEXT("Unsupported simple Blueprint variable type: %s"), *Requested); return false; } return true;
		}

		UClass* ComponentClass(const FString& Requested)
		{
			const FString Name = Requested.TrimStartAndEnd(); if (Name.IsEmpty() || Name == TEXT("SceneComponent") || Name == TEXT("USceneComponent")) return USceneComponent::StaticClass(); if (Name == TEXT("StaticMeshComponent") || Name == TEXT("UStaticMeshComponent")) return UStaticMeshComponent::StaticClass(); if (Name == TEXT("ActorComponent") || Name == TEXT("UActorComponent")) return UActorComponent::StaticClass(); return StaticLoadClass(UActorComponent::StaticClass(), nullptr, *Name);
		}

		bool HasVariable(const UBlueprint* Blueprint, const FName Name)
		{
			return Blueprint && !Name.IsNone() && (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name) != INDEX_NONE || (Blueprint->SkeletonGeneratedClass && Blueprint->SkeletonGeneratedClass->FindPropertyByName(Name)) || (Blueprint->GeneratedClass && Blueprint->GeneratedClass->FindPropertyByName(Name)));
		}

		bool HasVariableOrPlanned(const FUEPIEditContext& Context, const UBlueprint* Blueprint, const FName Name)
		{
			if (HasVariable(Blueprint, Name)) return true; if (!Context.ExecutionState || !Blueprint) return false; const TSet<FName>* Names = Context.ExecutionState->PlannedVariables.Find(Blueprint->GetPathName()); return Names && Names->Contains(Name);
		}

		UClass* ResolveClass(const FString& Requested)
		{
			const FString Text = Requested.TrimStartAndEnd(); if (Text.IsEmpty()) return nullptr; if (Text == TEXT("KismetSystemLibrary") || Text == TEXT("UKismetSystemLibrary") || Text == TEXT("/Script/Engine.KismetSystemLibrary")) return UKismetSystemLibrary::StaticClass(); if (UClass* Class = LoadObject<UClass>(nullptr, *Text)) return Class; if (UClass* Class = StaticLoadClass(UObject::StaticClass(), nullptr, *Text)) return Class; for (TObjectIterator<UClass> It; It; ++It) if (*It && ((*It)->GetPathName().Equals(Text, ESearchCase::IgnoreCase) || (*It)->GetName().Equals(Text, ESearchCase::IgnoreCase))) return *It; return nullptr;
		}

		UFunction* ResolveFunction(const FUEPIEditContext& Context, UBlueprint* Blueprint, const FJsonObject& Params, FName& OutSelf, FString& OutError)
		{
			OutSelf = NAME_None; FString Name = JsonString(Params, TEXT("function_name"), JsonString(Params, TEXT("name"))); FString ClassText = JsonString(Params, TEXT("function_class"), JsonString(Params, TEXT("class"))); const FString Path = JsonString(Params, TEXT("function_path"), JsonString(Params, TEXT("function")));
			if (!Path.IsEmpty()) { if (UFunction* Direct = FindObject<UFunction>(nullptr, *Path)) return Direct; FString ParsedClass, ParsedName; if (Path.Split(TEXT(":"), &ParsedClass, &ParsedName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || Path.Split(TEXT("."), &ParsedClass, &ParsedName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) { if (ClassText.IsEmpty()) ClassText = ParsedClass; if (Name.IsEmpty()) Name = ParsedName; } else if (Name.IsEmpty()) Name = Path; }
			if (Name.IsEmpty()) { OutError = TEXT("Function call operation requires function_name or function_path."); return nullptr; }
			if (!ClassText.IsEmpty()) { UClass* Class = ResolveClass(ClassText); if (!Class) { OutError = FString::Printf(TEXT("Function class was not found: %s"), *ClassText); return nullptr; } if (UFunction* Function = Class->FindFunctionByName(FName(*Name))) return Function; OutError = FString::Printf(TEXT("Function %s was not found on class %s."), *Name, *Class->GetPathName()); return nullptr; }
			if (Blueprint && Blueprint->SkeletonGeneratedClass) if (UFunction* Function = Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*Name))) return Function; if (Blueprint && Blueprint->GeneratedClass) if (UFunction* Function = Blueprint->GeneratedClass->FindFunctionByName(FName(*Name))) return Function;
			if (Context.ExecutionState && Blueprint) { const TSet<FName>* Planned = Context.ExecutionState->PlannedFunctions.Find(Blueprint->GetPathName()); if (Planned && Planned->Contains(FName(*Name))) { OutSelf = FName(*Name); return nullptr; } }
			OutError = FString::Printf(TEXT("Function was not found on the Blueprint or a requested class: %s"), *Name); return nullptr;
		}

		TSharedPtr<FJsonObject> PropertyWrites(const FJsonObject& Params, FString& OutError)
		{
			const TSharedPtr<FJsonObject> Direct = JsonObject(Params, TEXT("properties")); if (Direct.IsValid()) return Direct; const TArray<TSharedPtr<FJsonValue>>* Writes = nullptr; if (!Params.TryGetArrayField(TEXT("writes"), Writes) || !Writes) { OutError = TEXT("Component property operation requires properties or writes."); return nullptr; }
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); for (const TSharedPtr<FJsonValue>& Item : *Writes) { const TSharedPtr<FJsonObject> Write = Item.IsValid() ? Item->AsObject() : nullptr; const FString Path = Write.IsValid() ? JsonString(*Write, TEXT("path")) : FString(); const TSharedPtr<FJsonValue> Value = Write.IsValid() ? Write->TryGetField(TEXT("value")) : nullptr; if (Path.IsEmpty() || !Value.IsValid()) { OutError = TEXT("Each property write requires path and value."); return nullptr; } TSharedRef<FJsonObject> Encoded = MakeShared<FJsonObject>(); Encoded->SetStringField(TEXT("mode"), JsonString(*Write, TEXT("mode"), TEXT("replace"))); Encoded->SetField(TEXT("value"), Value); Result->SetObjectField(Path, Encoded); } return Result;
		}

		void DecodeWrite(const TSharedPtr<FJsonValue>& Encoded, FString& OutMode, TSharedPtr<FJsonValue>& OutValue)
		{
			const TSharedPtr<FJsonObject> Object = Encoded.IsValid() && Encoded->Type == EJson::Object ? Encoded->AsObject() : nullptr; OutMode = Object.IsValid() ? JsonString(*Object, TEXT("mode"), TEXT("replace")) : TEXT("replace"); const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(TEXT("value")) : nullptr; OutValue = Value.IsValid() ? Value : Encoded;
		}

		bool SetSimpleProperty(UObject* Object, const FString& Name, const FJsonObject& Params, FString& OutError)
		{
			if (!Object) { OutError = TEXT("Property target object is null."); return false; } FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*Name)); if (!Property) { OutError = FString::Printf(TEXT("Property was not found on %s: %s"), *Object->GetClass()->GetName(), *Name); return false; } void* Ptr = Property->ContainerPtrToValuePtr<void>(Object); const TSharedPtr<FJsonValue> Value = Params.TryGetField(TEXT("value")); if (!Value.IsValid()) { OutError = TEXT("Property value is required."); return false; } return FUEPIPropertyCodec::WriteValue(Property, Ptr, Value, OutError);
		}

		FString BlueprintStatus(const UBlueprint* Blueprint)
		{
			if (!Blueprint) return TEXT("unknown"); if (const UEnum* Enum = StaticEnum<EBlueprintStatus>()) return Enum->GetNameStringByValue(static_cast<int64>(Blueprint->Status)); return FString::FromInt(static_cast<int32>(Blueprint->Status));
		}

		TSharedRef<FJsonObject> Compile(UBlueprint* Blueprint)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); if (!Blueprint) { Result->SetBoolField(TEXT("ok"), false); Result->SetStringField(TEXT("error"), TEXT("Blueprint was null.")); return Result; }
			FCompilerResultsLog Log; Log.SetSourcePath(Blueprint->GetPathName()); FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Log); TArray<TSharedPtr<FJsonValue>> Messages; const int32 Count = FMath::Min(Log.Messages.Num(), 40); for (int32 Index = 0; Index < Count; ++Index) { TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("severity"), Log.Messages[Index]->GetSeverity() == EMessageSeverity::Error ? TEXT("error") : Log.Messages[Index]->GetSeverity() == EMessageSeverity::Warning ? TEXT("warning") : TEXT("message")); Item->SetStringField(TEXT("message"), Log.Messages[Index]->ToText().ToString()); Messages.Add(MakeShared<FJsonValueObject>(Item)); } Result->SetBoolField(TEXT("ok"), Log.NumErrors == 0); Result->SetStringField(TEXT("asset"), Blueprint->GetPathName()); Result->SetStringField(TEXT("status"), BlueprintStatus(Blueprint)); Result->SetNumberField(TEXT("error_count"), Log.NumErrors); Result->SetNumberField(TEXT("warning_count"), Log.NumWarnings); Result->SetNumberField(TEXT("message_count"), Log.Messages.Num()); Result->SetArrayField(TEXT("messages"), Messages); return Result;
		}

		FString EndpointRef(const FJsonObject& Params, const TCHAR* Prefix)
		{
			const FString PrefixText(Prefix); TSharedPtr<FJsonObject> Endpoint = JsonObject(Params, Prefix); if (!Endpoint.IsValid()) Endpoint = JsonObject(Params, PrefixText == TEXT("source") ? TEXT("from") : TEXT("to")); return Endpoint.IsValid() ? JsonString(*Endpoint, TEXT("node_ref"), JsonString(*Endpoint, TEXT("ref"))) : JsonString(Params, *(PrefixText + TEXT("_node_ref")), JsonString(Params, *(PrefixText + TEXT("_ref"))));
		}

		bool NeedsGraph(const FString& Type)
		{
			return Type == TEXT("animgraph.add_slot_node") || Type == TEXT("animgraph.set_node_property") || Type == TEXT("blueprint.add_node") || Type == TEXT("blueprint.add_event_node") || Type == TEXT("blueprint.add_function_call_node") || Type == TEXT("blueprint.add_variable_get_node") || Type == TEXT("blueprint.add_variable_set_node") || Type == TEXT("blueprint.add_branch_node") || Type == TEXT("blueprint.add_print_string_node") || Type == TEXT("blueprint.connect_pins") || Type == TEXT("blueprint.set_pin_default") || Type == TEXT("blueprint.disconnect_pins") || Type == TEXT("blueprint.break_all_links") || Type == TEXT("blueprint.remove_node") || Type == TEXT("blueprint.move_node") || Type == TEXT("blueprint.set_node_comment");
		}

		class FUEPIBlueprintOperation final : public IUEPIEditOperation
		{
		public:
			explicit FUEPIBlueprintOperation(FUEPIEditOperationDescriptor InDescriptor) : Descriptor(MoveTemp(InDescriptor)) {}
			virtual FString GetOperationType() const override { return Descriptor.Name; }
			virtual FUEPIEditOperationDescriptor GetDescriptor() const override { return Descriptor; }
			virtual FUEPIEditResult Validate(const FUEPIEditContext& Context, const FJsonObject& Params) override { return Preview(Context, Params); }

			virtual FUEPIEditResult Preview(const FUEPIEditContext& Context, const FJsonObject& Params) override
			{
				if (!Context.bAllowBlueprintEdits) return Failure(TEXT("UEPI_EDIT_CAPABILITY_DISABLED"), TEXT("Blueprint edits are disabled in UEPI Project Settings."));
				const FString AssetPath = ResolveAsset(Context, Params); UBlueprint* Blueprint = ResolveBlueprint(Context, Params); if (!Blueprint) return Failure(TEXT("UEPI_EDIT_ASSET_NOT_FOUND"), FString::Printf(TEXT("Failed to load Blueprint asset or same-plan Blueprint probe: %s"), *AssetPath));
				const FString Type = EffectiveType(Descriptor.Name, Params); if (Descriptor.Name == TEXT("blueprint.add_node")) { const FString Kind = JsonString(Params, TEXT("kind")).ToLower(); if (!(Kind == TEXT("custom_event") || Kind == TEXT("function_call") || Kind == TEXT("variable_get") || Kind == TEXT("variable_set") || Kind == TEXT("branch") || Kind == TEXT("print_string") || Kind == TEXT("make_struct") || Kind == TEXT("input_key"))) return Failure(TEXT("UEPI_EDIT_OPERATION_UNSUPPORTED"), FString::Printf(TEXT("Unsupported blueprint.add_node kind: %s"), *Kind)); }
				FString Error; UEdGraph* Graph = NeedsGraph(Type) ? ResolveGraph(Blueprint, Params, Error) : nullptr; if (NeedsGraph(Type) && !Graph) return Failure(TEXT("UEPI_EDIT_GRAPH_NOT_FOUND"), Error);

				if (Type == TEXT("blueprint.add_variable"))
				{
					const FName Name(*JsonString(Params, TEXT("name"))); FEdGraphPinType PinType; if (Name.IsNone()) return Failure(TEXT("UEPI_EDIT_PARAM_REQUIRED"), TEXT("blueprint.add_variable requires params.name.")); if (!BuildPinType(JsonString(Params, TEXT("pin_type"), JsonString(Params, TEXT("type_name"), TEXT("float"))), PinType, Error)) return Failure(TEXT("UEPI_EDIT_TYPE_UNSUPPORTED"), Error); if (HasVariableOrPlanned(Context, Blueprint, Name)) return Failure(TEXT("UEPI_EDIT_TARGET_ALREADY_EXISTS"), FString::Printf(TEXT("Blueprint variable already exists: %s"), *Name.ToString())); if (Context.ExecutionState) Context.ExecutionState->PlannedVariables.FindOrAdd(Blueprint->GetPathName()).Add(Name);
				}
				else if (Type == TEXT("blueprint.set_variable_default") || Type == TEXT("blueprint.add_variable_get_node") || Type == TEXT("blueprint.add_variable_set_node"))
				{
					const FName Name(*JsonString(Params, Type == TEXT("blueprint.set_variable_default") ? TEXT("name") : TEXT("variable"), JsonString(Params, TEXT("name")))); if (!HasVariableOrPlanned(Context, Blueprint, Name)) return Failure(TEXT("UEPI_EDIT_TARGET_NOT_FOUND"), FString::Printf(TEXT("Blueprint variable was not found: %s"), *Name.ToString()));
				}
				else if (Type == TEXT("blueprint.add_component"))
				{
					const FName Name(*JsonString(Params, TEXT("name"), TEXT("UEPIComponent"))); UClass* Class = ComponentClass(JsonString(Params, TEXT("component_class"))); if (!Blueprint->SimpleConstructionScript || !Class || !Class->IsChildOf(UActorComponent::StaticClass())) return Failure(TEXT("UEPI_EDIT_COMPONENT_CLASS_INVALID"), TEXT("A SimpleConstructionScript and ActorComponent class are required.")); if (Blueprint->SimpleConstructionScript->FindSCSNode(Name) || (Context.ExecutionState && Context.ExecutionState->PlannedComponents.FindOrAdd(Blueprint->GetPathName()).Contains(Name))) return Failure(TEXT("UEPI_EDIT_TARGET_ALREADY_EXISTS"), FString::Printf(TEXT("Component already exists: %s"), *Name.ToString())); if (Context.ExecutionState) Context.ExecutionState->PlannedComponents.FindOrAdd(Blueprint->GetPathName()).Add(Name, Class);
				}
				else if (Type == TEXT("blueprint.set_component_property") || Type == TEXT("blueprint.set_component_properties"))
				{
					const FName Name(*JsonString(Params, TEXT("component"), JsonString(Params, TEXT("component_name")))); USCS_Node* Node = Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->FindSCSNode(Name) : nullptr; UClass* PlannedClass = nullptr; if (!Node && Context.ExecutionState) if (TMap<FName, UClass*>* Components = Context.ExecutionState->PlannedComponents.Find(Blueprint->GetPathName())) if (UClass** Found = Components->Find(Name)) PlannedClass = *Found; UObject* Probe = Node && Node->ComponentTemplate ? DuplicateObject(Node->ComponentTemplate, GetTransientPackage()) : PlannedClass ? NewObject<UObject>(GetTransientPackage(), PlannedClass) : nullptr; if (!Probe) return Failure(TEXT("UEPI_EDIT_TARGET_NOT_FOUND"), FString::Printf(TEXT("Component template not found: %s"), *Name.ToString()));
					if (Type == TEXT("blueprint.set_component_property")) { const FString Property = JsonString(Params, TEXT("property")); if (Property.IsEmpty() || !SetSimpleProperty(Probe, Property, Params, Error)) return Failure(TEXT("UEPI_EDIT_PROPERTY_TYPE_MISMATCH"), Error.IsEmpty() ? TEXT("component and property are required.") : Error); }
					else { const TSharedPtr<FJsonObject> Writes = PropertyWrites(Params, Error); if (!Writes.IsValid()) return Failure(TEXT("UEPI_EDIT_PROPERTY_PREFLIGHT_FAILED"), Error); for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Writes->Values) { FString Mode; TSharedPtr<FJsonValue> Value, Before, After; DecodeWrite(Pair.Value, Mode, Value); if (!FUEPIPropertyCodec::SetPropertyPath(Probe, Pair.Key, Value, Before, After, Error, Mode)) return Failure(TEXT("UEPI_EDIT_PROPERTY_TYPE_MISMATCH"), Error); } }
				}
				else if (Type == TEXT("blueprint.create_function"))
				{
					const FName Name(*JsonString(Params, TEXT("name"), JsonString(Params, TEXT("function_name")))); const TSet<FName>* Planned = Context.ExecutionState ? Context.ExecutionState->PlannedFunctions.Find(Blueprint->GetPathName()) : nullptr; if (Name.IsNone()) return Failure(TEXT("UEPI_EDIT_PARAM_REQUIRED"), TEXT("Function name is required.")); if (FindGraph(Blueprint, Name.ToString()) || (Planned && Planned->Contains(Name))) return Failure(TEXT("UEPI_EDIT_TARGET_ALREADY_EXISTS"), FString::Printf(TEXT("Blueprint function already exists: %s"), *Name.ToString())); if (Context.ExecutionState) Context.ExecutionState->PlannedFunctions.FindOrAdd(Blueprint->GetPathName()).Add(Name);
				}
				else if (Type == TEXT("blueprint.add_event_node"))
				{
					const FString Kind = JsonString(Params, TEXT("event_kind"), JsonString(Params, TEXT("kind"), TEXT("custom_event"))); const FName Name(*JsonString(Params, TEXT("event_name"), JsonString(Params, TEXT("name")))); if (!(Kind.Equals(TEXT("custom"), ESearchCase::IgnoreCase) || Kind.Equals(TEXT("custom_event"), ESearchCase::IgnoreCase)) || Name.IsNone()) return Failure(TEXT("UEPI_EDIT_PARAM_INVALID"), TEXT("A custom event name is required.")); if (FBlueprintEditorUtils::FindCustomEventNode(Blueprint, Name)) return Failure(TEXT("UEPI_EDIT_TARGET_ALREADY_EXISTS"), FString::Printf(TEXT("Custom event already exists: %s"), *Name.ToString())); if (Context.ExecutionState) Context.ExecutionState->PlannedFunctions.FindOrAdd(Blueprint->GetPathName()).Add(Name); RegisterRefs(Context, nullptr, UK2Node_CustomEvent::StaticClass());
				}
				else if (Type == TEXT("blueprint.add_function_call_node")) { FName Self; if (!ResolveFunction(Context, Blueprint, Params, Self, Error) && Self.IsNone()) return Failure(TEXT("UEPI_EDIT_FUNCTION_NOT_FOUND"), Error); RegisterRefs(Context, nullptr, UK2Node_CallFunction::StaticClass()); }
				else if (Type == TEXT("blueprint.add_variable_get_node")) RegisterRefs(Context, nullptr, UK2Node_VariableGet::StaticClass());
				else if (Type == TEXT("blueprint.add_variable_set_node")) RegisterRefs(Context, nullptr, UK2Node_VariableSet::StaticClass());
				else if (Type == TEXT("blueprint.add_branch_node")) RegisterRefs(Context, nullptr, UK2Node_IfThenElse::StaticClass());
				else if (Type == TEXT("blueprint.add_print_string_node")) RegisterRefs(Context, nullptr, UK2Node_CallFunction::StaticClass());
				else if (Type == TEXT("blueprint.add_node"))
				{
					const FString Kind = JsonString(Params, TEXT("kind")).ToLower(); UClass* NodeClass = nullptr; if (Kind == TEXT("make_struct")) { UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *JsonString(Params, TEXT("struct_path"))); if (!Struct || !UK2Node_MakeStruct::CanBeMade(Struct)) return Failure(TEXT("UEPI_EDIT_STRUCT_INVALID"), TEXT("Struct cannot be used by a Make Struct node.")); NodeClass = UK2Node_MakeStruct::StaticClass(); } else if (Kind == TEXT("input_key")) { if (!FKey(FName(*JsonString(Params, TEXT("key")))).IsValid()) return Failure(TEXT("UEPI_EDIT_KEY_INVALID"), TEXT("Input key is invalid.")); NodeClass = UK2Node_InputKey::StaticClass(); } RegisterRefs(Context, nullptr, NodeClass);
				}
				else if (Type == TEXT("animgraph.add_slot_node")) { UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Blueprint); const FName Slot(*JsonString(Params, TEXT("slot_name"), TEXT("DefaultSlot"))); if (!AnimBlueprint || !AnimBlueprint->TargetSkeleton || !AnimBlueprint->TargetSkeleton->ContainsSlotName(Slot)) return Failure(TEXT("UEPI_EDIT_ANIM_SLOT_INVALID"), TEXT("Slot is not registered on the AnimBlueprint target Skeleton.")); RegisterRefs(Context, nullptr, UAnimGraphNode_Slot::StaticClass()); }
				else if (Type == TEXT("blueprint.connect_pins"))
				{
					for (const TCHAR* Prefix : { TEXT("source"), TEXT("target") })
					{
						const FString Ref = EndpointRef(Params, Prefix); if (IsPlannedRef(Context, Ref)) continue; UEdGraphNode* Node = nullptr; FString PinError; if (!ResolveEndpoint(Context, Graph, Params, Prefix, FString(Prefix) == TEXT("source") ? TEXT("output") : TEXT("input"), Node, PinError)) return Failure(TEXT("UEPI_EDIT_PIN_NOT_FOUND"), PinError);
					}
				}
				else if (Type == TEXT("blueprint.set_pin_default") || Type == TEXT("blueprint.disconnect_pins"))
				{
					const FString Ref = EndpointRef(Params, TEXT("target")); if (!IsPlannedRef(Context, Ref)) { UEdGraphNode* Node = nullptr; FString PinError; if (!ResolveEndpoint(Context, Graph, Params, TEXT("target"), TEXT("input"), Node, PinError)) return Failure(TEXT("UEPI_EDIT_PIN_NOT_FOUND"), PinError); }
				}
				else if (Type == TEXT("animgraph.set_node_property"))
				{
					const FString Ref = NodeRef(Params); UClass* NodeClass = nullptr; if (IsPlannedRef(Context, Ref) && Context.ExecutionState) { if (UClass** Found = Context.ExecutionState->PlannedObjectClasses.Find(Ref)) NodeClass = *Found; } else { FString NodeError; UEdGraphNode* Node = ResolveNode(Context, Graph, Params, NodeError); if (!Node) return Failure(TEXT("UEPI_EDIT_NODE_NOT_FOUND"), NodeError); NodeClass = Node->GetClass(); }
					FString Property = JsonString(Params, TEXT("property_path"), JsonString(Params, TEXT("property"))); FString Root = Property; int32 Separator = INDEX_NONE; if (Root.FindChar(TEXT('.'), Separator) || Root.FindChar(TEXT('['), Separator) || Root.FindChar(TEXT('{'), Separator)) Root = Root.Left(Separator); if (!NodeClass || Root.IsEmpty() || !NodeClass->FindPropertyByName(FName(*Root)) || !Params.TryGetField(TEXT("value")).IsValid()) return Failure(TEXT("UEPI_EDIT_PROPERTY_PREFLIGHT_FAILED"), FString::Printf(TEXT("AnimGraph node property is unavailable: %s"), *Property));
				}
				else if (Type == TEXT("blueprint.break_all_links") || Type == TEXT("blueprint.remove_node") || Type == TEXT("blueprint.move_node") || Type == TEXT("blueprint.set_node_comment")) { const FString Ref = NodeRef(Params); if (!IsPlannedRef(Context, Ref)) { FString NodeError; if (!ResolveNode(Context, Graph, Params, NodeError)) return Failure(TEXT("UEPI_EDIT_NODE_NOT_FOUND"), NodeError); } }
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName()); Detail->SetStringField(TEXT("operation"), Type); return Success(TEXT("Blueprint operation preflight passed."), Detail);
			}

			virtual FUEPIEditResult Apply(const FUEPIEditContext& Context, const FJsonObject& Params) override
			{
				FUEPIEditResult Check = Preview(Context, Params); if (!Check.bOk) return Check; const FString Type = EffectiveType(Descriptor.Name, Params); UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ResolveAsset(Context, Params)); FString Error; UEdGraph* Graph = NeedsGraph(Type) ? ResolveGraph(Blueprint, Params, Error) : nullptr;
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("asset"), Blueprint->GetPathName());

				if (Type == TEXT("blueprint.add_variable"))
				{
					const FName Name(*JsonString(Params, TEXT("name"))); FEdGraphPinType PinType; BuildPinType(JsonString(Params, TEXT("pin_type"), JsonString(Params, TEXT("type_name"), TEXT("float"))), PinType, Error); const FString Default = JsonString(Params, TEXT("default_value"), JsonString(Params, TEXT("default"))); if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, PinType, Default)) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("FBlueprintEditorUtils::AddMemberVariable failed.")); Detail->SetStringField(TEXT("variable"), Name.ToString()); Detail->SetStringField(TEXT("pin_category"), PinType.PinCategory.ToString()); Detail->SetStringField(TEXT("default_value"), Default); return Success(TEXT("Variable added."), Detail);
				}
				if (Type == TEXT("blueprint.set_variable_default"))
				{
					const FName Name(*JsonString(Params, TEXT("name"), JsonString(Params, TEXT("variable")))); const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name); if (Index == INDEX_NONE) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), FString::Printf(TEXT("Blueprint variable not found: %s"), *Name.ToString())); Blueprint->Modify(); const FString Default = JsonString(Params, TEXT("default_value"), JsonString(Params, TEXT("value"))); Blueprint->NewVariables[Index].DefaultValue = Default; FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint); Detail->SetStringField(TEXT("variable"), Name.ToString()); Detail->SetStringField(TEXT("default_value"), Default); return Success(TEXT("Variable default updated."), Detail);
				}
				if (Type == TEXT("blueprint.add_component"))
				{
					const FName Name(*JsonString(Params, TEXT("name"), TEXT("UEPIComponent"))); UClass* Class = ComponentClass(JsonString(Params, TEXT("component_class"))); Blueprint->Modify(); Blueprint->SimpleConstructionScript->Modify(); USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(Class, Name); if (!Node) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("Failed to create component node.")); Blueprint->SimpleConstructionScript->AddNode(Node); FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); Detail->SetStringField(TEXT("component"), Name.ToString()); Detail->SetStringField(TEXT("component_class"), Class->GetPathName()); return Success(TEXT("Component added."), Detail);
				}
				if (Type == TEXT("blueprint.set_component_property") || Type == TEXT("blueprint.set_component_properties"))
				{
					const FString Name = JsonString(Params, TEXT("component"), JsonString(Params, TEXT("component_name"))); USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(FName(*Name)); UObject* Template = Node ? Node->ComponentTemplate : nullptr; if (!Template) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), FString::Printf(TEXT("Component template not found: %s"), *Name)); Template->Modify(); Detail->SetStringField(TEXT("component"), Name);
					if (Type == TEXT("blueprint.set_component_property")) { const FString Property = JsonString(Params, TEXT("property")); if (!SetSimpleProperty(Template, Property, Params, Error)) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), Error); Detail->SetStringField(TEXT("property"), Property); }
					else { const TSharedPtr<FJsonObject> Writes = PropertyWrites(Params, Error); TArray<TSharedPtr<FJsonValue>> Diffs; for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Writes->Values) { FString Mode; TSharedPtr<FJsonValue> Value, Before, After; DecodeWrite(Pair.Value, Mode, Value); if (!FUEPIPropertyCodec::SetPropertyPath(Template, Pair.Key, Value, Before, After, Error, Mode)) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), Error); TSharedRef<FJsonObject> Diff = MakeShared<FJsonObject>(); Diff->SetStringField(TEXT("property_path"), Pair.Key); Diff->SetField(TEXT("before"), Before); Diff->SetField(TEXT("after"), After); Diffs.Add(MakeShared<FJsonValueObject>(Diff)); } Detail->SetArrayField(TEXT("property_diff"), Diffs); }
					Template->PostEditChange(); FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint); return Success(Type == TEXT("blueprint.set_component_property") ? TEXT("Component property updated.") : TEXT("Component typed properties updated."), Detail);
				}
				if (Type == TEXT("blueprint.create_function"))
				{
					const FString Name = JsonString(Params, TEXT("name"), JsonString(Params, TEXT("function_name"))); Blueprint->Modify(); UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass()); if (!NewGraph) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), FString::Printf(TEXT("Failed to create function graph: %s"), *Name)); NewGraph->Modify(); FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, true, nullptr); FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); TArray<TSharedPtr<FJsonValue>> Nodes; for (UEdGraphNode* Node : NewGraph->Nodes) Nodes.Add(MakeShared<FJsonValueObject>(NodeJson(Node, NewGraph))); Detail->SetStringField(TEXT("function"), Name); Detail->SetStringField(TEXT("graph"), NewGraph->GetName()); Detail->SetArrayField(TEXT("nodes"), Nodes); return Success(TEXT("Function graph created."), Detail);
				}
				if (Type == TEXT("blueprint.compile"))
				{
					TSharedRef<FJsonObject> Result = Compile(Blueprint); if (Result->GetBoolField(TEXT("ok"))) return Success(TEXT("Blueprint compiled."), Result); FUEPIEditResult Failed = Failure(TEXT("UEPI_EDIT_BLUEPRINT_COMPILE_FAILED"), TEXT("Blueprint compile returned errors.")); Failed.Result = Result; return Failed;
				}

				UEdGraphNode* NewNode = nullptr;
				if (Type == TEXT("blueprint.add_event_node"))
				{
					const FName Name(*JsonString(Params, TEXT("event_name"), JsonString(Params, TEXT("name")))); Graph->Modify(); NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CustomEvent>(Graph, NodePosition(Params, Context.OperationIndex), EK2NewNodeFlags::None, [Name](UK2Node_CustomEvent* Node) { Node->CustomFunctionName = Name; }); Detail->SetStringField(TEXT("event"), Name.ToString());
				}
				else if (Type == TEXT("blueprint.add_function_call_node"))
				{
					FName Self; UFunction* Function = ResolveFunction(Context, Blueprint, Params, Self, Error); Graph->Modify(); NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CallFunction>(Graph, NodePosition(Params, Context.OperationIndex), EK2NewNodeFlags::None, [Function, Self](UK2Node_CallFunction* Node) { if (Function) Node->SetFromFunction(Function); else Node->FunctionReference.SetSelfMember(Self); }); Detail->SetStringField(TEXT("function"), Function ? Function->GetPathName() : Self.ToString());
				}
				else if (Type == TEXT("blueprint.add_variable_get_node") || Type == TEXT("blueprint.add_variable_set_node"))
				{
					const FString NameText = JsonString(Params, TEXT("variable"), JsonString(Params, TEXT("name"))); const FName Name(*NameText); Graph->Modify();
					if (Type == TEXT("blueprint.add_variable_get_node")) NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_VariableGet>(Graph, NodePosition(Params, Context.OperationIndex), EK2NewNodeFlags::None, [Name](UK2Node_VariableGet* Node) { Node->VariableReference.SetSelfMember(Name); });
					else { UK2Node_VariableSet* SetNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_VariableSet>(Graph, NodePosition(Params, Context.OperationIndex), EK2NewNodeFlags::None, [Name](UK2Node_VariableSet* Node) { Node->VariableReference.SetSelfMember(Name); }); NewNode = SetNode; const TSharedPtr<FJsonValue> Value = Params.TryGetField(TEXT("value")).IsValid() ? Params.TryGetField(TEXT("value")) : Params.TryGetField(TEXT("default_value")); if (SetNode && Value.IsValid()) if (UEdGraphPin* Pin = FindPin(SetNode, NameText, TEXT("input"))) GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*Pin, PinValue(Value), true); }
					Detail->SetStringField(TEXT("variable"), NameText);
				}
				else if (Type == TEXT("blueprint.add_branch_node"))
				{
					Graph->Modify(); UK2Node_IfThenElse* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_IfThenElse>(Graph, NodePosition(Params, Context.OperationIndex), EK2NewNodeFlags::None); NewNode = Node; if (Node && Params.HasField(TEXT("condition_default"))) GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*Node->GetConditionPin(), JsonBool(Params, TEXT("condition_default"), true) ? TEXT("true") : TEXT("false"), true);
				}
				else if (Type == TEXT("blueprint.add_print_string_node"))
				{
					UFunction* Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)); Graph->Modify(); UK2Node_CallFunction* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CallFunction>(Graph, NodePosition(Params, Context.OperationIndex), EK2NewNodeFlags::None, [Function](UK2Node_CallFunction* Item) { Item->SetFromFunction(Function); }); NewNode = Node; const FString Text = JsonString(Params, TEXT("in_string"), JsonString(Params, TEXT("text"), JsonString(Params, TEXT("message")))); if (Node && !Text.IsEmpty()) if (UEdGraphPin* Pin = FindPin(Node, TEXT("InString"), TEXT("input"))) GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*Pin, Text, true);
				}
				else if (Type == TEXT("blueprint.add_node"))
				{
					const FString Kind = JsonString(Params, TEXT("kind")).ToLower(); Graph->Modify(); if (Kind == TEXT("make_struct")) { UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *JsonString(Params, TEXT("struct_path"))); NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_MakeStruct>(Graph, NodePosition(Params, Context.OperationIndex), EK2NewNodeFlags::None, [Struct](UK2Node_MakeStruct* Node) { Node->StructType = Struct; }); } else if (Kind == TEXT("input_key")) { const FKey Key(FName(*JsonString(Params, TEXT("key")))); NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_InputKey>(Graph, NodePosition(Params, Context.OperationIndex), EK2NewNodeFlags::None, [Key](UK2Node_InputKey* Node) { Node->InputKey = Key; }); }
				}
				else if (Type == TEXT("animgraph.add_slot_node"))
				{
					const FName Slot(*JsonString(Params, TEXT("slot_name"), TEXT("DefaultSlot"))); Graph->Modify(); NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UAnimGraphNode_Slot>(Graph, NodePosition(Params, Context.OperationIndex), EK2NewNodeFlags::None, [Slot](UAnimGraphNode_Slot* Node) { Node->Node.SlotName = Slot; }); Detail->SetStringField(TEXT("slot_name"), Slot.ToString());
				}

				if (NewNode)
				{
					ApplyDefaults(NewNode, Params); RegisterRefs(Context, NewNode, NewNode->GetClass()); FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); Detail->SetObjectField(TEXT("node"), NodeJson(NewNode, Graph)); return Success(Type == TEXT("animgraph.add_slot_node") ? TEXT("AnimGraph Slot node added.") : TEXT("Blueprint node added."), Detail);
				}
				if (Type == TEXT("blueprint.add_event_node") || Type == TEXT("blueprint.add_function_call_node") || Type == TEXT("blueprint.add_variable_get_node") || Type == TEXT("blueprint.add_variable_set_node") || Type == TEXT("blueprint.add_branch_node") || Type == TEXT("blueprint.add_print_string_node") || Type == TEXT("blueprint.add_node") || Type == TEXT("animgraph.add_slot_node")) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), TEXT("Blueprint node creation failed."));

				if (Type == TEXT("animgraph.set_node_property"))
				{
					UEdGraphNode* Node = ResolveNode(Context, Graph, Params, Error); const FString Property = JsonString(Params, TEXT("property_path"), JsonString(Params, TEXT("property"))); const TSharedPtr<FJsonValue> Value = Params.TryGetField(TEXT("value")); TSharedPtr<FJsonValue> Before, After; if (!Node || Property.IsEmpty() || !Value.IsValid()) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), Error.IsEmpty() ? TEXT("AnimGraph node property request is invalid.") : Error); Node->Modify(); if (!FUEPIPropertyCodec::SetPropertyPath(Node, Property, Value, Before, After, Error)) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), Error); Node->ReconstructNode(); FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); Detail->SetStringField(TEXT("property_path"), Property); Detail->SetField(TEXT("before"), Before); Detail->SetField(TEXT("after"), After); return Success(TEXT("AnimGraph node property updated."), Detail);
				}
				if (Type == TEXT("blueprint.connect_pins"))
				{
					UEdGraphNode* SourceNode = nullptr; UEdGraphNode* TargetNode = nullptr; UEdGraphPin* Source = ResolveEndpoint(Context, Graph, Params, TEXT("source"), TEXT("output"), SourceNode, Error); if (!Source) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), Error); UEdGraphPin* Target = ResolveEndpoint(Context, Graph, Params, TEXT("target"), TEXT("input"), TargetNode, Error); if (!Target) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), Error); Graph->Modify(); SourceNode->Modify(); TargetNode->Modify(); const UEdGraphSchema* Schema = Graph->GetSchema(); if (!Schema || !Schema->TryCreateConnection(Source, Target)) return Failure(TEXT("UEPI_EDIT_PIN_CONNECTION_FAILED"), FString::Printf(TEXT("Failed to connect %s.%s to %s.%s."), *GuidString(SourceNode), *Source->PinName.ToString(), *GuidString(TargetNode), *Target->PinName.ToString())); FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint); Detail->SetStringField(TEXT("graph"), Graph->GetName()); Detail->SetObjectField(TEXT("source_node"), NodeJson(SourceNode, Graph)); Detail->SetObjectField(TEXT("target_node"), NodeJson(TargetNode, Graph)); return Success(TEXT("Blueprint pins connected."), Detail);
				}
				if (Type == TEXT("blueprint.set_pin_default") || Type == TEXT("blueprint.disconnect_pins"))
				{
					UEdGraphNode* Node = nullptr; UEdGraphPin* Pin = ResolveEndpoint(Context, Graph, Params, TEXT("target"), TEXT("input"), Node, Error); if (!Pin) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), Error); Pin->Modify(); if (Type == TEXT("blueprint.set_pin_default")) { const TSharedPtr<FJsonValue> Value = Params.TryGetField(TEXT("value")); if (!Value.IsValid()) return Failure(TEXT("UEPI_EDIT_PIN_DEFAULT_FAILED"), TEXT("Pin default value is required.")); Graph->GetSchema()->TrySetDefaultValue(*Pin, PinValue(Value), true); Detail->SetObjectField(TEXT("node"), NodeJson(Node, Graph)); Detail->SetStringField(TEXT("pin"), Pin->PinName.ToString()); } else Pin->BreakAllPinLinks(true); FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint); return Success(Type == TEXT("blueprint.set_pin_default") ? TEXT("Pin default updated.") : TEXT("Pin links disconnected."), Detail);
				}
				if (Type == TEXT("blueprint.break_all_links") || Type == TEXT("blueprint.remove_node") || Type == TEXT("blueprint.move_node") || Type == TEXT("blueprint.set_node_comment"))
				{
					UEdGraphNode* Node = ResolveNode(Context, Graph, Params, Error); if (!Node) return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"), Error); Graph->Modify(); Node->Modify();
					if (Type == TEXT("blueprint.break_all_links")) { for (UEdGraphPin* Pin : Node->Pins) if (Pin) { Pin->Modify(); Pin->BreakAllPinLinks(true); } }
					else if (Type == TEXT("blueprint.remove_node")) Graph->RemoveNode(Node);
					else if (Type == TEXT("blueprint.move_node")) { const FVector2D Position = NodePosition(Params, Context.OperationIndex); Node->NodePosX = FMath::RoundToInt(Position.X); Node->NodePosY = FMath::RoundToInt(Position.Y); }
					else { Node->NodeComment = JsonString(Params, TEXT("comment")); Node->bCommentBubbleVisible = JsonBool(Params, TEXT("comment_visible"), false); }
					FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint); return Success(Type == TEXT("blueprint.break_all_links") ? TEXT("All node pin links disconnected.") : TEXT("Blueprint node updated."), Detail);
				}
				return Failure(TEXT("UEPI_EDIT_OPERATION_UNSUPPORTED"), FString::Printf(TEXT("Blueprint operation is not implemented: %s"), *Type));
			}

		private:
			FUEPIEditOperationDescriptor Descriptor;
		};
	}

	TSharedRef<IUEPIEditOperation> MakeUEPIBlueprintOperation(const FUEPIEditOperationDescriptor& Descriptor)
	{
		return MakeShared<BlueprintOperationsPrivate::FUEPIBlueprintOperation>(Descriptor);
	}
}
