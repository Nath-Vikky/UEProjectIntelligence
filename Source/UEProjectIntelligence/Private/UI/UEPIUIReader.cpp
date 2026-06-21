#include "UEPIUIReader.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "WidgetBlueprint.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString UIBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString BindingKindString(EBindingKind Kind)
{
	switch (Kind)
	{
	case EBindingKind::Function:
		return TEXT("function");
	case EBindingKind::Property:
		return TEXT("property");
	default:
		return TEXT("unknown");
	}
}

void AddUIEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		Path,
		Detail
	});
}

void AddUIRelation(
	const FString& ProjectId,
	const FString& Type,
	const FString& FromId,
	const FString& ToId,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FRelationRecord>& OutRelations)
{
	FRelationRecord Relation;
	Relation.Id = MakeRelationId(ProjectId, Type, FromId, ToId);
	Relation.Type = Type;
	Relation.FromId = FromId;
	Relation.ToId = ToId;
	Relation.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Relation.bDerived = false;
	Relation.Confidence = 1.0f;
	Relation.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		EvidencePath,
		EvidenceDetail
	});
	OutRelations.Add(MoveTemp(Relation));
}

FEntityRecord* FindUIEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

UWidgetBlueprintGeneratedClass* WidgetGeneratedClass(const UWidgetBlueprint& Blueprint)
{
	return Cast<UWidgetBlueprintGeneratedClass>(Blueprint.GeneratedClass);
}

UWidgetTree* WidgetTreeForBlueprint(const UWidgetBlueprint& Blueprint)
{
	if (UWidgetBlueprintGeneratedClass* GeneratedClass = WidgetGeneratedClass(Blueprint))
	{
		return GeneratedClass->GetWidgetTreeArchetype();
	}
	return nullptr;
}

TArray<UWidget*> WidgetTreeWidgets(const UWidgetTree& WidgetTree)
{
	TArray<UWidget*> Widgets;
	WidgetTree.GetAllWidgets(Widgets);
	Widgets.RemoveAll([](const UWidget* Widget) { return Widget == nullptr; });
	Widgets.Sort([](const UWidget& Left, const UWidget& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});
	return Widgets;
}

void BuildWidgetParentMap(UWidget* Parent, TMap<UWidget*, UWidget*>& OutParentByChild)
{
	if (!Parent)
	{
		return;
	}

	if (UPanelWidget* Panel = Cast<UPanelWidget>(Parent))
	{
		for (int32 ChildIndex = 0; ChildIndex < Panel->GetChildrenCount(); ++ChildIndex)
		{
			if (UWidget* Child = Panel->GetChildAt(ChildIndex))
			{
				OutParentByChild.Add(Child, Parent);
				BuildWidgetParentMap(Child, OutParentByChild);
			}
		}
	}
}

TMap<UWidget*, UWidget*> WidgetParentMap(const UWidgetTree& WidgetTree)
{
	TMap<UWidget*, UWidget*> ParentByChild;
	BuildWidgetParentMap(WidgetTree.RootWidget, ParentByChild);
	for (const TPair<FName, TObjectPtr<UWidget>>& NamedSlotBinding : WidgetTree.NamedSlotBindings)
	{
		BuildWidgetParentMap(NamedSlotBinding.Value, ParentByChild);
	}
	return ParentByChild;
}

