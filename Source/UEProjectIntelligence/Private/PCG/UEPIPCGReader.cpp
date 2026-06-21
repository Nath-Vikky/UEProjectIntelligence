#include "UEPIPCGReader.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "PCGCommon.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGSubgraph.h"
#include "UObject/UnrealType.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString PCGBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString PCGEnumValue(const UEnum* Enum, int64 Value)
{
	return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(static_cast<int32>(Value));
}

FString TruncatePCGValue(FString Value)
{
	constexpr int32 MaxLength = 512;
	if (Value.Len() > MaxLength)
	{
		Value.LeftInline(MaxLength);
		Value.Append(TEXT("..."));
	}
	return Value;
}

FString PCGPropertyValue(const UObject& Object, const FProperty& Property)
{
	FString Value;
	const void* ValuePtr = Property.ContainerPtrToValuePtr<void>(&Object);
	Property.ExportTextItem_Direct(Value, ValuePtr, nullptr, const_cast<UObject*>(&Object), PPF_None);
	return TruncatePCGValue(MoveTemp(Value));
}

void AddPCGEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

void AddPCGRelation(
	const FString& ProjectId,
	const FString& Type,
	const FString& FromId,
	const FString& ToId,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FRelationRecord>& OutRelations,
	const TMap<FString, FString>& Attributes = TMap<FString, FString>())
{
	if (FromId.IsEmpty() || ToId.IsEmpty())
	{
		return;
	}

	FRelationRecord Relation;
	Relation.Id = MakeRelationId(ProjectId, Type, FromId, ToId, Attributes.Num() > 0 ? &Attributes : nullptr);
	Relation.Type = Type;
	Relation.FromId = FromId;
	Relation.ToId = ToId;
	Relation.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Relation.bDerived = false;
	Relation.Confidence = 1.0f;
	Relation.Attributes = Attributes;
	Relation.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		EvidencePath,
		EvidenceDetail
	});
	OutRelations.Add(MoveTemp(Relation));
}

FEntityRecord* FindPCGEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

FString PCGDataTypeString(EPCGDataType DataType)
{
	const uint32 Value = static_cast<uint32>(DataType);
	if (Value == static_cast<uint32>(EPCGDataType::None))
	{
		return TEXT("None");
	}
	if (Value == static_cast<uint32>(EPCGDataType::Any))
	{
		return TEXT("Any");
	}

	TArray<FString> Names;
	const auto AddFlag = [&Names, Value](EPCGDataType Flag, const TCHAR* Name)
	{
		if ((Value & static_cast<uint32>(Flag)) != 0)
		{
			Names.Add(Name);
		}
	};

	AddFlag(EPCGDataType::Point, TEXT("Point"));
	AddFlag(EPCGDataType::Spline, TEXT("Spline"));
	AddFlag(EPCGDataType::LandscapeSpline, TEXT("LandscapeSpline"));
	AddFlag(EPCGDataType::Landscape, TEXT("Landscape"));
	AddFlag(EPCGDataType::Texture, TEXT("Texture"));
	AddFlag(EPCGDataType::RenderTarget, TEXT("RenderTarget"));
	AddFlag(EPCGDataType::Volume, TEXT("Volume"));
	AddFlag(EPCGDataType::Primitive, TEXT("Primitive"));
	AddFlag(EPCGDataType::Composite, TEXT("Composite"));
	AddFlag(EPCGDataType::Param, TEXT("Param"));
	AddFlag(EPCGDataType::Settings, TEXT("Settings"));
	AddFlag(EPCGDataType::Other, TEXT("Other"));

	if (Names.Num() == 0)
	{
		return FString::FromInt(static_cast<int32>(Value));
	}
	return FString::Join(Names, TEXT("|"));
}

FString PCGSettingsTypeString(const UPCGSettings* Settings)
{
#if WITH_EDITOR
	if (Settings)
	{
		return PCGEnumValue(StaticEnum<EPCGSettingsType>(), static_cast<int64>(Settings->GetType()));
	}
#endif
	return FString();
}

TArray<TSharedPtr<FJsonValue>> PCGStringSetValues(const TSet<FString>& Values)
{
	TArray<FString> SortedValues = Values.Array();
	SortedValues.Sort();

	TArray<TSharedPtr<FJsonValue>> JsonValues;
	for (const FString& Value : SortedValues)
	{
		JsonValues.Add(MakeShared<FJsonValueString>(Value));
	}
	return JsonValues;
}

