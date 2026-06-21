#include "UEPIAIReader.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryTypes.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString AIBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString AIClassPath(const UObject* Object)
{
	return Object && Object->GetClass() ? Object->GetClass()->GetPathName() : FString();
}

FString AIObjectPath(const UObject* Object)
{
	return Object ? Object->GetPathName() : FString();
}

FString AIEnumValueString(const UEnum* Enum, int64 Value)
{
	if (!Enum)
	{
		return FString::FromInt(static_cast<int32>(Value));
	}
	return Enum->GetNameStringByValue(Value);
}

FString BTFlowAbortModeString(EBTFlowAbortMode::Type Mode)
{
	return AIEnumValueString(StaticEnum<EBTFlowAbortMode::Type>(), static_cast<int64>(Mode));
}

FString EQSTestPurposeString(EEnvTestPurpose::Type Purpose)
{
	return AIEnumValueString(StaticEnum<EEnvTestPurpose::Type>(), static_cast<int64>(Purpose));
}

FString EQSFilterTypeString(EEnvTestFilterType::Type FilterType)
{
	return AIEnumValueString(StaticEnum<EEnvTestFilterType::Type>(), static_cast<int64>(FilterType));
}

FString EQSScoreEquationString(EEnvTestScoreEquation::Type ScoreEquation)
{
	return AIEnumValueString(StaticEnum<EEnvTestScoreEquation::Type>(), static_cast<int64>(ScoreEquation));
}

FString EQSTestCostString(EEnvTestCost::Type Cost)
{
	return AIEnumValueString(StaticEnum<EEnvTestCost::Type>(), static_cast<int64>(Cost));
}

void AddAIEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

