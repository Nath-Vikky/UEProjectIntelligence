#include "UEPIBlueprintGraphReader.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "AnimationStateMachineGraph.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimStateAliasNode.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "ControlRigBlueprint.h"
#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_LoadAsset.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_SpawnActor.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Switch.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyElements.h"
#include "UObject/UnrealType.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString GuidString(const FGuid& Guid)
{
	return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString();
}

FString PinDirectionString(EEdGraphPinDirection Direction)
{
	switch (Direction)
	{
	case EGPD_Input:
		return TEXT("input");
	case EGPD_Output:
		return TEXT("output");
	default:
		return TEXT("unknown");
	}
}

FString GraphKey(const UBlueprint& Blueprint, const UEdGraph& Graph)
{
	const FString GraphGuid = GuidString(Graph.GraphGuid);
	if (!GraphGuid.IsEmpty())
	{
		return Blueprint.GetPathName() + TEXT(":graph:") + GraphGuid;
	}

	return Blueprint.GetPathName() + TEXT(":graph:") + Graph.GetName();
}

FString NodeKey(const FString& GraphId, const UEdGraphNode& Node)
{
	const FString NodeGuid = GuidString(Node.NodeGuid);
	if (!NodeGuid.IsEmpty())
	{
		return GraphId + TEXT(":node:") + NodeGuid;
	}

	return GraphId + TEXT(":node:") + Node.GetName();
}

FString NodeCanonicalKey(const UEdGraph& Graph, const UEdGraphNode& Node)
{
	return Graph.GetPathName() + TEXT(":node:") + (GuidString(Node.NodeGuid).IsEmpty() ? Node.GetName() : GuidString(Node.NodeGuid));
}

FString PinKey(const FString& NodeId, const UEdGraphPin& Pin, int32 PinIndex)
{
	const FString PinGuid = GuidString(Pin.PinId);
	if (!PinGuid.IsEmpty())
	{
		return NodeId + TEXT(":pin:") + PinGuid;
	}

	return NodeId + TEXT(":pin:") + Pin.PinName.ToString() + TEXT(":") + FString::FromInt(PinIndex);
}

TSharedRef<FJsonObject> PinTypeToJson(const FEdGraphPinType& PinType)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
	Object->SetStringField(TEXT("subcategory"), PinType.PinSubCategory.ToString());
	Object->SetStringField(
		TEXT("subcategory_object"),
		PinType.PinSubCategoryObject.IsValid() ? PinType.PinSubCategoryObject->GetPathName() : FString());
	Object->SetBoolField(TEXT("is_array"), PinType.IsArray());
	Object->SetBoolField(TEXT("is_set"), PinType.IsSet());
	Object->SetBoolField(TEXT("is_map"), PinType.IsMap());
	Object->SetBoolField(TEXT("is_reference"), PinType.bIsReference);
	Object->SetBoolField(TEXT("is_const"), PinType.bIsConst);
	return Object;
}

void AddRelation(
	const FString& ProjectId,
	const FString& Type,
	const FString& FromId,
	const FString& ToId,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FRelationRecord>& OutRelations,
	bool bDerived = false,
	const TMap<FString, FString>& Attributes = TMap<FString, FString>())
{
	FRelationRecord Relation;
	Relation.Id = MakeRelationId(ProjectId, Type, FromId, ToId, Attributes.Num() > 0 ? &Attributes : nullptr);
	Relation.Type = Type;
	Relation.FromId = FromId;
	Relation.ToId = ToId;
	Relation.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Relation.bDerived = bDerived;
	Relation.Confidence = 1.0f;
	Relation.Attributes = Attributes;
	Relation.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		EvidencePath,
		EvidenceDetail
	});
	OutRelations.Add(MoveTemp(Relation));
}

FString ProjectedFlowRelationType(const UEdGraphPin& SourcePin)
{
	const FString Category = SourcePin.PinType.PinCategory.ToString();
	if (Category.Equals(TEXT("exec"), ESearchCase::IgnoreCase))
	{
		return TEXT("exec_flows_to");
	}

	if (Category.Equals(TEXT("delegate"), ESearchCase::IgnoreCase) || Category.Equals(TEXT("mc_delegate"), ESearchCase::IgnoreCase))
	{
		return TEXT("delegate_flows_to");
	}

	return TEXT("data_flows_to");
}

bool IsPinCategory(const UEdGraphPin& Pin, const TCHAR* Category)
{
	return Pin.PinType.PinCategory.ToString().Equals(Category, ESearchCase::IgnoreCase);
}

int32 CountPinsByCategory(const UEdGraphNode& Node, EEdGraphPinDirection Direction, const TCHAR* Category)
{
	int32 Count = 0;
	for (const UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin && Pin->Direction == Direction && IsPinCategory(*Pin, Category))
		{
			++Count;
		}
	}

	return Count;
}

TMap<FString, FString> MakePinLinkAttributes(
	const UEdGraphNode& SourceNode,
	const UEdGraphPin& SourcePin,
	const FString& SourcePinId,
	const UEdGraphNode& TargetNode,
	const UEdGraphPin& TargetPin,
	const FString& TargetPinId)
{
	TMap<FString, FString> Attributes;
	Attributes.Add(TEXT("source_node_class"), SourceNode.GetClass()->GetPathName());
	Attributes.Add(TEXT("source_pin_id"), SourcePinId);
	Attributes.Add(TEXT("source_pin_name"), SourcePin.PinName.ToString());
	Attributes.Add(TEXT("source_pin_category"), SourcePin.PinType.PinCategory.ToString());
	Attributes.Add(TEXT("source_pin_subcategory"), SourcePin.PinType.PinSubCategory.ToString());
	Attributes.Add(TEXT("source_pin_direction"), PinDirectionString(SourcePin.Direction));
	Attributes.Add(TEXT("target_node_class"), TargetNode.GetClass()->GetPathName());
	Attributes.Add(TEXT("target_pin_id"), TargetPinId);
	Attributes.Add(TEXT("target_pin_name"), TargetPin.PinName.ToString());
	Attributes.Add(TEXT("target_pin_category"), TargetPin.PinType.PinCategory.ToString());
	Attributes.Add(TEXT("target_pin_subcategory"), TargetPin.PinType.PinSubCategory.ToString());
	Attributes.Add(TEXT("target_pin_direction"), PinDirectionString(TargetPin.Direction));

	if (IsPinCategory(SourcePin, TEXT("exec")))
	{
		Attributes.Add(TEXT("branch_label"), SourcePin.PinName.ToString());
	}

	return Attributes;
}

void SetOptionalPinName(TSharedRef<FJsonObject> Object, const TCHAR* FieldName, const UEdGraphPin* Pin)
{
	Object->SetStringField(FieldName, Pin ? Pin->PinName.ToString() : FString());
}

struct FNodeProjectionRecord
{
	FString NodeId;
	FString CanonicalKey;
	FString DisplayName;
	const UEdGraphNode* Node = nullptr;
	bool bHasExecPin = false;
	FString SemanticKind;
	FString VariableName;
	FString VariableEntityId;
};

struct FExecProjectionEdge
{
	FString FromNodeId;
	FString ToNodeId;
	FString EvidencePath;
	TMap<FString, FString> Attributes;
};

struct FDataProjectionEdge
{
	FString FromNodeId;
	FString ToNodeId;
	FString FromPinId;
	FString ToPinId;
	FString PinCategory;
	FString EvidencePath;
	TMap<FString, FString> Attributes;
};

struct FBasicBlockRecord
{
	FString BlockId;
	FString CanonicalKey;
	int32 Index = 0;
	TArray<FString> NodeIds;
	TSet<FString> OutgoingBlockIds;
};

struct FAnimBlueprintNodeRecord
{
	const UEdGraph* Graph = nullptr;
	const UEdGraphNode* Node = nullptr;
	FString GraphId;
	FString NodeId;
};

int32 FlowCount(const TMap<FString, TArray<FString>>& EdgesByNode, const FString& NodeId)
{
	if (const TArray<FString>* Edges = EdgesByNode.Find(NodeId))
	{
		return Edges->Num();
	}

	return 0;
}

FString VariableKey(const UBlueprint& Blueprint, const FString& VariableName)
{
	return Blueprint.GetPathName() + TEXT(":variable:") + VariableName;
}

FString AddSemanticEntity(
	const FString& ProjectId,
	const FString& Kind,
	const FString& CanonicalKey,
	const FString& DisplayName,
	const TMap<FString, FString>& Attributes,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString EntityId = MakeStableId(ProjectId, Kind, CanonicalKey);
	for (const FEntityRecord& ExistingEntity : OutEntities)
	{
		if (ExistingEntity.Id == EntityId)
		{
			return EntityId;
		}
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = Kind;
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = DisplayName;
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes = Attributes;
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("semantic_reference") };
	Entity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		EvidencePath,
		TEXT("Semantic reference extracted from a K2 node.")
	});
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddClassSemanticEntity(
	const FString& ProjectId,
	const UClass* Class,
	const FString& FallbackPath,
	const FString& FallbackName,
	const TMap<FString, FString>& ExtraAttributes,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString ClassPath = Class ? Class->GetPathName() : FallbackPath;
	const FString ClassName = Class ? Class->GetName() : (FallbackName.IsEmpty() ? ClassPath : FallbackName);
	const FString ClassKey = ClassPath.IsEmpty() ? EvidencePath + TEXT(":class") : ClassPath;

	TMap<FString, FString> Attributes = ExtraAttributes;
	Attributes.Add(TEXT("class_path"), ClassPath);
	Attributes.Add(TEXT("class_name"), ClassName);
	Attributes.Add(TEXT("is_native"), Class && Class->ClassGeneratedBy == nullptr ? TEXT("true") : TEXT("false"));
	Attributes.Add(TEXT("generated_by"), Class && Class->ClassGeneratedBy ? Class->ClassGeneratedBy->GetPathName() : FString());

	return AddSemanticEntity(ProjectId, TEXT("u_class"), ClassKey, ClassName, Attributes, EvidencePath, OutEntities);
}

FString BpBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString BpNumber(double Value)
{
	return FString::SanitizeFloat(Value);
}

FString BpName(FName Name)
{
	return Name.IsNone() ? FString() : Name.ToString();
}

FString NodeTitleString(const UEdGraphNode& Node)
{
	return Node.GetNodeTitle(ENodeTitleType::FullTitle).ToString();
}

FString CompactSemanticToken(FString Value)
{
	Value.ToLowerInline();
	Value.ReplaceInline(TEXT(" "), TEXT(""));
	Value.ReplaceInline(TEXT("_"), TEXT(""));
	Value.ReplaceInline(TEXT("-"), TEXT(""));
	return Value;
}

bool HasPinNamed(const UEdGraphNode& Node, const TCHAR* Name)
{
	const FString TargetName = CompactSemanticToken(Name);
	for (const UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin && CompactSemanticToken(Pin->PinName.ToString()).Equals(TargetName, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}

	return false;
}

FString InferLoopKind(const UEdGraphNode& Node, const FString& MacroPath, const FString& MacroName)
{
	const FString Token = CompactSemanticToken(MacroPath + TEXT(" ") + MacroName + TEXT(" ") + NodeTitleString(Node));
	if (Token.Contains(TEXT("foreachloopwithbreak")))
	{
		return TEXT("for_each_with_break");
	}
	if (Token.Contains(TEXT("foreachloop")))
	{
		return TEXT("for_each");
	}
	if (Token.Contains(TEXT("reverseforloop")))
	{
		return TEXT("reverse_for");
	}
	if (Token.Contains(TEXT("forloopwithbreak")))
	{
		return TEXT("for_with_break");
	}
	if (Token.Contains(TEXT("forloop")))
	{
		return TEXT("for");
	}
	if (Token.Contains(TEXT("whileloop")))
	{
		return TEXT("while");
	}

	if (HasPinNamed(Node, TEXT("LoopBody")) && HasPinNamed(Node, TEXT("Completed")))
	{
		if (HasPinNamed(Node, TEXT("ArrayElement")) || HasPinNamed(Node, TEXT("ArrayIndex")))
		{
			return TEXT("for_each");
		}
		if (HasPinNamed(Node, TEXT("LastIndex")) || HasPinNamed(Node, TEXT("FirstIndex")))
		{
			return TEXT("for");
		}
		return TEXT("loop");
	}

	return FString();
}

FString NormalizePinReferencePath(FString Value)
{
	Value.TrimStartAndEndInline();
	if (Value.IsEmpty() || Value.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		return FString();
	}

	const int32 SingleQuoteStart = Value.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
	const int32 SingleQuoteEnd = Value.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (SingleQuoteStart != INDEX_NONE && SingleQuoteEnd != INDEX_NONE && SingleQuoteEnd > SingleQuoteStart)
	{
		return Value.Mid(SingleQuoteStart + 1, SingleQuoteEnd - SingleQuoteStart - 1);
	}

	if ((Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\""))) ||
		(Value.StartsWith(TEXT("'")) && Value.EndsWith(TEXT("'"))))
	{
		return Value.Mid(1, Value.Len() - 2);
	}

	return Value;
}

FString PinReferenceDefaultPath(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return FString();
	}

	if (Pin->DefaultObject)
	{
		return Pin->DefaultObject->GetPathName();
	}

	const FString DefaultValue = NormalizePinReferencePath(Pin->DefaultValue);
	if (!DefaultValue.IsEmpty())
	{
		return DefaultValue;
	}

	return NormalizePinReferencePath(Pin->AutogeneratedDefaultValue);
}

FString PinSubCategoryObjectPath(const UEdGraphPin* Pin)
{
	if (!Pin || !Pin->PinType.PinSubCategoryObject.IsValid())
	{
		return FString();
	}

	return Pin->PinType.PinSubCategoryObject->GetPathName();
}

const UEdGraphPin* FindFirstDataOutputPin(const UEdGraphNode& Node)
{
	for (const UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && !IsPinCategory(*Pin, TEXT("exec")))
		{
			return Pin;
		}
	}

	return nullptr;
}

const UEdGraphPin* FindAssetReferenceInputPin(const UEdGraphNode& Node)
{
	const UEdGraphPin* FirstTypedPin = nullptr;
	const UEdGraphPin* FirstDefaultedPin = nullptr;

	for (const UEdGraphPin* Pin : Node.Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input || IsPinCategory(*Pin, TEXT("exec")))
		{
			continue;
		}

		const FString Category = Pin->PinType.PinCategory.ToString();
		const bool bSoftReferenceCategory =
			Category.Equals(TEXT("softobject"), ESearchCase::IgnoreCase) ||
			Category.Equals(TEXT("softclass"), ESearchCase::IgnoreCase);
		const bool bObjectLikeCategory =
			bSoftReferenceCategory ||
			Category.Contains(TEXT("object"), ESearchCase::IgnoreCase) ||
			Category.Contains(TEXT("class"), ESearchCase::IgnoreCase);
		const bool bHasReferenceDefault = !PinReferenceDefaultPath(Pin).IsEmpty();

		if (bSoftReferenceCategory && bHasReferenceDefault)
		{
			return Pin;
		}

		if (bHasReferenceDefault && !FirstDefaultedPin)
		{
			FirstDefaultedPin = Pin;
		}

		if (bObjectLikeCategory && !FirstTypedPin)
		{
			FirstTypedPin = Pin;
		}
	}

	return FirstDefaultedPin ? FirstDefaultedPin : FirstTypedPin;
}

const FAnimBlueprintNodeRecord* FindAnimRecord(const TArray<FAnimBlueprintNodeRecord>& Records, const UEdGraphNode* Node)
{
	if (!Node)
	{
		return nullptr;
	}

	for (const FAnimBlueprintNodeRecord& Record : Records)
	{
		if (Record.Node == Node)
		{
			return &Record;
		}
	}

	return nullptr;
}

bool HasEntity(const TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	for (const FEntityRecord& Entity : Entities)
	{
		if (Entity.Id == EntityId)
		{
			return true;
		}
	}

	return false;
}

FString AddAnimStaticEntity(
	const FString& ProjectId,
	const FString& Kind,
	const FString& CanonicalKey,
	const FString& DisplayName,
	TMap<FString, FString> Attributes,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FEntityRecord>& OutEntities)
{
	const FString EntityId = MakeStableId(ProjectId, Kind, CanonicalKey);
	if (HasEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = Kind;
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = DisplayName;
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes = MoveTemp(Attributes);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("anim_blueprint_static_metadata") };
	Entity.Completeness.Omitted = { TEXT("runtime_pose_evaluation") };
	Entity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		EvidencePath,
		EvidenceDetail
	});
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddControlRigStaticEntity(
	const FString& ProjectId,
	const FString& Kind,
	const FString& CanonicalKey,
	const FString& DisplayName,
	TMap<FString, FString> Attributes,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FEntityRecord>& OutEntities)
{
	const FString EntityId = MakeStableId(ProjectId, Kind, CanonicalKey);
	if (HasEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = Kind;
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = DisplayName;
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes = MoveTemp(Attributes);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("control_rig_static_metadata") };
	Entity.Completeness.Omitted = { TEXT("control_rig_runtime_evaluation") };
	Entity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		EvidencePath,
		EvidenceDetail
	});
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddReferencedAnimationAssetEntity(
	const FString& ProjectId,
	const UAnimationAsset& AnimationAsset,
	TArray<FEntityRecord>& OutEntities)
{
	const FString Kind = AnimationAsset.IsA<UBlendSpace>() ? TEXT("blend_space") : TEXT("animation_sequence");
	const FString AssetPath = AnimationAsset.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, Kind, AssetPath);
	if (HasEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = Kind;
	Entity.CanonicalKey = AssetPath;
	Entity.DisplayName = AnimationAsset.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes.Add(TEXT("animation_asset_path"), AssetPath);
	Entity.Attributes.Add(TEXT("animation_asset_class"), AnimationAsset.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Attributes.Add(TEXT("referenced_from_anim_blueprint"), TEXT("true"));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("animation_asset_reference") };
	Entity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		AssetPath,
		TEXT("Animation asset referenced by an AnimBlueprint editor node.")
	});
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AnimStateTypeString(EAnimStateType StateType)
{
	switch (StateType)
	{
	case EAnimStateType::AST_SingleAnimation:
		return TEXT("single_animation");
	case EAnimStateType::AST_BlendGraph:
		return TEXT("blend_graph");
	default:
		return TEXT("unknown");
	}
}