TSharedRef<FJsonObject> PCGPinObject(
	const UPCGPin& Pin,
	const FString& PinId,
	const FString& NodeId,
	int32 PinIndex,
	const FString& Direction)
{
	const FPCGPinProperties& Properties = Pin.Properties;
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), PinId);
	Object->SetNumberField(TEXT("index"), PinIndex);
	Object->SetStringField(TEXT("node_id"), NodeId);
	Object->SetStringField(TEXT("direction"), Direction);
	Object->SetStringField(TEXT("label"), Properties.Label.ToString());
	Object->SetStringField(TEXT("allowed_types"), PCGDataTypeString(Properties.AllowedTypes));
	Object->SetStringField(TEXT("current_types"), PCGDataTypeString(Pin.GetCurrentTypes()));
	Object->SetBoolField(TEXT("allow_multiple_data"), Properties.bAllowMultipleData);
	Object->SetBoolField(TEXT("allow_multiple_connections"), Properties.bAllowMultipleConnections);
	Object->SetBoolField(TEXT("advanced_pin"), Properties.bAdvancedPin);
	Object->SetNumberField(TEXT("edge_count"), Pin.EdgeCount());
#if WITH_EDITORONLY_DATA
	Object->SetStringField(TEXT("tooltip"), Properties.Tooltip.ToString());
#else
	Object->SetStringField(TEXT("tooltip"), FString());
#endif
	return Object;
}

TSharedRef<FJsonObject> UniversalPortObject(const TSharedRef<FJsonObject>& Pin)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), Pin->GetStringField(TEXT("id")));
	Object->SetStringField(TEXT("node_id"), Pin->GetStringField(TEXT("node_id")));
	Object->SetStringField(TEXT("name"), Pin->GetStringField(TEXT("label")));
	Object->SetStringField(TEXT("direction"), Pin->GetStringField(TEXT("direction")));
	Object->SetStringField(TEXT("type"), Pin->GetStringField(TEXT("allowed_types")));
	Object->SetStringField(TEXT("domain_type"), Pin->GetStringField(TEXT("current_types")));
	return Object;
}

TArray<TSharedPtr<FJsonValue>> PCGSettingPropertyValues(const UPCGSettings& Settings)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	int32 PropertyIndex = 0;
	for (TFieldIterator<FProperty> PropertyIt(Settings.GetClass(), EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
	{
		const FProperty* Property = *PropertyIt;
		if (!Property ||
			!Property->HasAnyPropertyFlags(CPF_Edit) ||
			Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient))
		{
			continue;
		}

		TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
		PropertyObject->SetNumberField(TEXT("index"), PropertyIndex++);
		PropertyObject->SetStringField(TEXT("name"), Property->GetName());
		PropertyObject->SetStringField(TEXT("type"), Property->GetCPPType());
		PropertyObject->SetStringField(TEXT("value"), PCGPropertyValue(Settings, *Property));
		PropertyObject->SetBoolField(TEXT("overridable"), Property->HasMetaData(PCGObjectMetadata::Overridable));
		Values.Add(MakeShared<FJsonValueObject>(PropertyObject));
	}
	return Values;
}

FString AddPCGGraphEntity(
	const FString& ProjectId,
	const UPCGGraph& Graph,
	TArray<FEntityRecord>& OutEntities)
{
	const FString GraphPath = Graph.GetPathName();
	const FString GraphId = MakeStableId(ProjectId, TEXT("pcg_graph"), GraphPath);
	if (FindPCGEntity(OutEntities, GraphId))
	{
		return GraphId;
	}

	FEntityRecord Entity;
	Entity.Id = GraphId;
	Entity.Kind = TEXT("pcg_graph");
	Entity.CanonicalKey = GraphPath;
	Entity.DisplayName = Graph.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("pcg_graph_path"), GraphPath);
	Entity.Attributes.Add(TEXT("pcg_graph_class"), Graph.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("node_count"), FString::FromInt(Graph.GetNodes().Num()));
	Entity.Attributes.Add(TEXT("hierarchical_generation"), PCGBool(Graph.IsHierarchicalGenerationEnabled()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("pcg_graph_metadata"), TEXT("pcg_nodes"), TEXT("pcg_pins"), TEXT("pcg_edges") };
	Entity.Completeness.Omitted = { TEXT("pcg_generated_results"), TEXT("runtime_world_actor_resolution") };
	AddPCGEvidence(Entity, GraphPath, TEXT("UPCGGraph structure read through public PCG runtime API."));
	OutEntities.Add(MoveTemp(Entity));
	return GraphId;
}

