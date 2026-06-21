#include "UEPIMetaSoundReader.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/MetasoundOutputFormatInterfaces.h"
#include "Metasound.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundSource.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString MetaSoundGuidString(const FGuid& Guid)
{
	return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString();
}

FString MetaSoundBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString MetaSoundEnumValue(const UEnum* Enum, int64 Value)
{
	return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(static_cast<int32>(Value));
}

FString TruncateMetaSoundValue(FString Value)
{
	constexpr int32 MaxLength = 512;
	if (Value.Len() > MaxLength)
	{
		Value.LeftInline(MaxLength);
		Value.Append(TEXT("..."));
	}
	return Value;
}

FString MetaSoundLiteralTypeString(EMetasoundFrontendLiteralType Type)
{
	return MetaSoundEnumValue(StaticEnum<EMetasoundFrontendLiteralType>(), static_cast<int64>(Type));
}

FString MetaSoundLiteralValueString(const FMetasoundFrontendLiteral& Literal)
{
	return TruncateMetaSoundValue(Literal.ToString());
}

FString MetaSoundVersionNumberString(const FMetasoundFrontendVersionNumber& Version)
{
	return FString::Printf(TEXT("%d.%d"), Version.Major, Version.Minor);
}

FString MetaSoundVersionString(const FMetasoundFrontendVersion& Version)
{
	return FString::Printf(TEXT("%s %s"), *Version.Name.ToString(), *MetaSoundVersionNumberString(Version.Number));
}

FString MetaSoundClassNameString(const FMetasoundFrontendClassName& ClassName)
{
	return ClassName.ToString();
}

FString MetaSoundClassTypeString(EMetasoundFrontendClassType Type)
{
	return ::LexToString(Type);
}

FString MetaSoundVertexAccessString(EMetasoundFrontendVertexAccessType AccessType)
{
	return ::LexToString(AccessType);
}

FString MetaSoundSourceLayer()
{
	return LexToString(ESourceLayer::EditorSourceGraph);
}

void AddMetaSoundEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		MetaSoundSourceLayer(),
		Path,
		Detail
	});
}

void AddMetaSoundRelation(
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
	Relation.SourceLayer = MetaSoundSourceLayer();
	Relation.bDerived = false;
	Relation.Confidence = 1.0f;
	Relation.Attributes = Attributes;
	Relation.Evidence.Add({
		MetaSoundSourceLayer(),
		EvidencePath,
		EvidenceDetail
	});
	OutRelations.Add(MoveTemp(Relation));
}

TSharedRef<FJsonObject> MetaSoundVersionObject(int32 Index, const FMetasoundFrontendVersion& Version)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("name"), Version.Name.ToString());
	Object->SetNumberField(TEXT("major"), Version.Number.Major);
	Object->SetNumberField(TEXT("minor"), Version.Number.Minor);
	Object->SetStringField(TEXT("version"), MetaSoundVersionNumberString(Version.Number));
	return Object;
}

TSharedRef<FJsonObject> MetaSoundClassMetadataObject(const FMetasoundFrontendClassMetadata& Metadata)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("class_name"), MetaSoundClassNameString(Metadata.GetClassName()));
	Object->SetStringField(TEXT("class_type"), MetaSoundClassTypeString(Metadata.GetType()));
	Object->SetNumberField(TEXT("version_major"), Metadata.GetVersion().Major);
	Object->SetNumberField(TEXT("version_minor"), Metadata.GetVersion().Minor);
	Object->SetBoolField(TEXT("deprecated"), Metadata.GetIsDeprecated());
#if WITH_EDITOR
	Object->SetStringField(TEXT("display_name"), Metadata.GetDisplayName().ToString());
	Object->SetStringField(TEXT("description"), Metadata.GetDescription().ToString());
	Object->SetStringField(TEXT("author"), Metadata.GetAuthor());
#else
	Object->SetStringField(TEXT("display_name"), MetaSoundClassNameString(Metadata.GetClassName()));
	Object->SetStringField(TEXT("description"), FString());
	Object->SetStringField(TEXT("author"), FString());
#endif
	return Object;
}

TSharedRef<FJsonObject> MetaSoundClassVertexObject(
	const FMetasoundFrontendClassVertex& Vertex,
	int32 Index,
	const FString& Direction,
	const FMetasoundFrontendLiteral* DefaultLiteral = nullptr)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("name"), Vertex.Name.ToString());
	Object->SetStringField(TEXT("type_name"), Vertex.TypeName.ToString());
	Object->SetStringField(TEXT("vertex_id"), MetaSoundGuidString(Vertex.VertexID));
	Object->SetStringField(TEXT("node_id"), MetaSoundGuidString(Vertex.NodeID));
	Object->SetStringField(TEXT("direction"), Direction);
	Object->SetStringField(TEXT("access_type"), MetaSoundVertexAccessString(Vertex.AccessType));
	if (DefaultLiteral)
	{
		Object->SetStringField(TEXT("default_literal_type"), MetaSoundLiteralTypeString(DefaultLiteral->GetType()));
		Object->SetStringField(TEXT("default_literal_value"), MetaSoundLiteralValueString(*DefaultLiteral));
	}
	return Object;
}