FString TransitionLogicString(ETransitionLogicType::Type LogicType)
{
	switch (LogicType)
	{
	case ETransitionLogicType::TLT_StandardBlend:
		return TEXT("standard_blend");
	case ETransitionLogicType::TLT_Inertialization:
		return TEXT("inertialization");
	case ETransitionLogicType::TLT_Custom:
		return TEXT("custom");
	default:
		return TEXT("unknown");
	}
}

FString AlphaBlendOptionString(EAlphaBlendOption BlendOption)
{
	if (const UEnum* Enum = StaticEnum<EAlphaBlendOption>())
	{
		return Enum->GetNameStringByValue(static_cast<int64>(BlendOption));
	}

	return FString::FromInt(static_cast<int32>(BlendOption));
}

FString GroupRoleString(EAnimGroupRole::Type Role)
{
	switch (Role)
	{
	case EAnimGroupRole::CanBeLeader:
		return TEXT("can_be_leader");
	case EAnimGroupRole::AlwaysFollower:
		return TEXT("always_follower");
	case EAnimGroupRole::AlwaysLeader:
		return TEXT("always_leader");
	default:
		return TEXT("unknown");
	}
}

FString SyncMethodString(EAnimSyncMethod Method)
{
	switch (Method)
	{
	case EAnimSyncMethod::DoNotSync:
		return TEXT("do_not_sync");
	case EAnimSyncMethod::SyncGroup:
		return TEXT("sync_group");
	case EAnimSyncMethod::Graph:
		return TEXT("graph");
	default:
		return TEXT("unknown");
	}
}

template<typename TEnum>
FString ReflectedEnumString(TEnum Value)
{
	if (const UEnum* Enum = StaticEnum<TEnum>())
	{
		FString Name = Enum->GetNameStringByValue(static_cast<int64>(Value));
		Name.ToLowerInline();
		return Name;
	}

	return FString::FromInt(static_cast<int32>(Value));
}

FString JoinNameList(const TArray<FName>& Names)
{
	TArray<FString> Values;
	for (const FName Name : Names)
	{
		if (!Name.IsNone())
		{
			Values.Add(Name.ToString());
		}
	}

	return FString::Join(Values, TEXT(","));
}

TSharedRef<FJsonObject> AttributesToJson(const TMap<FString, FString>& Attributes)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Attributes)
	{
		Object->SetStringField(Pair.Key, Pair.Value);
	}
	return Object;
}

