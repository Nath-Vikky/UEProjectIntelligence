#include "UEPIStateTreeReader.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreeExecutionTypes.h"
#include "StateTreeNodeBase.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString STBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString STGuidString(const FGuid& Guid)
{
	return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString();
}

FString STStructPath(const UStruct* Struct)
{
	return Struct ? Struct->GetPathName() : FString();
}

FString STClassPath(const UObject* Object)
{
	return Object && Object->GetClass() ? Object->GetClass()->GetPathName() : FString();
}

FString STEnumValueString(const UEnum* Enum, int64 Value)
{
	if (!Enum)
	{
		return FString::FromInt(static_cast<int32>(Value));
	}
	return Enum->GetNameStringByValue(Value);
}

template <typename EnumType>
FString STEnumString(EnumType Value)
{
	return STEnumValueString(StaticEnum<EnumType>(), static_cast<int64>(Value));
}

void AddSTEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

void AddSTRelation(
	const FString& ProjectId,
	const FString& Type,
	const FString& FromId,
	const FString& ToId,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FRelationRecord>& OutRelations,
	const TMap<FString, FString>* Attributes = nullptr)
{
	FRelationRecord Relation;
	Relation.Id = MakeRelationId(ProjectId, Type, FromId, ToId, Attributes);
	Relation.Type = Type;
	Relation.FromId = FromId;
	Relation.ToId = ToId;
	Relation.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Relation.bDerived = false;
	Relation.Confidence = 1.0f;
	if (Attributes)
	{
		Relation.Attributes = *Attributes;
	}
	Relation.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		EvidencePath,
		EvidenceDetail
	});
	OutRelations.Add(MoveTemp(Relation));
}

FEntityRecord* FindSTEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

int32 CountRuntimeTransitions(const UStateTree& Tree)
{
	int32 Count = 0;
	for (int32 TransitionIndex = 0; FStateTreeIndex16::IsValidIndex(TransitionIndex); ++TransitionIndex)
	{
		if (!Tree.GetTransitionFromIndex(FStateTreeIndex16(TransitionIndex)))
		{
			break;
		}
		++Count;
	}
	return Count;
}

struct FStateVisit
{
	const UStateTreeState* State = nullptr;
	const UStateTreeState* Parent = nullptr;
	int32 Depth = 0;
	int32 Index = 0;
};

void CollectStates(const UStateTreeState* State, const UStateTreeState* Parent, int32 Depth, TArray<FStateVisit>& OutStates)
{
	if (!State)
	{
		return;
	}

	FStateVisit Visit;
	Visit.State = State;
	Visit.Parent = Parent;
	Visit.Depth = Depth;
	Visit.Index = OutStates.Num();
	OutStates.Add(Visit);

	for (const TObjectPtr<UStateTreeState>& Child : State->Children)
	{
		CollectStates(Child.Get(), State, Depth + 1, OutStates);
	}
}

TArray<FStateVisit> CollectEditorStates(const UStateTreeEditorData& EditorData)
{
	TArray<FStateVisit> States;
	for (const TObjectPtr<UStateTreeState>& RootState : EditorData.SubTrees)
	{
		CollectStates(RootState.Get(), nullptr, 0, States);
	}
	return States;
}