FString AddWidgetBlueprintEntity(
	const FString& ProjectId,
	const UWidgetBlueprint& Blueprint,
	int32 WidgetCount,
	int32 AnimationCount,
	int32 BindingCount,
	int32 NamedSlotCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString BlueprintPath = Blueprint.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("widget_blueprint"), BlueprintPath);
	if (FindUIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("widget_blueprint");
	Entity.CanonicalKey = BlueprintPath;
	Entity.DisplayName = Blueprint.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes.Add(TEXT("widget_blueprint_path"), BlueprintPath);
	Entity.Attributes.Add(TEXT("generated_class_path"), Blueprint.GeneratedClass ? Blueprint.GeneratedClass->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("parent_class_path"), Blueprint.ParentClass ? Blueprint.ParentClass->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("widget_count"), FString::FromInt(WidgetCount));
	Entity.Attributes.Add(TEXT("animation_count"), FString::FromInt(AnimationCount));
	Entity.Attributes.Add(TEXT("binding_count"), FString::FromInt(BindingCount));
	Entity.Attributes.Add(TEXT("named_slot_count"), FString::FromInt(NamedSlotCount));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("widget_tree"), TEXT("widget_templates"), TEXT("widget_animations"), TEXT("widget_bindings") };
	Entity.Completeness.Omitted = { TEXT("runtime_slate_widgets"), TEXT("runtime_widget_state") };
	AddUIEvidence(Entity, BlueprintPath, TEXT("WidgetBlueprint static tree read from generated class widget tree archetype."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddWidgetEntity(
	const FString& ProjectId,
	const UWidgetBlueprint& Blueprint,
	const UWidget& Widget,
	int32 WidgetIndex,
	bool bIsRoot,
	int32 ChildCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = Widget.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("widget"), CanonicalKey);
	if (FindUIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("widget");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Widget.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes.Add(TEXT("widget_blueprint_path"), Blueprint.GetPathName());
	Entity.Attributes.Add(TEXT("widget_path"), Widget.GetPathName());
	Entity.Attributes.Add(TEXT("widget_name"), Widget.GetName());
	Entity.Attributes.Add(TEXT("widget_class"), Widget.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("widget_index"), FString::FromInt(WidgetIndex));
	Entity.Attributes.Add(TEXT("is_root"), UIBool(bIsRoot));
	Entity.Attributes.Add(TEXT("is_variable"), UIBool(Widget.bIsVariable));
	Entity.Attributes.Add(TEXT("child_count"), FString::FromInt(ChildCount));
	Entity.Attributes.Add(TEXT("visibility"), FString::FromInt(static_cast<int32>(Widget.GetVisibility())));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("widget_template_metadata"), TEXT("widget_tree_parentage") };
	Entity.Completeness.Omitted = { TEXT("runtime_slate_widget"), TEXT("runtime_widget_state") };
	AddUIEvidence(Entity, Blueprint.GetPathName(), TEXT("Widget template read from WidgetTree archetype."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddWidgetAnimationEntity(
	const FString& ProjectId,
	const UWidgetBlueprint& Blueprint,
	const UWidgetAnimation& Animation,
	int32 AnimationIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = Animation.GetPathName();
	const FString EntityId = MakeStableId(ProjectId, TEXT("widget_animation"), CanonicalKey);
	if (FindUIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("widget_animation");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Animation.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes.Add(TEXT("widget_blueprint_path"), Blueprint.GetPathName());
	Entity.Attributes.Add(TEXT("animation_path"), Animation.GetPathName());
	Entity.Attributes.Add(TEXT("animation_index"), FString::FromInt(AnimationIndex));
	Entity.Attributes.Add(TEXT("start_time"), FString::SanitizeFloat(Animation.GetStartTime()));
	Entity.Attributes.Add(TEXT("end_time"), FString::SanitizeFloat(Animation.GetEndTime()));
	Entity.Attributes.Add(TEXT("binding_count"), FString::FromInt(Animation.GetBindings().Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("widget_animation_metadata"), TEXT("widget_animation_bindings") };
	Entity.Completeness.Omitted = { TEXT("movie_scene_tracks"), TEXT("runtime_animation_state") };
	AddUIEvidence(Entity, Blueprint.GetPathName(), TEXT("Widget animation metadata read from WidgetBlueprint."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

FString AddWidgetBindingEntity(
	const FString& ProjectId,
	const UWidgetBlueprint& Blueprint,
	const FDelegateEditorBinding& Binding,
	int32 BindingIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = FString::Printf(TEXT("%s:binding:%d:%s:%s"), *Blueprint.GetPathName(), BindingIndex, *Binding.ObjectName, *Binding.PropertyName.ToString());
	const FString EntityId = MakeStableId(ProjectId, TEXT("widget_binding"), CanonicalKey);
	if (FindUIEntity(OutEntities, EntityId))
	{
		return EntityId;
	}

	FEntityRecord Entity;
	Entity.Id = EntityId;
	Entity.Kind = TEXT("widget_binding");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Binding.ObjectName + TEXT(".") + Binding.PropertyName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::EditorSourceGraph);
	Entity.Attributes.Add(TEXT("widget_blueprint_path"), Blueprint.GetPathName());
	Entity.Attributes.Add(TEXT("binding_index"), FString::FromInt(BindingIndex));
	Entity.Attributes.Add(TEXT("object_name"), Binding.ObjectName);
	Entity.Attributes.Add(TEXT("property_name"), Binding.PropertyName.ToString());
	Entity.Attributes.Add(TEXT("function_name"), Binding.FunctionName.ToString());
	Entity.Attributes.Add(TEXT("source_property"), Binding.SourceProperty.ToString());
	Entity.Attributes.Add(TEXT("binding_kind"), BindingKindString(Binding.Kind));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("widget_binding_metadata") };
	Entity.Completeness.Omitted = { TEXT("runtime_binding_evaluation") };
	AddUIEvidence(Entity, Blueprint.GetPathName(), TEXT("Widget binding metadata read from WidgetBlueprint editor bindings."));
	OutEntities.Add(MoveTemp(Entity));
	return EntityId;
}

TSharedRef<FJsonObject> WidgetSnapshotObject(
	const FString& WidgetId,
	const UWidget& Widget,
	const TMap<UWidget*, FString>& WidgetIds,
	const TMap<UWidget*, UWidget*>& ParentByChild,
	int32 WidgetIndex,
	bool bIsRoot)
{
	const UWidget* const* ParentWidgetPtr = ParentByChild.Find(const_cast<UWidget*>(&Widget));
	const UWidget* ParentWidget = ParentWidgetPtr ? *ParentWidgetPtr : nullptr;
	const FString* ParentId = ParentWidget ? WidgetIds.Find(const_cast<UWidget*>(ParentWidget)) : nullptr;
	const UPanelWidget* Panel = Cast<UPanelWidget>(&Widget);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), WidgetId);
	Object->SetNumberField(TEXT("index"), WidgetIndex);
	Object->SetStringField(TEXT("name"), Widget.GetName());
	Object->SetStringField(TEXT("path"), Widget.GetPathName());
	Object->SetStringField(TEXT("class"), Widget.GetClass()->GetPathName());
	Object->SetStringField(TEXT("parent_id"), ParentId ? *ParentId : FString());
	Object->SetStringField(TEXT("parent_name"), ParentWidget ? ParentWidget->GetName() : FString());
	Object->SetBoolField(TEXT("is_root"), bIsRoot);
	Object->SetBoolField(TEXT("is_variable"), Widget.bIsVariable);
	Object->SetNumberField(TEXT("child_count"), Panel ? Panel->GetChildrenCount() : 0);
	Object->SetStringField(TEXT("visibility"), FString::FromInt(static_cast<int32>(Widget.GetVisibility())));
	return Object;
}

TSharedRef<FJsonObject> WidgetAnimationSnapshotObject(const FString& AnimationId, const UWidgetAnimation& Animation, int32 AnimationIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), AnimationId);
	Object->SetNumberField(TEXT("index"), AnimationIndex);
	Object->SetStringField(TEXT("name"), Animation.GetName());
	Object->SetStringField(TEXT("path"), Animation.GetPathName());
	Object->SetNumberField(TEXT("start_time"), Animation.GetStartTime());
	Object->SetNumberField(TEXT("end_time"), Animation.GetEndTime());
	Object->SetNumberField(TEXT("binding_count"), Animation.GetBindings().Num());
	return Object;
}

TSharedRef<FJsonObject> WidgetBindingSnapshotObject(const FString& BindingId, const FDelegateEditorBinding& Binding, int32 BindingIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), BindingId);
	Object->SetNumberField(TEXT("index"), BindingIndex);
	Object->SetStringField(TEXT("object_name"), Binding.ObjectName);
	Object->SetStringField(TEXT("property_name"), Binding.PropertyName.ToString());
	Object->SetStringField(TEXT("function_name"), Binding.FunctionName.ToString());
	Object->SetStringField(TEXT("source_property"), Binding.SourceProperty.ToString());
	Object->SetStringField(TEXT("binding_kind"), BindingKindString(Binding.Kind));
	return Object;
}

TSharedRef<FJsonObject> WidgetBlueprintSnapshot(
	const FString& ProjectId,
	const UWidgetBlueprint& Blueprint,
	const FString& WidgetBlueprintId,
	UWidgetTree& WidgetTree,
	const TArray<UWidget*>& Widgets,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const TMap<UWidget*, UWidget*> ParentByChild = WidgetParentMap(WidgetTree);

	TMap<UWidget*, FString> WidgetIds;
	TArray<TSharedPtr<FJsonValue>> WidgetValues;
	for (int32 WidgetIndex = 0; WidgetIndex < Widgets.Num(); ++WidgetIndex)
	{
		UWidget* Widget = Widgets[WidgetIndex];
		if (!Widget)
		{
			continue;
		}

		const UPanelWidget* Panel = Cast<UPanelWidget>(Widget);
		const bool bIsRoot = Widget == WidgetTree.RootWidget;
		const FString WidgetId = AddWidgetEntity(ProjectId, Blueprint, *Widget, WidgetIndex, bIsRoot, Panel ? Panel->GetChildrenCount() : 0, OutEntities);
		WidgetIds.Add(Widget, WidgetId);
		AddUIRelation(ProjectId, TEXT("contains_widget"), WidgetBlueprintId, WidgetId, Blueprint.GetPathName(), TEXT("WidgetBlueprint contains this widget template."), OutRelations);
	}

	for (int32 WidgetIndex = 0; WidgetIndex < Widgets.Num(); ++WidgetIndex)
	{
		UWidget* Widget = Widgets[WidgetIndex];
		if (!Widget)
		{
			continue;
		}
		const FString* WidgetId = WidgetIds.Find(Widget);
		if (!WidgetId)
		{
			continue;
		}

		if (UWidget* const* ParentWidget = ParentByChild.Find(Widget))
		{
			if (const FString* ParentId = WidgetIds.Find(*ParentWidget))
			{
				AddUIRelation(ProjectId, TEXT("widget_parent"), *ParentId, *WidgetId, Blueprint.GetPathName(), TEXT("WidgetTree parent contains this child widget."), OutRelations);
			}
		}

		WidgetValues.Add(MakeShared<FJsonValueObject>(WidgetSnapshotObject(*WidgetId, *Widget, WidgetIds, ParentByChild, WidgetIndex, Widget == WidgetTree.RootWidget)));
	}

	TArray<TSharedPtr<FJsonValue>> AnimationValues;
	for (int32 AnimationIndex = 0; AnimationIndex < Blueprint.Animations.Num(); ++AnimationIndex)
	{
		UWidgetAnimation* Animation = Blueprint.Animations[AnimationIndex];
		if (!Animation)
		{
			continue;
		}

		const FString AnimationId = AddWidgetAnimationEntity(ProjectId, Blueprint, *Animation, AnimationIndex, OutEntities);
		AddUIRelation(ProjectId, TEXT("contains_widget_animation"), WidgetBlueprintId, AnimationId, Blueprint.GetPathName(), TEXT("WidgetBlueprint contains this widget animation."), OutRelations);
		AnimationValues.Add(MakeShared<FJsonValueObject>(WidgetAnimationSnapshotObject(AnimationId, *Animation, AnimationIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> BindingValues;
	for (int32 BindingIndex = 0; BindingIndex < Blueprint.Bindings.Num(); ++BindingIndex)
	{
		const FDelegateEditorBinding& Binding = Blueprint.Bindings[BindingIndex];
		const FString BindingId = AddWidgetBindingEntity(ProjectId, Blueprint, Binding, BindingIndex, OutEntities);
		AddUIRelation(ProjectId, TEXT("contains_widget_binding"), WidgetBlueprintId, BindingId, Blueprint.GetPathName(), TEXT("WidgetBlueprint contains this editor binding."), OutRelations);
		BindingValues.Add(MakeShared<FJsonValueObject>(WidgetBindingSnapshotObject(BindingId, Binding, BindingIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> NamedSlotValues;
	TArray<FName> NamedSlotNames;
	WidgetTree.NamedSlotBindings.GetKeys(NamedSlotNames);
	NamedSlotNames.Sort([](const FName& Left, const FName& Right)
	{
		return Left.ToString() < Right.ToString();
	});
	for (const FName& SlotName : NamedSlotNames)
	{
		TSharedRef<FJsonObject> SlotObject = MakeShared<FJsonObject>();
		SlotObject->SetStringField(TEXT("name"), SlotName.ToString());
		if (const TObjectPtr<UWidget>* ContentWidget = WidgetTree.NamedSlotBindings.Find(SlotName))
		{
			SlotObject->SetStringField(TEXT("content_widget_name"), *ContentWidget ? (*ContentWidget)->GetName() : FString());
		}
		else
		{
			SlotObject->SetStringField(TEXT("content_widget_name"), FString());
		}
		NamedSlotValues.Add(MakeShared<FJsonValueObject>(SlotObject));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.widget_blueprint.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::EditorSourceGraph));
	Object->SetStringField(TEXT("widget_blueprint_path"), Blueprint.GetPathName());
	Object->SetStringField(TEXT("generated_class_path"), Blueprint.GeneratedClass ? Blueprint.GeneratedClass->GetPathName() : FString());
	Object->SetStringField(TEXT("parent_class_path"), Blueprint.ParentClass ? Blueprint.ParentClass->GetPathName() : FString());
	Object->SetStringField(TEXT("root_widget_id"), WidgetTree.RootWidget && WidgetIds.Contains(WidgetTree.RootWidget) ? WidgetIds[WidgetTree.RootWidget] : FString());
	Object->SetStringField(TEXT("root_widget_name"), WidgetTree.RootWidget ? WidgetTree.RootWidget->GetName() : FString());
	Object->SetNumberField(TEXT("widget_count"), WidgetValues.Num());
	Object->SetNumberField(TEXT("animation_count"), AnimationValues.Num());
	Object->SetNumberField(TEXT("binding_count"), BindingValues.Num());
	Object->SetNumberField(TEXT("named_slot_count"), NamedSlotValues.Num());
	Object->SetArrayField(TEXT("widgets"), WidgetValues);
	Object->SetArrayField(TEXT("animations"), AnimationValues);
	Object->SetArrayField(TEXT("bindings"), BindingValues);
	Object->SetArrayField(TEXT("named_slots"), NamedSlotValues);
	return Object;
}
}

bool FUIReader::AppendUIAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(&Asset);
	if (!WidgetBlueprint)
	{
		return false;
	}

	UWidgetTree* WidgetTree = WidgetTreeForBlueprint(*WidgetBlueprint);
	if (!WidgetTree)
	{
		return false;
	}

	const TArray<UWidget*> Widgets = WidgetTreeWidgets(*WidgetTree);
	const FString WidgetBlueprintId = AddWidgetBlueprintEntity(
		ProjectId,
		*WidgetBlueprint,
		Widgets.Num(),
		WidgetBlueprint->Animations.Num(),
		WidgetBlueprint->Bindings.Num(),
		WidgetTree->NamedSlotBindings.Num(),
		OutEntities);
	AddUIRelation(ProjectId, TEXT("contains_widget_blueprint"), AssetEntity.Id, WidgetBlueprintId, WidgetBlueprint->GetPathName(), TEXT("Asset contains the extracted WidgetBlueprint record."), OutRelations);

	if (!AssetEntity.Snapshot.IsValid())
	{
		AssetEntity.Snapshot = MakeShared<FJsonObject>();
	}
	AssetEntity.Snapshot->SetObjectField(TEXT("widget_blueprint"), WidgetBlueprintSnapshot(ProjectId, *WidgetBlueprint, WidgetBlueprintId, *WidgetTree, Widgets, OutEntities, OutRelations));
	AssetEntity.Attributes.Add(TEXT("widget_count"), FString::FromInt(Widgets.Num()));
	AssetEntity.Attributes.Add(TEXT("widget_animation_count"), FString::FromInt(WidgetBlueprint->Animations.Num()));
	AssetEntity.Attributes.Add(TEXT("widget_binding_count"), FString::FromInt(WidgetBlueprint->Bindings.Num()));
	AssetEntity.Completeness.State = ECompletenessState::Partial;
	AssetEntity.Completeness.Covered.AddUnique(TEXT("widget_tree"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("widget_templates"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("widget_animations"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("widget_bindings"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_slate_widgets"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_widget_state"));
	AssetEntity.Evidence.Add({
		LexToString(ESourceLayer::EditorSourceGraph),
		WidgetBlueprint->GetPathName(),
		TEXT("WidgetBlueprint static tree extracted from generated class widget tree archetype.")
	});
	return true;
}
}