void AppendAnimBlueprintStaticSummary(
	UAnimBlueprint& AnimBlueprint,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	const TArray<FAnimBlueprintNodeRecord>& NodeRecords,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString AnimBlueprintPath = AnimBlueprint.GetPathName();
	TMap<FString, FString> BlueprintAttributes;
	BlueprintAttributes.Add(TEXT("anim_blueprint_path"), AnimBlueprintPath);
	BlueprintAttributes.Add(TEXT("target_skeleton_path"), AnimBlueprint.TargetSkeleton ? AnimBlueprint.TargetSkeleton->GetPathName() : FString());
	BlueprintAttributes.Add(TEXT("is_template"), BpBool(AnimBlueprint.bIsTemplate));
	BlueprintAttributes.Add(TEXT("use_multi_threaded_animation_update"), BpBool(AnimBlueprint.bUseMultiThreadedAnimationUpdate));
	BlueprintAttributes.Add(TEXT("warn_about_blueprint_usage"), BpBool(AnimBlueprint.bWarnAboutBlueprintUsage));
	BlueprintAttributes.Add(TEXT("sync_group_count"), FString::FromInt(AnimBlueprint.Groups.Num()));
	const FString AnimBlueprintId = AddAnimStaticEntity(
		ProjectId,
		TEXT("anim_blueprint"),
		AnimBlueprintPath,
		AnimBlueprint.GetName(),
		BlueprintAttributes,
		AnimBlueprintPath,
		TEXT("UAnimBlueprint static editor metadata."),
		OutEntities);
	AddRelation(ProjectId, TEXT("contains_anim_blueprint"), AssetEntity.Id, AnimBlueprintId, AnimBlueprintPath, TEXT("Asset contains AnimBlueprint static metadata."), OutRelations);

	TArray<TSharedPtr<FJsonValue>> StateMachineValues;
	TArray<TSharedPtr<FJsonValue>> StateValues;
	TArray<TSharedPtr<FJsonValue>> TransitionValues;
	TArray<TSharedPtr<FJsonValue>> AssetPlayerValues;
	TArray<TSharedPtr<FJsonValue>> CachedPoseValues;
	TArray<TSharedPtr<FJsonValue>> SlotValues;
	TArray<TSharedPtr<FJsonValue>> ControlRigValues;
	TMap<const UEdGraphNode*, FString> AnimStateEntityByNode;
	TMap<const UEdGraphNode*, FString> CachedPoseEntityByNode;

	for (const FAnimBlueprintNodeRecord& Record : NodeRecords)
	{
		if (!Record.Node)
		{
			continue;
		}

		if (const UAnimGraphNode_StateMachineBase* StateMachineNode = Cast<UAnimGraphNode_StateMachineBase>(Record.Node))
		{
			const UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode->EditorStateMachineGraph;
			int32 StateCount = 0;
			int32 TransitionCount = 0;
			for (const FAnimBlueprintNodeRecord& InnerRecord : NodeRecords)
			{
				if (InnerRecord.Graph == StateMachineGraph)
				{
					if (InnerRecord.Node && InnerRecord.Node->IsA<UAnimStateTransitionNode>())
					{
						++TransitionCount;
					}
					else if (InnerRecord.Node && InnerRecord.Node->IsA<UAnimStateNodeBase>())
					{
						++StateCount;
					}
				}
			}

			TMap<FString, FString> Attributes;
			Attributes.Add(TEXT("anim_blueprint_path"), AnimBlueprintPath);
			Attributes.Add(TEXT("node_id"), Record.NodeId);
			Attributes.Add(TEXT("graph_id"), Record.GraphId);
			Attributes.Add(TEXT("graph_path"), Record.Graph ? Record.Graph->GetPathName() : FString());
			Attributes.Add(TEXT("node_class"), Record.Node->GetClass()->GetPathName());
			Attributes.Add(TEXT("state_machine_name"), NodeTitleString(*Record.Node));
			Attributes.Add(TEXT("state_machine_graph_path"), StateMachineGraph ? StateMachineGraph->GetPathName() : FString());
			Attributes.Add(TEXT("state_count"), FString::FromInt(StateCount));
			Attributes.Add(TEXT("transition_count"), FString::FromInt(TransitionCount));
			const FString StateMachineId = AddAnimStaticEntity(
				ProjectId,
				TEXT("anim_state_machine"),
				Record.Node->GetPathName(),
				NodeTitleString(*Record.Node),
				Attributes,
				Record.Node->GetPathName(),
				TEXT("AnimGraph state machine node static metadata."),
				OutEntities);
			AddRelation(ProjectId, TEXT("contains_anim_state_machine"), AnimBlueprintId, StateMachineId, Record.Node->GetPathName(), TEXT("AnimBlueprint contains this state machine."), OutRelations);
			AddRelation(ProjectId, TEXT("contains_node"), StateMachineId, Record.NodeId, Record.Node->GetPathName(), TEXT("Anim state machine is backed by this blueprint node."), OutRelations);

			TSharedRef<FJsonObject> Object = AttributesToJson(Attributes);
			Object->SetStringField(TEXT("id"), StateMachineId);
			StateMachineValues.Add(MakeShared<FJsonValueObject>(Object));
		}
	}

	for (const FAnimBlueprintNodeRecord& Record : NodeRecords)
	{
		if (!Record.Node || Record.Node->IsA<UAnimStateTransitionNode>())
		{
			continue;
		}

		const UAnimStateNodeBase* StateNodeBase = Cast<UAnimStateNodeBase>(Record.Node);
		if (!StateNodeBase)
		{
			continue;
		}

		const UAnimStateNode* StateNode = Cast<UAnimStateNode>(Record.Node);
		const UAnimStateAliasNode* AliasNode = Cast<UAnimStateAliasNode>(Record.Node);
		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("anim_blueprint_path"), AnimBlueprintPath);
		Attributes.Add(TEXT("node_id"), Record.NodeId);
		Attributes.Add(TEXT("graph_id"), Record.GraphId);
		Attributes.Add(TEXT("graph_path"), Record.Graph ? Record.Graph->GetPathName() : FString());
		Attributes.Add(TEXT("node_class"), Record.Node->GetClass()->GetPathName());
		Attributes.Add(TEXT("state_name"), StateNodeBase->GetStateName());
		Attributes.Add(TEXT("state_kind"), AliasNode ? TEXT("alias") : TEXT("state"));
		Attributes.Add(TEXT("bound_graph_path"), StateNodeBase->GetBoundGraph() ? StateNodeBase->GetBoundGraph()->GetPathName() : FString());
		if (StateNode)
		{
			Attributes.Add(TEXT("state_type"), AnimStateTypeString(StateNode->StateType));
			Attributes.Add(TEXT("always_reset_on_entry"), BpBool(StateNode->bAlwaysResetOnEntry));
		}
		if (AliasNode)
		{
			Attributes.Add(TEXT("global_alias"), BpBool(AliasNode->bGlobalAlias));
			Attributes.Add(TEXT("aliased_state_count"), FString::FromInt(AliasNode->GetAliasedStates().Num()));
		}

		const FString StateId = AddAnimStaticEntity(
			ProjectId,
			TEXT("anim_state"),
			Record.Node->GetPathName(),
			StateNodeBase->GetStateName(),
			Attributes,
			Record.Node->GetPathName(),
			TEXT("AnimBlueprint state or state alias static metadata."),
			OutEntities);
		AnimStateEntityByNode.Add(Record.Node, StateId);
		AddRelation(ProjectId, TEXT("contains_anim_state"), AnimBlueprintId, StateId, Record.Node->GetPathName(), TEXT("AnimBlueprint contains this state node."), OutRelations);
		AddRelation(ProjectId, TEXT("contains_node"), StateId, Record.NodeId, Record.Node->GetPathName(), TEXT("Anim state is backed by this blueprint node."), OutRelations);

		TSharedRef<FJsonObject> Object = AttributesToJson(Attributes);
		Object->SetStringField(TEXT("id"), StateId);
		StateValues.Add(MakeShared<FJsonValueObject>(Object));
	}

	for (const FAnimBlueprintNodeRecord& Record : NodeRecords)
	{
		const UAnimStateTransitionNode* TransitionNode = Record.Node ? Cast<UAnimStateTransitionNode>(Record.Node) : nullptr;
		if (!TransitionNode)
		{
			continue;
		}

		UAnimStateTransitionNode* MutableTransition = const_cast<UAnimStateTransitionNode*>(TransitionNode);
		UAnimStateNodeBase* PreviousState = MutableTransition->GetPreviousState();
		UAnimStateNodeBase* NextState = MutableTransition->GetNextState();
		const FString PreviousStateId = PreviousState && AnimStateEntityByNode.Contains(PreviousState) ? AnimStateEntityByNode[PreviousState] : FString();
		const FString NextStateId = NextState && AnimStateEntityByNode.Contains(NextState) ? AnimStateEntityByNode[NextState] : FString();

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("anim_blueprint_path"), AnimBlueprintPath);
		Attributes.Add(TEXT("node_id"), Record.NodeId);
		Attributes.Add(TEXT("graph_id"), Record.GraphId);
		Attributes.Add(TEXT("graph_path"), Record.Graph ? Record.Graph->GetPathName() : FString());
		Attributes.Add(TEXT("node_class"), Record.Node->GetClass()->GetPathName());
		Attributes.Add(TEXT("transition_name"), NodeTitleString(*Record.Node));
		Attributes.Add(TEXT("previous_state_name"), PreviousState ? PreviousState->GetStateName() : FString());
		Attributes.Add(TEXT("next_state_name"), NextState ? NextState->GetStateName() : FString());
		Attributes.Add(TEXT("previous_state_id"), PreviousStateId);
		Attributes.Add(TEXT("next_state_id"), NextStateId);
		Attributes.Add(TEXT("rule_graph_path"), TransitionNode->BoundGraph ? TransitionNode->BoundGraph->GetPathName() : FString());
		Attributes.Add(TEXT("custom_transition_graph_path"), TransitionNode->GetCustomTransitionGraph() ? TransitionNode->GetCustomTransitionGraph()->GetPathName() : FString());
		Attributes.Add(TEXT("priority_order"), FString::FromInt(TransitionNode->PriorityOrder));
		Attributes.Add(TEXT("crossfade_duration"), BpNumber(TransitionNode->CrossfadeDuration));
		Attributes.Add(TEXT("blend_mode"), AlphaBlendOptionString(TransitionNode->BlendMode));
		Attributes.Add(TEXT("logic_type"), TransitionLogicString(TransitionNode->LogicType));
		Attributes.Add(TEXT("bidirectional"), BpBool(TransitionNode->Bidirectional));
		Attributes.Add(TEXT("shared_rules"), BpBool(TransitionNode->bSharedRules));
		Attributes.Add(TEXT("shared_crossfade"), BpBool(TransitionNode->bSharedCrossfade));
		Attributes.Add(TEXT("automatic_rule_based_on_sequence_player"), BpBool(TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState));
		Attributes.Add(TEXT("automatic_rule_trigger_time"), BpNumber(TransitionNode->AutomaticRuleTriggerTime));
		Attributes.Add(TEXT("sync_group_required"), BpName(TransitionNode->SyncGroupNameToRequireValidMarkersRule));
		Attributes.Add(TEXT("custom_blend_curve_path"), TransitionNode->CustomBlendCurve ? TransitionNode->CustomBlendCurve->GetPathName() : FString());
		Attributes.Add(TEXT("blend_profile_path"), TransitionNode->BlendProfile ? TransitionNode->BlendProfile->GetPathName() : FString());

		const FString TransitionId = AddAnimStaticEntity(
			ProjectId,
			TEXT("anim_transition"),
			Record.Node->GetPathName(),
			NodeTitleString(*Record.Node),
			Attributes,
			Record.Node->GetPathName(),
			TEXT("AnimBlueprint transition static metadata."),
			OutEntities);
		AddRelation(ProjectId, TEXT("contains_anim_transition"), AnimBlueprintId, TransitionId, Record.Node->GetPathName(), TEXT("AnimBlueprint contains this state transition."), OutRelations);
		AddRelation(ProjectId, TEXT("contains_node"), TransitionId, Record.NodeId, Record.Node->GetPathName(), TEXT("Anim transition is backed by this blueprint node."), OutRelations);
		if (!PreviousStateId.IsEmpty() && !NextStateId.IsEmpty())
		{
			AddRelation(ProjectId, TEXT("state_transitions_to"), PreviousStateId, NextStateId, Record.Node->GetPathName(), TEXT("Anim state transition edge."), OutRelations, false, Attributes);
		}

		TSharedRef<FJsonObject> Object = AttributesToJson(Attributes);
		Object->SetStringField(TEXT("id"), TransitionId);
		TransitionValues.Add(MakeShared<FJsonValueObject>(Object));
	}

	for (const FAnimBlueprintNodeRecord& Record : NodeRecords)
	{
		if (!Record.Node)
		{
			continue;
		}

		const UAnimationAsset* AnimationAsset = nullptr;
		FString PlayerKind;
		TMap<FString, FString> Attributes;
		if (const UAnimGraphNode_SequencePlayer* SequencePlayer = Cast<UAnimGraphNode_SequencePlayer>(Record.Node))
		{
			AnimationAsset = SequencePlayer->GetAnimationAsset();
			PlayerKind = TEXT("sequence_player");
			Attributes.Add(TEXT("loop"), BpBool(SequencePlayer->Node.IsLooping()));
			Attributes.Add(TEXT("play_rate"), BpNumber(SequencePlayer->Node.GetPlayRate()));
			Attributes.Add(TEXT("start_position"), BpNumber(SequencePlayer->Node.GetStartPosition()));
			Attributes.Add(TEXT("sync_group"), BpName(SequencePlayer->Node.GetGroupName()));
			Attributes.Add(TEXT("sync_group_role"), GroupRoleString(SequencePlayer->Node.GetGroupRole()));
			Attributes.Add(TEXT("sync_method"), SyncMethodString(SequencePlayer->Node.GetGroupMethod()));
		}
		else if (const UAnimGraphNode_BlendSpacePlayer* BlendSpacePlayer = Cast<UAnimGraphNode_BlendSpacePlayer>(Record.Node))
		{
			AnimationAsset = BlendSpacePlayer->GetAnimationAsset();
			PlayerKind = TEXT("blend_space_player");
			Attributes.Add(TEXT("loop"), BpBool(BlendSpacePlayer->Node.IsLooping()));
			Attributes.Add(TEXT("play_rate"), BpNumber(BlendSpacePlayer->Node.GetPlayRate()));
			Attributes.Add(TEXT("start_position"), BpNumber(BlendSpacePlayer->Node.GetStartPosition()));
			Attributes.Add(TEXT("sync_group"), BpName(BlendSpacePlayer->Node.GetGroupName()));
			Attributes.Add(TEXT("sync_group_role"), GroupRoleString(BlendSpacePlayer->Node.GetGroupRole()));
			Attributes.Add(TEXT("sync_method"), SyncMethodString(BlendSpacePlayer->Node.GetGroupMethod()));
			const FVector Position = BlendSpacePlayer->Node.GetPosition();
			Attributes.Add(TEXT("sample_x"), BpNumber(Position.X));
			Attributes.Add(TEXT("sample_y"), BpNumber(Position.Y));
			Attributes.Add(TEXT("sample_z"), BpNumber(Position.Z));
			Attributes.Add(TEXT("reset_play_time_when_blend_space_changes"), BpBool(BlendSpacePlayer->Node.ShouldResetPlayTimeWhenBlendSpaceChanges()));
		}

		if (PlayerKind.IsEmpty())
		{
			continue;
		}

		Attributes.Add(TEXT("anim_blueprint_path"), AnimBlueprintPath);
		Attributes.Add(TEXT("node_id"), Record.NodeId);
		Attributes.Add(TEXT("graph_id"), Record.GraphId);
		Attributes.Add(TEXT("graph_path"), Record.Graph ? Record.Graph->GetPathName() : FString());
		Attributes.Add(TEXT("node_class"), Record.Node->GetClass()->GetPathName());
		Attributes.Add(TEXT("player_kind"), PlayerKind);
		Attributes.Add(TEXT("animation_asset_path"), AnimationAsset ? AnimationAsset->GetPathName() : FString());
		Attributes.Add(TEXT("animation_asset_class"), AnimationAsset ? AnimationAsset->GetClass()->GetPathName() : FString());

		const FString PlayerId = AddAnimStaticEntity(
			ProjectId,
			TEXT("anim_asset_player"),
			Record.Node->GetPathName(),
			NodeTitleString(*Record.Node),
			Attributes,
			Record.Node->GetPathName(),
			TEXT("AnimBlueprint asset player node static metadata."),
			OutEntities);
		AddRelation(ProjectId, TEXT("contains_anim_asset_player"), AnimBlueprintId, PlayerId, Record.Node->GetPathName(), TEXT("AnimBlueprint contains this asset player."), OutRelations);
		AddRelation(ProjectId, TEXT("contains_node"), PlayerId, Record.NodeId, Record.Node->GetPathName(), TEXT("Anim asset player is backed by this blueprint node."), OutRelations);
		if (AnimationAsset)
		{
			const FString AnimationId = AddReferencedAnimationAssetEntity(ProjectId, *AnimationAsset, OutEntities);
			AddRelation(ProjectId, AnimationAsset->IsA<UBlendSpace>() ? TEXT("uses_blendspace") : TEXT("samples_animation"), PlayerId, AnimationId, Record.Node->GetPathName(), TEXT("Anim asset player references this animation asset."), OutRelations);
		}

		TSharedRef<FJsonObject> Object = AttributesToJson(Attributes);
		Object->SetStringField(TEXT("id"), PlayerId);
		AssetPlayerValues.Add(MakeShared<FJsonValueObject>(Object));
	}

	for (const FAnimBlueprintNodeRecord& Record : NodeRecords)
	{
		if (!Record.Node)
		{
			continue;
		}

		const UAnimGraphNode_SaveCachedPose* SaveCachedPose = Cast<UAnimGraphNode_SaveCachedPose>(Record.Node);
		const UAnimGraphNode_UseCachedPose* UseCachedPose = Cast<UAnimGraphNode_UseCachedPose>(Record.Node);
		if (!SaveCachedPose && !UseCachedPose)
		{
			continue;
		}

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("anim_blueprint_path"), AnimBlueprintPath);
		Attributes.Add(TEXT("node_id"), Record.NodeId);
		Attributes.Add(TEXT("graph_id"), Record.GraphId);
		Attributes.Add(TEXT("graph_path"), Record.Graph ? Record.Graph->GetPathName() : FString());
		Attributes.Add(TEXT("node_class"), Record.Node->GetClass()->GetPathName());
		Attributes.Add(TEXT("cache_kind"), SaveCachedPose ? TEXT("save") : TEXT("use"));
		if (SaveCachedPose)
		{
			Attributes.Add(TEXT("cache_name"), SaveCachedPose->CacheName);
		}
		else if (UseCachedPose)
		{
			const UAnimGraphNode_SaveCachedPose* TargetSaveNode = UseCachedPose->SaveCachedPoseNode.Get();
			Attributes.Add(TEXT("cache_name"), TargetSaveNode ? TargetSaveNode->CacheName : FString());
			if (const FAnimBlueprintNodeRecord* TargetRecord = FindAnimRecord(NodeRecords, TargetSaveNode))
			{
				Attributes.Add(TEXT("target_cache_node_id"), TargetRecord->NodeId);
			}
		}

		const FString CachedPoseId = AddAnimStaticEntity(
			ProjectId,
			TEXT("anim_cached_pose"),
			Record.Node->GetPathName(),
			NodeTitleString(*Record.Node),
			Attributes,
			Record.Node->GetPathName(),
			TEXT("AnimBlueprint cached pose node static metadata."),
			OutEntities);
		CachedPoseEntityByNode.Add(Record.Node, CachedPoseId);
		AddRelation(ProjectId, TEXT("contains_anim_cached_pose"), AnimBlueprintId, CachedPoseId, Record.Node->GetPathName(), TEXT("AnimBlueprint contains this cached pose node."), OutRelations);
		AddRelation(ProjectId, TEXT("contains_node"), CachedPoseId, Record.NodeId, Record.Node->GetPathName(), TEXT("Anim cached pose is backed by this blueprint node."), OutRelations);

		TSharedRef<FJsonObject> Object = AttributesToJson(Attributes);
		Object->SetStringField(TEXT("id"), CachedPoseId);
		CachedPoseValues.Add(MakeShared<FJsonValueObject>(Object));
	}

	for (const FAnimBlueprintNodeRecord& Record : NodeRecords)
	{
		const UAnimGraphNode_UseCachedPose* UseCachedPose = Record.Node ? Cast<UAnimGraphNode_UseCachedPose>(Record.Node) : nullptr;
		const UAnimGraphNode_SaveCachedPose* TargetSaveNode = UseCachedPose ? UseCachedPose->SaveCachedPoseNode.Get() : nullptr;
		if (UseCachedPose && TargetSaveNode && CachedPoseEntityByNode.Contains(UseCachedPose) && CachedPoseEntityByNode.Contains(TargetSaveNode))
		{
			AddRelation(ProjectId, TEXT("uses_cached_pose"), CachedPoseEntityByNode[UseCachedPose], CachedPoseEntityByNode[TargetSaveNode], Record.Node->GetPathName(), TEXT("Use cached pose node references a save cached pose node."), OutRelations);
		}
	}

	for (const FAnimBlueprintNodeRecord& Record : NodeRecords)
	{
		const UAnimGraphNode_Slot* SlotNode = Record.Node ? Cast<UAnimGraphNode_Slot>(Record.Node) : nullptr;
		if (!SlotNode)
		{
			continue;
		}

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("anim_blueprint_path"), AnimBlueprintPath);
		Attributes.Add(TEXT("node_id"), Record.NodeId);
		Attributes.Add(TEXT("graph_id"), Record.GraphId);
		Attributes.Add(TEXT("graph_path"), Record.Graph ? Record.Graph->GetPathName() : FString());
		Attributes.Add(TEXT("node_class"), Record.Node->GetClass()->GetPathName());
		Attributes.Add(TEXT("slot_name"), BpName(SlotNode->Node.SlotName));
		Attributes.Add(TEXT("always_update_source_pose"), BpBool(SlotNode->Node.bAlwaysUpdateSourcePose));
		const FString SlotId = AddAnimStaticEntity(
			ProjectId,
			TEXT("anim_slot"),
			Record.Node->GetPathName(),
			NodeTitleString(*Record.Node),
			Attributes,
			Record.Node->GetPathName(),
			TEXT("AnimBlueprint slot node static metadata."),
			OutEntities);
		AddRelation(ProjectId, TEXT("contains_anim_slot"), AnimBlueprintId, SlotId, Record.Node->GetPathName(), TEXT("AnimBlueprint contains this slot node."), OutRelations);
		AddRelation(ProjectId, TEXT("contains_node"), SlotId, Record.NodeId, Record.Node->GetPathName(), TEXT("Anim slot is backed by this blueprint node."), OutRelations);

		TSharedRef<FJsonObject> Object = AttributesToJson(Attributes);
		Object->SetStringField(TEXT("id"), SlotId);
		SlotValues.Add(MakeShared<FJsonValueObject>(Object));
	}

	for (const FAnimBlueprintNodeRecord& Record : NodeRecords)
	{
		if (!Record.Node || !Record.Node->GetClass()->GetPathName().Contains(TEXT("AnimGraphNode_ControlRig")))
		{
			continue;
		}

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("anim_blueprint_path"), AnimBlueprintPath);
		Attributes.Add(TEXT("node_id"), Record.NodeId);
		Attributes.Add(TEXT("graph_id"), Record.GraphId);
		Attributes.Add(TEXT("graph_path"), Record.Graph ? Record.Graph->GetPathName() : FString());
		Attributes.Add(TEXT("node_class"), Record.Node->GetClass()->GetPathName());
		Attributes.Add(TEXT("node_title"), NodeTitleString(*Record.Node));
		const FString ControlRigId = AddAnimStaticEntity(
			ProjectId,
			TEXT("anim_control_rig_node"),
			Record.Node->GetPathName(),
			NodeTitleString(*Record.Node),
			Attributes,
			Record.Node->GetPathName(),
			TEXT("AnimBlueprint Control Rig editor node static metadata."),
			OutEntities);
		AddRelation(ProjectId, TEXT("contains_anim_control_rig_node"), AnimBlueprintId, ControlRigId, Record.Node->GetPathName(), TEXT("AnimBlueprint contains this Control Rig node."), OutRelations);
		AddRelation(ProjectId, TEXT("contains_node"), ControlRigId, Record.NodeId, Record.Node->GetPathName(), TEXT("Anim Control Rig summary is backed by this blueprint node."), OutRelations);

		TSharedRef<FJsonObject> Object = AttributesToJson(Attributes);
		Object->SetStringField(TEXT("id"), ControlRigId);
		ControlRigValues.Add(MakeShared<FJsonValueObject>(Object));
	}

	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("schema_version"), TEXT("uepi.anim_blueprint.v1"));
	Snapshot->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::EditorSourceGraph));
	Snapshot->SetStringField(TEXT("anim_blueprint_path"), AnimBlueprintPath);
	Snapshot->SetStringField(TEXT("target_skeleton_path"), AnimBlueprint.TargetSkeleton ? AnimBlueprint.TargetSkeleton->GetPathName() : FString());
	Snapshot->SetBoolField(TEXT("is_template"), AnimBlueprint.bIsTemplate);
	Snapshot->SetBoolField(TEXT("use_multi_threaded_animation_update"), AnimBlueprint.bUseMultiThreadedAnimationUpdate);
	Snapshot->SetBoolField(TEXT("warn_about_blueprint_usage"), AnimBlueprint.bWarnAboutBlueprintUsage);
	Snapshot->SetNumberField(TEXT("sync_group_count"), AnimBlueprint.Groups.Num());
	Snapshot->SetArrayField(TEXT("state_machines"), StateMachineValues);
	Snapshot->SetArrayField(TEXT("states"), StateValues);
	Snapshot->SetArrayField(TEXT("transitions"), TransitionValues);
	Snapshot->SetArrayField(TEXT("asset_players"), AssetPlayerValues);
	Snapshot->SetArrayField(TEXT("cached_poses"), CachedPoseValues);
	Snapshot->SetArrayField(TEXT("slots"), SlotValues);
	Snapshot->SetArrayField(TEXT("control_rig_nodes"), ControlRigValues);
	Snapshot->SetNumberField(TEXT("state_machine_count"), StateMachineValues.Num());
	Snapshot->SetNumberField(TEXT("state_count"), StateValues.Num());
	Snapshot->SetNumberField(TEXT("transition_count"), TransitionValues.Num());
	Snapshot->SetNumberField(TEXT("asset_player_count"), AssetPlayerValues.Num());
	Snapshot->SetNumberField(TEXT("cached_pose_count"), CachedPoseValues.Num());
	Snapshot->SetNumberField(TEXT("slot_count"), SlotValues.Num());
	Snapshot->SetNumberField(TEXT("control_rig_node_count"), ControlRigValues.Num());

	if (!AssetEntity.Snapshot.IsValid())
	{
		AssetEntity.Snapshot = MakeShared<FJsonObject>();
	}
	AssetEntity.Snapshot->SetObjectField(TEXT("anim_blueprint"), Snapshot);
	AssetEntity.Attributes.Add(TEXT("anim_blueprint_state_machine_count"), FString::FromInt(StateMachineValues.Num()));
	AssetEntity.Attributes.Add(TEXT("anim_blueprint_state_count"), FString::FromInt(StateValues.Num()));
	AssetEntity.Attributes.Add(TEXT("anim_blueprint_transition_count"), FString::FromInt(TransitionValues.Num()));
	AssetEntity.Attributes.Add(TEXT("anim_blueprint_asset_player_count"), FString::FromInt(AssetPlayerValues.Num()));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("anim_blueprint_static_summary"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("anim_state_machines"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("anim_state_transitions"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("anim_blueprint_runtime_pose_evaluation"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("anim_blueprint_preview_world_tick"));
	AssetEntity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		AnimBlueprintPath,
		TEXT("AnimBlueprint static state-machine and asset-player metadata extracted from editor source graphs.")
	});
}