TSharedRef<FJsonObject> MetaSoundNodeVertexObject(
	const FMetasoundFrontendVertex& Vertex,
	const FString& VertexId,
	const FString& NodeId,
	int32 Index,
	const FString& Direction)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), VertexId);
	Object->SetNumberField(TEXT("index"), Index);
	Object->SetStringField(TEXT("node_id"), NodeId);
	Object->SetStringField(TEXT("direction"), Direction);
	Object->SetStringField(TEXT("name"), Vertex.Name.ToString());
	Object->SetStringField(TEXT("type_name"), Vertex.TypeName.ToString());
	Object->SetStringField(TEXT("vertex_id"), MetaSoundGuidString(Vertex.VertexID));
	return Object;
}

FString MetaSoundVertexHandleKey(const FGuid& NodeId, const FGuid& VertexId, const FString& Direction)
{
	return MetaSoundGuidString(NodeId) + TEXT("|") + MetaSoundGuidString(VertexId) + TEXT("|") + Direction;
}

const FMetasoundFrontendClass* FindMetaSoundClassById(const FMetasoundFrontendDocument& Document, const FGuid& ClassId)
{
	if (Document.RootGraph.ID == ClassId)
	{
		return &Document.RootGraph;
	}

	for (const FMetasoundFrontendGraphClass& Subgraph : Document.Subgraphs)
	{
		if (Subgraph.ID == ClassId)
		{
			return &Subgraph;
		}
	}

	for (const FMetasoundFrontendClass& Dependency : Document.Dependencies)
	{
		if (Dependency.ID == ClassId)
		{
			return &Dependency;
		}
	}

	return nullptr;
}

FEntityRecord MakeMetaSoundEntity(
	const FString& ProjectId,
	const FString& Kind,
	const FString& CanonicalKey,
	const FString& DisplayName,
	const FString& EvidencePath,
	const FString& EvidenceDetail)
{
	FEntityRecord Entity;
	Entity.Id = MakeStableId(ProjectId, Kind, CanonicalKey);
	Entity.Kind = Kind;
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = DisplayName;
	Entity.SourceLayer = MetaSoundSourceLayer();
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Complete;
	Entity.Completeness.Covered = { TEXT("metasound_frontend_document") };
	AddMetaSoundEvidence(Entity, EvidencePath, EvidenceDetail);
	return Entity;
}

void AddUniversalNode(
	TArray<TSharedPtr<FJsonValue>>& UniversalNodes,
	const FString& NodeId,
	const FString& Name,
	const FString& ClassName,
	const FString& SemanticKind)
{
	TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), NodeId);
	Node->SetStringField(TEXT("name"), Name);
	Node->SetStringField(TEXT("class"), ClassName);
	Node->SetStringField(TEXT("semantic_kind"), SemanticKind);
	Node->SetNumberField(TEXT("x"), 0.0);
	Node->SetNumberField(TEXT("y"), 0.0);
	UniversalNodes.Add(MakeShared<FJsonValueObject>(Node));
}

void AddUniversalPort(
	TArray<TSharedPtr<FJsonValue>>& UniversalPorts,
	const FString& PortId,
	const FString& NodeId,
	const FString& Name,
	const FString& Direction,
	const FString& TypeName)
{
	TSharedRef<FJsonObject> Port = MakeShared<FJsonObject>();
	Port->SetStringField(TEXT("id"), PortId);
	Port->SetStringField(TEXT("node_id"), NodeId);
	Port->SetStringField(TEXT("name"), Name);
	Port->SetStringField(TEXT("direction"), Direction);
	Port->SetStringField(TEXT("type"), TypeName);
	Port->SetStringField(TEXT("domain_type"), TypeName);
	UniversalPorts.Add(MakeShared<FJsonValueObject>(Port));
}

void AddUniversalLink(
	TArray<TSharedPtr<FJsonValue>>& UniversalLinks,
	const FString& LinkId,
	const FString& SourcePortId,
	const FString& TargetPortId)
{
	if (SourcePortId.IsEmpty() || TargetPortId.IsEmpty())
	{
		return;
	}

	TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
	Link->SetStringField(TEXT("id"), LinkId);
	Link->SetStringField(TEXT("source_port_id"), SourcePortId);
	Link->SetStringField(TEXT("target_port_id"), TargetPortId);
	UniversalLinks.Add(MakeShared<FJsonValueObject>(Link));
}