FString AddStateTreeSchemaEntity(
	const FString& ProjectId,
	const UStateTreeSchema* Schema,
	const FString& EvidencePath,
	TArray<FEntityRecord>& OutEntities)
{
	const FString SchemaClassPath = STClassPath(Schema);
	if (SchemaClassPath.IsEmpty())
	{
		return FString();
	}

	const FString EntityId = MakeStableId(ProjectId, TEXT("state_tree_schema"), SchemaClassPath);
	if (FindSTEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("state_tree_schema");
	Entity.CanonicalKey = SchemaClassPath;
	Entity.DisplayName = Schema && Schema->GetClass() ? Schema->GetClass()->GetName() : FPackageName::ObjectPathToObjectName(SchemaClassPath);
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("schema_class_path"), SchemaClassPath);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("schema_class_reference") };
	Entity.Completeness.Omitted = { TEXT("schema_runtime_validation_rules") };
	AddSTEvidence(Entity, EvidencePath, TEXT("StateTree schema reference read from UStateTree."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddStateTreeEntity(
	const FString& ProjectId,
	const UStateTree& Tree,
	const UStateTreeEditorData* EditorData,
	TArray<FEntityRecord>& OutEntities)
{
	const FString TreePath = Tree.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("state_tree"), TreePath);
	if (FindSTEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	const int32 EditorStateCount = EditorData ? CollectEditorStates(*EditorData).Num() : 0;
	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("state_tree");
	Entity.CanonicalKey = TreePath;
	Entity.DisplayName = Tree.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("tree_path"), TreePath);
	Entity.Attributes.Add(TEXT("schema_class_path"), STClassPath(Tree.GetSchema()));
	Entity.Attributes.Add(TEXT("ready_to_run"), STBool(Tree.IsReadyToRun()));
	Entity.Attributes.Add(TEXT("num_data_views"), FString::FromInt(Tree.GetNumDataViews()));
	Entity.Attributes.Add(TEXT("runtime_state_count"), FString::FromInt(Tree.GetStates().Num()));
	Entity.Attributes.Add(TEXT("runtime_transition_count"), FString::FromInt(CountRuntimeTransitions(Tree)));
	Entity.Attributes.Add(TEXT("runtime_node_count"), FString::FromInt(Tree.GetNodes().Num()));
	Entity.Attributes.Add(TEXT("editor_state_count"), FString::FromInt(EditorStateCount));
	Entity.Attributes.Add(TEXT("external_data_count"), FString::FromInt(Tree.GetExternalDataDescs().Num()));
	Entity.Attributes.Add(TEXT("context_data_count"), FString::FromInt(Tree.GetContextDataDescs().Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = EditorData ? ECompletenessState::Partial : ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("compiled_state_tree_summary"), TEXT("external_data_descriptors") };
	if (EditorData)
	{
		Entity.Completeness.Covered.Add(TEXT("editor_state_hierarchy"));
		Entity.Completeness.Covered.Add(TEXT("editor_nodes"));
		Entity.Completeness.Covered.Add(TEXT("editor_transitions"));
	}
	Entity.Completeness.Omitted = { TEXT("runtime_active_states"), TEXT("runtime_instance_data_values") };
	AddSTEvidence(Entity, TreePath, TEXT("StateTree asset metadata read from UStateTree."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddStateTreeStateEntity(
	const FString& ProjectId,
	const UStateTree& Tree,
	const FStateVisit& Visit,
	TArray<FEntityRecord>& OutEntities)
{
	const UStateTreeState& State = *Visit.State;
	const FString StateGuid = STGuidString(State.ID);
	const FString CanonicalKey = Tree.GetPathName() + TEXT(":state:") + (StateGuid.IsEmpty() ? State.GetPathName() : StateGuid);
	const FString EntityId = MakeStableId(ProjectId, TEXT("state_tree_state"), CanonicalKey);
	if (FindSTEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("state_tree_state");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = State.Name.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("tree_path"), Tree.GetPathName());
	Entity.Attributes.Add(TEXT("state_path"), State.GetPathName());
	Entity.Attributes.Add(TEXT("state_guid"), StateGuid);
	Entity.Attributes.Add(TEXT("state_index"), FString::FromInt(Visit.Index));
	Entity.Attributes.Add(TEXT("state_depth"), FString::FromInt(Visit.Depth));
	Entity.Attributes.Add(TEXT("state_type"), STEnumString(State.Type));
	Entity.Attributes.Add(TEXT("selection_behavior"), STEnumString(State.SelectionBehavior));
	Entity.Attributes.Add(TEXT("enabled"), STBool(State.bEnabled));
	Entity.Attributes.Add(TEXT("child_count"), FString::FromInt(State.Children.Num()));
	Entity.Attributes.Add(TEXT("task_count"), FString::FromInt(State.Tasks.Num() + (State.SingleTask.Node.IsValid() ? 1 : 0)));
	Entity.Attributes.Add(TEXT("enter_condition_count"), FString::FromInt(State.EnterConditions.Num()));
	Entity.Attributes.Add(TEXT("transition_count"), FString::FromInt(State.Transitions.Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("state_metadata"), TEXT("state_nodes"), TEXT("state_transitions") };
	Entity.Completeness.Omitted = { TEXT("runtime_selection_status") };
	AddSTEvidence(Entity, Tree.GetPathName(), TEXT("StateTree editor state read from UStateTreeEditorData."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddStateTreeNodeEntity(
	const FString& ProjectId,
	const UStateTree& Tree,
	const FStateTreeEditorNode& Node,
	const FString& NodeKind,
	const FString& OwnerId,
	const FString& OwnerPath,
	int32 NodeIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString NodeGuid = STGuidString(Node.ID);
	const FString CanonicalKey = Tree.GetPathName() + TEXT(":node:") + (NodeGuid.IsEmpty() ? OwnerPath + TEXT(":") + NodeKind + TEXT(":") + FString::FromInt(NodeIndex) : NodeGuid);
	const FString EntityId = MakeStableId(ProjectId, TEXT("state_tree_node"), CanonicalKey);
	if (FindSTEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	const UScriptStruct* NodeStruct = Node.Node.GetScriptStruct();
	const UScriptStruct* InstanceStruct = Node.Instance.GetScriptStruct();

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("state_tree_node");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Node.GetName().ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("tree_path"), Tree.GetPathName());
	Entity.Attributes.Add(TEXT("owner_id"), OwnerId);
	Entity.Attributes.Add(TEXT("node_guid"), NodeGuid);
	Entity.Attributes.Add(TEXT("node_kind"), NodeKind);
	Entity.Attributes.Add(TEXT("node_index"), FString::FromInt(NodeIndex));
	Entity.Attributes.Add(TEXT("node_name"), Node.GetName().ToString());
	Entity.Attributes.Add(TEXT("node_struct_path"), STStructPath(NodeStruct));
	Entity.Attributes.Add(TEXT("instance_struct_path"), STStructPath(InstanceStruct));
	Entity.Attributes.Add(TEXT("instance_object_class_path"), STClassPath(Node.InstanceObject));
	Entity.Attributes.Add(TEXT("condition_indent"), FString::FromInt(Node.ConditionIndent));
	Entity.Attributes.Add(TEXT("condition_operand"), STEnumString(Node.ConditionOperand));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("node_struct"), TEXT("instance_type"), TEXT("condition_metadata") };
	Entity.Completeness.Omitted = { TEXT("runtime_node_instance_values") };
	AddSTEvidence(Entity, Tree.GetPathName(), TEXT("StateTree editor node read from UStateTreeEditorData."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddStateTreeTransitionEntity(
	const FString& ProjectId,
	const UStateTree& Tree,
	const FStateTreeTransition& Transition,
	const FString& StateId,
	const FString& StatePath,
	int32 TransitionIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString TransitionGuid = STGuidString(Transition.ID);
	const FString CanonicalKey = Tree.GetPathName() + TEXT(":transition:") + (TransitionGuid.IsEmpty() ? StatePath + TEXT(":") + FString::FromInt(TransitionIndex) : TransitionGuid);
	const FString EntityId = MakeStableId(ProjectId, TEXT("state_tree_transition"), CanonicalKey);
	if (FindSTEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("state_tree_transition");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Transition.State.Name.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("tree_path"), Tree.GetPathName());
	Entity.Attributes.Add(TEXT("state_id"), StateId);
	Entity.Attributes.Add(TEXT("transition_guid"), TransitionGuid);
	Entity.Attributes.Add(TEXT("transition_index"), FString::FromInt(TransitionIndex));
	Entity.Attributes.Add(TEXT("trigger"), STEnumString(Transition.Trigger));
	Entity.Attributes.Add(TEXT("event_tag"), Transition.EventTag.ToString());
	Entity.Attributes.Add(TEXT("target_state_name"), Transition.State.Name.ToString());
	Entity.Attributes.Add(TEXT("target_state_guid"), STGuidString(Transition.State.ID));
	Entity.Attributes.Add(TEXT("target_link_type"), STEnumString(Transition.State.LinkType));
	Entity.Attributes.Add(TEXT("priority"), STEnumString(Transition.Priority));
	Entity.Attributes.Add(TEXT("delay_transition"), STBool(Transition.bDelayTransition));
	Entity.Attributes.Add(TEXT("delay_duration"), FString::SanitizeFloat(Transition.DelayDuration));
	Entity.Attributes.Add(TEXT("delay_random_variance"), FString::SanitizeFloat(Transition.DelayRandomVariance));
	Entity.Attributes.Add(TEXT("condition_count"), FString::FromInt(Transition.Conditions.Num()));
	Entity.Attributes.Add(TEXT("enabled"), STBool(Transition.bTransitionEnabled));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("transition_target"), TEXT("trigger"), TEXT("priority"), TEXT("conditions") };
	Entity.Completeness.Omitted = { TEXT("runtime_transition_result") };
	AddSTEvidence(Entity, Tree.GetPathName(), TEXT("StateTree editor transition read from UStateTreeEditorData."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddStateTreeExternalDataEntity(
	const FString& ProjectId,
	const UStateTree& Tree,
	const FStateTreeExternalDataDesc& Desc,
	const FString& Scope,
	int32 Index,
	TArray<FEntityRecord>& OutEntities)
{
	const FString StructPath = STStructPath(Desc.Struct);
	const FString Guid = STGuidString(Desc.ID);
	const FString CanonicalKey = Tree.GetPathName() + TEXT(":external_data:") + Scope + TEXT(":") + (Guid.IsEmpty() ? StructPath + TEXT(":") + FString::FromInt(Index) : Guid);
	const FString EntityId = MakeStableId(ProjectId, TEXT("state_tree_external_data"), CanonicalKey);
	if (FindSTEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("state_tree_external_data");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Desc.Name.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("tree_path"), Tree.GetPathName());
	Entity.Attributes.Add(TEXT("scope"), Scope);
	Entity.Attributes.Add(TEXT("index"), FString::FromInt(Index));
	Entity.Attributes.Add(TEXT("name"), Desc.Name.ToString());
	Entity.Attributes.Add(TEXT("struct_path"), StructPath);
	Entity.Attributes.Add(TEXT("requirement"), STEnumString(Desc.Requirement));
	Entity.Attributes.Add(TEXT("data_view_index"), FString::FromInt(Desc.Handle.DataViewIndex.AsInt32()));
	Entity.Attributes.Add(TEXT("guid"), Guid);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Complete;
	Entity.Completeness.Covered = { TEXT("external_data_descriptor") };
	AddSTEvidence(Entity, Tree.GetPathName(), TEXT("StateTree external data descriptor read from UStateTree."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

TSharedRef<FJsonObject> NodeSnapshot(
	const FString& NodeId,
	const FStateTreeEditorNode& Node,
	const FString& NodeKind,
	const FString& OwnerId,
	int32 NodeIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), NodeId);
	Object->SetNumberField(TEXT("index"), NodeIndex);
	Object->SetStringField(TEXT("guid"), STGuidString(Node.ID));
	Object->SetStringField(TEXT("owner_id"), OwnerId);
	Object->SetStringField(TEXT("node_kind"), NodeKind);
	Object->SetStringField(TEXT("name"), Node.GetName().ToString());
	Object->SetStringField(TEXT("node_struct_path"), STStructPath(Node.Node.GetScriptStruct()));
	Object->SetStringField(TEXT("instance_struct_path"), STStructPath(Node.Instance.GetScriptStruct()));
	Object->SetStringField(TEXT("instance_object_class_path"), STClassPath(Node.InstanceObject));
	Object->SetNumberField(TEXT("condition_indent"), Node.ConditionIndent);
	Object->SetStringField(TEXT("condition_operand"), STEnumString(Node.ConditionOperand));
	return Object;
}

TSharedRef<FJsonObject> StateSnapshot(const FString& StateId, const FStateVisit& Visit, const FString& ParentId)
{
	const UStateTreeState& State = *Visit.State;
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), StateId);
	Object->SetNumberField(TEXT("index"), Visit.Index);
	Object->SetNumberField(TEXT("depth"), Visit.Depth);
	Object->SetStringField(TEXT("state_path"), State.GetPathName());
	Object->SetStringField(TEXT("name"), State.Name.ToString());
	Object->SetStringField(TEXT("guid"), STGuidString(State.ID));
	Object->SetStringField(TEXT("parent_id"), ParentId);
	Object->SetStringField(TEXT("parent_guid"), Visit.Parent ? STGuidString(Visit.Parent->ID) : FString());
	Object->SetStringField(TEXT("type"), STEnumString(State.Type));
	Object->SetStringField(TEXT("selection_behavior"), STEnumString(State.SelectionBehavior));
	Object->SetStringField(TEXT("linked_subtree_name"), State.LinkedSubtree.Name.ToString());
	Object->SetStringField(TEXT("linked_subtree_guid"), STGuidString(State.LinkedSubtree.ID));
	Object->SetStringField(TEXT("linked_subtree_type"), STEnumString(State.LinkedSubtree.LinkType));
	Object->SetBoolField(TEXT("enabled"), State.bEnabled);
	Object->SetNumberField(TEXT("child_count"), State.Children.Num());
	Object->SetNumberField(TEXT("enter_condition_count"), State.EnterConditions.Num());
	Object->SetNumberField(TEXT("task_count"), State.Tasks.Num() + (State.SingleTask.Node.IsValid() ? 1 : 0));
	Object->SetNumberField(TEXT("transition_count"), State.Transitions.Num());
	return Object;
}

TSharedRef<FJsonObject> TransitionSnapshot(const FString& TransitionId, const FStateTreeTransition& Transition, const FString& StateId, const FString& TargetStateId, int32 TransitionIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), TransitionId);
	Object->SetNumberField(TEXT("index"), TransitionIndex);
	Object->SetStringField(TEXT("guid"), STGuidString(Transition.ID));
	Object->SetStringField(TEXT("state_id"), StateId);
	Object->SetStringField(TEXT("trigger"), STEnumString(Transition.Trigger));
	Object->SetStringField(TEXT("event_tag"), Transition.EventTag.ToString());
	Object->SetStringField(TEXT("target_state_name"), Transition.State.Name.ToString());
	Object->SetStringField(TEXT("target_state_guid"), STGuidString(Transition.State.ID));
	Object->SetStringField(TEXT("target_state_id"), TargetStateId);
	Object->SetStringField(TEXT("target_link_type"), STEnumString(Transition.State.LinkType));
	Object->SetStringField(TEXT("priority"), STEnumString(Transition.Priority));
	Object->SetBoolField(TEXT("delay_transition"), Transition.bDelayTransition);
	Object->SetNumberField(TEXT("delay_duration"), Transition.DelayDuration);
	Object->SetNumberField(TEXT("delay_random_variance"), Transition.DelayRandomVariance);
	Object->SetNumberField(TEXT("condition_count"), Transition.Conditions.Num());
	Object->SetBoolField(TEXT("enabled"), Transition.bTransitionEnabled);
	return Object;
}

TSharedRef<FJsonObject> ExternalDataSnapshot(const FStateTreeExternalDataDesc& Desc, const FString& EntityId, const FString& Scope, int32 Index)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), EntityId);
	Object->SetStringField(TEXT("scope"), Scope);
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("name"), Desc.Name.ToString());
	Object->SetStringField(TEXT("struct_path"), STStructPath(Desc.Struct));
	Object->SetStringField(TEXT("requirement"), STEnumString(Desc.Requirement));
	Object->SetNumberField(TEXT("data_view_index"), Desc.Handle.DataViewIndex.AsInt32());
	Object->SetStringField(TEXT("guid"), STGuidString(Desc.ID));
	return Object;
}

TSharedRef<FJsonObject> BindingSnapshot(const FStateTreePropertyPathBinding& Binding, int32 Index)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("source_path"), Binding.GetSourcePath().ToString());
	Object->SetStringField(TEXT("target_path"), Binding.GetTargetPath().ToString());
	return Object;
}

void AppendNodeWithRelation(
	const FString& ProjectId,
	const UStateTree& Tree,
	const FStateTreeEditorNode& Node,
	const FString& NodeKind,
	const FString& OwnerId,
	const FString& OwnerPath,
	const FString& RelationType,
	int32 NodeIndex,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations,
	TArray<TSharedPtr<FJsonValue>>& NodeValues)
{
	if (!Node.Node.IsValid() && !Node.Instance.IsValid() && !Node.InstanceObject)
	{
		return;
	}

	const FString NodeId = AddStateTreeNodeEntity(ProjectId, Tree, Node, NodeKind, OwnerId, OwnerPath, NodeIndex, OutEntities);
	AddSTRelation(ProjectId, TEXT("contains_state_tree_node"), MakeStableId(ProjectId, TEXT("state_tree"), Tree.GetPathName()), NodeId, Tree.GetPathName(), TEXT("StateTree contains an editor node."), OutRelations);
	AddSTRelation(ProjectId, RelationType, OwnerId, NodeId, Tree.GetPathName(), TEXT("StateTree node is owned by this editor scope."), OutRelations);
	NodeValues.Add(MakeShared<FJsonValueObject>(NodeSnapshot(NodeId, Node, NodeKind, OwnerId, NodeIndex)));
}

TSharedRef<FJsonObject> StateTreeSnapshot(
	const UStateTree& Tree,
	const UStateTreeEditorData* EditorData,
	const FString& TreeId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations,
	const FString& ProjectId)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.state_tree.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), TreeId);
	Object->SetStringField(TEXT("tree_path"), Tree.GetPathName());
	Object->SetStringField(TEXT("schema_class_path"), STClassPath(Tree.GetSchema()));
	Object->SetBoolField(TEXT("ready_to_run"), Tree.IsReadyToRun());
	Object->SetNumberField(TEXT("num_data_views"), Tree.GetNumDataViews());
	Object->SetNumberField(TEXT("runtime_state_count"), Tree.GetStates().Num());
	Object->SetNumberField(TEXT("runtime_transition_count"), CountRuntimeTransitions(Tree));
	Object->SetNumberField(TEXT("runtime_node_count"), Tree.GetNodes().Num());
	Object->SetNumberField(TEXT("last_compiled_editor_data_hash"), static_cast<double>(Tree.LastCompiledEditorDataHash));

	TArray<TSharedPtr<FJsonValue>> StateValues;
	TArray<TSharedPtr<FJsonValue>> NodeValues;
	TArray<TSharedPtr<FJsonValue>> TransitionValues;
	TMap<FGuid, FString> StateIdByGuid;

	if (EditorData)
	{
		const TArray<FStateVisit> States = CollectEditorStates(*EditorData);
		for (const FStateVisit& Visit : States)
		{
			const FString StateId = AddStateTreeStateEntity(ProjectId, Tree, Visit, OutEntities);
			if (Visit.State->ID.IsValid())
			{
				StateIdByGuid.Add(Visit.State->ID, StateId);
			}
		}

		for (const FStateVisit& Visit : States)
		{
			const FString StateId = StateIdByGuid.FindRef(Visit.State->ID);
			const FString ParentId = Visit.Parent ? StateIdByGuid.FindRef(Visit.Parent->ID) : FString();
			AddSTRelation(ProjectId, TEXT("contains_state_tree_state"), TreeId, StateId, Tree.GetPathName(), TEXT("StateTree contains this editor state."), OutRelations);
			if (!ParentId.IsEmpty())
			{
				AddSTRelation(ProjectId, TEXT("state_tree_state_child"), ParentId, StateId, Tree.GetPathName(), TEXT("StateTree editor state is a child of another state."), OutRelations);
			}
			StateValues.Add(MakeShared<FJsonValueObject>(StateSnapshot(StateId, Visit, ParentId)));

			for (int32 ConditionIndex = 0; ConditionIndex < Visit.State->EnterConditions.Num(); ++ConditionIndex)
			{
				AppendNodeWithRelation(ProjectId, Tree, Visit.State->EnterConditions[ConditionIndex], TEXT("enter_condition"), StateId, Visit.State->GetPathName(), TEXT("state_tree_state_enter_condition"), ConditionIndex, OutEntities, OutRelations, NodeValues);
			}
			for (int32 TaskIndex = 0; TaskIndex < Visit.State->Tasks.Num(); ++TaskIndex)
			{
				AppendNodeWithRelation(ProjectId, Tree, Visit.State->Tasks[TaskIndex], TEXT("task"), StateId, Visit.State->GetPathName(), TEXT("state_tree_state_task"), TaskIndex, OutEntities, OutRelations, NodeValues);
			}
			AppendNodeWithRelation(ProjectId, Tree, Visit.State->SingleTask, TEXT("single_task"), StateId, Visit.State->GetPathName(), TEXT("state_tree_state_task"), Visit.State->Tasks.Num(), OutEntities, OutRelations, NodeValues);

			for (int32 TransitionIndex = 0; TransitionIndex < Visit.State->Transitions.Num(); ++TransitionIndex)
			{
				const FStateTreeTransition& Transition = Visit.State->Transitions[TransitionIndex];
				const FString TransitionId = AddStateTreeTransitionEntity(ProjectId, Tree, Transition, StateId, Visit.State->GetPathName(), TransitionIndex, OutEntities);
				AddSTRelation(ProjectId, TEXT("contains_state_tree_transition"), StateId, TransitionId, Tree.GetPathName(), TEXT("StateTree state contains this transition."), OutRelations);
				const FString TargetStateId = StateIdByGuid.FindRef(Transition.State.ID);
				if (!TargetStateId.IsEmpty())
				{
					AddSTRelation(ProjectId, TEXT("state_tree_transition_target"), TransitionId, TargetStateId, Tree.GetPathName(), TEXT("StateTree transition points to this target state."), OutRelations);
				}
				TransitionValues.Add(MakeShared<FJsonValueObject>(TransitionSnapshot(TransitionId, Transition, StateId, TargetStateId, TransitionIndex)));
				for (int32 ConditionIndex = 0; ConditionIndex < Transition.Conditions.Num(); ++ConditionIndex)
				{
					AppendNodeWithRelation(ProjectId, Tree, Transition.Conditions[ConditionIndex], TEXT("transition_condition"), TransitionId, Visit.State->GetPathName(), TEXT("state_tree_transition_condition"), ConditionIndex, OutEntities, OutRelations, NodeValues);
				}
			}
		}

		for (int32 EvaluatorIndex = 0; EvaluatorIndex < EditorData->Evaluators.Num(); ++EvaluatorIndex)
		{
			AppendNodeWithRelation(ProjectId, Tree, EditorData->Evaluators[EvaluatorIndex], TEXT("global_evaluator"), TreeId, Tree.GetPathName(), TEXT("state_tree_global_evaluator"), EvaluatorIndex, OutEntities, OutRelations, NodeValues);
		}
		for (int32 TaskIndex = 0; TaskIndex < EditorData->GlobalTasks.Num(); ++TaskIndex)
		{
			AppendNodeWithRelation(ProjectId, Tree, EditorData->GlobalTasks[TaskIndex], TEXT("global_task"), TreeId, Tree.GetPathName(), TEXT("state_tree_global_task"), TaskIndex, OutEntities, OutRelations, NodeValues);
		}

		TArray<TSharedPtr<FJsonValue>> BindingValues;
		const TConstArrayView<FStateTreePropertyPathBinding> Bindings = EditorData->EditorBindings.GetBindings();
		for (int32 BindingIndex = 0; BindingIndex < Bindings.Num(); ++BindingIndex)
		{
			BindingValues.Add(MakeShared<FJsonValueObject>(BindingSnapshot(Bindings[BindingIndex], BindingIndex)));
		}
		Object->SetNumberField(TEXT("property_binding_count"), BindingValues.Num());
		Object->SetArrayField(TEXT("property_bindings"), BindingValues);
		Object->SetNumberField(TEXT("global_evaluator_count"), EditorData->Evaluators.Num());
		Object->SetNumberField(TEXT("global_task_count"), EditorData->GlobalTasks.Num());
	}
	else
	{
		Object->SetNumberField(TEXT("property_binding_count"), 0);
		Object->SetArrayField(TEXT("property_bindings"), TArray<TSharedPtr<FJsonValue>>());
		Object->SetNumberField(TEXT("global_evaluator_count"), 0);
		Object->SetNumberField(TEXT("global_task_count"), 0);
	}

	Object->SetNumberField(TEXT("editor_state_count"), StateValues.Num());
	Object->SetArrayField(TEXT("states"), StateValues);
	Object->SetNumberField(TEXT("editor_node_count"), NodeValues.Num());
	Object->SetArrayField(TEXT("nodes"), NodeValues);
	Object->SetNumberField(TEXT("editor_transition_count"), TransitionValues.Num());
	Object->SetArrayField(TEXT("transitions"), TransitionValues);

	TArray<TSharedPtr<FJsonValue>> ExternalDataValues;
	const TConstArrayView<FStateTreeExternalDataDesc> ExternalData = Tree.GetExternalDataDescs();
	for (int32 ExternalIndex = 0; ExternalIndex < ExternalData.Num(); ++ExternalIndex)
	{
		const FString ExternalDataId = AddStateTreeExternalDataEntity(ProjectId, Tree, ExternalData[ExternalIndex], TEXT("external"), ExternalIndex, OutEntities);
		AddSTRelation(ProjectId, TEXT("uses_state_tree_external_data"), TreeId, ExternalDataId, Tree.GetPathName(), TEXT("StateTree references this external data descriptor."), OutRelations);
		ExternalDataValues.Add(MakeShared<FJsonValueObject>(ExternalDataSnapshot(ExternalData[ExternalIndex], ExternalDataId, TEXT("external"), ExternalIndex)));
	}
	Object->SetNumberField(TEXT("external_data_count"), ExternalDataValues.Num());
	Object->SetArrayField(TEXT("external_data"), ExternalDataValues);

	TArray<TSharedPtr<FJsonValue>> ContextDataValues;
	const TConstArrayView<FStateTreeExternalDataDesc> ContextData = Tree.GetContextDataDescs();
	for (int32 ContextIndex = 0; ContextIndex < ContextData.Num(); ++ContextIndex)
	{
		const FString ContextDataId = AddStateTreeExternalDataEntity(ProjectId, Tree, ContextData[ContextIndex], TEXT("context"), ContextIndex, OutEntities);
		AddSTRelation(ProjectId, TEXT("uses_state_tree_external_data"), TreeId, ContextDataId, Tree.GetPathName(), TEXT("StateTree schema requires this context data descriptor."), OutRelations);
		ContextDataValues.Add(MakeShared<FJsonValueObject>(ExternalDataSnapshot(ContextData[ContextIndex], ContextDataId, TEXT("context"), ContextIndex)));
	}
	Object->SetNumberField(TEXT("context_data_count"), ContextDataValues.Num());
	Object->SetArrayField(TEXT("context_data"), ContextDataValues);
	return Object;
}
}