FString AddPCGNodeEntity(
	const FString& ProjectId,
	const FString& GraphPath,
	const FString& GraphId,
	const UPCGNode& Node,
	int32 NodeIndex,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString NodePath = Node.GetPathName();
	const FString NodeId = MakeStableId(ProjectId, TEXT("pcg_node"), NodePath);
	if (!FindPCGEntity(OutEntities, NodeId))
	{
		FEntityRecord Entity;
		Entity.Id = NodeId;
		Entity.Kind = TEXT("pcg_node");
		Entity.CanonicalKey = NodePath;
		Entity.DisplayName = Node.GetNodeTitle().ToString();
		Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
		Entity.Attributes.Add(TEXT("pcg_graph_path"), GraphPath);
		Entity.Attributes.Add(TEXT("node_path"), NodePath);
		Entity.Attributes.Add(TEXT("node_index"), FString::FromInt(NodeIndex));
		Entity.Attributes.Add(TEXT("node_class"), Node.GetClass()->GetPathName());
		Entity.Attributes.Add(TEXT("title"), Node.GetNodeTitle().ToString());
		if (const UPCGSettings* Settings = Node.GetSettings())
		{
			Entity.Attributes.Add(TEXT("settings_class"), Settings->GetClass()->GetPathName());
			Entity.Attributes.Add(TEXT("settings_type"), PCGSettingsTypeString(Settings));
		}
		Entity.Attributes.Add(TEXT("input_pin_count"), FString::FromInt(Node.GetInputPins().Num()));
		Entity.Attributes.Add(TEXT("output_pin_count"), FString::FromInt(Node.GetOutputPins().Num()));
		Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
		Entity.Completeness.State = ECompletenessState::Partial;
		Entity.Completeness.Covered = { TEXT("node_metadata"), TEXT("pins"), TEXT("settings_reference") };
		Entity.Completeness.Omitted = { TEXT("runtime_execution_state"), TEXT("generated_data") };
		AddPCGEvidence(Entity, GraphPath, TEXT("UPCGNode metadata read from a PCG graph asset."));
		OutEntities.Add(MoveTemp(Entity));
	}

	TMap<FString, FString> Attributes;
	Attributes.Add(TEXT("node_index"), FString::FromInt(NodeIndex));
	AddPCGRelation(ProjectId, TEXT("contains_pcg_node"), GraphId, NodeId, GraphPath, TEXT("PCG graph contains this node."), OutRelations, Attributes);
	return NodeId;
}

FString AddPCGPinEntity(
	const FString& ProjectId,
	const FString& GraphPath,
	const FString& NodeId,
	const UPCGPin& Pin,
	int32 PinIndex,
	const FString& Direction,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString PinPath = Pin.GetPathName();
	const FString PinId = MakeStableId(ProjectId, TEXT("pcg_pin"), PinPath);
	if (!FindPCGEntity(OutEntities, PinId))
	{
		FEntityRecord Entity;
		Entity.Id = PinId;
		Entity.Kind = TEXT("pcg_pin");
		Entity.CanonicalKey = PinPath;
		Entity.DisplayName = Pin.Properties.Label.ToString();
		Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
		Entity.Attributes.Add(TEXT("pin_path"), PinPath);
		Entity.Attributes.Add(TEXT("node_id"), NodeId);
		Entity.Attributes.Add(TEXT("direction"), Direction);
		Entity.Attributes.Add(TEXT("label"), Pin.Properties.Label.ToString());
		Entity.Attributes.Add(TEXT("allowed_types"), PCGDataTypeString(Pin.Properties.AllowedTypes));
		Entity.Attributes.Add(TEXT("current_types"), PCGDataTypeString(Pin.GetCurrentTypes()));
		Entity.Attributes.Add(TEXT("edge_count"), FString::FromInt(Pin.EdgeCount()));
		Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
		Entity.Completeness.State = ECompletenessState::Partial;
		Entity.Completeness.Covered = { TEXT("pin_metadata"), TEXT("edge_references") };
		Entity.Completeness.Omitted = { TEXT("runtime_data_payload") };
		AddPCGEvidence(Entity, GraphPath, TEXT("UPCGPin metadata read from a PCG node."));
		OutEntities.Add(MoveTemp(Entity));
	}

	TMap<FString, FString> Attributes;
	Attributes.Add(TEXT("pin_index"), FString::FromInt(PinIndex));
	Attributes.Add(TEXT("direction"), Direction);
	AddPCGRelation(ProjectId, TEXT("contains_pcg_pin"), NodeId, PinId, GraphPath, TEXT("PCG node contains this pin."), OutRelations, Attributes);
	return PinId;
}