void AppendControlRigBlueprintStaticSummary(
	UControlRigBlueprint& ControlRigBlueprint,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString ControlRigPath = ControlRigBlueprint.GetPathName();
	const USkeletalMesh* PreviewMesh = ControlRigBlueprint.GetPreviewMesh();
	const FRigVMClient* RigVMClient = ControlRigBlueprint.GetRigVMClient();
	URigHierarchy* Hierarchy = ControlRigBlueprint.Hierarchy.Get();

	TMap<FString, FString> BlueprintAttributes;
	BlueprintAttributes.Add(TEXT("control_rig_blueprint_path"), ControlRigPath);
	BlueprintAttributes.Add(TEXT("preview_skeletal_mesh_path"), PreviewMesh ? PreviewMesh->GetPathName() : FString());
	BlueprintAttributes.Add(TEXT("rigvm_model_count"), RigVMClient ? FString::FromInt(RigVMClient->Num()) : TEXT("0"));
	BlueprintAttributes.Add(TEXT("rigvm_entry_names"), RigVMClient ? JoinNameList(RigVMClient->GetEntryNames()) : FString());
	BlueprintAttributes.Add(TEXT("rigvm_execute_context_struct"), RigVMClient && RigVMClient->GetExecuteContextStruct() ? RigVMClient->GetExecuteContextStruct()->GetPathName() : FString());
	BlueprintAttributes.Add(TEXT("hierarchy_element_count"), Hierarchy ? FString::FromInt(Hierarchy->Num()) : TEXT("0"));
#if WITH_EDITORONLY_DATA
	BlueprintAttributes.Add(TEXT("shape_library_count"), FString::FromInt(ControlRigBlueprint.ShapeLibraries.Num()));
#else
	BlueprintAttributes.Add(TEXT("shape_library_count"), TEXT("0"));
#endif

	const FString ControlRigId = AddControlRigStaticEntity(
		ProjectId,
		TEXT("control_rig_blueprint"),
		ControlRigPath,
		ControlRigBlueprint.GetName(),
		BlueprintAttributes,
		ControlRigPath,
		TEXT("UControlRigBlueprint static editor metadata."),
		OutEntities);
	AddRelation(ProjectId, TEXT("contains_control_rig_blueprint"), AssetEntity.Id, ControlRigId, ControlRigPath, TEXT("Asset contains Control Rig Blueprint static metadata."), OutRelations);

	TArray<URigVMGraph*> RigVMGraphs;
	if (RigVMClient)
	{
		TSet<const URigVMGraph*> SeenGraphs;
		auto AddGraphIfUnique = [&RigVMGraphs, &SeenGraphs](URigVMGraph* Graph)
		{
			if (Graph && !SeenGraphs.Contains(Graph))
			{
				SeenGraphs.Add(Graph);
				RigVMGraphs.Add(Graph);
			}
		};

		AddGraphIfUnique(RigVMClient->GetDefaultModel());
		for (URigVMGraph* Graph : RigVMClient->GetAllModels(true, true))
		{
			AddGraphIfUnique(Graph);
		}
	}
	RigVMGraphs.Sort([](const URigVMGraph& Left, const URigVMGraph& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});

	TArray<TSharedPtr<FJsonValue>> GraphValues;
	TArray<TSharedPtr<FJsonValue>> NodeValues;
	TArray<TSharedPtr<FJsonValue>> PinValues;
	TArray<TSharedPtr<FJsonValue>> LinkValues;
	TArray<TSharedPtr<FJsonValue>> HierarchyElementValues;
	TMap<const URigVMGraph*, FString> GraphEntityByGraph;
	TMap<const URigVMNode*, FString> NodeEntityByNode;
	TMap<const URigVMPin*, FString> PinEntityByPin;

	for (URigVMGraph* Graph : RigVMGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		const FString GraphNodePath = Graph->GetNodePath();
		const FString GraphPath = GraphNodePath.IsEmpty() ? Graph->GetPathName() : GraphNodePath;
		const FString GraphCanonicalKey = ControlRigPath + TEXT(":rigvm_graph:") + GraphPath;
		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("control_rig_blueprint_path"), ControlRigPath);
		Attributes.Add(TEXT("graph_path"), GraphPath);
		Attributes.Add(TEXT("graph_object_path"), Graph->GetPathName());
		Attributes.Add(TEXT("graph_name"), Graph->GetGraphName());
		Attributes.Add(TEXT("graph_depth"), FString::FromInt(Graph->GetGraphDepth()));
		Attributes.Add(TEXT("is_root_graph"), BpBool(Graph->IsRootGraph()));
		Attributes.Add(TEXT("is_top_level_graph"), BpBool(Graph->IsTopLevelGraph()));
		Attributes.Add(TEXT("event_names"), JoinNameList(Graph->GetEventNames()));
		Attributes.Add(TEXT("node_count"), FString::FromInt(Graph->GetNodes().Num()));
		Attributes.Add(TEXT("link_count"), FString::FromInt(Graph->GetLinks().Num()));
		Attributes.Add(TEXT("variable_count"), FString::FromInt(Graph->GetVariableDescriptions().Num()));
		Attributes.Add(TEXT("local_variable_count"), FString::FromInt(Graph->GetLocalVariables(false).Num()));
		Attributes.Add(TEXT("input_argument_count"), FString::FromInt(Graph->GetInputArguments().Num()));
		Attributes.Add(TEXT("output_argument_count"), FString::FromInt(Graph->GetOutputArguments().Num()));
		Attributes.Add(TEXT("execute_context_struct"), Graph->GetExecuteContextStruct() ? Graph->GetExecuteContextStruct()->GetPathName() : FString());

		const FString GraphId = AddControlRigStaticEntity(
			ProjectId,
			TEXT("control_rig_vm_graph"),
			GraphCanonicalKey,
			Graph->GetGraphName().IsEmpty() ? Graph->GetName() : Graph->GetGraphName(),
			Attributes,
			Graph->GetPathName(),
			TEXT("Control Rig RigVM graph static metadata."),
			OutEntities);
		GraphEntityByGraph.Add(Graph, GraphId);
		AddRelation(ProjectId, TEXT("contains_rigvm_graph"), ControlRigId, GraphId, Graph->GetPathName(), TEXT("Control Rig Blueprint contains this RigVM graph."), OutRelations);

		TSharedRef<FJsonObject> Object = AttributesToJson(Attributes);
		Object->SetStringField(TEXT("id"), GraphId);
		GraphValues.Add(MakeShared<FJsonValueObject>(Object));
	}

	for (URigVMGraph* Graph : RigVMGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		const FString* GraphId = GraphEntityByGraph.Find(Graph);
		if (!GraphId)
		{
			continue;
		}

		TArray<URigVMNode*> Nodes = Graph->GetNodes();
		Nodes.RemoveAll([](const URigVMNode* Node)
		{
			return Node == nullptr;
		});
		Nodes.Sort([](const URigVMNode& Left, const URigVMNode& Right)
		{
			return Left.GetNodePath(true) < Right.GetNodePath(true);
		});

		for (URigVMNode* Node : Nodes)
		{
			const FVector2D Position = Node->GetPosition();
			const FString NodePath = Node->GetNodePath(true);
			const FString NodeCanonicalKey = ControlRigPath + TEXT(":rigvm_node:") + Graph->GetPathName() + TEXT(":") + NodePath;
			TMap<FString, FString> Attributes;
			Attributes.Add(TEXT("control_rig_blueprint_path"), ControlRigPath);
			Attributes.Add(TEXT("graph_id"), *GraphId);
			Attributes.Add(TEXT("graph_path"), Graph->GetNodePath().IsEmpty() ? Graph->GetPathName() : Graph->GetNodePath());
			Attributes.Add(TEXT("node_path"), NodePath);
			Attributes.Add(TEXT("node_title"), Node->GetNodeTitle());
			Attributes.Add(TEXT("node_class"), Node->GetClass()->GetPathName());
			Attributes.Add(TEXT("node_index"), FString::FromInt(Node->GetNodeIndex()));
			Attributes.Add(TEXT("graph_depth"), FString::FromInt(Node->GetGraphDepth()));
			Attributes.Add(TEXT("position_x"), BpNumber(Position.X));
			Attributes.Add(TEXT("position_y"), BpNumber(Position.Y));
			Attributes.Add(TEXT("pin_count"), FString::FromInt(Node->GetPins().Num()));
			Attributes.Add(TEXT("recursive_pin_count"), FString::FromInt(Node->GetAllPinsRecursively().Num()));
			Attributes.Add(TEXT("link_count"), FString::FromInt(Node->GetLinks().Num()));
			Attributes.Add(TEXT("is_injected"), BpBool(Node->IsInjected()));
			Attributes.Add(TEXT("is_visible_in_ui"), BpBool(Node->IsVisibleInUI()));
			Attributes.Add(TEXT("is_pure"), BpBool(Node->IsPure()));
			Attributes.Add(TEXT("is_mutable"), BpBool(Node->IsMutable()));
			Attributes.Add(TEXT("is_event"), BpBool(Node->IsEvent()));
			Attributes.Add(TEXT("event_name"), BpName(Node->GetEventName()));

			const FString NodeId = AddControlRigStaticEntity(
				ProjectId,
				TEXT("control_rig_vm_node"),
				NodeCanonicalKey,
				Node->GetNodeTitle().IsEmpty() ? Node->GetName() : Node->GetNodeTitle(),
				Attributes,
				Node->GetPathName(),
				TEXT("Control Rig RigVM node static metadata."),
				OutEntities);
			NodeEntityByNode.Add(Node, NodeId);
			AddRelation(ProjectId, TEXT("contains_rigvm_node"), *GraphId, NodeId, Node->GetPathName(), TEXT("RigVM graph contains this node."), OutRelations);

			TSharedRef<FJsonObject> Object = AttributesToJson(Attributes);
			Object->SetStringField(TEXT("id"), NodeId);
			NodeValues.Add(MakeShared<FJsonValueObject>(Object));
		}
	}

	for (const TPair<const URigVMNode*, FString>& NodePair : NodeEntityByNode)
	{
		const URigVMNode* Node = NodePair.Key;
		if (!Node)
		{
			continue;
		}

		const URigVMGraph* Graph = Node->GetGraph();
		const FString GraphPath = Graph ? (Graph->GetNodePath().IsEmpty() ? Graph->GetPathName() : Graph->GetNodePath()) : FString();
		TArray<URigVMPin*> Pins = Node->GetAllPinsRecursively();
		Pins.RemoveAll([](const URigVMPin* Pin)
		{
			return Pin == nullptr;
		});
		Pins.Sort([](const URigVMPin& Left, const URigVMPin& Right)
		{
			return Left.GetPinPath(true) < Right.GetPinPath(true);
		});

		for (URigVMPin* Pin : Pins)
		{
			const FString PinPath = Pin->GetPinPath(true);
			const FString PinCanonicalKey = ControlRigPath + TEXT(":rigvm_pin:") + GraphPath + TEXT(":") + PinPath;
			const UObject* CPPTypeObject = Pin->GetCPPTypeObject();
			TMap<FString, FString> Attributes;
			Attributes.Add(TEXT("control_rig_blueprint_path"), ControlRigPath);
			Attributes.Add(TEXT("node_id"), NodePair.Value);
			Attributes.Add(TEXT("graph_path"), GraphPath);
			Attributes.Add(TEXT("pin_path"), PinPath);
			Attributes.Add(TEXT("segment_path"), Pin->GetSegmentPath(true));
			Attributes.Add(TEXT("display_name"), BpName(Pin->GetDisplayName()));
			Attributes.Add(TEXT("direction"), ReflectedEnumString(Pin->GetDirection()));
			Attributes.Add(TEXT("cpp_type"), Pin->GetCPPType());
			Attributes.Add(TEXT("cpp_type_object_path"), CPPTypeObject ? CPPTypeObject->GetPathName() : FString());
			Attributes.Add(TEXT("default_value"), Pin->GetDefaultValue());
			Attributes.Add(TEXT("array_size"), FString::FromInt(Pin->GetArraySize()));
			Attributes.Add(TEXT("is_array"), BpBool(Pin->IsArray()));
			Attributes.Add(TEXT("is_struct"), BpBool(Pin->IsStruct()));
			Attributes.Add(TEXT("is_uobject"), BpBool(Pin->IsUObject()));
			Attributes.Add(TEXT("is_interface"), BpBool(Pin->IsInterface()));
			Attributes.Add(TEXT("is_enum"), BpBool(Pin->IsEnum()));
			Attributes.Add(TEXT("is_expanded"), BpBool(Pin->IsExpanded()));
			Attributes.Add(TEXT("is_constant"), BpBool(Pin->IsDefinedAsConstant()));
			Attributes.Add(TEXT("is_linked"), BpBool(Pin->IsLinked(false)));
			Attributes.Add(TEXT("requires_watch"), BpBool(Pin->RequiresWatch()));
			Attributes.Add(TEXT("source_link_count"), FString::FromInt(Pin->GetSourceLinks(false).Num()));
			Attributes.Add(TEXT("target_link_count"), FString::FromInt(Pin->GetTargetLinks(false).Num()));

			const FString PinId = AddControlRigStaticEntity(
				ProjectId,
				TEXT("control_rig_vm_pin"),
				PinCanonicalKey,
				Pin->GetDisplayName().IsNone() ? Pin->GetName() : Pin->GetDisplayName().ToString(),
				Attributes,
				Pin->GetPathName(),
				TEXT("Control Rig RigVM pin static metadata."),
				OutEntities);
			PinEntityByPin.Add(Pin, PinId);
			AddRelation(ProjectId, TEXT("contains_rigvm_pin"), NodePair.Value, PinId, Pin->GetPathName(), TEXT("RigVM node contains this pin."), OutRelations);

			TSharedRef<FJsonObject> Object = AttributesToJson(Attributes);
			Object->SetStringField(TEXT("id"), PinId);
			PinValues.Add(MakeShared<FJsonValueObject>(Object));
		}
	}

	for (URigVMGraph* Graph : RigVMGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		const FString* GraphId = GraphEntityByGraph.Find(Graph);
		if (!GraphId)
		{
			continue;
		}

		TArray<URigVMLink*> Links = Graph->GetLinks();
		Links.RemoveAll([](const URigVMLink* Link)
		{
			return Link == nullptr;
		});
		Links.Sort([](const URigVMLink& Left, const URigVMLink& Right)
		{
			return Left.GetPinPathRepresentation() < Right.GetPinPathRepresentation();
		});

		for (URigVMLink* Link : Links)
		{
			URigVMPin* SourcePin = Link->GetSourcePin();
			URigVMPin* TargetPin = Link->GetTargetPin();
			URigVMNode* SourceNode = Link->GetSourceNode();
			URigVMNode* TargetNode = Link->GetTargetNode();
			const FString SourcePinId = SourcePin && PinEntityByPin.Contains(SourcePin) ? PinEntityByPin[SourcePin] : FString();
			const FString TargetPinId = TargetPin && PinEntityByPin.Contains(TargetPin) ? PinEntityByPin[TargetPin] : FString();

			TMap<FString, FString> Attributes;
			Attributes.Add(TEXT("control_rig_blueprint_path"), ControlRigPath);
			Attributes.Add(TEXT("graph_id"), *GraphId);
			Attributes.Add(TEXT("graph_path"), Graph->GetNodePath().IsEmpty() ? Graph->GetPathName() : Graph->GetNodePath());
			Attributes.Add(TEXT("link_index"), FString::FromInt(Link->GetLinkIndex()));
			Attributes.Add(TEXT("pin_path_representation"), Link->GetPinPathRepresentation());
			Attributes.Add(TEXT("source_pin_path"), Link->GetSourcePinPath());
			Attributes.Add(TEXT("target_pin_path"), Link->GetTargetPinPath());
			Attributes.Add(TEXT("source_pin_id"), SourcePinId);
			Attributes.Add(TEXT("target_pin_id"), TargetPinId);
			Attributes.Add(TEXT("source_node_path"), SourceNode ? SourceNode->GetNodePath(true) : FString());
			Attributes.Add(TEXT("target_node_path"), TargetNode ? TargetNode->GetNodePath(true) : FString());

			const FString LinkCanonicalKey = ControlRigPath + TEXT(":rigvm_link:") + Graph->GetPathName() + TEXT(":") + Link->GetPinPathRepresentation();
			const FString LinkId = AddControlRigStaticEntity(
				ProjectId,
				TEXT("control_rig_vm_link"),
				LinkCanonicalKey,
				Link->GetPinPathRepresentation(),
				Attributes,
				Graph->GetPathName(),
				TEXT("Control Rig RigVM link static metadata."),
				OutEntities);
			AddRelation(ProjectId, TEXT("contains_rigvm_link"), *GraphId, LinkId, Graph->GetPathName(), TEXT("RigVM graph contains this link."), OutRelations);
			if (!SourcePinId.IsEmpty() && !TargetPinId.IsEmpty())
			{
				AddRelation(ProjectId, TEXT("rigvm_connects_to"), SourcePinId, TargetPinId, Graph->GetPathName(), TEXT("RigVM link connects source pin to target pin."), OutRelations, false, Attributes);
			}

			TSharedRef<FJsonObject> Object = AttributesToJson(Attributes);
			Object->SetStringField(TEXT("id"), LinkId);
			LinkValues.Add(MakeShared<FJsonValueObject>(Object));
		}
	}

	TMap<FString, FString> HierarchyEntityByKey;
	TArray<FRigElementKey> HierarchyKeys;
	if (Hierarchy)
	{
		Hierarchy->ForEach([&](FRigBaseElement* Element)
		{
			if (!Element)
			{
				return true;
			}

			const FRigElementKey ElementKey = Element->GetKey();
			const FString ElementKeyString = ElementKey.ToString();
			TMap<FString, FString> Attributes;
			Attributes.Add(TEXT("control_rig_blueprint_path"), ControlRigPath);
			Attributes.Add(TEXT("element_key"), ElementKeyString);
			Attributes.Add(TEXT("element_name"), Element->GetName().ToString());
			Attributes.Add(TEXT("display_name"), Element->GetDisplayName().ToString());
			Attributes.Add(TEXT("element_type"), ReflectedEnumString(Element->GetType()));
			Attributes.Add(TEXT("index"), FString::FromInt(Element->GetIndex()));
			Attributes.Add(TEXT("parent_count"), FString::FromInt(Hierarchy->GetNumberOfParents(ElementKey)));
			if (const FRigControlElement* ControlElement = Cast<FRigControlElement>(Element))
			{
				Attributes.Add(TEXT("control_type"), ReflectedEnumString(ControlElement->Settings.ControlType));
				Attributes.Add(TEXT("control_animation_type"), ReflectedEnumString(ControlElement->Settings.AnimationType));
				Attributes.Add(TEXT("animatable"), BpBool(ControlElement->Settings.IsAnimatable()));
			}

			const FString ElementId = AddControlRigStaticEntity(
				ProjectId,
				TEXT("control_rig_hierarchy_element"),
				ControlRigPath + TEXT(":hierarchy:") + ElementKeyString,
				Element->GetDisplayName().ToString(),
				Attributes,
				ControlRigPath,
				TEXT("Control Rig hierarchy element static metadata."),
				OutEntities);
			HierarchyEntityByKey.Add(ElementKeyString, ElementId);
			HierarchyKeys.Add(ElementKey);
			AddRelation(ProjectId, TEXT("contains_rig_hierarchy_element"), ControlRigId, ElementId, ControlRigPath, TEXT("Control Rig Blueprint hierarchy contains this element."), OutRelations);

			TSharedRef<FJsonObject> Object = AttributesToJson(Attributes);
			Object->SetStringField(TEXT("id"), ElementId);
			HierarchyElementValues.Add(MakeShared<FJsonValueObject>(Object));
			return true;
		});

		for (const FRigElementKey& ElementKey : HierarchyKeys)
		{
			const FString ChildKeyString = ElementKey.ToString();
			const FString* ChildId = HierarchyEntityByKey.Find(ChildKeyString);
			if (!ChildId)
			{
				continue;
			}

			const TArray<FRigElementKey> ParentKeys = Hierarchy->GetParents(ElementKey, false);
			for (const FRigElementKey& ParentKey : ParentKeys)
			{
				const FString ParentKeyString = ParentKey.ToString();
				const FString* ParentId = HierarchyEntityByKey.Find(ParentKeyString);
				if (!ParentId)
				{
					continue;
				}

				TMap<FString, FString> Attributes;
				Attributes.Add(TEXT("child_key"), ChildKeyString);
				Attributes.Add(TEXT("parent_key"), ParentKeyString);
				AddRelation(ProjectId, TEXT("rig_hierarchy_parent"), *ChildId, *ParentId, ControlRigPath, TEXT("Control Rig hierarchy parent edge."), OutRelations, false, Attributes);
			}
		}
	}

	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("schema_version"), TEXT("uepi.control_rig_blueprint.v1"));
	Snapshot->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::EditorSourceGraph));
	Snapshot->SetStringField(TEXT("control_rig_blueprint_path"), ControlRigPath);
	Snapshot->SetStringField(TEXT("preview_skeletal_mesh_path"), PreviewMesh ? PreviewMesh->GetPathName() : FString());
	Snapshot->SetNumberField(TEXT("rigvm_graph_count"), GraphValues.Num());
	Snapshot->SetNumberField(TEXT("rigvm_node_count"), NodeValues.Num());
	Snapshot->SetNumberField(TEXT("rigvm_pin_count"), PinValues.Num());
	Snapshot->SetNumberField(TEXT("rigvm_link_count"), LinkValues.Num());
	Snapshot->SetNumberField(TEXT("hierarchy_element_count"), HierarchyElementValues.Num());
	Snapshot->SetArrayField(TEXT("rigvm_graphs"), GraphValues);
	Snapshot->SetArrayField(TEXT("rigvm_nodes"), NodeValues);
	Snapshot->SetArrayField(TEXT("rigvm_pins"), PinValues);
	Snapshot->SetArrayField(TEXT("rigvm_links"), LinkValues);
	Snapshot->SetArrayField(TEXT("hierarchy_elements"), HierarchyElementValues);

	if (!AssetEntity.Snapshot.IsValid())
	{
		AssetEntity.Snapshot = MakeShared<FJsonObject>();
	}
	AssetEntity.Snapshot->SetObjectField(TEXT("control_rig_blueprint"), Snapshot);
	AssetEntity.Attributes.Add(TEXT("control_rig_vm_graph_count"), FString::FromInt(GraphValues.Num()));
	AssetEntity.Attributes.Add(TEXT("control_rig_vm_node_count"), FString::FromInt(NodeValues.Num()));
	AssetEntity.Attributes.Add(TEXT("control_rig_hierarchy_element_count"), FString::FromInt(HierarchyElementValues.Num()));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("control_rig_blueprint_static_summary"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("control_rig_rigvm_model"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("control_rig_hierarchy"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("control_rig_runtime_evaluation"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("control_rig_preview_execution"));
	AssetEntity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		ControlRigPath,
		TEXT("Control Rig Blueprint static RigVM model and hierarchy metadata extracted from editor source data.")
	});
}