void AddAIRelation(
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

FEntityRecord* FindAIEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

FString AddBlackboardEntity(
	const FString& ProjectId,
	const UBlackboardData& Blackboard,
	TArray<FEntityRecord>& OutEntities)
{
	const FString BlackboardPath = Blackboard.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("blackboard"), BlackboardPath);
	if (FindAIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("blackboard");
	Entity.CanonicalKey = BlackboardPath;
	Entity.DisplayName = Blackboard.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("blackboard_path"), BlackboardPath);
	Entity.Attributes.Add(TEXT("parent_path"), AIObjectPath(Blackboard.Parent));
	Entity.Attributes.Add(TEXT("key_count"), FString::FromInt(Blackboard.Keys.Num()));
	Entity.Attributes.Add(TEXT("has_synchronized_keys"), AIBool(Blackboard.HasSynchronizedKeys()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("blackboard_keys"), TEXT("parent_reference") };
	Entity.Completeness.Omitted = { TEXT("runtime_blackboard_values") };
	AddAIEvidence(Entity, BlackboardPath, TEXT("Blackboard key metadata read from UBlackboardData."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddBlackboardKeyEntity(
	const FString& ProjectId,
	const UBlackboardData& Blackboard,
	const FBlackboardEntry& Key,
	int32 KeyIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = Blackboard.GetPathName() + TEXT(":key:") + Key.EntryName.ToString();
	const FString EntityId = MakeStableId(ProjectId, TEXT("blackboard_key"), CanonicalKey);
	if (FindAIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("blackboard_key");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Key.EntryName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("blackboard_path"), Blackboard.GetPathName());
	Entity.Attributes.Add(TEXT("key_name"), Key.EntryName.ToString());
	Entity.Attributes.Add(TEXT("key_index"), FString::FromInt(KeyIndex));
	Entity.Attributes.Add(TEXT("key_type_class"), AIClassPath(Key.KeyType));
	Entity.Attributes.Add(TEXT("instance_synced"), AIBool(Key.bInstanceSynced != 0));
#if WITH_EDITORONLY_DATA
	Entity.Attributes.Add(TEXT("description"), Key.EntryDescription);
	Entity.Attributes.Add(TEXT("category"), Key.EntryCategory.ToString());
#endif
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("blackboard_key_metadata"), TEXT("key_type") };
	Entity.Completeness.Omitted = { TEXT("runtime_blackboard_value") };
	AddAIEvidence(Entity, Blackboard.GetPathName(), TEXT("Blackboard key metadata read from UBlackboardData key entries."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddBehaviorTreeEntity(
	const FString& ProjectId,
	const UBehaviorTree& Tree,
	TArray<FEntityRecord>& OutEntities)
{
	const FString TreePath = Tree.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("behavior_tree"), TreePath);
	if (FindAIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("behavior_tree");
	Entity.CanonicalKey = TreePath;
	Entity.DisplayName = Tree.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("tree_path"), TreePath);
	Entity.Attributes.Add(TEXT("blackboard_path"), AIObjectPath(Tree.BlackboardAsset));
	Entity.Attributes.Add(TEXT("root_node_path"), AIObjectPath(Tree.RootNode));
	Entity.Attributes.Add(TEXT("root_decorator_count"), FString::FromInt(Tree.RootDecorators.Num()));
	Entity.Attributes.Add(TEXT("instance_memory_size"), FString::FromInt(Tree.InstanceMemorySize));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("behavior_tree_template_nodes"), TEXT("blackboard_reference") };
	Entity.Completeness.Omitted = { TEXT("runtime_behavior_tree_execution"), TEXT("node_memory_values") };
	AddAIEvidence(Entity, TreePath, TEXT("BehaviorTree template nodes read from UBehaviorTree."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddBehaviorTreeNodeEntity(
	const FString& ProjectId,
	const UBehaviorTree& Tree,
	const UBTNode& Node,
	const FString& NodeRole,
	int32 NodeIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString NodePath = Node.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("behavior_tree_node"), NodePath);
	if (FindAIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("behavior_tree_node");
	Entity.CanonicalKey = NodePath;
	Entity.DisplayName = Node.GetNodeName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("tree_path"), Tree.GetPathName());
	Entity.Attributes.Add(TEXT("node_path"), NodePath);
	Entity.Attributes.Add(TEXT("node_role"), NodeRole);
	Entity.Attributes.Add(TEXT("node_index"), FString::FromInt(NodeIndex));
	Entity.Attributes.Add(TEXT("node_class_path"), AIClassPath(&Node));
	Entity.Attributes.Add(TEXT("node_name"), Node.GetNodeName());
	Entity.Attributes.Add(TEXT("execution_index"), FString::FromInt(Node.GetExecutionIndex()));
	Entity.Attributes.Add(TEXT("tree_depth"), FString::FromInt(Node.GetTreeDepth()));
	if (const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(&Node))
	{
		Entity.Attributes.Add(TEXT("child_count"), FString::FromInt(Composite->Children.Num()));
		Entity.Attributes.Add(TEXT("service_count"), FString::FromInt(Composite->Services.Num()));
	}
	else if (const UBTTaskNode* Task = Cast<UBTTaskNode>(&Node))
	{
		Entity.Attributes.Add(TEXT("service_count"), FString::FromInt(Task->Services.Num()));
	}
	if (const UBTDecorator* Decorator = Cast<UBTDecorator>(&Node))
	{
		Entity.Attributes.Add(TEXT("flow_abort_mode"), BTFlowAbortModeString(Decorator->GetFlowAbortMode()));
		Entity.Attributes.Add(TEXT("inversed"), AIBool(Decorator->IsInversed()));
	}
	Entity.Attributes.Add(TEXT("static_description"), Node.GetStaticDescription());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("behavior_tree_node_metadata"), TEXT("template_links") };
	Entity.Completeness.Omitted = { TEXT("runtime_node_status"), TEXT("runtime_node_memory") };
	AddAIEvidence(Entity, Tree.GetPathName(), TEXT("BehaviorTree node metadata read from template node objects."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddEnvQueryEntity(
	const FString& ProjectId,
	const UEnvQuery& Query,
	TArray<FEntityRecord>& OutEntities)
{
	const FString QueryPath = Query.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("env_query"), QueryPath);
	if (FindAIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("env_query");
	Entity.CanonicalKey = QueryPath;
	Entity.DisplayName = Query.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("query_path"), QueryPath);
	Entity.Attributes.Add(TEXT("query_name"), Query.GetQueryName().ToString());
	Entity.Attributes.Add(TEXT("option_count"), FString::FromInt(Query.GetOptions().Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("env_query_options"), TEXT("generators"), TEXT("tests") };
	Entity.Completeness.Omitted = { TEXT("runtime_query_results"), TEXT("runtime_item_scores") };
	AddAIEvidence(Entity, QueryPath, TEXT("EQS option/generator/test metadata read from UEnvQuery."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddEnvQueryOptionEntity(
	const FString& ProjectId,
	const UEnvQuery& Query,
	const UEnvQueryOption& Option,
	int32 OptionIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = Query.GetPathName() + TEXT(":option:") + FString::FromInt(OptionIndex);
	const FString EntityId = MakeStableId(ProjectId, TEXT("env_query_option"), CanonicalKey);
	if (FindAIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("env_query_option");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Option.Generator ? Option.Generator->OptionName : FString::Printf(TEXT("Option %d"), OptionIndex);
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("query_path"), Query.GetPathName());
	Entity.Attributes.Add(TEXT("option_index"), FString::FromInt(OptionIndex));
	Entity.Attributes.Add(TEXT("generator_class_path"), AIClassPath(Option.Generator));
	Entity.Attributes.Add(TEXT("generator_name"), Option.Generator ? Option.Generator->OptionName : FString());
	Entity.Attributes.Add(TEXT("test_count"), FString::FromInt(Option.Tests.Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("env_query_option_metadata"), TEXT("generator_reference"), TEXT("test_references") };
	Entity.Completeness.Omitted = { TEXT("runtime_generated_items") };
	AddAIEvidence(Entity, Query.GetPathName(), TEXT("EQS option metadata read from UEnvQuery option objects."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddEnvQueryGeneratorEntity(
	const FString& ProjectId,
	const UEnvQuery& Query,
	const UEnvQueryGenerator& Generator,
	int32 OptionIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString GeneratorPath = Generator.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("env_query_generator"), GeneratorPath);
	if (FindAIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("env_query_generator");
	Entity.CanonicalKey = GeneratorPath;
	Entity.DisplayName = Generator.OptionName;
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("query_path"), Query.GetPathName());
	Entity.Attributes.Add(TEXT("option_index"), FString::FromInt(OptionIndex));
	Entity.Attributes.Add(TEXT("generator_path"), GeneratorPath);
	Entity.Attributes.Add(TEXT("generator_class_path"), AIClassPath(&Generator));
	Entity.Attributes.Add(TEXT("option_name"), Generator.OptionName);
	Entity.Attributes.Add(TEXT("item_type_class"), Generator.ItemType ? Generator.ItemType->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("auto_sort_tests"), AIBool(Generator.bAutoSortTests != 0));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("env_query_generator_metadata") };
	Entity.Completeness.Omitted = { TEXT("runtime_generated_items") };
	AddAIEvidence(Entity, Query.GetPathName(), TEXT("EQS generator metadata read from UEnvQueryOption."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddEnvQueryTestEntity(
	const FString& ProjectId,
	const UEnvQuery& Query,
	const UEnvQueryTest& Test,
	int32 OptionIndex,
	int32 TestIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString TestPath = Test.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("env_query_test"), TestPath);
	if (FindAIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("env_query_test");
	Entity.CanonicalKey = TestPath;
	Entity.DisplayName = Test.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("query_path"), Query.GetPathName());
	Entity.Attributes.Add(TEXT("option_index"), FString::FromInt(OptionIndex));
	Entity.Attributes.Add(TEXT("test_index"), FString::FromInt(TestIndex));
	Entity.Attributes.Add(TEXT("test_order"), FString::FromInt(Test.TestOrder));
	Entity.Attributes.Add(TEXT("test_path"), TestPath);
	Entity.Attributes.Add(TEXT("test_class_path"), AIClassPath(&Test));
	Entity.Attributes.Add(TEXT("purpose"), EQSTestPurposeString(Test.TestPurpose.GetValue()));
	Entity.Attributes.Add(TEXT("filter_type"), EQSFilterTypeString(Test.FilterType.GetValue()));
	Entity.Attributes.Add(TEXT("scoring_equation"), EQSScoreEquationString(Test.ScoringEquation.GetValue()));
	Entity.Attributes.Add(TEXT("cost"), EQSTestCostString(Test.Cost.GetValue()));
	Entity.Attributes.Add(TEXT("comment"), Test.TestComment);
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Semantic));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("env_query_test_metadata"), TEXT("filter_and_score_metadata") };
	Entity.Completeness.Omitted = { TEXT("runtime_item_scores"), TEXT("data_provider_runtime_values") };
	AddAIEvidence(Entity, Query.GetPathName(), TEXT("EQS test metadata read from UEnvQueryOption."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

TSharedRef<FJsonObject> BlackboardKeySnapshot(const FString& KeyId, const FBlackboardEntry& Key, int32 KeyIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), KeyId);
	Object->SetNumberField(TEXT("index"), KeyIndex);
	Object->SetStringField(TEXT("name"), Key.EntryName.ToString());
	Object->SetStringField(TEXT("key_type_class"), AIClassPath(Key.KeyType));
	Object->SetBoolField(TEXT("instance_synced"), Key.bInstanceSynced != 0);
#if WITH_EDITORONLY_DATA
	Object->SetStringField(TEXT("description"), Key.EntryDescription);
	Object->SetStringField(TEXT("category"), Key.EntryCategory.ToString());
#else
	Object->SetStringField(TEXT("description"), FString());
	Object->SetStringField(TEXT("category"), FString());
#endif
	return Object;
}

TSharedRef<FJsonObject> BlackboardSnapshot(
	const FString& ProjectId,
	const UBlackboardData& Blackboard,
	const FString& BlackboardId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.blackboard.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), BlackboardId);
	Object->SetStringField(TEXT("blackboard_path"), Blackboard.GetPathName());
	Object->SetStringField(TEXT("parent_path"), AIObjectPath(Blackboard.Parent));
	Object->SetBoolField(TEXT("has_synchronized_keys"), Blackboard.HasSynchronizedKeys());
	Object->SetNumberField(TEXT("key_count"), Blackboard.Keys.Num());

	TArray<TSharedPtr<FJsonValue>> KeyValues;
	for (int32 KeyIndex = 0; KeyIndex < Blackboard.Keys.Num(); ++KeyIndex)
	{
		const FBlackboardEntry& Key = Blackboard.Keys[KeyIndex];
		const FString KeyId = AddBlackboardKeyEntity(ProjectId, Blackboard, Key, KeyIndex, OutEntities);
		AddAIRelation(ProjectId, TEXT("contains_blackboard_key"), BlackboardId, KeyId, Blackboard.GetPathName(), TEXT("Blackboard contains this key entry."), OutRelations);
		KeyValues.Add(MakeShared<FJsonValueObject>(BlackboardKeySnapshot(KeyId, Key, KeyIndex)));
	}

	Object->SetArrayField(TEXT("keys"), KeyValues);
	return Object;
}

TSharedRef<FJsonObject> BehaviorTreeNodeSnapshot(const FString& NodeId, const UBTNode& Node, const FString& NodeRole, int32 NodeIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), NodeId);
	Object->SetNumberField(TEXT("index"), NodeIndex);
	Object->SetStringField(TEXT("node_path"), Node.GetPathName());
	Object->SetStringField(TEXT("node_name"), Node.GetNodeName());
	Object->SetStringField(TEXT("node_role"), NodeRole);
	Object->SetStringField(TEXT("node_class_path"), AIClassPath(&Node));
	Object->SetNumberField(TEXT("execution_index"), Node.GetExecutionIndex());
	Object->SetNumberField(TEXT("tree_depth"), Node.GetTreeDepth());
	Object->SetStringField(TEXT("static_description"), Node.GetStaticDescription());
	Object->SetNumberField(TEXT("child_count"), Cast<UBTCompositeNode>(&Node) ? CastChecked<UBTCompositeNode>(&Node)->Children.Num() : 0);
	Object->SetNumberField(TEXT("service_count"), Cast<UBTCompositeNode>(&Node) ? CastChecked<UBTCompositeNode>(&Node)->Services.Num() : (Cast<UBTTaskNode>(&Node) ? CastChecked<UBTTaskNode>(&Node)->Services.Num() : 0));
	return Object;
}

void AppendBehaviorTreeServiceNodes(
	const FString& ProjectId,
	const UBehaviorTree& Tree,
	const UBTNode& OwnerNode,
	const FString& OwnerNodeId,
	const TArray<TObjectPtr<UBTService>>& Services,
	TArray<TSharedPtr<FJsonValue>>& NodeValues,
	int32& NodeIndex,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	for (const TObjectPtr<UBTService>& Service : Services)
	{
		if (!Service)
		{
			continue;
		}
		const FString ServiceId = AddBehaviorTreeNodeEntity(ProjectId, Tree, *Service, TEXT("service"), NodeIndex, OutEntities);
		AddAIRelation(ProjectId, TEXT("contains_behavior_tree_node"), MakeStableId(ProjectId, TEXT("behavior_tree"), Tree.GetPathName()), ServiceId, Tree.GetPathName(), TEXT("BehaviorTree contains this service node."), OutRelations);
		AddAIRelation(ProjectId, TEXT("behavior_tree_service"), OwnerNodeId, ServiceId, Tree.GetPathName(), TEXT("BehaviorTree node owns this service."), OutRelations);
		NodeValues.Add(MakeShared<FJsonValueObject>(BehaviorTreeNodeSnapshot(ServiceId, *Service, TEXT("service"), NodeIndex)));
		++NodeIndex;
	}
}

void TraverseBehaviorTreeNode(
	const FString& ProjectId,
	const UBehaviorTree& Tree,
	const UBTNode& Node,
	const FString& NodeRole,
	const FString& ParentNodeId,
	TArray<TSharedPtr<FJsonValue>>& NodeValues,
	int32& NodeIndex,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString NodeId = AddBehaviorTreeNodeEntity(ProjectId, Tree, Node, NodeRole, NodeIndex, OutEntities);
	const FString TreeId = MakeStableId(ProjectId, TEXT("behavior_tree"), Tree.GetPathName());
	AddAIRelation(ProjectId, TEXT("contains_behavior_tree_node"), TreeId, NodeId, Tree.GetPathName(), TEXT("BehaviorTree contains this template node."), OutRelations);
	if (!ParentNodeId.IsEmpty())
	{
		AddAIRelation(ProjectId, TEXT("behavior_tree_child"), ParentNodeId, NodeId, Tree.GetPathName(), TEXT("BehaviorTree parent node links to this child node."), OutRelations);
	}
	NodeValues.Add(MakeShared<FJsonValueObject>(BehaviorTreeNodeSnapshot(NodeId, Node, NodeRole, NodeIndex)));
	++NodeIndex;

	if (const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(&Node))
	{
		AppendBehaviorTreeServiceNodes(ProjectId, Tree, Node, NodeId, Composite->Services, NodeValues, NodeIndex, OutEntities, OutRelations);
		for (int32 ChildIndex = 0; ChildIndex < Composite->Children.Num(); ++ChildIndex)
		{
			const FBTCompositeChild& Child = Composite->Children[ChildIndex];
			const UBTNode* ChildNode = Child.ChildComposite ? StaticCast<const UBTNode*>(Child.ChildComposite.Get()) : StaticCast<const UBTNode*>(Child.ChildTask.Get());
			if (!ChildNode)
			{
				continue;
			}

			TraverseBehaviorTreeNode(ProjectId, Tree, *ChildNode, Child.ChildComposite ? TEXT("composite") : TEXT("task"), NodeId, NodeValues, NodeIndex, OutEntities, OutRelations);
			const FString ChildNodeId = MakeStableId(ProjectId, TEXT("behavior_tree_node"), ChildNode->GetPathName());
			for (const TObjectPtr<UBTDecorator>& Decorator : Child.Decorators)
			{
				if (!Decorator)
				{
					continue;
				}
				const FString DecoratorId = AddBehaviorTreeNodeEntity(ProjectId, Tree, *Decorator, TEXT("decorator"), NodeIndex, OutEntities);
				AddAIRelation(ProjectId, TEXT("contains_behavior_tree_node"), TreeId, DecoratorId, Tree.GetPathName(), TEXT("BehaviorTree contains this decorator node."), OutRelations);
				AddAIRelation(ProjectId, TEXT("behavior_tree_decorator"), ChildNodeId, DecoratorId, Tree.GetPathName(), TEXT("BehaviorTree child branch is guarded by this decorator."), OutRelations);
				NodeValues.Add(MakeShared<FJsonValueObject>(BehaviorTreeNodeSnapshot(DecoratorId, *Decorator, TEXT("decorator"), NodeIndex)));
				++NodeIndex;
			}
		}
	}
	else if (const UBTTaskNode* Task = Cast<UBTTaskNode>(&Node))
	{
		AppendBehaviorTreeServiceNodes(ProjectId, Tree, Node, NodeId, Task->Services, NodeValues, NodeIndex, OutEntities, OutRelations);
	}
}

TSharedRef<FJsonObject> BehaviorTreeSnapshot(
	const FString& ProjectId,
	const UBehaviorTree& Tree,
	const FString& TreeId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.behavior_tree.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), TreeId);
	Object->SetStringField(TEXT("tree_path"), Tree.GetPathName());
	Object->SetStringField(TEXT("blackboard_path"), AIObjectPath(Tree.BlackboardAsset));
	Object->SetStringField(TEXT("root_node_id"), Tree.RootNode ? MakeStableId(ProjectId, TEXT("behavior_tree_node"), Tree.RootNode->GetPathName()) : FString());
	Object->SetNumberField(TEXT("root_decorator_count"), Tree.RootDecorators.Num());
	Object->SetNumberField(TEXT("instance_memory_size"), Tree.InstanceMemorySize);

	TArray<TSharedPtr<FJsonValue>> NodeValues;
	int32 NodeIndex = 0;
	if (Tree.RootNode)
	{
		TraverseBehaviorTreeNode(ProjectId, Tree, *Tree.RootNode, TEXT("root"), FString(), NodeValues, NodeIndex, OutEntities, OutRelations);
	}

	for (const TObjectPtr<UBTDecorator>& Decorator : Tree.RootDecorators)
	{
		if (!Decorator)
		{
			continue;
		}
		const FString DecoratorId = AddBehaviorTreeNodeEntity(ProjectId, Tree, *Decorator, TEXT("root_decorator"), NodeIndex, OutEntities);
		AddAIRelation(ProjectId, TEXT("contains_behavior_tree_node"), TreeId, DecoratorId, Tree.GetPathName(), TEXT("BehaviorTree contains this root-level decorator node."), OutRelations);
		AddAIRelation(ProjectId, TEXT("behavior_tree_root_decorator"), TreeId, DecoratorId, Tree.GetPathName(), TEXT("BehaviorTree has this root-level decorator."), OutRelations);
		NodeValues.Add(MakeShared<FJsonValueObject>(BehaviorTreeNodeSnapshot(DecoratorId, *Decorator, TEXT("root_decorator"), NodeIndex)));
		++NodeIndex;
	}

	Object->SetNumberField(TEXT("node_count"), NodeValues.Num());
	Object->SetArrayField(TEXT("nodes"), NodeValues);
	return Object;
}

TSharedRef<FJsonObject> EQSGeneratorSnapshot(const FString& GeneratorId, const UEnvQueryGenerator* Generator)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), GeneratorId);
	Object->SetStringField(TEXT("path"), AIObjectPath(Generator));
	Object->SetStringField(TEXT("class_path"), AIClassPath(Generator));
	Object->SetStringField(TEXT("option_name"), Generator ? Generator->OptionName : FString());
	Object->SetStringField(TEXT("item_type_class"), Generator && Generator->ItemType ? Generator->ItemType->GetPathName() : FString());
	Object->SetBoolField(TEXT("auto_sort_tests"), Generator ? Generator->bAutoSortTests != 0 : false);
	return Object;
}

TSharedRef<FJsonObject> EQSTestSnapshot(const FString& TestId, const UEnvQueryTest& Test, int32 TestIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), TestId);
	Object->SetNumberField(TEXT("index"), TestIndex);
	Object->SetNumberField(TEXT("test_order"), Test.TestOrder);
	Object->SetStringField(TEXT("path"), Test.GetPathName());
	Object->SetStringField(TEXT("class_path"), AIClassPath(&Test));
	Object->SetStringField(TEXT("purpose"), EQSTestPurposeString(Test.TestPurpose.GetValue()));
	Object->SetStringField(TEXT("filter_type"), EQSFilterTypeString(Test.FilterType.GetValue()));
	Object->SetStringField(TEXT("scoring_equation"), EQSScoreEquationString(Test.ScoringEquation.GetValue()));
	Object->SetStringField(TEXT("cost"), EQSTestCostString(Test.Cost.GetValue()));
	Object->SetStringField(TEXT("comment"), Test.TestComment);
	return Object;
}

TSharedRef<FJsonObject> EnvQuerySnapshot(
	const FString& ProjectId,
	const UEnvQuery& Query,
	const FString& QueryId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.env_query.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), QueryId);
	Object->SetStringField(TEXT("query_path"), Query.GetPathName());
	Object->SetStringField(TEXT("query_name"), Query.GetQueryName().ToString());
	Object->SetNumberField(TEXT("option_count"), Query.GetOptions().Num());

	TArray<TSharedPtr<FJsonValue>> OptionValues;
	const TArray<UEnvQueryOption*>& Options = Query.GetOptions();
	for (int32 OptionIndex = 0; OptionIndex < Options.Num(); ++OptionIndex)
	{
		const UEnvQueryOption* Option = Options[OptionIndex];
		if (!Option)
		{
			continue;
		}

		const FString OptionId = AddEnvQueryOptionEntity(ProjectId, Query, *Option, OptionIndex, OutEntities);
		AddAIRelation(ProjectId, TEXT("contains_env_query_option"), QueryId, OptionId, Query.GetPathName(), TEXT("EQS query contains this option."), OutRelations);

		TSharedRef<FJsonObject> OptionObject = MakeShared<FJsonObject>();
		OptionObject->SetStringField(TEXT("id"), OptionId);
		OptionObject->SetNumberField(TEXT("index"), OptionIndex);
		OptionObject->SetStringField(TEXT("title"), Option->GetDescriptionTitle().ToString());
		OptionObject->SetStringField(TEXT("details"), Option->GetDescriptionDetails().ToString());

		const FString GeneratorId = Option->Generator
			? AddEnvQueryGeneratorEntity(ProjectId, Query, *Option->Generator, OptionIndex, OutEntities)
			: FString();
		if (Option->Generator)
		{
			AddAIRelation(ProjectId, TEXT("uses_env_query_generator"), OptionId, GeneratorId, Query.GetPathName(), TEXT("EQS option uses this generator."), OutRelations);
		}
		OptionObject->SetObjectField(TEXT("generator"), EQSGeneratorSnapshot(GeneratorId, Option->Generator));

		TArray<TSharedPtr<FJsonValue>> TestValues;
		for (int32 TestIndex = 0; TestIndex < Option->Tests.Num(); ++TestIndex)
		{
			const UEnvQueryTest* Test = Option->Tests[TestIndex];
			if (!Test)
			{
				continue;
			}
			const FString TestId = AddEnvQueryTestEntity(ProjectId, Query, *Test, OptionIndex, TestIndex, OutEntities);
			AddAIRelation(ProjectId, TEXT("contains_env_query_test"), OptionId, TestId, Query.GetPathName(), TEXT("EQS option contains this test."), OutRelations);
			TestValues.Add(MakeShared<FJsonValueObject>(EQSTestSnapshot(TestId, *Test, TestIndex)));
		}
		OptionObject->SetNumberField(TEXT("test_count"), TestValues.Num());
		OptionObject->SetArrayField(TEXT("tests"), TestValues);
		OptionValues.Add(MakeShared<FJsonValueObject>(OptionObject));
	}

	Object->SetArrayField(TEXT("options"), OptionValues);
	return Object;
}
}