TSharedRef<FJsonObject> MetaSoundUniversalGraphObject(
	const FString& DocumentId,
	const UObject& Asset,
	const FMetasoundFrontendDocument& Document,
	const TArray<TSharedPtr<FJsonValue>>& UniversalNodes,
	const TArray<TSharedPtr<FJsonValue>>& UniversalPorts,
	const TArray<TSharedPtr<FJsonValue>>& UniversalLinks,
	int32 GraphCount,
	int32 NodeCount,
	int32 VertexCount,
	int32 EdgeCount,
	int32 DependencyCount)
{
	TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
	Graph->SetStringField(TEXT("id"), DocumentId);
	Graph->SetStringField(TEXT("path"), Asset.GetPathName());
	Graph->SetStringField(TEXT("class"), Asset.GetClass() ? Asset.GetClass()->GetPathName() : FString());
	Graph->SetStringField(TEXT("name"), MetaSoundClassNameString(Document.RootGraph.Metadata.GetClassName()));

	TSharedRef<FJsonObject> DomainSemantics = MakeShared<FJsonObject>();
	DomainSemantics->SetNumberField(TEXT("graph_count"), GraphCount);
	DomainSemantics->SetNumberField(TEXT("node_count"), NodeCount);
	DomainSemantics->SetNumberField(TEXT("vertex_count"), VertexCount);
	DomainSemantics->SetNumberField(TEXT("edge_count"), EdgeCount);
	DomainSemantics->SetNumberField(TEXT("dependency_count"), DependencyCount);
	DomainSemantics->SetBoolField(TEXT("preset"), Document.RootGraph.PresetOptions.bIsPreset);
	DomainSemantics->SetStringField(TEXT("runtime_evaluation_state"), TEXT("not_evaluated_static_scan"));

	TSharedRef<FJsonObject> UniversalGraph = MakeShared<FJsonObject>();
	UniversalGraph->SetStringField(TEXT("domain"), TEXT("metasound"));
	UniversalGraph->SetObjectField(TEXT("graph"), Graph);
	UniversalGraph->SetArrayField(TEXT("nodes"), UniversalNodes);
	UniversalGraph->SetArrayField(TEXT("ports"), UniversalPorts);
	UniversalGraph->SetArrayField(TEXT("links"), UniversalLinks);
	UniversalGraph->SetObjectField(TEXT("domain_semantics"), DomainSemantics);
	return UniversalGraph;
}