TSharedPtr<FJsonObject> AnnotateNodeSemantics(
	const FString& ProjectId,
	const UBlueprint& Blueprint,
	const FString& NodeId,
	const UEdGraphNode& Node,
	FEntityRecord& NodeEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TSharedPtr<FJsonObject> SemanticObject;

	if (const UK2Node_IfThenElse* BranchNode = Cast<UK2Node_IfThenElse>(&Node))
	{
		NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("branch"));
		NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_branch"));

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), TEXT("branch"));
		SetOptionalPinName(SemanticObject.ToSharedRef(), TEXT("condition_pin"), BranchNode->GetConditionPin());
		SetOptionalPinName(SemanticObject.ToSharedRef(), TEXT("then_pin"), BranchNode->GetThenPin());
		SetOptionalPinName(SemanticObject.ToSharedRef(), TEXT("else_pin"), BranchNode->GetElsePin());
		return SemanticObject;
	}

	if (const UK2Node_ExecutionSequence* SequenceNode = Cast<UK2Node_ExecutionSequence>(&Node))
	{
		const int32 ExecOutputCount = CountPinsByCategory(*SequenceNode, EGPD_Output, TEXT("exec"));
		NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("sequence"));
		NodeEntity.Attributes.Add(TEXT("semantic_exec_output_count"), FString::FromInt(ExecOutputCount));
		NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_sequence"));

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), TEXT("sequence"));
		SemanticObject->SetNumberField(TEXT("exec_output_count"), ExecOutputCount);
		return SemanticObject;
	}

	if (const UK2Node_Switch* SwitchNode = Cast<UK2Node_Switch>(&Node))
	{
		const int32 ExecOutputCount = CountPinsByCategory(*SwitchNode, EGPD_Output, TEXT("exec"));
		const int32 CaseOutputCount = FMath::Max(0, ExecOutputCount - (SwitchNode->bHasDefaultPin ? 1 : 0));
		NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("switch"));
		NodeEntity.Attributes.Add(TEXT("semantic_exec_output_count"), FString::FromInt(ExecOutputCount));
		NodeEntity.Attributes.Add(TEXT("semantic_case_output_count"), FString::FromInt(CaseOutputCount));
		NodeEntity.Attributes.Add(TEXT("semantic_has_default_pin"), SwitchNode->bHasDefaultPin ? TEXT("true") : TEXT("false"));
		NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_switch"));

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), TEXT("switch"));
		SemanticObject->SetStringField(TEXT("switch_class"), Node.GetClass()->GetPathName());
		SemanticObject->SetNumberField(TEXT("exec_output_count"), ExecOutputCount);
		SemanticObject->SetNumberField(TEXT("case_output_count"), CaseOutputCount);
		SemanticObject->SetBoolField(TEXT("has_default_pin"), SwitchNode->bHasDefaultPin);
		SetOptionalPinName(SemanticObject.ToSharedRef(), TEXT("selection_pin"), SwitchNode->GetSelectionPin());
		SetOptionalPinName(SemanticObject.ToSharedRef(), TEXT("default_pin"), SwitchNode->GetDefaultPin());
		return SemanticObject;
	}

	if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(&Node))
	{
		const UEdGraph* MacroGraph = MacroNode->GetMacroGraph();
		const UBlueprint* SourceBlueprint = MacroNode->GetSourceBlueprint();
		const FString MacroPath = MacroGraph ? MacroGraph->GetPathName() : Node.GetPathName();
		const FString MacroName = MacroGraph ? MacroGraph->GetName() : Node.GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		const FString LoopKind = InferLoopKind(Node, MacroPath, MacroName);

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("macro_graph_path"), MacroPath);
		Attributes.Add(TEXT("macro_name"), MacroName);
		Attributes.Add(TEXT("source_blueprint"), SourceBlueprint ? SourceBlueprint->GetPathName() : FString());
		Attributes.Add(TEXT("loop_kind"), LoopKind);

		const FString MacroId = AddSemanticEntity(ProjectId, TEXT("blueprint_macro"), MacroPath, MacroName, Attributes, Node.GetPathName(), OutEntities);
		AddRelation(ProjectId, TEXT("calls_macro"), NodeId, MacroId, Node.GetPathName(), TEXT("UK2Node_MacroInstance target macro graph."), OutRelations, false, Attributes);

		NodeEntity.Attributes.Add(TEXT("semantic_kind"), LoopKind.IsEmpty() ? TEXT("macro") : TEXT("loop"));
		NodeEntity.Attributes.Add(TEXT("semantic_macro_graph"), MacroPath);
		if (!LoopKind.IsEmpty())
		{
			NodeEntity.Attributes.Add(TEXT("semantic_loop_kind"), LoopKind);
			NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_loop"));
		}
		NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_macro"));

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), LoopKind.IsEmpty() ? TEXT("macro") : TEXT("loop"));
		SemanticObject->SetStringField(TEXT("macro_graph"), MacroPath);
		SemanticObject->SetStringField(TEXT("macro_name"), MacroName);
		SemanticObject->SetStringField(TEXT("source_blueprint"), SourceBlueprint ? SourceBlueprint->GetPathName() : FString());
		SemanticObject->SetStringField(TEXT("loop_kind"), LoopKind);
		return SemanticObject;
	}

	if (const UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(&Node))
	{
		const UEdGraph* BoundGraph = CompositeNode->BoundGraph;
		const UK2Node_Tunnel* EntryNode = CompositeNode->GetEntryNode();
		const UK2Node_Tunnel* ExitNode = CompositeNode->GetExitNode();
		const FString CompositePath = BoundGraph ? BoundGraph->GetPathName() : Node.GetPathName();
		const FString CompositeName = BoundGraph ? BoundGraph->GetName() : NodeTitleString(Node);

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("composite_graph_path"), CompositePath);
		Attributes.Add(TEXT("composite_graph_name"), CompositeName);
		Attributes.Add(TEXT("entry_node_path"), EntryNode ? EntryNode->GetPathName() : FString());
		Attributes.Add(TEXT("exit_node_path"), ExitNode ? ExitNode->GetPathName() : FString());
		Attributes.Add(TEXT("input_pin_count"), FString::FromInt(CountPinsByCategory(*CompositeNode, EGPD_Input, TEXT("exec"))));
		Attributes.Add(TEXT("output_pin_count"), FString::FromInt(CountPinsByCategory(*CompositeNode, EGPD_Output, TEXT("exec"))));

		const FString CompositeId = AddSemanticEntity(ProjectId, TEXT("blueprint_composite_graph"), CompositePath, CompositeName, Attributes, Node.GetPathName(), OutEntities);
		AddRelation(ProjectId, TEXT("calls_composite"), NodeId, CompositeId, Node.GetPathName(), TEXT("UK2Node_Composite collapsed graph."), OutRelations, false, Attributes);

		NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("composite"));
		NodeEntity.Attributes.Add(TEXT("semantic_composite_graph"), CompositePath);
		NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_composite"));

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), TEXT("composite"));
		SemanticObject->SetStringField(TEXT("composite_graph"), CompositePath);
		SemanticObject->SetStringField(TEXT("composite_name"), CompositeName);
		SemanticObject->SetStringField(TEXT("entry_node"), EntryNode ? EntryNode->GetPathName() : FString());
		SemanticObject->SetStringField(TEXT("exit_node"), ExitNode ? ExitNode->GetPathName() : FString());
		return SemanticObject;
	}

	if (const UK2Node_BaseAsyncTask* AsyncNode = Cast<UK2Node_BaseAsyncTask>(&Node))
	{
		const FString AsyncClassPath = AsyncNode->GetClass()->GetPathName();
		const FString AsyncName = Node.GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		const int32 ExecOutputCount = CountPinsByCategory(*AsyncNode, EGPD_Output, TEXT("exec"));
		const int32 DelegateOutputCount = CountPinsByCategory(*AsyncNode, EGPD_Output, TEXT("delegate")) +
			CountPinsByCategory(*AsyncNode, EGPD_Output, TEXT("mc_delegate"));

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("async_node_class"), AsyncClassPath);
		Attributes.Add(TEXT("async_node_title"), AsyncName);
		Attributes.Add(TEXT("exec_output_count"), FString::FromInt(ExecOutputCount));
		Attributes.Add(TEXT("delegate_output_count"), FString::FromInt(DelegateOutputCount));

		const FString AsyncId = AddSemanticEntity(ProjectId, TEXT("blueprint_async_action"), AsyncClassPath, AsyncName, Attributes, Node.GetPathName(), OutEntities);
		AddRelation(ProjectId, TEXT("starts_async_action"), NodeId, AsyncId, Node.GetPathName(), TEXT("UK2Node_BaseAsyncTask async action node."), OutRelations, false, Attributes);

		NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("async_action"));
		NodeEntity.Attributes.Add(TEXT("semantic_async_node_class"), AsyncClassPath);
		NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_async_action"));

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), TEXT("async_action"));
		SemanticObject->SetStringField(TEXT("async_node_class"), AsyncClassPath);
		SemanticObject->SetNumberField(TEXT("exec_output_count"), ExecOutputCount);
		SemanticObject->SetNumberField(TEXT("delegate_output_count"), DelegateOutputCount);
		return SemanticObject;
	}

	if (const UK2Node_CallFunction* CallFunctionNode = Cast<UK2Node_CallFunction>(&Node))
	{
		UFunction* TargetFunction = CallFunctionNode->GetTargetFunction();
		const UClass* OwnerClass = TargetFunction && TargetFunction->GetOwnerClass()
			? TargetFunction->GetOwnerClass()
			: CallFunctionNode->FunctionReference.GetMemberParentClass(Blueprint.GeneratedClass);
		const FString FunctionPath = TargetFunction
			? TargetFunction->GetPathName()
			: CallFunctionNode->FunctionReference.GetMemberName().ToString();
		const FString FunctionName = TargetFunction
			? TargetFunction->GetName()
			: CallFunctionNode->FunctionReference.GetMemberName().ToString();
		const FString OwnerClassPath = OwnerClass ? OwnerClass->GetPathName() : FString();
		const FString OwnerClassName = OwnerClass ? OwnerClass->GetName() : FString();

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("function_name"), FunctionName);
		Attributes.Add(TEXT("function_path"), FunctionPath);
		Attributes.Add(TEXT("owner_class"), OwnerClassPath);
		Attributes.Add(TEXT("owner_class_name"), OwnerClassName);
		Attributes.Add(TEXT("is_native_function"), TargetFunction && TargetFunction->GetOwnerClass() && TargetFunction->GetOwnerClass()->ClassGeneratedBy == nullptr ? TEXT("true") : TEXT("false"));
		Attributes.Add(TEXT("is_latent"), CallFunctionNode->IsLatentFunction() ? TEXT("true") : TEXT("false"));
		Attributes.Add(TEXT("is_interface_call"), CallFunctionNode->bIsInterfaceCall ? TEXT("true") : TEXT("false"));

		const FString FunctionId = AddSemanticEntity(ProjectId, TEXT("u_function"), FunctionPath, FunctionName, Attributes, Node.GetPathName(), OutEntities);
		AddRelation(ProjectId, TEXT("calls_function"), NodeId, FunctionId, Node.GetPathName(), TEXT("UK2Node_CallFunction target function."), OutRelations, false, Attributes);
		if (OwnerClass)
		{
			TMap<FString, FString> ClassAttributes;
			ClassAttributes.Add(TEXT("reference_role"), TEXT("function_owner"));
			ClassAttributes.Add(TEXT("function_path"), FunctionPath);
			ClassAttributes.Add(TEXT("function_name"), FunctionName);
			const FString OwnerClassId = AddClassSemanticEntity(ProjectId, OwnerClass, OwnerClassPath, OwnerClassName, ClassAttributes, Node.GetPathName(), OutEntities);
			AddRelation(ProjectId, TEXT("declares_function"), OwnerClassId, FunctionId, Node.GetPathName(), TEXT("UFunction owner class declares the called function."), OutRelations, false, Attributes);
			AddRelation(ProjectId, TEXT("class_references"), NodeId, OwnerClassId, Node.GetPathName(), TEXT("UK2Node_CallFunction references the target owner class."), OutRelations, false, ClassAttributes);
			NodeEntity.Attributes.Add(TEXT("semantic_owner_class"), OwnerClassPath);
			NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_function_owner_class"));
		}
		if (CallFunctionNode->IsLatentFunction())
		{
			AddRelation(ProjectId, TEXT("starts_latent_operation"), NodeId, FunctionId, Node.GetPathName(), TEXT("UK2Node_CallFunction target is a latent function."), OutRelations, false, Attributes);
			NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_latent_function"));
		}
		if (CallFunctionNode->bIsInterfaceCall)
		{
			AddRelation(ProjectId, TEXT("sends_interface_message"), NodeId, FunctionId, Node.GetPathName(), TEXT("UK2Node_CallFunction target is an interface call."), OutRelations, false, Attributes);
			NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_interface_message"));
		}

		NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("call_function"));
		NodeEntity.Attributes.Add(TEXT("semantic_function"), FunctionPath);
		NodeEntity.Attributes.Add(TEXT("semantic_is_latent"), CallFunctionNode->IsLatentFunction() ? TEXT("true") : TEXT("false"));
		NodeEntity.Attributes.Add(TEXT("semantic_is_interface_call"), CallFunctionNode->bIsInterfaceCall ? TEXT("true") : TEXT("false"));
		NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_call_function"));

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), TEXT("call_function"));
		SemanticObject->SetStringField(TEXT("function_path"), FunctionPath);
		SemanticObject->SetStringField(TEXT("function_name"), FunctionName);
		SemanticObject->SetStringField(TEXT("owner_class"), OwnerClassPath);
		SemanticObject->SetStringField(TEXT("owner_class_name"), OwnerClassName);
		SemanticObject->SetBoolField(TEXT("is_latent"), CallFunctionNode->IsLatentFunction());
		SemanticObject->SetBoolField(TEXT("is_interface_call"), CallFunctionNode->bIsInterfaceCall);
		return SemanticObject;
	}

	if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(&Node))
	{
		const FString EventName = EventNode->GetFunctionName().ToString();
		const FString EventKey = Blueprint.GetPathName() + TEXT(":event:") + EventName;

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("event_name"), EventName);
		Attributes.Add(TEXT("blueprint_path"), Blueprint.GetPathName());
		Attributes.Add(TEXT("is_override"), EventNode->bOverrideFunction ? TEXT("true") : TEXT("false"));

		const FString EventId = AddSemanticEntity(ProjectId, TEXT("blueprint_event"), EventKey, EventName, Attributes, Node.GetPathName(), OutEntities);
		AddRelation(ProjectId, TEXT("overrides_event"), NodeId, EventId, Node.GetPathName(), TEXT("UK2Node_Event function entry."), OutRelations);

		NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("event"));
		NodeEntity.Attributes.Add(TEXT("semantic_event"), EventName);
		NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_event"));

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), TEXT("event"));
		SemanticObject->SetStringField(TEXT("event_name"), EventName);
		SemanticObject->SetBoolField(TEXT("is_override"), EventNode->bOverrideFunction);
		return SemanticObject;
	}

	if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(&Node))
	{
		const bool bIsWrite = Node.IsA<UK2Node_VariableSet>();
		const bool bIsRead = Node.IsA<UK2Node_VariableGet>();
		const FString VariableName = VariableNode->GetVarNameString();
		const FString VariableEntityKey = VariableKey(Blueprint, VariableName);
		const UClass* VariableSourceClass = VariableNode->GetVariableSourceClass();
		const FProperty* VariableProperty = VariableNode->GetPropertyForVariable();
		if (!VariableSourceClass && VariableProperty)
		{
			VariableSourceClass = VariableProperty->GetOwnerClass();
		}
		const FString VariableSourceClassPath = VariableSourceClass ? VariableSourceClass->GetPathName() : FString();
		const FString VariableSourceClassName = VariableSourceClass ? VariableSourceClass->GetName() : FString();
		const FString PropertyName = VariableProperty ? VariableProperty->GetName() : VariableName;
		const FString PropertyCppType = VariableProperty ? VariableProperty->GetCPPType() : FString();
		const FString PropertyKind = VariableProperty && VariableProperty->GetClass() ? VariableProperty->GetClass()->GetName() : FString();

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("variable_name"), VariableName);
		Attributes.Add(TEXT("blueprint_path"), Blueprint.GetPathName());
		Attributes.Add(TEXT("source_class"), VariableSourceClassPath);
		Attributes.Add(TEXT("source_class_name"), VariableSourceClassName);
		Attributes.Add(TEXT("property_name"), PropertyName);
		Attributes.Add(TEXT("property_cpp_type"), PropertyCppType);
		Attributes.Add(TEXT("property_kind"), PropertyKind);

		const FString VariableId = AddSemanticEntity(ProjectId, TEXT("blueprint_variable"), VariableEntityKey, VariableName, Attributes, Node.GetPathName(), OutEntities);
		if (VariableSourceClass)
		{
			TMap<FString, FString> ClassAttributes;
			ClassAttributes.Add(TEXT("reference_role"), TEXT("variable_owner"));
			ClassAttributes.Add(TEXT("variable_name"), VariableName);
			ClassAttributes.Add(TEXT("property_cpp_type"), PropertyCppType);
			const FString SourceClassId = AddClassSemanticEntity(ProjectId, VariableSourceClass, VariableSourceClassPath, VariableSourceClassName, ClassAttributes, Node.GetPathName(), OutEntities);
			AddRelation(ProjectId, TEXT("declares_variable"), SourceClassId, VariableId, Node.GetPathName(), TEXT("Variable source class declares the accessed variable."), OutRelations, false, Attributes);
			AddRelation(ProjectId, TEXT("class_references"), NodeId, SourceClassId, Node.GetPathName(), TEXT("UK2Node_Variable references the variable source class."), OutRelations, false, ClassAttributes);
			NodeEntity.Attributes.Add(TEXT("semantic_variable_source_class"), VariableSourceClassPath);
			NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_variable_owner_class"));
		}
		if (bIsWrite)
		{
			AddRelation(ProjectId, TEXT("writes_variable"), NodeId, VariableId, Node.GetPathName(), TEXT("UK2Node_VariableSet target variable."), OutRelations);
			NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("write_variable"));
			NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_write_variable"));
		}
		else if (bIsRead)
		{
			AddRelation(ProjectId, TEXT("reads_variable"), NodeId, VariableId, Node.GetPathName(), TEXT("UK2Node_VariableGet target variable."), OutRelations);
			NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("read_variable"));
			NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_read_variable"));
		}
		NodeEntity.Attributes.Add(TEXT("semantic_variable"), VariableName);

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), bIsWrite ? TEXT("write_variable") : TEXT("read_variable"));
		SemanticObject->SetStringField(TEXT("variable_name"), VariableName);
		SemanticObject->SetStringField(TEXT("source_class"), VariableSourceClassPath);
		SemanticObject->SetStringField(TEXT("property_cpp_type"), PropertyCppType);
		SemanticObject->SetStringField(TEXT("property_kind"), PropertyKind);
		return SemanticObject;
	}

	if (const UK2Node_DynamicCast* DynamicCastNode = Cast<UK2Node_DynamicCast>(&Node))
	{
		const UClass* TargetType = DynamicCastNode->TargetType;
		const FString TargetPath = TargetType ? TargetType->GetPathName() : FString();
		const FString TargetName = TargetType ? TargetType->GetName() : FString();

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("class_path"), TargetPath);
		Attributes.Add(TEXT("class_name"), TargetName);

		const FString ClassId = AddSemanticEntity(ProjectId, TEXT("u_class"), TargetPath, TargetName, Attributes, Node.GetPathName(), OutEntities);
		AddRelation(ProjectId, TEXT("casts_to"), NodeId, ClassId, Node.GetPathName(), TEXT("UK2Node_DynamicCast target class."), OutRelations);

		NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("dynamic_cast"));
		NodeEntity.Attributes.Add(TEXT("semantic_target_class"), TargetPath);
		NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_dynamic_cast"));

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), TEXT("dynamic_cast"));
		SemanticObject->SetStringField(TEXT("target_class"), TargetPath);
		return SemanticObject;
	}

	if (const UK2Node_SpawnActorFromClass* SpawnNode = Cast<UK2Node_SpawnActorFromClass>(&Node))
	{
		const UEdGraphPin* ClassPin = SpawnNode->GetClassPin();
		const UEdGraphPin* ResultPin = SpawnNode->GetResultPin();
		const UClass* TargetClass = SpawnNode->GetClassToSpawn();
		FString TargetPath = TargetClass ? TargetClass->GetPathName() : PinReferenceDefaultPath(ClassPin);
		if (TargetPath.IsEmpty())
		{
			TargetPath = PinSubCategoryObjectPath(ResultPin);
		}

		const FString TargetKey = TargetPath.IsEmpty() ? Node.GetPathName() + TEXT(":spawn_class") : TargetPath;
		const FString TargetName = TargetClass
			? TargetClass->GetName()
			: (TargetPath.IsEmpty() ? NodeTitleString(Node) : TargetPath);

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("spawn_node_class"), Node.GetClass()->GetPathName());
		Attributes.Add(TEXT("target_class_path"), TargetPath);
		Attributes.Add(TEXT("target_class_name"), TargetName);
		Attributes.Add(TEXT("class_pin_name"), ClassPin ? ClassPin->PinName.ToString() : FString());
		Attributes.Add(TEXT("class_pin_default"), PinReferenceDefaultPath(ClassPin));
		Attributes.Add(TEXT("result_pin_type"), PinSubCategoryObjectPath(ResultPin));

		const FString ClassId = AddSemanticEntity(ProjectId, TEXT("u_class"), TargetKey, TargetName, Attributes, Node.GetPathName(), OutEntities);
		AddRelation(ProjectId, TEXT("spawns_class"), NodeId, ClassId, Node.GetPathName(), TEXT("UK2Node_SpawnActorFromClass target class."), OutRelations, false, Attributes);

		NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("spawn_actor"));
		NodeEntity.Attributes.Add(TEXT("semantic_target_class"), TargetPath);
		NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_spawn_actor"));

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), TEXT("spawn_actor"));
		SemanticObject->SetStringField(TEXT("target_class"), TargetPath);
		SemanticObject->SetStringField(TEXT("target_class_name"), TargetName);
		SetOptionalPinName(SemanticObject.ToSharedRef(), TEXT("class_pin"), ClassPin);
		SetOptionalPinName(SemanticObject.ToSharedRef(), TEXT("result_pin"), ResultPin);
		return SemanticObject;
	}

	if (const UK2Node_SpawnActor* LegacySpawnNode = Cast<UK2Node_SpawnActor>(&Node))
	{
		const UEdGraphPin* BlueprintPin = LegacySpawnNode->GetBlueprintPin();
		const UEdGraphPin* ResultPin = LegacySpawnNode->GetResultPin();
		FString TargetPath = PinReferenceDefaultPath(BlueprintPin);
		if (TargetPath.IsEmpty())
		{
			TargetPath = PinSubCategoryObjectPath(ResultPin);
		}

		const FString TargetKey = TargetPath.IsEmpty() ? Node.GetPathName() + TEXT(":spawn_class") : TargetPath;
		const FString TargetName = TargetPath.IsEmpty() ? NodeTitleString(Node) : TargetPath;

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("spawn_node_class"), Node.GetClass()->GetPathName());
		Attributes.Add(TEXT("target_class_path"), TargetPath);
		Attributes.Add(TEXT("target_class_name"), TargetName);
		Attributes.Add(TEXT("blueprint_pin_name"), BlueprintPin ? BlueprintPin->PinName.ToString() : FString());
		Attributes.Add(TEXT("blueprint_pin_default"), PinReferenceDefaultPath(BlueprintPin));
		Attributes.Add(TEXT("result_pin_type"), PinSubCategoryObjectPath(ResultPin));

		const FString ClassId = AddSemanticEntity(ProjectId, TEXT("u_class"), TargetKey, TargetName, Attributes, Node.GetPathName(), OutEntities);
		AddRelation(ProjectId, TEXT("spawns_class"), NodeId, ClassId, Node.GetPathName(), TEXT("UK2Node_SpawnActor target class or Blueprint asset."), OutRelations, false, Attributes);

		NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("spawn_actor"));
		NodeEntity.Attributes.Add(TEXT("semantic_target_class"), TargetPath);
		NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_spawn_actor"));

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), TEXT("spawn_actor"));
		SemanticObject->SetStringField(TEXT("target_class"), TargetPath);
		SemanticObject->SetStringField(TEXT("target_class_name"), TargetName);
		SetOptionalPinName(SemanticObject.ToSharedRef(), TEXT("blueprint_pin"), BlueprintPin);
		SetOptionalPinName(SemanticObject.ToSharedRef(), TEXT("result_pin"), ResultPin);
		return SemanticObject;
	}

	if (const UK2Node_LoadAsset* LoadAssetNode = Cast<UK2Node_LoadAsset>(&Node))
	{
		const bool bLoadsClass = LoadAssetNode->IsA<UK2Node_LoadAssetClass>();
		const UEdGraphPin* AssetPin = FindAssetReferenceInputPin(*LoadAssetNode);
		const UEdGraphPin* OutputPin = FindFirstDataOutputPin(*LoadAssetNode);
		const FString TargetAssetPath = PinReferenceDefaultPath(AssetPin);
		const FString OutputTypePath = PinSubCategoryObjectPath(OutputPin);
		const FString TargetKey = !TargetAssetPath.IsEmpty()
			? TargetAssetPath
			: (!OutputTypePath.IsEmpty() ? OutputTypePath : Node.GetPathName() + TEXT(":load_asset"));
		const FString TargetName = !TargetAssetPath.IsEmpty()
			? TargetAssetPath
			: (!OutputTypePath.IsEmpty() ? OutputTypePath : NodeTitleString(Node));
		const FString LoadKind = bLoadsClass ? TEXT("load_asset_class") : TEXT("load_asset");

		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("load_kind"), LoadKind);
		Attributes.Add(TEXT("target_asset_path"), TargetAssetPath);
		Attributes.Add(TEXT("output_type_path"), OutputTypePath);
		Attributes.Add(TEXT("asset_pin_name"), AssetPin ? AssetPin->PinName.ToString() : FString());
		Attributes.Add(TEXT("asset_pin_default"), PinReferenceDefaultPath(AssetPin));
		Attributes.Add(TEXT("output_pin_name"), OutputPin ? OutputPin->PinName.ToString() : FString());

		const FString AssetId = AddSemanticEntity(ProjectId, TEXT("asset_reference"), TargetKey, TargetName, Attributes, Node.GetPathName(), OutEntities);
		AddRelation(ProjectId, TEXT("loads_asset"), NodeId, AssetId, Node.GetPathName(), TEXT("UK2Node_LoadAsset target asset reference."), OutRelations, false, Attributes);

		NodeEntity.Attributes.Add(TEXT("semantic_kind"), LoadKind);
		NodeEntity.Attributes.Add(TEXT("semantic_target_asset"), TargetAssetPath);
		NodeEntity.Attributes.Add(TEXT("semantic_output_type"), OutputTypePath);
		NodeEntity.Completeness.Covered.AddUnique(TEXT("node_semantic_load_asset"));

		SemanticObject = MakeShared<FJsonObject>();
		SemanticObject->SetStringField(TEXT("kind"), LoadKind);
		SemanticObject->SetStringField(TEXT("target_asset"), TargetAssetPath);
		SemanticObject->SetStringField(TEXT("output_type"), OutputTypePath);
		SetOptionalPinName(SemanticObject.ToSharedRef(), TEXT("asset_pin"), AssetPin);
		SetOptionalPinName(SemanticObject.ToSharedRef(), TEXT("output_pin"), OutputPin);
		return SemanticObject;
	}

	NodeEntity.Attributes.Add(TEXT("semantic_kind"), TEXT("generic_only"));
	NodeEntity.Attributes.Add(TEXT("semantic_support"), TEXT("generic_only"));
	NodeEntity.Completeness.Warnings.AddUnique(TEXT("semantic_generic_only"));
	return nullptr;
}