bool FAIReader::AppendAIAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (UBlackboardData* Blackboard = Cast<UBlackboardData>(&Asset))
	{
		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}

		const FString BlackboardId = AddBlackboardEntity(ProjectId, *Blackboard, OutEntities);
		AddAIRelation(ProjectId, TEXT("contains_blackboard"), AssetEntity.Id, BlackboardId, Blackboard->GetPathName(), TEXT("Asset contains the extracted Blackboard record."), OutRelations);
		if (Blackboard->Parent)
		{
			const FString ParentId = AddBlackboardEntity(ProjectId, *Blackboard->Parent, OutEntities);
			AddAIRelation(ProjectId, TEXT("blackboard_parent"), BlackboardId, ParentId, Blackboard->GetPathName(), TEXT("Blackboard inherits keys from this parent blackboard."), OutRelations);
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("blackboard"), BlackboardSnapshot(ProjectId, *Blackboard, BlackboardId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("blackboard_key_count"), FString::FromInt(Blackboard->Keys.Num()));
		AssetEntity.Attributes.Add(TEXT("blackboard_parent"), AIObjectPath(Blackboard->Parent));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("blackboard_keys"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_blackboard_values"));
		AddAIEvidence(AssetEntity, Blackboard->GetPathName(), TEXT("Blackboard key structure extracted from UBlackboardData."));
		return true;
	}

	if (UBehaviorTree* BehaviorTree = Cast<UBehaviorTree>(&Asset))
	{
		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}

		const FString TreeId = AddBehaviorTreeEntity(ProjectId, *BehaviorTree, OutEntities);
		AddAIRelation(ProjectId, TEXT("contains_behavior_tree"), AssetEntity.Id, TreeId, BehaviorTree->GetPathName(), TEXT("Asset contains the extracted BehaviorTree record."), OutRelations);
		if (BehaviorTree->BlackboardAsset)
		{
			const FString BlackboardId = AddBlackboardEntity(ProjectId, *BehaviorTree->BlackboardAsset, OutEntities);
			AddAIRelation(ProjectId, TEXT("uses_blackboard"), TreeId, BlackboardId, BehaviorTree->GetPathName(), TEXT("BehaviorTree references this Blackboard asset."), OutRelations);
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("behavior_tree"), BehaviorTreeSnapshot(ProjectId, *BehaviorTree, TreeId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("behavior_tree_blackboard"), AIObjectPath(BehaviorTree->BlackboardAsset));
		AssetEntity.Attributes.Add(TEXT("behavior_tree_root_node"), AIObjectPath(BehaviorTree->RootNode));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("behavior_tree_template_nodes"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_behavior_tree_execution"));
		AddAIEvidence(AssetEntity, BehaviorTree->GetPathName(), TEXT("BehaviorTree template structure extracted from UBehaviorTree."));
		return true;
	}

	if (UEnvQuery* Query = Cast<UEnvQuery>(&Asset))
	{
		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}

		const FString QueryId = AddEnvQueryEntity(ProjectId, *Query, OutEntities);
		AddAIRelation(ProjectId, TEXT("contains_env_query"), AssetEntity.Id, QueryId, Query->GetPathName(), TEXT("Asset contains the extracted EQS query record."), OutRelations);
		AssetEntity.Snapshot->SetObjectField(TEXT("env_query"), EnvQuerySnapshot(ProjectId, *Query, QueryId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("env_query_name"), Query->GetQueryName().ToString());
		AssetEntity.Attributes.Add(TEXT("env_query_option_count"), FString::FromInt(Query->GetOptions().Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("env_query_options"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_query_results"));
		AddAIEvidence(AssetEntity, Query->GetPathName(), TEXT("EQS structure extracted from UEnvQuery."));
		return true;
	}

	return false;
}
}