TSharedRef<FJsonObject> MetaSoundSnapshot(
	const UObject& Asset,
	const IMetaSoundDocumentInterface& DocumentInterface,
	const FString& ProjectId,
	const FString& DocumentId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const FMetasoundFrontendDocument& Document = DocumentInterface.GetDocument();
	const FString AssetPath = Asset.GetPathName();

	TMap<FString, FString> DependencyClassIdToEntityId;
	TMap<FString, FString> NodeGuidToEntityId;
	TMap<FString, FString> VertexHandleToEntityId;

	TArray<TSharedPtr<FJsonValue>> InterfaceValues;
	TArray<FMetasoundFrontendVersion> Interfaces = Document.Interfaces.Array();
	Interfaces.Sort([](const FMetasoundFrontendVersion& Left, const FMetasoundFrontendVersion& Right)
	{
		return MetaSoundVersionString(Left) < MetaSoundVersionString(Right);
	});
	for (int32 InterfaceIndex = 0; InterfaceIndex < Interfaces.Num(); ++InterfaceIndex)
	{
		InterfaceValues.Add(MakeShared<FJsonValueObject>(MetaSoundVersionObject(InterfaceIndex, Interfaces[InterfaceIndex])));
	}

	TArray<TSharedPtr<FJsonValue>> GraphValues;
	TArray<TSharedPtr<FJsonValue>> NodeValues;
	TArray<TSharedPtr<FJsonValue>> VertexValues;
	TArray<TSharedPtr<FJsonValue>> EdgeValues;
	TArray<TSharedPtr<FJsonValue>> LiteralValues;
	TArray<TSharedPtr<FJsonValue>> DependencyValues;
	TArray<TSharedPtr<FJsonValue>> UniversalNodes;
	TArray<TSharedPtr<FJsonValue>> UniversalPorts;
	TArray<TSharedPtr<FJsonValue>> UniversalLinks;

	for (int32 DependencyIndex = 0; DependencyIndex < Document.Dependencies.Num(); ++DependencyIndex)
	{
		const FMetasoundFrontendClass& Dependency = Document.Dependencies[DependencyIndex];
		const FString ClassGuid = MetaSoundGuidString(Dependency.ID);
		const FString ClassName = MetaSoundClassNameString(Dependency.Metadata.GetClassName());
		const FString DependencyKey = AssetPath + TEXT(":dependency:") + (ClassGuid.IsEmpty() ? ClassName : ClassGuid);
		FEntityRecord DependencyEntity = MakeMetaSoundEntity(
			ProjectId,
			TEXT("metasound_dependency_class"),
			DependencyKey,
			ClassName,
			AssetPath,
			TEXT("MetaSound frontend dependency class referenced by the document."));
		DependencyEntity.Attributes.Add(TEXT("class_id"), ClassGuid);
		DependencyEntity.Attributes.Add(TEXT("class_name"), ClassName);
		DependencyEntity.Attributes.Add(TEXT("class_type"), MetaSoundClassTypeString(Dependency.Metadata.GetType()));
		DependencyClassIdToEntityId.Add(ClassGuid, DependencyEntity.Id);

		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("id"), DependencyEntity.Id);
		Object->SetNumberField(TEXT("index"), DependencyIndex);
		Object->SetStringField(TEXT("class_id"), ClassGuid);
		Object->SetObjectField(TEXT("metadata"), MetaSoundClassMetadataObject(Dependency.Metadata));
		Object->SetNumberField(TEXT("input_count"), Dependency.Interface.Inputs.Num());
		Object->SetNumberField(TEXT("output_count"), Dependency.Interface.Outputs.Num());
		Object->SetNumberField(TEXT("environment_count"), Dependency.Interface.Environment.Num());
		DependencyValues.Add(MakeShared<FJsonValueObject>(Object));
		OutEntities.Add(MoveTemp(DependencyEntity));
	}

	const auto ExtractGraph = [&](
		const FMetasoundFrontendGraphClass& GraphClass,
		int32 GraphIndex,
		const FString& Role)
	{
		const FString GraphGuid = MetaSoundGuidString(GraphClass.ID);
		const FString GraphName = MetaSoundClassNameString(GraphClass.Metadata.GetClassName());
		const FString GraphKey = AssetPath + TEXT(":graph:") + Role + TEXT(":") + (GraphGuid.IsEmpty() ? FString::FromInt(GraphIndex) : GraphGuid);
		FEntityRecord GraphEntity = MakeMetaSoundEntity(
			ProjectId,
			TEXT("metasound_graph"),
			GraphKey,
			GraphName,
			AssetPath,
			TEXT("MetaSound frontend graph class extracted from the serialized document."));
		GraphEntity.Attributes.Add(TEXT("role"), Role);
		GraphEntity.Attributes.Add(TEXT("graph_class_id"), GraphGuid);
		GraphEntity.Attributes.Add(TEXT("class_name"), GraphName);
		GraphEntity.Attributes.Add(TEXT("preset"), MetaSoundBool(GraphClass.PresetOptions.bIsPreset));
		AddMetaSoundRelation(ProjectId, TEXT("contains_metasound_graph"), DocumentId, GraphEntity.Id, AssetPath, TEXT("MetaSound document contains frontend graph."), OutRelations);

		TArray<TSharedPtr<FJsonValue>> Inputs;
		for (int32 InputIndex = 0; InputIndex < GraphClass.Interface.Inputs.Num(); ++InputIndex)
		{
			const FMetasoundFrontendClassInput& Input = GraphClass.Interface.Inputs[InputIndex];
			Inputs.Add(MakeShared<FJsonValueObject>(MetaSoundClassVertexObject(Input, InputIndex, TEXT("input"), &Input.DefaultLiteral)));
		}

		TArray<TSharedPtr<FJsonValue>> Outputs;
		for (int32 OutputIndex = 0; OutputIndex < GraphClass.Interface.Outputs.Num(); ++OutputIndex)
		{
			Outputs.Add(MakeShared<FJsonValueObject>(MetaSoundClassVertexObject(GraphClass.Interface.Outputs[OutputIndex], OutputIndex, TEXT("output"))));
		}

		TArray<TSharedPtr<FJsonValue>> Environment;
		for (int32 EnvironmentIndex = 0; EnvironmentIndex < GraphClass.Interface.Environment.Num(); ++EnvironmentIndex)
		{
			const FMetasoundFrontendClassEnvironmentVariable& Variable = GraphClass.Interface.Environment[EnvironmentIndex];
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("index"), EnvironmentIndex);
			Object->SetStringField(TEXT("name"), Variable.Name.ToString());
			Object->SetStringField(TEXT("type_name"), Variable.TypeName.ToString());
			Object->SetBoolField(TEXT("required"), Variable.bIsRequired);
			Environment.Add(MakeShared<FJsonValueObject>(Object));
		}

		TSharedRef<FJsonObject> GraphObject = MakeShared<FJsonObject>();
		GraphObject->SetStringField(TEXT("id"), GraphEntity.Id);
		GraphObject->SetNumberField(TEXT("index"), GraphIndex);
		GraphObject->SetStringField(TEXT("role"), Role);
		GraphObject->SetStringField(TEXT("graph_class_id"), GraphGuid);
		GraphObject->SetObjectField(TEXT("metadata"), MetaSoundClassMetadataObject(GraphClass.Metadata));
		GraphObject->SetBoolField(TEXT("preset"), GraphClass.PresetOptions.bIsPreset);
		GraphObject->SetNumberField(TEXT("input_count"), Inputs.Num());
		GraphObject->SetNumberField(TEXT("output_count"), Outputs.Num());
		GraphObject->SetNumberField(TEXT("environment_count"), Environment.Num());
		GraphObject->SetNumberField(TEXT("node_count"), GraphClass.Graph.Nodes.Num());
		GraphObject->SetNumberField(TEXT("edge_count"), GraphClass.Graph.Edges.Num());
		GraphObject->SetNumberField(TEXT("variable_count"), GraphClass.Graph.Variables.Num());
		GraphObject->SetArrayField(TEXT("inputs"), Inputs);
		GraphObject->SetArrayField(TEXT("outputs"), Outputs);
		GraphObject->SetArrayField(TEXT("environment"), Environment);
		GraphValues.Add(MakeShared<FJsonValueObject>(GraphObject));

		for (int32 NodeIndex = 0; NodeIndex < GraphClass.Graph.Nodes.Num(); ++NodeIndex)
		{
			const FMetasoundFrontendNode& Node = GraphClass.Graph.Nodes[NodeIndex];
			const FString RawNodeGuid = MetaSoundGuidString(Node.GetID());
			const FMetasoundFrontendClass* NodeClass = FindMetaSoundClassById(Document, Node.ClassID);
			const FString NodeClassName = NodeClass ? MetaSoundClassNameString(NodeClass->Metadata.GetClassName()) : FString();
			const FString NodeClassType = NodeClass ? MetaSoundClassTypeString(NodeClass->Metadata.GetType()) : FString();
			const FString NodeKey = GraphKey + TEXT(":node:") + (RawNodeGuid.IsEmpty() ? FString::FromInt(NodeIndex) : RawNodeGuid);

			FEntityRecord NodeEntity = MakeMetaSoundEntity(
				ProjectId,
				TEXT("metasound_node"),
				NodeKey,
				Node.Name.ToString(),
				AssetPath,
				TEXT("MetaSound frontend node instance extracted from graph."));
			NodeEntity.Attributes.Add(TEXT("node_guid"), RawNodeGuid);
			NodeEntity.Attributes.Add(TEXT("class_id"), MetaSoundGuidString(Node.ClassID));
			NodeEntity.Attributes.Add(TEXT("class_name"), NodeClassName);
			NodeEntity.Attributes.Add(TEXT("class_type"), NodeClassType);
			NodeGuidToEntityId.Add(RawNodeGuid, NodeEntity.Id);
			AddMetaSoundRelation(ProjectId, TEXT("contains_metasound_node"), GraphEntity.Id, NodeEntity.Id, AssetPath, TEXT("MetaSound graph contains frontend node."), OutRelations);

			if (const FString* DependencyEntityId = DependencyClassIdToEntityId.Find(MetaSoundGuidString(Node.ClassID)))
			{
				AddMetaSoundRelation(ProjectId, TEXT("metasound_node_uses_dependency"), NodeEntity.Id, *DependencyEntityId, AssetPath, TEXT("MetaSound node instantiates a dependency class."), OutRelations);
				if (GraphClass.PresetOptions.bIsPreset)
				{
					AddMetaSoundRelation(ProjectId, TEXT("metasound_preset_parent"), DocumentId, *DependencyEntityId, AssetPath, TEXT("MetaSound preset references parent dependency class."), OutRelations);
				}
			}

			TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
			NodeObject->SetStringField(TEXT("id"), NodeEntity.Id);
			NodeObject->SetNumberField(TEXT("index"), NodeIndex);
			NodeObject->SetStringField(TEXT("graph_id"), GraphEntity.Id);
			NodeObject->SetStringField(TEXT("node_guid"), RawNodeGuid);
			NodeObject->SetStringField(TEXT("node_name"), Node.Name.ToString());
			NodeObject->SetStringField(TEXT("class_id"), MetaSoundGuidString(Node.ClassID));
			NodeObject->SetStringField(TEXT("class_name"), NodeClassName);
			NodeObject->SetStringField(TEXT("class_type"), NodeClassType);
			NodeObject->SetNumberField(TEXT("input_count"), Node.Interface.Inputs.Num());
			NodeObject->SetNumberField(TEXT("output_count"), Node.Interface.Outputs.Num());
			NodeObject->SetNumberField(TEXT("environment_count"), Node.Interface.Environment.Num());
			NodeObject->SetNumberField(TEXT("input_literal_count"), Node.InputLiterals.Num());
			NodeValues.Add(MakeShared<FJsonValueObject>(NodeObject));
			AddUniversalNode(UniversalNodes, NodeEntity.Id, Node.Name.ToString(), NodeClassName, NodeClassType);

			for (int32 InputIndex = 0; InputIndex < Node.Interface.Inputs.Num(); ++InputIndex)
			{
				const FMetasoundFrontendVertex& Vertex = Node.Interface.Inputs[InputIndex];
				const FString VertexKey = NodeKey + TEXT(":input:") + MetaSoundGuidString(Vertex.VertexID);
				const FString VertexId = MakeStableId(ProjectId, TEXT("metasound_vertex"), VertexKey);
				VertexHandleToEntityId.Add(MetaSoundVertexHandleKey(Node.GetID(), Vertex.VertexID, TEXT("input")), VertexId);

				FEntityRecord VertexEntity = MakeMetaSoundEntity(ProjectId, TEXT("metasound_vertex"), VertexKey, Vertex.Name.ToString(), AssetPath, TEXT("MetaSound node input vertex."));
				VertexEntity.Id = VertexId;
				VertexEntity.Attributes.Add(TEXT("direction"), TEXT("input"));
				VertexEntity.Attributes.Add(TEXT("type_name"), Vertex.TypeName.ToString());
				AddMetaSoundRelation(ProjectId, TEXT("contains_metasound_vertex"), NodeEntity.Id, VertexEntity.Id, AssetPath, TEXT("MetaSound node contains input vertex."), OutRelations);
				OutEntities.Add(MoveTemp(VertexEntity));

				VertexValues.Add(MakeShared<FJsonValueObject>(MetaSoundNodeVertexObject(Vertex, VertexId, NodeEntity.Id, InputIndex, TEXT("input"))));
				AddUniversalPort(UniversalPorts, VertexId, NodeEntity.Id, Vertex.Name.ToString(), TEXT("input"), Vertex.TypeName.ToString());
			}

			for (int32 OutputIndex = 0; OutputIndex < Node.Interface.Outputs.Num(); ++OutputIndex)
			{
				const FMetasoundFrontendVertex& Vertex = Node.Interface.Outputs[OutputIndex];
				const FString VertexKey = NodeKey + TEXT(":output:") + MetaSoundGuidString(Vertex.VertexID);
				const FString VertexId = MakeStableId(ProjectId, TEXT("metasound_vertex"), VertexKey);
				VertexHandleToEntityId.Add(MetaSoundVertexHandleKey(Node.GetID(), Vertex.VertexID, TEXT("output")), VertexId);

				FEntityRecord VertexEntity = MakeMetaSoundEntity(ProjectId, TEXT("metasound_vertex"), VertexKey, Vertex.Name.ToString(), AssetPath, TEXT("MetaSound node output vertex."));
				VertexEntity.Id = VertexId;
				VertexEntity.Attributes.Add(TEXT("direction"), TEXT("output"));
				VertexEntity.Attributes.Add(TEXT("type_name"), Vertex.TypeName.ToString());
				AddMetaSoundRelation(ProjectId, TEXT("contains_metasound_vertex"), NodeEntity.Id, VertexEntity.Id, AssetPath, TEXT("MetaSound node contains output vertex."), OutRelations);
				OutEntities.Add(MoveTemp(VertexEntity));

				VertexValues.Add(MakeShared<FJsonValueObject>(MetaSoundNodeVertexObject(Vertex, VertexId, NodeEntity.Id, OutputIndex, TEXT("output"))));
				AddUniversalPort(UniversalPorts, VertexId, NodeEntity.Id, Vertex.Name.ToString(), TEXT("output"), Vertex.TypeName.ToString());
			}

			for (int32 LiteralIndex = 0; LiteralIndex < Node.InputLiterals.Num(); ++LiteralIndex)
			{
				const FMetasoundFrontendVertexLiteral& Literal = Node.InputLiterals[LiteralIndex];
				const FString LiteralKey = NodeKey + TEXT(":literal:") + MetaSoundGuidString(Literal.VertexID) + TEXT(":") + FString::FromInt(LiteralIndex);
				FEntityRecord LiteralEntity = MakeMetaSoundEntity(ProjectId, TEXT("metasound_literal"), LiteralKey, Literal.Value.ToString(), AssetPath, TEXT("MetaSound node input literal default."));
				LiteralEntity.Attributes.Add(TEXT("literal_type"), MetaSoundLiteralTypeString(Literal.Value.GetType()));
				AddMetaSoundRelation(ProjectId, TEXT("contains_metasound_literal"), NodeEntity.Id, LiteralEntity.Id, AssetPath, TEXT("MetaSound node contains input literal."), OutRelations);

				TSharedRef<FJsonObject> LiteralObject = MakeShared<FJsonObject>();
				LiteralObject->SetStringField(TEXT("id"), LiteralEntity.Id);
				LiteralObject->SetNumberField(TEXT("index"), LiteralValues.Num());
				LiteralObject->SetStringField(TEXT("graph_id"), GraphEntity.Id);
				LiteralObject->SetStringField(TEXT("node_id"), NodeEntity.Id);
				LiteralObject->SetStringField(TEXT("vertex_id"), MetaSoundGuidString(Literal.VertexID));
				LiteralObject->SetStringField(TEXT("literal_type"), MetaSoundLiteralTypeString(Literal.Value.GetType()));
				LiteralObject->SetStringField(TEXT("literal_value"), MetaSoundLiteralValueString(Literal.Value));
				LiteralValues.Add(MakeShared<FJsonValueObject>(LiteralObject));
				OutEntities.Add(MoveTemp(LiteralEntity));
			}

			OutEntities.Add(MoveTemp(NodeEntity));
		}

		for (int32 EdgeIndex = 0; EdgeIndex < GraphClass.Graph.Edges.Num(); ++EdgeIndex)
		{
			const FMetasoundFrontendEdge& Edge = GraphClass.Graph.Edges[EdgeIndex];
			const FString SourcePortId = VertexHandleToEntityId.FindRef(MetaSoundVertexHandleKey(Edge.FromNodeID, Edge.FromVertexID, TEXT("output")));
			const FString TargetPortId = VertexHandleToEntityId.FindRef(MetaSoundVertexHandleKey(Edge.ToNodeID, Edge.ToVertexID, TEXT("input")));
			const FString SourceNodeId = NodeGuidToEntityId.FindRef(MetaSoundGuidString(Edge.FromNodeID));
			const FString TargetNodeId = NodeGuidToEntityId.FindRef(MetaSoundGuidString(Edge.ToNodeID));
			const FString EdgeKey = GraphKey + TEXT(":edge:") + MetaSoundGuidString(Edge.FromNodeID) + TEXT(":") + MetaSoundGuidString(Edge.FromVertexID) + TEXT(":") + MetaSoundGuidString(Edge.ToNodeID) + TEXT(":") + MetaSoundGuidString(Edge.ToVertexID) + TEXT(":") + FString::FromInt(EdgeIndex);
			const FString EdgeId = MakeStableId(ProjectId, TEXT("metasound_edge"), EdgeKey);

			TSharedRef<FJsonObject> EdgeObject = MakeShared<FJsonObject>();
			EdgeObject->SetStringField(TEXT("id"), EdgeId);
			EdgeObject->SetNumberField(TEXT("index"), EdgeIndex);
			EdgeObject->SetStringField(TEXT("graph_id"), GraphEntity.Id);
			EdgeObject->SetStringField(TEXT("source_node_id"), SourceNodeId);
			EdgeObject->SetStringField(TEXT("source_vertex_id"), SourcePortId);
			EdgeObject->SetStringField(TEXT("target_node_id"), TargetNodeId);
			EdgeObject->SetStringField(TEXT("target_vertex_id"), TargetPortId);
			EdgeObject->SetStringField(TEXT("source_node_guid"), MetaSoundGuidString(Edge.FromNodeID));
			EdgeObject->SetStringField(TEXT("source_vertex_guid"), MetaSoundGuidString(Edge.FromVertexID));
			EdgeObject->SetStringField(TEXT("target_node_guid"), MetaSoundGuidString(Edge.ToNodeID));
			EdgeObject->SetStringField(TEXT("target_vertex_guid"), MetaSoundGuidString(Edge.ToVertexID));
			EdgeValues.Add(MakeShared<FJsonValueObject>(EdgeObject));

			TMap<FString, FString> Attributes;
			Attributes.Add(TEXT("graph_id"), GraphEntity.Id);
			Attributes.Add(TEXT("index"), FString::FromInt(EdgeIndex));
			AddMetaSoundRelation(ProjectId, TEXT("metasound_edge"), SourcePortId, TargetPortId, AssetPath, TEXT("MetaSound frontend edge connects output vertex to input vertex."), OutRelations, Attributes);
			AddUniversalLink(UniversalLinks, EdgeId, SourcePortId, TargetPortId);
		}

		OutEntities.Add(MoveTemp(GraphEntity));
	};

	ExtractGraph(Document.RootGraph, 0, TEXT("root"));
	for (int32 SubgraphIndex = 0; SubgraphIndex < Document.Subgraphs.Num(); ++SubgraphIndex)
	{
		ExtractGraph(Document.Subgraphs[SubgraphIndex], SubgraphIndex + 1, TEXT("subgraph"));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.metasound.v1"));
	Object->SetStringField(TEXT("source_layer"), MetaSoundSourceLayer());
	Object->SetStringField(TEXT("asset_path"), AssetPath);
	Object->SetStringField(TEXT("asset_class"), Asset.GetClass() ? Asset.GetClass()->GetPathName() : FString());
	Object->SetStringField(TEXT("base_metasound_class"), DocumentInterface.GetBaseMetaSoundUClass().GetPathName());
	Object->SetStringField(TEXT("asset_type"), Cast<UMetaSoundSource>(&Asset) ? TEXT("source") : TEXT("patch"));
	if (const UMetaSoundSource* Source = Cast<UMetaSoundSource>(&Asset))
	{
		Object->SetStringField(TEXT("output_audio_format"), MetaSoundEnumValue(StaticEnum<EMetaSoundOutputAudioFormat>(), static_cast<int64>(Source->OutputFormat)));
	}
	else
	{
		Object->SetStringField(TEXT("output_audio_format"), FString());
	}
	Object->SetStringField(TEXT("document_version_name"), Document.Metadata.Version.Name.ToString());
	Object->SetNumberField(TEXT("document_version_major"), Document.Metadata.Version.Number.Major);
	Object->SetNumberField(TEXT("document_version_minor"), Document.Metadata.Version.Number.Minor);
	Object->SetBoolField(TEXT("preset"), Document.RootGraph.PresetOptions.bIsPreset);
	Object->SetNumberField(TEXT("interface_count"), InterfaceValues.Num());
	Object->SetNumberField(TEXT("graph_count"), GraphValues.Num());
	Object->SetNumberField(TEXT("subgraph_count"), Document.Subgraphs.Num());
	Object->SetNumberField(TEXT("node_count"), NodeValues.Num());
	Object->SetNumberField(TEXT("vertex_count"), VertexValues.Num());
	Object->SetNumberField(TEXT("edge_count"), EdgeValues.Num());
	Object->SetNumberField(TEXT("literal_count"), LiteralValues.Num());
	Object->SetNumberField(TEXT("dependency_count"), DependencyValues.Num());
	Object->SetArrayField(TEXT("interfaces"), InterfaceValues);
	Object->SetArrayField(TEXT("graphs"), GraphValues);
	Object->SetArrayField(TEXT("nodes"), NodeValues);
	Object->SetArrayField(TEXT("vertices"), VertexValues);
	Object->SetArrayField(TEXT("edges"), EdgeValues);
	Object->SetArrayField(TEXT("literals"), LiteralValues);
	Object->SetArrayField(TEXT("dependencies"), DependencyValues);
	Object->SetObjectField(
		TEXT("universal_graph"),
		MetaSoundUniversalGraphObject(
			DocumentId,
			Asset,
			Document,
			UniversalNodes,
			UniversalPorts,
			UniversalLinks,
			GraphValues.Num(),
			NodeValues.Num(),
			VertexValues.Num(),
			EdgeValues.Num(),
			DependencyValues.Num()));
	return Object;
}
}