bool FStateTreeReader::AppendStateTreeAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	UStateTree* Tree = Cast<UStateTree>(&Asset);
	if (!Tree)
	{
		return false;
	}

	if (!AssetEntity.Snapshot.IsValid())
	{
		AssetEntity.Snapshot = MakeShared<FJsonObject>();
	}

	const UStateTreeEditorData* EditorData = nullptr;
#if WITH_EDITORONLY_DATA
	EditorData = Cast<UStateTreeEditorData>(Tree->EditorData);
#endif

	const FString TreeId = AddStateTreeEntity(ProjectId, *Tree, EditorData, OutEntities);
	AddSTRelation(ProjectId, TEXT("contains_state_tree"), AssetEntity.Id, TreeId, Tree->GetPathName(), TEXT("Asset contains the extracted StateTree record."), OutRelations);
	if (const FString SchemaId = AddStateTreeSchemaEntity(ProjectId, Tree->GetSchema(), Tree->GetPathName(), OutEntities); !SchemaId.IsEmpty())
	{
		AddSTRelation(ProjectId, TEXT("uses_state_tree_schema"), TreeId, SchemaId, Tree->GetPathName(), TEXT("StateTree uses this schema class."), OutRelations);
	}

	AssetEntity.Snapshot->SetObjectField(TEXT("state_tree"), StateTreeSnapshot(*Tree, EditorData, TreeId, OutEntities, OutRelations, ProjectId));
	AssetEntity.Attributes.Add(TEXT("state_tree_schema_class"), STClassPath(Tree->GetSchema()));
	AssetEntity.Attributes.Add(TEXT("state_tree_ready_to_run"), STBool(Tree->IsReadyToRun()));
	AssetEntity.Attributes.Add(TEXT("state_tree_runtime_state_count"), FString::FromInt(Tree->GetStates().Num()));
	AssetEntity.Attributes.Add(TEXT("state_tree_runtime_node_count"), FString::FromInt(Tree->GetNodes().Num()));
	AssetEntity.Completeness.State = EditorData ? ECompletenessState::Partial : ECompletenessState::MetadataOnly;
	AssetEntity.Completeness.Covered.AddUnique(TEXT("state_tree_summary"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("state_tree_external_data"));
	if (EditorData)
	{
		AssetEntity.Completeness.Covered.AddUnique(TEXT("state_tree_editor_hierarchy"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("state_tree_editor_nodes"));
	}
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_active_states"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_instance_data_values"));
	AddSTEvidence(AssetEntity, Tree->GetPathName(), TEXT("StateTree structure extracted from loaded UStateTree asset."));
	return true;
}
}