FString AddPCGSettingsEntity(
	const FString& ProjectId,
	const FString& GraphPath,
	const FString& NodeId,
	const UPCGSettings& Settings,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString SettingsPath = Settings.GetPathName();
	const FString SettingsId = MakeStableId(ProjectId, TEXT("pcg_settings"), SettingsPath);
	if (!FindPCGEntity(OutEntities, SettingsId))
	{
		const TArray<TSharedPtr<FJsonValue>> Properties = PCGSettingPropertyValues(Settings);

		FEntityRecord Entity;
		Entity.Id = SettingsId;
		Entity.Kind = TEXT("pcg_settings");
		Entity.CanonicalKey = SettingsPath;
		Entity.DisplayName = Settings.GetName();
		Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
		Entity.Attributes.Add(TEXT("settings_path"), SettingsPath);
		Entity.Attributes.Add(TEXT("settings_class"), Settings.GetClass()->GetPathName());
		Entity.Attributes.Add(TEXT("settings_type"), PCGSettingsTypeString(&Settings));
		Entity.Attributes.Add(TEXT("enabled"), PCGBool(Settings.bEnabled));
		Entity.Attributes.Add(TEXT("debug"), PCGBool(Settings.bDebug));
		Entity.Attributes.Add(TEXT("uses_seed"), PCGBool(Settings.UseSeed()));
		Entity.Attributes.Add(TEXT("seed"), FString::FromInt(Settings.Seed));
		Entity.Attributes.Add(TEXT("setting_property_count"), FString::FromInt(Properties.Num()));
		Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
		Entity.Completeness.State = ECompletenessState::Partial;
		Entity.Completeness.Covered = { TEXT("settings_identity"), TEXT("editable_property_summary"), TEXT("tags") };
		Entity.Completeness.Omitted = { TEXT("compiled_runtime_element"), TEXT("generated_data") };
		AddPCGEvidence(Entity, GraphPath, TEXT("UPCGSettings metadata and editable properties read from a PCG node."));
		OutEntities.Add(MoveTemp(Entity));
	}

	AddPCGRelation(ProjectId, TEXT("pcg_node_uses_settings"), NodeId, SettingsId, GraphPath, TEXT("PCG node owns or references this settings object."), OutRelations);
	return SettingsId;
}

FString AddPCGSubgraphReferenceEntity(
	const FString& ProjectId,
	const FString& EvidencePath,
	const UPCGGraph& Subgraph,
	TArray<FEntityRecord>& OutEntities)
{
	const FString SubgraphPath = Subgraph.GetPathName();
	const FString SubgraphId = MakeStableId(ProjectId, TEXT("pcg_subgraph_reference"), SubgraphPath);
	if (FindPCGEntity(OutEntities, SubgraphId))
	{
		return SubgraphId;
	}

	FEntityRecord Entity;
	Entity.Id = SubgraphId;
	Entity.Kind = TEXT("pcg_subgraph_reference");
	Entity.CanonicalKey = SubgraphPath;
	Entity.DisplayName = Subgraph.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("subgraph_path"), SubgraphPath);
	Entity.Attributes.Add(TEXT("subgraph_class"), Subgraph.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Registry));
	Entity.Completeness.State = ECompletenessState::MetadataOnly;
	Entity.Completeness.Covered = { TEXT("subgraph_reference") };
	Entity.Completeness.Omitted = { TEXT("subgraph_inline_expansion") };
	AddPCGEvidence(Entity, EvidencePath, TEXT("PCG subgraph reference read from a subgraph node."));
	OutEntities.Add(MoveTemp(Entity));
	return SubgraphId;
}

TSharedRef<FJsonObject> PCGSettingsObject(const UPCGSettings& Settings, const FString& SettingsId, const FString& NodeId)
{
	TArray<TSharedPtr<FJsonValue>> Properties = PCGSettingPropertyValues(Settings);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), SettingsId);
	Object->SetStringField(TEXT("node_id"), NodeId);
	Object->SetStringField(TEXT("settings_path"), Settings.GetPathName());
	Object->SetStringField(TEXT("settings_class"), Settings.GetClass()->GetPathName());
	Object->SetStringField(TEXT("settings_type"), PCGSettingsTypeString(&Settings));
	Object->SetBoolField(TEXT("enabled"), Settings.bEnabled);
	Object->SetBoolField(TEXT("debug"), Settings.bDebug);
	Object->SetBoolField(TEXT("uses_seed"), Settings.UseSeed());
	Object->SetNumberField(TEXT("seed"), Settings.Seed);
	Object->SetNumberField(TEXT("filter_on_tag_count"), Settings.FilterOnTags.Num());
	Object->SetNumberField(TEXT("tags_applied_on_output_count"), Settings.TagsAppliedOnOutput.Num());
	Object->SetArrayField(TEXT("filter_on_tags"), PCGStringSetValues(Settings.FilterOnTags));
	Object->SetArrayField(TEXT("tags_applied_on_output"), PCGStringSetValues(Settings.TagsAppliedOnOutput));
	Object->SetNumberField(TEXT("property_count"), Properties.Num());
	Object->SetArrayField(TEXT("properties"), MoveTemp(Properties));
	return Object;
}