FEntityRecord MakeGraphEntity(const FString& ProjectId, const FString& GraphId, const UBlueprint& Blueprint, const UEdGraph& Graph)
{
	FEntityRecord Entity;
	Entity.Id = GraphId;
	Entity.Kind = TEXT("blueprint_graph");
	Entity.CanonicalKey = GraphKey(Blueprint, Graph);
	Entity.DisplayName = Graph.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes.Add(TEXT("asset_path"), Blueprint.GetPathName());
	Entity.Attributes.Add(TEXT("graph_name"), Graph.GetName());
	Entity.Attributes.Add(TEXT("graph_guid"), GuidString(Graph.GraphGuid));
	Entity.Attributes.Add(TEXT("schema_class"), Graph.Schema ? Graph.Schema->GetClass()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Complete;
	Entity.Completeness.Covered = { TEXT("graph_metadata"), TEXT("node_membership") };
	Entity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		Graph.GetPathName(),
		TEXT("UEdGraph read from UBlueprint editor source graph.")
	});
	return Entity;
}

FEntityRecord MakeNodeEntity(const FString& ProjectId, const FString& NodeId, const UEdGraph& Graph, const UEdGraphNode& Node)
{
	FEntityRecord Entity;
	Entity.Id = NodeId;
	Entity.Kind = TEXT("blueprint_node");
	Entity.CanonicalKey = NodeCanonicalKey(Graph, Node);
	Entity.DisplayName = Node.GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes.Add(TEXT("graph_path"), Graph.GetPathName());
	Entity.Attributes.Add(TEXT("node_name"), Node.GetName());
	Entity.Attributes.Add(TEXT("node_guid"), GuidString(Node.NodeGuid));
	Entity.Attributes.Add(TEXT("node_class"), Node.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("node_title"), Entity.DisplayName);
	Entity.Attributes.Add(TEXT("node_pos_x"), FString::FromInt(Node.NodePosX));
	Entity.Attributes.Add(TEXT("node_pos_y"), FString::FromInt(Node.NodePosY));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Complete;
	Entity.Completeness.Covered = { TEXT("node_metadata"), TEXT("pin_membership") };
	Entity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		Node.GetPathName(),
		TEXT("UEdGraphNode read from UBlueprint editor source graph.")
	});
	return Entity;
}

FEntityRecord MakePinEntity(const FString& ProjectId, const FString& PinId, const UEdGraphNode& Node, const UEdGraphPin& Pin)
{
	FEntityRecord Entity;
	Entity.Id = PinId;
	Entity.Kind = TEXT("blueprint_pin");
	Entity.CanonicalKey = Node.GetPathName() + TEXT(":pin:") + (GuidString(Pin.PinId).IsEmpty() ? Pin.PinName.ToString() : GuidString(Pin.PinId));
	Entity.DisplayName = Pin.PinName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes.Add(TEXT("node_path"), Node.GetPathName());
	Entity.Attributes.Add(TEXT("pin_id"), GuidString(Pin.PinId));
	Entity.Attributes.Add(TEXT("pin_name"), Pin.PinName.ToString());
	Entity.Attributes.Add(TEXT("direction"), PinDirectionString(Pin.Direction));
	Entity.Attributes.Add(TEXT("pin_category"), Pin.PinType.PinCategory.ToString());
	Entity.Attributes.Add(TEXT("pin_subcategory"), Pin.PinType.PinSubCategory.ToString());
	Entity.Attributes.Add(TEXT("default_value"), Pin.DefaultValue);
	Entity.Attributes.Add(TEXT("default_object"), Pin.DefaultObject ? Pin.DefaultObject->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("parent_pin_id"), Pin.ParentPin ? GuidString(Pin.ParentPin->PinId) : FString());
	Entity.Attributes.Add(TEXT("parent_pin_name"), Pin.ParentPin ? Pin.ParentPin->PinName.ToString() : FString());
	Entity.Attributes.Add(TEXT("sub_pin_count"), FString::FromInt(Pin.SubPins.Num()));
	Entity.Attributes.Add(TEXT("is_split_or_sub_pin"), BpBool(Pin.ParentPin != nullptr || Pin.SubPins.Num() > 0));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Complete;
	Entity.Completeness.Covered = { TEXT("pin_metadata"), TEXT("pin_links"), TEXT("split_sub_pin_metadata") };
	Entity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		Node.GetPathName(),
		TEXT("UEdGraphPin read from UBlueprint editor source graph.")
	});
	return Entity;
}