bool FMetaSoundReader::AppendMetaSoundAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const IMetaSoundDocumentInterface* DocumentInterface = Cast<IMetaSoundDocumentInterface>(&Asset);
	if (!DocumentInterface)
	{
		return false;
	}

	const FString AssetPath = Asset.GetPathName();
	const FString DocumentKey = AssetPath + TEXT(":metasound_document");
	FEntityRecord DocumentEntity = MakeMetaSoundEntity(
		ProjectId,
		TEXT("metasound_document"),
		DocumentKey,
		Asset.GetName(),
		AssetPath,
		TEXT("MetaSound frontend document extracted from a UMetaSound asset."));
	DocumentEntity.Attributes.Add(TEXT("asset_type"), Cast<UMetaSoundSource>(&Asset) ? TEXT("source") : TEXT("patch"));
	DocumentEntity.Attributes.Add(TEXT("preset"), MetaSoundBool(DocumentInterface->GetDocument().RootGraph.PresetOptions.bIsPreset));
	const FString DocumentId = DocumentEntity.Id;

	AddMetaSoundRelation(ProjectId, TEXT("contains_metasound_document"), AssetEntity.Id, DocumentId, AssetPath, TEXT("Asset contains serialized MetaSound frontend document."), OutRelations);

	if (!AssetEntity.Snapshot.IsValid())
	{
		AssetEntity.Snapshot = MakeShared<FJsonObject>();
	}
	AssetEntity.Snapshot->SetObjectField(TEXT("metasound"), MetaSoundSnapshot(Asset, *DocumentInterface, ProjectId, DocumentId, OutEntities, OutRelations));
	AssetEntity.Attributes.Add(TEXT("metasound_graph_count"), FString::FromInt(1 + DocumentInterface->GetDocument().Subgraphs.Num()));
	AssetEntity.Attributes.Add(TEXT("metasound_node_count"), FString::FromInt(DocumentInterface->GetDocument().RootGraph.Graph.Nodes.Num()));
	AssetEntity.Attributes.Add(TEXT("metasound_preset"), MetaSoundBool(DocumentInterface->GetDocument().RootGraph.PresetOptions.bIsPreset));
	AssetEntity.Completeness.State = ECompletenessState::Partial;
	AssetEntity.Completeness.Covered.AddUnique(TEXT("metasound_frontend_document"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("metasound_graphs"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("metasound_nodes"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("metasound_vertices"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("metasound_edges"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("metasound_dependencies"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("universal_graph_ir"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("metasound_runtime_operator_graph"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("metasound_dsp_evaluation"));
	AssetEntity.Evidence.Add({
		MetaSoundSourceLayer(),
		AssetPath,
		TEXT("FMetasoundFrontendDocument static structure extracted through IMetaSoundDocumentInterface.")
	});

	OutEntities.Add(MoveTemp(DocumentEntity));
	return true;
}
}