TSharedRef<FJsonObject> PCGNodeObject(
	const UPCGNode& Node,
	const FString& NodeId,
	const FString& SettingsId,
	const FString& SettingsPath,
	int32 NodeIndex)
{
	int32 PositionX = 0;
	int32 PositionY = 0;
#if WITH_EDITOR
	const_cast<UPCGNode&>(Node).GetNodePosition(PositionX, PositionY);
#endif

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), NodeId);
	Object->SetNumberField(TEXT("index"), NodeIndex);
	Object->SetStringField(TEXT("node_path"), Node.GetPathName());
	Object->SetStringField(TEXT("node_class"), Node.GetClass()->GetPathName());
	Object->SetStringField(TEXT("title"), Node.GetNodeTitle().ToString());
	Object->SetStringField(TEXT("node_title"), Node.NodeTitle.ToString());
	Object->SetStringField(TEXT("settings_id"), SettingsId);
	Object->SetStringField(TEXT("settings_path"), SettingsPath);
	Object->SetNumberField(TEXT("position_x"), PositionX);
	Object->SetNumberField(TEXT("position_y"), PositionY);
	Object->SetNumberField(TEXT("input_pin_count"), Node.GetInputPins().Num());
	Object->SetNumberField(TEXT("output_pin_count"), Node.GetOutputPins().Num());
	Object->SetBoolField(TEXT("has_inbound_edges"), Node.HasInboundEdges());
	Object->SetNumberField(TEXT("inbound_edge_count"), Node.GetInboundEdgesNum());
#if WITH_EDITORONLY_DATA
	Object->SetStringField(TEXT("comment"), Node.NodeComment);
	Object->SetBoolField(TEXT("comment_bubble_pinned"), Node.bCommentBubblePinned != 0);
	Object->SetBoolField(TEXT("comment_bubble_visible"), Node.bCommentBubbleVisible != 0);
#else
	Object->SetStringField(TEXT("comment"), FString());
	Object->SetBoolField(TEXT("comment_bubble_pinned"), false);
	Object->SetBoolField(TEXT("comment_bubble_visible"), false);
#endif
	return Object;
}

TSharedRef<FJsonObject> UniversalNodeObject(
	const TSharedRef<FJsonObject>& Node,
	const UPCGSettings* Settings)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), Node->GetStringField(TEXT("id")));
	Object->SetStringField(TEXT("name"), Node->GetStringField(TEXT("title")));
	Object->SetStringField(TEXT("class"), Node->GetStringField(TEXT("node_class")));
	Object->SetStringField(TEXT("semantic_kind"), Settings ? PCGSettingsTypeString(Settings) : FString());
	Object->SetNumberField(TEXT("x"), Node->GetNumberField(TEXT("position_x")));
	Object->SetNumberField(TEXT("y"), Node->GetNumberField(TEXT("position_y")));
	return Object;
}

TSharedRef<FJsonObject> PCGEdgeObject(
	const FString& EdgeId,
	int32 EdgeIndex,
	const UPCGPin& OutputPin,
	const UPCGPin& InputPin,
	const FString& OutputPinId,
	const FString& InputPinId,
	const TMap<const UPCGNode*, FString>& NodeIds)
{
	const FString SourceNodeId = OutputPin.Node ? NodeIds.FindRef(OutputPin.Node.Get()) : FString();
	const FString TargetNodeId = InputPin.Node ? NodeIds.FindRef(InputPin.Node.Get()) : FString();

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), EdgeId);
	Object->SetNumberField(TEXT("index"), EdgeIndex);
	Object->SetStringField(TEXT("source_node_id"), SourceNodeId);
	Object->SetStringField(TEXT("source_pin_id"), OutputPinId);
	Object->SetStringField(TEXT("source_pin_label"), OutputPin.Properties.Label.ToString());
	Object->SetStringField(TEXT("target_node_id"), TargetNodeId);
	Object->SetStringField(TEXT("target_pin_id"), InputPinId);
	Object->SetStringField(TEXT("target_pin_label"), InputPin.Properties.Label.ToString());
	return Object;
}

TSharedRef<FJsonObject> UniversalLinkObject(const TSharedRef<FJsonObject>& Edge)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), Edge->GetStringField(TEXT("id")));
	Object->SetStringField(TEXT("source_port_id"), Edge->GetStringField(TEXT("source_pin_id")));
	Object->SetStringField(TEXT("target_port_id"), Edge->GetStringField(TEXT("target_pin_id")));
	return Object;
}