FEntityRecord MakeBasicBlockEntity(
	const FString& ProjectId,
	const UEdGraph& Graph,
	const FBasicBlockRecord& Block,
	const TMap<FString, FNodeProjectionRecord>& NodeById)
{
	FEntityRecord Entity;
	Entity.Id = Block.BlockId;
	Entity.Kind = TEXT("cfg_basic_block");
	Entity.CanonicalKey = Block.CanonicalKey;
	Entity.DisplayName = FString::Printf(TEXT("Block %d"), Block.Index);
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes.Add(TEXT("graph_path"), Graph.GetPathName());
	Entity.Attributes.Add(TEXT("block_index"), FString::FromInt(Block.Index));
	Entity.Attributes.Add(TEXT("node_count"), FString::FromInt(Block.NodeIds.Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));

	if (Block.NodeIds.Num() > 0)
	{
		const FNodeProjectionRecord* EntryNode = NodeById.Find(Block.NodeIds[0]);
		const FNodeProjectionRecord* ExitNode = NodeById.Find(Block.NodeIds.Last());
		Entity.Attributes.Add(TEXT("entry_node_id"), Block.NodeIds[0]);
		Entity.Attributes.Add(TEXT("exit_node_id"), Block.NodeIds.Last());
		Entity.Attributes.Add(TEXT("entry_node_key"), EntryNode ? EntryNode->CanonicalKey : FString());
		Entity.Attributes.Add(TEXT("exit_node_key"), ExitNode ? ExitNode->CanonicalKey : FString());
	}

	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("basic_block_membership"), TEXT("basic_block_flow") };
	Entity.Completeness.Omitted = { TEXT("dominators"), TEXT("loop_nesting") };
	Entity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		Graph.GetPathName(),
		TEXT("Derived CFG basic block from Node-level exec_flows_to projection.")
	});
	return Entity;
}

void AppendCfgBasicBlocks(
	const FString& ProjectId,
	const FString& GraphId,
	const UEdGraph& Graph,
	const TArray<FNodeProjectionRecord>& NodeRecords,
	const TArray<FExecProjectionEdge>& ExecEdges,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations,
	TSharedRef<FJsonObject> GraphObject)
{
	TSet<FString> ExecNodeIds;
	TMap<FString, FNodeProjectionRecord> NodeById;
	TMap<FString, TArray<FString>> OutEdgesByNode;
	TMap<FString, TArray<FString>> InEdgesByNode;

	for (const FNodeProjectionRecord& NodeRecord : NodeRecords)
	{
		NodeById.Add(NodeRecord.NodeId, NodeRecord);
		if (NodeRecord.bHasExecPin)
		{
			ExecNodeIds.Add(NodeRecord.NodeId);
		}
	}

	for (const FExecProjectionEdge& Edge : ExecEdges)
	{
		if (Edge.FromNodeId.IsEmpty() || Edge.ToNodeId.IsEmpty() || Edge.FromNodeId == Edge.ToNodeId)
		{
			continue;
		}

		ExecNodeIds.Add(Edge.FromNodeId);
		ExecNodeIds.Add(Edge.ToNodeId);
		OutEdgesByNode.FindOrAdd(Edge.FromNodeId).AddUnique(Edge.ToNodeId);
		InEdgesByNode.FindOrAdd(Edge.ToNodeId).AddUnique(Edge.FromNodeId);
	}

	TSet<FString> LeaderNodeIds;
	for (const FString& NodeId : ExecNodeIds)
	{
		const int32 InCount = FlowCount(InEdgesByNode, NodeId);
		if (InCount != 1)
		{
			LeaderNodeIds.Add(NodeId);
			continue;
		}

		const TArray<FString>* Incoming = InEdgesByNode.Find(NodeId);
		const FString& PredecessorId = (*Incoming)[0];
		if (FlowCount(OutEdgesByNode, PredecessorId) != 1)
		{
			LeaderNodeIds.Add(NodeId);
		}
	}

	TSet<FString> VisitedNodeIds;
	TArray<FBasicBlockRecord> Blocks;
	TMap<FString, FString> NodeIdToBlockId;

	for (const FNodeProjectionRecord& NodeRecord : NodeRecords)
	{
		if (!ExecNodeIds.Contains(NodeRecord.NodeId) || VisitedNodeIds.Contains(NodeRecord.NodeId))
		{
			continue;
		}

		FBasicBlockRecord Block;
		Block.Index = Blocks.Num();
		Block.CanonicalKey = Graph.GetPathName() + TEXT(":cfg_block:") + FString::FromInt(Block.Index);
		Block.BlockId = MakeStableId(ProjectId, TEXT("cfg_basic_block"), Block.CanonicalKey);

		FString CurrentNodeId = NodeRecord.NodeId;
		while (!CurrentNodeId.IsEmpty() && ExecNodeIds.Contains(CurrentNodeId) && !VisitedNodeIds.Contains(CurrentNodeId))
		{
			VisitedNodeIds.Add(CurrentNodeId);
			Block.NodeIds.Add(CurrentNodeId);
			NodeIdToBlockId.Add(CurrentNodeId, Block.BlockId);

			const TArray<FString>* OutEdges = OutEdgesByNode.Find(CurrentNodeId);
			if (!OutEdges || OutEdges->Num() != 1)
			{
				break;
			}

			const FString NextNodeId = (*OutEdges)[0];
			if (!ExecNodeIds.Contains(NextNodeId) || VisitedNodeIds.Contains(NextNodeId))
			{
				break;
			}

			if (FlowCount(InEdgesByNode, NextNodeId) != 1)
			{
				break;
			}

			if (LeaderNodeIds.Contains(NextNodeId))
			{
				break;
			}

			CurrentNodeId = NextNodeId;
		}

		if (Block.NodeIds.Num() > 0)
		{
			Blocks.Add(MoveTemp(Block));
		}
	}

	for (const FExecProjectionEdge& Edge : ExecEdges)
	{
		const FString* FromBlockId = NodeIdToBlockId.Find(Edge.FromNodeId);
		const FString* ToBlockId = NodeIdToBlockId.Find(Edge.ToNodeId);
		if (!FromBlockId || !ToBlockId || *FromBlockId == *ToBlockId)
		{
			continue;
		}

		for (FBasicBlockRecord& Block : Blocks)
		{
			if (Block.BlockId == *FromBlockId)
			{
				Block.OutgoingBlockIds.Add(*ToBlockId);
				break;
			}
		}

		TMap<FString, FString> BlockFlowAttributes = Edge.Attributes;
		BlockFlowAttributes.Add(TEXT("projection"), TEXT("cfg_basic_block"));
		BlockFlowAttributes.Add(TEXT("source_node_id"), Edge.FromNodeId);
		BlockFlowAttributes.Add(TEXT("target_node_id"), Edge.ToNodeId);
		AddRelation(
			ProjectId,
			TEXT("exec_flows_to"),
			*FromBlockId,
			*ToBlockId,
			Edge.EvidencePath,
			TEXT("Derived CFG block-level flow from Node-level exec_flows_to."),
			OutRelations,
			true,
			BlockFlowAttributes);
	}

	TArray<TSharedPtr<FJsonValue>> BlockValues;
	for (const FBasicBlockRecord& Block : Blocks)
	{
		OutEntities.Add(MakeBasicBlockEntity(ProjectId, Graph, Block, NodeById));
		AddRelation(
			ProjectId,
			TEXT("contains_basic_block"),
			GraphId,
			Block.BlockId,
			Graph.GetPathName(),
			TEXT("Graph contains derived CFG basic block."),
			OutRelations,
			true);

		TSharedRef<FJsonObject> BlockObject = MakeShared<FJsonObject>();
		BlockObject->SetStringField(TEXT("id"), Block.BlockId);
		BlockObject->SetNumberField(TEXT("index"), Block.Index);

		TArray<TSharedPtr<FJsonValue>> NodeIdValues;
		for (const FString& NodeId : Block.NodeIds)
		{
			NodeIdValues.Add(MakeShared<FJsonValueString>(NodeId));
			AddRelation(
				ProjectId,
				TEXT("contains_node"),
				Block.BlockId,
				NodeId,
				Graph.GetPathName(),
				TEXT("CFG basic block contains Blueprint node."),
				OutRelations,
				true);
		}
		BlockObject->SetArrayField(TEXT("node_ids"), NodeIdValues);

		TArray<TSharedPtr<FJsonValue>> OutgoingBlockValues;
		TArray<FString> SortedOutgoing = Block.OutgoingBlockIds.Array();
		SortedOutgoing.Sort();
		for (const FString& OutgoingBlockId : SortedOutgoing)
		{
			OutgoingBlockValues.Add(MakeShared<FJsonValueString>(OutgoingBlockId));
		}
		BlockObject->SetArrayField(TEXT("outgoing_block_ids"), OutgoingBlockValues);
		BlockValues.Add(MakeShared<FJsonValueObject>(BlockObject));
	}

	GraphObject->SetArrayField(TEXT("cfg_basic_blocks"), BlockValues);
	GraphObject->SetNumberField(TEXT("cfg_basic_block_count"), BlockValues.Num());
}

FEntityRecord MakeDfgValueEntity(
	const FString& ProjectId,
	const UEdGraph& Graph,
	const FDataProjectionEdge& Edge,
	int32 Index)
{
	FEntityRecord Entity;
	const FString CanonicalKey = Graph.GetPathName() + TEXT(":dfg_value:") + Edge.FromPinId + TEXT(":to:") + Edge.ToPinId;
	Entity.Id = MakeStableId(ProjectId, TEXT("dfg_value"), CanonicalKey);
	Entity.Kind = TEXT("dfg_value");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = FString::Printf(TEXT("Value %d"), Index);
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes.Add(TEXT("graph_path"), Graph.GetPathName());
	Entity.Attributes.Add(TEXT("source_node_id"), Edge.FromNodeId);
	Entity.Attributes.Add(TEXT("target_node_id"), Edge.ToNodeId);
	Entity.Attributes.Add(TEXT("source_pin_id"), Edge.FromPinId);
	Entity.Attributes.Add(TEXT("target_pin_id"), Edge.ToPinId);
	if (const FString* SourcePinName = Edge.Attributes.Find(TEXT("source_pin_name")))
	{
		Entity.Attributes.Add(TEXT("source_pin_name"), *SourcePinName);
	}
	if (const FString* TargetPinName = Edge.Attributes.Find(TEXT("target_pin_name")))
	{
		Entity.Attributes.Add(TEXT("target_pin_name"), *TargetPinName);
	}
	Entity.Attributes.Add(TEXT("pin_category"), Edge.PinCategory);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("pin_data_flow_value") };
	Entity.Completeness.Omitted = { TEXT("ssa_versioning"), TEXT("path_conditions") };
	Entity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		Edge.EvidencePath,
		TEXT("Derived DFG value from a canonical data Pin link.")
	});
	return Entity;
}

void AppendDfgDefUse(
	const FString& ProjectId,
	const UBlueprint& Blueprint,
	const UEdGraph& Graph,
	const TArray<FNodeProjectionRecord>& NodeRecords,
	const TArray<FDataProjectionEdge>& DataEdges,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations,
	TSharedRef<FJsonObject> GraphObject)
{
	TArray<TSharedPtr<FJsonValue>> ValueValues;
	for (int32 EdgeIndex = 0; EdgeIndex < DataEdges.Num(); ++EdgeIndex)
	{
		const FDataProjectionEdge& Edge = DataEdges[EdgeIndex];
		FEntityRecord ValueEntity = MakeDfgValueEntity(ProjectId, Graph, Edge, EdgeIndex);
		const FString ValueId = ValueEntity.Id;
		OutEntities.Add(MoveTemp(ValueEntity));

		TMap<FString, FString> ValueAttributes = Edge.Attributes;
		ValueAttributes.Add(TEXT("value_kind"), TEXT("pin_link"));
		AddRelation(
			ProjectId,
			TEXT("defines_value"),
			Edge.FromNodeId,
			ValueId,
			Edge.EvidencePath,
			TEXT("Source node defines a data value through an output Pin."),
			OutRelations,
			true,
			ValueAttributes);

		AddRelation(
			ProjectId,
			TEXT("uses_value"),
			ValueId,
			Edge.ToNodeId,
			Edge.EvidencePath,
			TEXT("Target node uses a data value through an input Pin."),
			OutRelations,
			true,
			ValueAttributes);

		TSharedRef<FJsonObject> ValueObject = MakeShared<FJsonObject>();
		ValueObject->SetStringField(TEXT("id"), ValueId);
		ValueObject->SetStringField(TEXT("source_node_id"), Edge.FromNodeId);
		ValueObject->SetStringField(TEXT("target_node_id"), Edge.ToNodeId);
		ValueObject->SetStringField(TEXT("source_pin_id"), Edge.FromPinId);
		ValueObject->SetStringField(TEXT("target_pin_id"), Edge.ToPinId);
		ValueObject->SetStringField(TEXT("source_pin_name"), Edge.Attributes.FindRef(TEXT("source_pin_name")));
		ValueObject->SetStringField(TEXT("target_pin_name"), Edge.Attributes.FindRef(TEXT("target_pin_name")));
		ValueObject->SetStringField(TEXT("pin_category"), Edge.PinCategory);
		ValueValues.Add(MakeShared<FJsonValueObject>(ValueObject));
	}

	TArray<TSharedPtr<FJsonValue>> VariableAccessValues;
	for (const FNodeProjectionRecord& NodeRecord : NodeRecords)
	{
		if (NodeRecord.VariableEntityId.IsEmpty() || NodeRecord.VariableName.IsEmpty())
		{
			continue;
		}

		const FString EvidencePath = NodeRecord.Node ? NodeRecord.Node->GetPathName() : NodeRecord.CanonicalKey;
		if (NodeRecord.SemanticKind == TEXT("write_variable"))
		{
			TMap<FString, FString> VariableAttributes;
			VariableAttributes.Add(TEXT("value_kind"), TEXT("blueprint_variable_storage"));
			VariableAttributes.Add(TEXT("variable_name"), NodeRecord.VariableName);
			VariableAttributes.Add(TEXT("access"), TEXT("write"));
			AddRelation(
				ProjectId,
				TEXT("defines_value"),
				NodeRecord.NodeId,
				NodeRecord.VariableEntityId,
				EvidencePath,
				TEXT("Variable write defines the current storage value."),
				OutRelations,
				true,
				VariableAttributes);
		}
		else if (NodeRecord.SemanticKind == TEXT("read_variable"))
		{
			TMap<FString, FString> VariableAttributes;
			VariableAttributes.Add(TEXT("value_kind"), TEXT("blueprint_variable_storage"));
			VariableAttributes.Add(TEXT("variable_name"), NodeRecord.VariableName);
			VariableAttributes.Add(TEXT("access"), TEXT("read"));
			AddRelation(
				ProjectId,
				TEXT("uses_value"),
				NodeRecord.VariableEntityId,
				NodeRecord.NodeId,
				EvidencePath,
				TEXT("Variable read uses the current storage value."),
				OutRelations,
				true,
				VariableAttributes);
		}
		else
		{
			continue;
		}

		TSharedRef<FJsonObject> AccessObject = MakeShared<FJsonObject>();
		AccessObject->SetStringField(TEXT("node_id"), NodeRecord.NodeId);
		AccessObject->SetStringField(TEXT("variable_id"), NodeRecord.VariableEntityId);
		AccessObject->SetStringField(TEXT("variable_name"), NodeRecord.VariableName);
		AccessObject->SetStringField(TEXT("access"), NodeRecord.SemanticKind == TEXT("write_variable") ? TEXT("write") : TEXT("read"));
		AccessObject->SetStringField(TEXT("blueprint_path"), Blueprint.GetPathName());
		VariableAccessValues.Add(MakeShared<FJsonValueObject>(AccessObject));
	}

	GraphObject->SetArrayField(TEXT("dfg_values"), ValueValues);
	GraphObject->SetNumberField(TEXT("dfg_value_count"), ValueValues.Num());
	GraphObject->SetArrayField(TEXT("dfg_variable_accesses"), VariableAccessValues);
	GraphObject->SetNumberField(TEXT("dfg_variable_access_count"), VariableAccessValues.Num());
}
}