TSharedRef<FJsonObject> PCGSubgraphObject(
	const FString& SubgraphReferenceId,
	const FString& NodeId,
	const UPCGGraph& Subgraph,
	int32 Index)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), SubgraphReferenceId);
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("node_id"), NodeId);
	Object->SetStringField(TEXT("subgraph_path"), Subgraph.GetPathName());
	Object->SetStringField(TEXT("subgraph_class"), Subgraph.GetClass()->GetPathName());
	Object->SetStringField(TEXT("subgraph_name"), Subgraph.GetName());
	return Object;
}

TSharedRef<FJsonObject> PCGGraphSnapshot(
	const FString& ProjectId,
	UPCGGraph& Graph,
	const FString& GraphId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FString GraphPath = Graph.GetPathName();
	TArray<UPCGNode*> Nodes = Graph.GetNodes();
	Nodes.AddUnique(Graph.GetInputNode());
	Nodes.AddUnique(Graph.GetOutputNode());
	Nodes.RemoveAll([](const UPCGNode* Node) { return Node == nullptr; });

	TMap<const UPCGNode*, FString> NodeIds;
	TMap<const UPCGPin*, FString> PinIds;

	TArray<TSharedPtr<FJsonValue>> NodeValues;
	TArray<TSharedPtr<FJsonValue>> PinValues;
	TArray<TSharedPtr<FJsonValue>> EdgeValues;
	TArray<TSharedPtr<FJsonValue>> SettingsValues;
	TArray<TSharedPtr<FJsonValue>> SubgraphValues;

	TArray<TSharedPtr<FJsonValue>> UniversalNodeValues;
	TArray<TSharedPtr<FJsonValue>> UniversalPortValues;
	TArray<TSharedPtr<FJsonValue>> UniversalLinkValues;

	int32 SettingsCount = 0;
	int32 PinCount = 0;
	int32 EdgeCount = 0;
	int32 SubgraphCount = 0;

	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		UPCGNode* Node = Nodes[NodeIndex];
		if (!Node)
		{
			continue;
		}

		const FString NodeId = AddPCGNodeEntity(ProjectId, GraphPath, GraphId, *Node, NodeIndex, OutEntities, OutRelations);
		NodeIds.Add(Node, NodeId);

		UPCGSettings* Settings = Node->GetSettings();
		FString SettingsId;
		FString SettingsPath;
		if (Settings)
		{
			SettingsId = AddPCGSettingsEntity(ProjectId, GraphPath, NodeId, *Settings, OutEntities, OutRelations);
			SettingsPath = Settings->GetPathName();
			SettingsValues.Add(MakeShared<FJsonValueObject>(PCGSettingsObject(*Settings, SettingsId, NodeId)));
			++SettingsCount;
		}

		const TSharedRef<FJsonObject> NodeObject = PCGNodeObject(*Node, NodeId, SettingsId, SettingsPath, NodeIndex);
		NodeValues.Add(MakeShared<FJsonValueObject>(NodeObject));
		UniversalNodeValues.Add(MakeShared<FJsonValueObject>(UniversalNodeObject(NodeObject, Settings)));

		auto AppendPins = [&](const TArray<TObjectPtr<UPCGPin>>& Pins, const FString& Direction)
		{
			for (int32 PinIndex = 0; PinIndex < Pins.Num(); ++PinIndex)
			{
				const UPCGPin* Pin = Pins[PinIndex];
				if (!Pin)
				{
					continue;
				}

				const FString PinId = AddPCGPinEntity(ProjectId, GraphPath, NodeId, *Pin, PinIndex, Direction, OutEntities, OutRelations);
				PinIds.Add(Pin, PinId);
				const TSharedRef<FJsonObject> PinObject = PCGPinObject(*Pin, PinId, NodeId, PinIndex, Direction);
				PinValues.Add(MakeShared<FJsonValueObject>(PinObject));
				UniversalPortValues.Add(MakeShared<FJsonValueObject>(UniversalPortObject(PinObject)));
				++PinCount;
			}
		};

		AppendPins(Node->GetInputPins(), TEXT("input"));
		AppendPins(Node->GetOutputPins(), TEXT("output"));

		if (const UPCGBaseSubgraphNode* SubgraphNode = Cast<UPCGBaseSubgraphNode>(Node))
		{
			const UPCGGraphInterface* SubgraphInterface = SubgraphNode->GetSubgraphInterface().Get();
			const UPCGGraph* Subgraph = SubgraphInterface ? SubgraphInterface->GetGraph() : nullptr;
			if (Subgraph)
			{
				const FString SubgraphReferenceId = AddPCGSubgraphReferenceEntity(ProjectId, GraphPath, *Subgraph, OutEntities);
				AddPCGRelation(ProjectId, TEXT("pcg_node_uses_subgraph"), NodeId, SubgraphReferenceId, GraphPath, TEXT("PCG subgraph node references this graph."), OutRelations);
				SubgraphValues.Add(MakeShared<FJsonValueObject>(PCGSubgraphObject(SubgraphReferenceId, NodeId, *Subgraph, SubgraphCount++)));
			}
		}
	}

	TSet<const UPCGEdge*> VisitedEdges;
	for (UPCGNode* Node : Nodes)
	{
		if (!Node)
		{
			continue;
		}

		for (const TObjectPtr<UPCGPin>& OutputPinPtr : Node->GetOutputPins())
		{
			const UPCGPin* OutputPin = OutputPinPtr.Get();
			if (!OutputPin)
			{
				continue;
			}

			for (const TObjectPtr<UPCGEdge>& EdgePtr : OutputPin->Edges)
			{
				const UPCGEdge* Edge = EdgePtr.Get();
				if (!Edge || VisitedEdges.Contains(Edge) || !Edge->IsValid())
				{
					continue;
				}
				VisitedEdges.Add(Edge);

				const UPCGPin* EdgeOutputPin = Edge->OutputPin.Get();
				const UPCGPin* EdgeInputPin = Edge->InputPin.Get();
				if (!EdgeOutputPin || !EdgeInputPin)
				{
					continue;
				}

				const FString OutputPinId = PinIds.FindRef(EdgeOutputPin);
				const FString InputPinId = PinIds.FindRef(EdgeInputPin);
				if (OutputPinId.IsEmpty() || InputPinId.IsEmpty())
				{
					continue;
				}

				TMap<FString, FString> EdgeAttributes;
				EdgeAttributes.Add(TEXT("edge_index"), FString::FromInt(EdgeCount));
				EdgeAttributes.Add(TEXT("source_pin_label"), EdgeOutputPin->Properties.Label.ToString());
				EdgeAttributes.Add(TEXT("target_pin_label"), EdgeInputPin->Properties.Label.ToString());
				AddPCGRelation(ProjectId, TEXT("pcg_edge"), OutputPinId, InputPinId, GraphPath, TEXT("PCG output pin is connected to this input pin."), OutRelations, EdgeAttributes);

				const FString EdgeId = MakeRelationId(ProjectId, TEXT("pcg_edge"), OutputPinId, InputPinId, &EdgeAttributes);
				const TSharedRef<FJsonObject> EdgeObject = PCGEdgeObject(EdgeId, EdgeCount++, *EdgeOutputPin, *EdgeInputPin, OutputPinId, InputPinId, NodeIds);
				EdgeValues.Add(MakeShared<FJsonValueObject>(EdgeObject));
				UniversalLinkValues.Add(MakeShared<FJsonValueObject>(UniversalLinkObject(EdgeObject)));
			}
		}
	}

	TSharedRef<FJsonObject> GraphObject = MakeShared<FJsonObject>();
	GraphObject->SetStringField(TEXT("id"), GraphId);
	GraphObject->SetStringField(TEXT("path"), GraphPath);
	GraphObject->SetStringField(TEXT("class"), Graph.GetClass()->GetPathName());
	GraphObject->SetStringField(TEXT("name"), Graph.GetName());

	TSharedRef<FJsonObject> DomainSemantics = MakeShared<FJsonObject>();
	DomainSemantics->SetStringField(TEXT("world_actor_resolution_state"), TEXT("not_resolved_static_scan"));
	DomainSemantics->SetBoolField(TEXT("hierarchical_generation"), Graph.IsHierarchicalGenerationEnabled());
	DomainSemantics->SetBoolField(TEXT("landscape_uses_metadata"), Graph.bLandscapeUsesMetadata);
	DomainSemantics->SetStringField(TEXT("default_grid"), Graph.IsHierarchicalGenerationEnabled()
		? PCGEnumValue(StaticEnum<EPCGHiGenGrid>(), static_cast<int64>(Graph.GetDefaultGrid()))
		: FString());
	DomainSemantics->SetNumberField(TEXT("default_grid_size"), Graph.IsHierarchicalGenerationEnabled() ? static_cast<double>(Graph.GetDefaultGridSize()) : 0.0);
	DomainSemantics->SetNumberField(TEXT("node_count"), NodeValues.Num());
	DomainSemantics->SetNumberField(TEXT("pin_count"), PinValues.Num());
	DomainSemantics->SetNumberField(TEXT("edge_count"), EdgeValues.Num());
	DomainSemantics->SetNumberField(TEXT("settings_count"), SettingsValues.Num());
	DomainSemantics->SetNumberField(TEXT("subgraph_reference_count"), SubgraphValues.Num());
	DomainSemantics->SetStringField(TEXT("user_parameter_state"), TEXT("not_expanded_static_scan"));

	TSharedRef<FJsonObject> UniversalGraph = MakeShared<FJsonObject>();
	UniversalGraph->SetStringField(TEXT("domain"), TEXT("pcg"));
	UniversalGraph->SetObjectField(TEXT("graph"), GraphObject);
	UniversalGraph->SetArrayField(TEXT("nodes"), UniversalNodeValues);
	UniversalGraph->SetArrayField(TEXT("ports"), UniversalPortValues);
	UniversalGraph->SetArrayField(TEXT("links"), UniversalLinkValues);
	UniversalGraph->SetObjectField(TEXT("domain_semantics"), DomainSemantics);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.pcg_graph.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("graph_path"), GraphPath);
	Object->SetStringField(TEXT("graph_class"), Graph.GetClass()->GetPathName());
	Object->SetStringField(TEXT("world_actor_resolution_state"), TEXT("not_resolved_static_scan"));
	Object->SetBoolField(TEXT("hierarchical_generation"), Graph.IsHierarchicalGenerationEnabled());
	Object->SetBoolField(TEXT("landscape_uses_metadata"), Graph.bLandscapeUsesMetadata);
	Object->SetStringField(TEXT("default_grid"), Graph.IsHierarchicalGenerationEnabled()
		? PCGEnumValue(StaticEnum<EPCGHiGenGrid>(), static_cast<int64>(Graph.GetDefaultGrid()))
		: FString());
	Object->SetNumberField(TEXT("default_grid_size"), Graph.IsHierarchicalGenerationEnabled() ? static_cast<double>(Graph.GetDefaultGridSize()) : 0.0);
	Object->SetNumberField(TEXT("node_count"), NodeValues.Num());
	Object->SetNumberField(TEXT("pin_count"), PinValues.Num());
	Object->SetNumberField(TEXT("edge_count"), EdgeValues.Num());
	Object->SetNumberField(TEXT("settings_count"), SettingsCount);
	Object->SetNumberField(TEXT("subgraph_reference_count"), SubgraphCount);
	Object->SetStringField(TEXT("user_parameter_state"), TEXT("not_expanded_static_scan"));
	Object->SetArrayField(TEXT("nodes"), NodeValues);
	Object->SetArrayField(TEXT("pins"), PinValues);
	Object->SetArrayField(TEXT("edges"), EdgeValues);
	Object->SetArrayField(TEXT("settings"), SettingsValues);
	Object->SetArrayField(TEXT("subgraphs"), SubgraphValues);
	Object->SetObjectField(TEXT("universal_graph"), UniversalGraph);
	return Object;
}
}