void FBlueprintGraphReader::AppendBlueprintGraph(
	UBlueprint& Blueprint,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<UEdGraph*> Graphs;
	Blueprint.GetAllGraphs(Graphs);
	Graphs.RemoveAll([](const UEdGraph* Graph)
	{
		return Graph == nullptr;
	});
	Graphs.Sort([](const UEdGraph& Left, const UEdGraph& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});

	TSharedRef<FJsonObject> BlueprintSnapshot = MakeShared<FJsonObject>();
	BlueprintSnapshot->SetStringField(TEXT("schema_version"), TEXT("uepi.blueprint_graph.v1"));
	BlueprintSnapshot->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::EditorSourceGraph));
	BlueprintSnapshot->SetStringField(TEXT("blueprint_path"), Blueprint.GetPathName());

	TArray<TSharedPtr<FJsonValue>> ComponentTemplateValues;
	if (Blueprint.SimpleConstructionScript)
	{
		UBlueprintGeneratedClass* BlueprintGeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint.GeneratedClass.Get());
		TArray<USCS_Node*> SCSNodes = Blueprint.SimpleConstructionScript->GetAllNodes();
		SCSNodes.RemoveAll([](const USCS_Node* Node)
		{
			return Node == nullptr;
		});
		SCSNodes.Sort([](const USCS_Node& Left, const USCS_Node& Right)
		{
			return Left.GetVariableName().ToString() < Right.GetVariableName().ToString();
		});

		for (int32 ComponentIndex = 0; ComponentIndex < SCSNodes.Num(); ++ComponentIndex)
		{
			const USCS_Node* SCSNode = SCSNodes[ComponentIndex];
			const UActorComponent* ComponentTemplate = SCSNode->ComponentTemplate.Get();
			if (BlueprintGeneratedClass)
			{
				ComponentTemplate = SCSNode->GetActualComponentTemplate(BlueprintGeneratedClass);
			}
			const UClass* ComponentClass = SCSNode->ComponentClass;
			const FString ComponentName = SCSNode->GetVariableName().ToString();
			const FString CanonicalKey = Blueprint.GetPathName() + TEXT(":scs_component:") + ComponentName;
			const FString ComponentId = MakeStableId(ProjectId, TEXT("blueprint_component_template"), CanonicalKey);

			FEntityRecord ComponentEntity;
			ComponentEntity.Id = ComponentId;
			ComponentEntity.Kind = TEXT("blueprint_component_template");
			ComponentEntity.CanonicalKey = CanonicalKey;
			ComponentEntity.DisplayName = ComponentName;
			ComponentEntity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
			ComponentEntity.Attributes.Add(TEXT("blueprint_path"), Blueprint.GetPathName());
			ComponentEntity.Attributes.Add(TEXT("component_name"), ComponentName);
			ComponentEntity.Attributes.Add(TEXT("component_class"), ComponentClass ? ComponentClass->GetPathName() : FString());
			ComponentEntity.Attributes.Add(TEXT("template_path"), ComponentTemplate ? ComponentTemplate->GetPathName() : FString());
			ComponentEntity.Attributes.Add(TEXT("parent_component_name"), SCSNode->ParentComponentOrVariableName.ToString());
			ComponentEntity.Attributes.Add(TEXT("attach_to_name"), SCSNode->AttachToName.ToString());
			ComponentEntity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
			ComponentEntity.Completeness.State = ECompletenessState::Partial;
			ComponentEntity.Completeness.Covered = { TEXT("scs_node_metadata"), TEXT("component_template_class") };
			ComponentEntity.Completeness.Omitted = { TEXT("component_template_property_diff") };
			ComponentEntity.Evidence.Add({
				LexToString(ESourceLayer::EditorSourceGraph),
				SCSNode->GetPathName(),
				TEXT("SimpleConstructionScript node read from UBlueprint editor source graph.")
			});
			OutEntities.Add(MoveTemp(ComponentEntity));
			AddRelation(ProjectId, TEXT("contains_component_template"), AssetEntity.Id, ComponentId, SCSNode->GetPathName(), TEXT("Blueprint SimpleConstructionScript contains this component template."), OutRelations);

			TSharedRef<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
			ComponentObject->SetStringField(TEXT("id"), ComponentId);
			ComponentObject->SetNumberField(TEXT("index"), ComponentIndex);
			ComponentObject->SetStringField(TEXT("name"), ComponentName);
			ComponentObject->SetStringField(TEXT("class"), ComponentClass ? ComponentClass->GetPathName() : FString());
			ComponentObject->SetStringField(TEXT("template_path"), ComponentTemplate ? ComponentTemplate->GetPathName() : FString());
			ComponentObject->SetStringField(TEXT("parent_name"), SCSNode->ParentComponentOrVariableName.ToString());
			ComponentObject->SetStringField(TEXT("attach_to_name"), SCSNode->AttachToName.ToString());
			ComponentTemplateValues.Add(MakeShared<FJsonValueObject>(ComponentObject));
		}
	}

	TArray<TSharedPtr<FJsonValue>> GraphValues;
	TArray<FAnimBlueprintNodeRecord> AnimBlueprintNodeRecords;

	for (const UEdGraph* Graph : Graphs)
	{
		const FString GraphId = MakeStableId(ProjectId, TEXT("blueprint_graph"), GraphKey(Blueprint, *Graph));
		OutEntities.Add(MakeGraphEntity(ProjectId, GraphId, Blueprint, *Graph));
		AddRelation(ProjectId, TEXT("contains_graph"), AssetEntity.Id, GraphId, Graph->GetPathName(), TEXT("Blueprint contains UEdGraph."), OutRelations);

		TSharedRef<FJsonObject> GraphObject = MakeShared<FJsonObject>();
		GraphObject->SetStringField(TEXT("id"), GraphId);
		GraphObject->SetStringField(TEXT("name"), Graph->GetName());
		GraphObject->SetStringField(TEXT("guid"), GuidString(Graph->GraphGuid));
		GraphObject->SetStringField(TEXT("path"), Graph->GetPathName());

		TArray<TSharedPtr<FJsonValue>> NodeValues;
		TArray<FNodeProjectionRecord> NodeProjectionRecords;
		TArray<FExecProjectionEdge> ExecProjectionEdges;
		TArray<FDataProjectionEdge> DataProjectionEdges;
		TArray<UEdGraphNode*> Nodes = Graph->Nodes;
		Nodes.RemoveAll([](const UEdGraphNode* Node)
		{
			return Node == nullptr;
		});
		Nodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
		{
			return Left.GetName() < Right.GetName();
		});

		for (const UEdGraphNode* Node : Nodes)
		{
			const FString NodeId = MakeStableId(ProjectId, TEXT("blueprint_node"), NodeKey(GraphId, *Node));
			FAnimBlueprintNodeRecord AnimNodeRecord;
			AnimNodeRecord.Graph = Graph;
			AnimNodeRecord.Node = Node;
			AnimNodeRecord.GraphId = GraphId;
			AnimNodeRecord.NodeId = NodeId;
			AnimBlueprintNodeRecords.Add(MoveTemp(AnimNodeRecord));

			FEntityRecord NodeEntity = MakeNodeEntity(ProjectId, NodeId, *Graph, *Node);
			TSharedPtr<FJsonObject> SemanticObject = AnnotateNodeSemantics(ProjectId, Blueprint, NodeId, *Node, NodeEntity, OutEntities, OutRelations);
			const FString NodeCanonical = NodeEntity.CanonicalKey;
			const FString NodeDisplayName = NodeEntity.DisplayName;
			OutEntities.Add(MoveTemp(NodeEntity));
			AddRelation(ProjectId, TEXT("contains_node"), GraphId, NodeId, Node->GetPathName(), TEXT("Graph contains UEdGraphNode."), OutRelations);

			TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
			NodeObject->SetStringField(TEXT("id"), NodeId);
			NodeObject->SetStringField(TEXT("name"), Node->GetName());
			NodeObject->SetStringField(TEXT("guid"), GuidString(Node->NodeGuid));
			NodeObject->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
			NodeObject->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			NodeObject->SetNumberField(TEXT("node_pos_x"), Node->NodePosX);
			NodeObject->SetNumberField(TEXT("node_pos_y"), Node->NodePosY);
			if (SemanticObject.IsValid())
			{
				NodeObject->SetObjectField(TEXT("semantic"), SemanticObject.ToSharedRef());
			}

			TArray<TSharedPtr<FJsonValue>> PinValues;
			bool bNodeHasExecPin = false;
			for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
			{
				const UEdGraphPin* Pin = Node->Pins[PinIndex];
				if (!Pin)
				{
					continue;
				}

				if (Pin->PinType.PinCategory.ToString().Equals(TEXT("exec"), ESearchCase::IgnoreCase))
				{
					bNodeHasExecPin = true;
				}

				const FString PinId = MakeStableId(ProjectId, TEXT("blueprint_pin"), PinKey(NodeId, *Pin, PinIndex));
				OutEntities.Add(MakePinEntity(ProjectId, PinId, *Node, *Pin));
				AddRelation(ProjectId, TEXT("has_pin"), NodeId, PinId, Node->GetPathName(), TEXT("Node owns UEdGraphPin."), OutRelations);

				TSharedRef<FJsonObject> PinObject = MakeShared<FJsonObject>();
				PinObject->SetStringField(TEXT("id"), PinId);
				PinObject->SetStringField(TEXT("pin_id"), GuidString(Pin->PinId));
				PinObject->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PinObject->SetStringField(TEXT("direction"), PinDirectionString(Pin->Direction));
				PinObject->SetObjectField(TEXT("type"), PinTypeToJson(Pin->PinType));
				PinObject->SetStringField(TEXT("default_value"), Pin->DefaultValue);
				PinObject->SetStringField(TEXT("default_object"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString());
				PinObject->SetStringField(TEXT("parent_pin_id"), Pin->ParentPin ? GuidString(Pin->ParentPin->PinId) : FString());
				PinObject->SetStringField(TEXT("parent_pin_name"), Pin->ParentPin ? Pin->ParentPin->PinName.ToString() : FString());
				PinObject->SetNumberField(TEXT("sub_pin_count"), Pin->SubPins.Num());
				PinObject->SetBoolField(TEXT("is_split_or_sub_pin"), Pin->ParentPin != nullptr || Pin->SubPins.Num() > 0);
				TArray<TSharedPtr<FJsonValue>> SubPinIdValues;
				for (const UEdGraphPin* SubPin : Pin->SubPins)
				{
					if (SubPin)
					{
						SubPinIdValues.Add(MakeShared<FJsonValueString>(GuidString(SubPin->PinId)));
					}
				}
				PinObject->SetArrayField(TEXT("sub_pin_ids"), SubPinIdValues);

				TArray<TSharedPtr<FJsonValue>> LinkedPinValues;
				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNode())
					{
						continue;
					}

					const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
					const FString LinkedNodeId = MakeStableId(ProjectId, TEXT("blueprint_node"), NodeKey(GraphId, *LinkedNode));
					const int32 LinkedPinIndex = LinkedNode->Pins.IndexOfByKey(LinkedPin);
					const FString LinkedPinId = MakeStableId(ProjectId, TEXT("blueprint_pin"), PinKey(LinkedNodeId, *LinkedPin, LinkedPinIndex));
					LinkedPinValues.Add(MakeShared<FJsonValueString>(LinkedPinId));
					if (Pin->Direction == EGPD_Output)
					{
						TMap<FString, FString> PinLinkAttributes = MakePinLinkAttributes(*Node, *Pin, PinId, *LinkedNode, *LinkedPin, LinkedPinId);
						PinLinkAttributes.Add(TEXT("edge_kind"), TEXT("canonical_pin_link"));
						AddRelation(
							ProjectId,
							TEXT("connects_to"),
							PinId,
							LinkedPinId,
							Node->GetPathName(),
							TEXT("Directed output-to-input pin link from UEdGraphPin::LinkedTo."),
							OutRelations,
							false,
							PinLinkAttributes);
						if (LinkedNode != Node)
						{
							const FString FlowRelationType = ProjectedFlowRelationType(*Pin);
							TMap<FString, FString> FlowAttributes = PinLinkAttributes;
							FlowAttributes.Add(TEXT("edge_kind"), TEXT("node_flow_projection"));
							FlowAttributes.Add(TEXT("flow_relation_type"), FlowRelationType);
							FlowAttributes.Add(TEXT("projection"), TEXT("node_level"));
							AddRelation(
								ProjectId,
								FlowRelationType,
								NodeId,
								LinkedNodeId,
								Node->GetPathName(),
								TEXT("Derived Node-level flow projected from the canonical Pin link."),
								OutRelations,
								true,
								FlowAttributes);

							if (FlowRelationType == TEXT("exec_flows_to"))
							{
								FExecProjectionEdge Edge;
								Edge.FromNodeId = NodeId;
								Edge.ToNodeId = LinkedNodeId;
								Edge.EvidencePath = Node->GetPathName();
								Edge.Attributes = FlowAttributes;
								ExecProjectionEdges.Add(MoveTemp(Edge));
							}
							else if (FlowRelationType == TEXT("data_flows_to"))
							{
								FDataProjectionEdge Edge;
								Edge.FromNodeId = NodeId;
								Edge.ToNodeId = LinkedNodeId;
								Edge.FromPinId = PinId;
								Edge.ToPinId = LinkedPinId;
								Edge.PinCategory = Pin->PinType.PinCategory.ToString();
								Edge.EvidencePath = Node->GetPathName();
								Edge.Attributes = FlowAttributes;
								DataProjectionEdges.Add(MoveTemp(Edge));
							}
						}
					}
				}
				PinObject->SetArrayField(TEXT("linked_to"), LinkedPinValues);
				PinValues.Add(MakeShared<FJsonValueObject>(PinObject));
			}
			NodeObject->SetArrayField(TEXT("pins"), PinValues);
			NodeValues.Add(MakeShared<FJsonValueObject>(NodeObject));

			FNodeProjectionRecord ProjectionRecord;
			ProjectionRecord.NodeId = NodeId;
			ProjectionRecord.CanonicalKey = NodeCanonical;
			ProjectionRecord.DisplayName = NodeDisplayName;
			ProjectionRecord.Node = Node;
			ProjectionRecord.bHasExecPin = bNodeHasExecPin;
			if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
			{
				ProjectionRecord.VariableName = VariableNode->GetVarNameString();
				ProjectionRecord.VariableEntityId = MakeStableId(ProjectId, TEXT("blueprint_variable"), VariableKey(Blueprint, ProjectionRecord.VariableName));
				if (Node->IsA<UK2Node_VariableSet>())
				{
					ProjectionRecord.SemanticKind = TEXT("write_variable");
				}
				else if (Node->IsA<UK2Node_VariableGet>())
				{
					ProjectionRecord.SemanticKind = TEXT("read_variable");
				}
			}
			NodeProjectionRecords.Add(MoveTemp(ProjectionRecord));
		}

		AppendCfgBasicBlocks(ProjectId, GraphId, *Graph, NodeProjectionRecords, ExecProjectionEdges, OutEntities, OutRelations, GraphObject);
		AppendDfgDefUse(ProjectId, Blueprint, *Graph, NodeProjectionRecords, DataProjectionEdges, OutEntities, OutRelations, GraphObject);
		GraphObject->SetArrayField(TEXT("nodes"), NodeValues);
		GraphValues.Add(MakeShared<FJsonValueObject>(GraphObject));
	}

	BlueprintSnapshot->SetNumberField(TEXT("graph_count"), GraphValues.Num());
	BlueprintSnapshot->SetArrayField(TEXT("graphs"), GraphValues);
	BlueprintSnapshot->SetNumberField(TEXT("component_template_count"), ComponentTemplateValues.Num());
	BlueprintSnapshot->SetArrayField(TEXT("component_templates"), ComponentTemplateValues);

	if (!AssetEntity.Snapshot.IsValid())
	{
		AssetEntity.Snapshot = MakeShared<FJsonObject>();
	}
	AssetEntity.Snapshot->SetObjectField(TEXT("blueprint_graphs"), BlueprintSnapshot);
	if (UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(&Blueprint))
	{
		AppendAnimBlueprintStaticSummary(*AnimBlueprint, ProjectId, AssetEntity, AnimBlueprintNodeRecords, OutEntities, OutRelations);
	}
	if (UControlRigBlueprint* ControlRigBlueprint = Cast<UControlRigBlueprint>(&Blueprint))
	{
		AppendControlRigBlueprintStaticSummary(*ControlRigBlueprint, ProjectId, AssetEntity, OutEntities, OutRelations);
	}
	AssetEntity.Attributes.Add(TEXT("blueprint_graph_count"), FString::FromInt(GraphValues.Num()));
	AssetEntity.Completeness.State = ECompletenessState::Partial;
	AssetEntity.Completeness.Covered.AddUnique(TEXT("blueprint_graphs"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("blueprint_nodes"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("blueprint_pins"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("blueprint_scs_components"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("node_level_flow_projection"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("cfg_basic_blocks"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("dfg_def_use"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("blueprint_semantics_first_pass"));
	AssetEntity.Completeness.Omitted.Remove(TEXT("blueprint_semantics"));
	AssetEntity.Completeness.Omitted.Remove(TEXT("cfg_basic_blocks"));
	AssetEntity.Completeness.Omitted.Remove(TEXT("dfg_def_use"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("cfg_loop_nesting"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("ssa_path_sensitive_def_use"));
	AssetEntity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		Blueprint.GetPathName(),
		TEXT("Blueprint Graph/Node/Pin structure extracted from editor source graph.")
	});
}
}