bool FPCGReader::AppendPCGAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	UPCGGraph* Graph = Cast<UPCGGraph>(&Asset);
	if (!Graph)
	{
		if (UPCGGraphInterface* GraphInterface = Cast<UPCGGraphInterface>(&Asset))
		{
			Graph = GraphInterface->GetGraph();
		}
	}

	if (!Graph)
	{
		return false;
	}

	const FString GraphId = AddPCGGraphEntity(ProjectId, *Graph, OutEntities);
	AddPCGRelation(ProjectId, TEXT("contains_pcg_graph"), AssetEntity.Id, GraphId, Asset.GetPathName(), TEXT("Asset contains the extracted PCG graph record."), OutRelations);

	if (!AssetEntity.Snapshot.IsValid())
	{
		AssetEntity.Snapshot = MakeShared<FJsonObject>();
	}
	AssetEntity.Snapshot->SetObjectField(TEXT("pcg_graph"), PCGGraphSnapshot(ProjectId, *Graph, GraphId, OutEntities, OutRelations));
	AssetEntity.Attributes.Add(TEXT("pcg_node_count"), FString::FromInt(Graph->GetNodes().Num()));
	AssetEntity.Attributes.Add(TEXT("pcg_hierarchical_generation"), PCGBool(Graph->IsHierarchicalGenerationEnabled()));
	AssetEntity.Completeness.State = ECompletenessState::Partial;
	AssetEntity.Completeness.Covered.AddUnique(TEXT("pcg_graph"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("pcg_nodes"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("pcg_pins"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("pcg_edges"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("universal_graph_ir"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("pcg_generated_results"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("pcg_runtime_world_actor_resolution"));
	AssetEntity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Asset.GetPathName(),
		TEXT("UPCGGraph static structure extracted through public PCG runtime API.")
	});
	return true;
}
}
