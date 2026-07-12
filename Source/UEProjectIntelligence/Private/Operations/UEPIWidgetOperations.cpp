#include "Operations/UEPIWidgetOperations.h"

#include "AssetToolsModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IAssetTools.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UEPISettings.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace UE::ProjectIntelligence
{
	namespace WidgetOperationsPrivate
	{
		FString JsonString(const FJsonObject& Object, const TCHAR* Field, const FString& DefaultValue = FString()) { FString Value; return Object.TryGetStringField(Field, Value) ? Value : DefaultValue; }
		bool JsonBool(const FJsonObject& Object, const TCHAR* Field, bool DefaultValue = false) { bool Value = false; return Object.TryGetBoolField(Field, Value) ? Value : DefaultValue; }
		int32 JsonInt(const FJsonObject& Object, const TCHAR* Field, int32 DefaultValue = 0) { int32 Value = 0; return Object.TryGetNumberField(Field, Value) ? Value : DefaultValue; }
		FUEPIEditResult Failure(const FString& Code, const FString& Message) { FUEPIEditResult Result; Result.ErrorCode = Code; Result.Message = Message; return Result; }
		FUEPIEditResult Success(const FString& Message, const TSharedPtr<FJsonObject>& Detail = nullptr) { FUEPIEditResult Result; Result.bOk = true; Result.Message = Message; Result.Result = Detail; return Result; }

		FString ReferenceId(const FJsonObject& Params, const TCHAR* Field)
		{
			const TSharedPtr<FJsonObject>* Reference = nullptr; if (!Params.TryGetObjectField(Field, Reference) || !Reference || !Reference->IsValid()) return FString(); FString Ref = JsonString(**Reference, TEXT("$ref")); int32 Hash = INDEX_NONE; if (Ref.FindChar(TEXT('#'), Hash)) Ref = Ref.Left(Hash); return Ref;
		}

		FString ResolveAsset(const FUEPIEditContext& Context, const FJsonObject& Params)
		{
			FString Direct = JsonString(Params, TEXT("asset"), JsonString(Params, TEXT("widget"))); if (!Direct.IsEmpty()) return Direct; const FString Ref = ReferenceId(Params, TEXT("asset")); if (const FString* Resolved = Context.ResolvedAssets.Find(Ref)) return *Resolved; return FString();
		}

		FString NormalizeObjectPath(const FString& Raw) { if (Raw.Contains(TEXT("."))) return Raw; const FString Name = FPackageName::GetLongPackageAssetName(Raw); return Name.IsEmpty() ? Raw : Raw + TEXT(".") + Name; }

		bool SplitDestination(const FJsonObject& Params, FString& OutPath, FString& OutName, FString& OutError)
		{
			FString Destination = JsonString(Params, TEXT("destination"), JsonString(Params, TEXT("destination_asset"))); Destination.ReplaceInline(TEXT("\\"), TEXT("/")); if (!Destination.IsEmpty()) { FString Package = Destination; int32 Dot = INDEX_NONE; if (Package.FindChar(TEXT('.'), Dot)) Package = Package.Left(Dot); OutPath = FPackageName::GetLongPackagePath(Package); OutName = FPackageName::GetLongPackageAssetName(Package); }
			OutPath = JsonString(Params, TEXT("destination_path"), JsonString(Params, TEXT("folder"), OutPath)); OutPath.ReplaceInline(TEXT("\\"), TEXT("/")); while (OutPath.EndsWith(TEXT("/"))) OutPath.LeftChopInline(1); OutName = JsonString(Params, TEXT("name"), JsonString(Params, TEXT("asset_name"), OutName.IsEmpty() ? TEXT("WBP_UEPIWidget") : OutName)).TrimStartAndEnd();
			if ((!OutPath.Equals(TEXT("/Game")) && !OutPath.StartsWith(TEXT("/Game/"))) || OutPath.Contains(TEXT("..")) || OutName.IsEmpty() || OutName.Contains(TEXT("/")) || OutName.Contains(TEXT("."))) { OutError = TEXT("Widget destination must be a valid asset name under /Game."); return false; } return true;
		}

		bool Vector2(const TSharedPtr<FJsonObject>& Object, FVector2D& Out) { if (!Object.IsValid()) return false; double X = 0, Y = 0; if (!Object->TryGetNumberField(TEXT("x"), X) || !Object->TryGetNumberField(TEXT("y"), Y)) return false; Out = FVector2D(X, Y); return true; }
		bool Margin(const TSharedPtr<FJsonObject>& Object, FMargin& Out) { if (!Object.IsValid()) return false; double L=0,T=0,R=0,B=0; if (!Object->TryGetNumberField(TEXT("left"),L)||!Object->TryGetNumberField(TEXT("top"),T)||!Object->TryGetNumberField(TEXT("right"),R)||!Object->TryGetNumberField(TEXT("bottom"),B)) return false; Out=FMargin(L,T,R,B); return true; }
		TSharedPtr<FJsonObject> ObjectField(const FJsonObject& Object, const TCHAR* Field) { const TSharedPtr<FJsonObject>* Value=nullptr; return Object.TryGetObjectField(Field,Value)&&Value?*Value:nullptr; }

		UCanvasPanel* EnsureCanvas(UWidgetBlueprint* Blueprint)
		{
			if (!Blueprint) return nullptr; if (!Blueprint->WidgetTree) Blueprint->WidgetTree = NewObject<UWidgetTree>(Blueprint, UWidgetTree::StaticClass(), TEXT("WidgetTree"), RF_Transactional); if (!Blueprint->WidgetTree) return nullptr; if (UCanvasPanel* Canvas = Cast<UCanvasPanel>(Blueprint->WidgetTree->RootWidget)) return Canvas; if (Blueprint->WidgetTree->RootWidget) return nullptr; UCanvasPanel* Canvas = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas")); Blueprint->WidgetTree->RootWidget = Canvas; return Canvas;
		}

		UPanelWidget* ParentPanel(UWidgetBlueprint* Blueprint, const FJsonObject& Params)
		{
			if (!Blueprint || !Blueprint->WidgetTree) return nullptr; const FString Name = JsonString(Params, TEXT("parent"), JsonString(Params, TEXT("parent_widget"))); return Name.IsEmpty() ? Cast<UPanelWidget>(Blueprint->WidgetTree->RootWidget) : Blueprint->WidgetTree->FindWidget<UPanelWidget>(FName(*Name));
		}

		UPanelSlot* AddToParent(UPanelWidget* Parent, UWidget* Widget) { if (!Parent || !Widget) return nullptr; if (UCanvasPanel* Canvas = Cast<UCanvasPanel>(Parent)) return Canvas->AddChildToCanvas(Widget); return Parent->AddChild(Widget); }

		void ApplySlot(UWidget* Widget, const FJsonObject& Params)
		{
			UCanvasPanelSlot* Slot = Widget ? Cast<UCanvasPanelSlot>(Widget->Slot) : nullptr; if (!Slot) return; FVector2D Vector; if (Vector2(ObjectField(Params,TEXT("position")),Vector)) Slot->SetPosition(Vector); if (Vector2(ObjectField(Params,TEXT("size")),Vector)) Slot->SetSize(Vector); if (Vector2(ObjectField(Params,TEXT("alignment")),Vector)) Slot->SetAlignment(Vector);
			if (TSharedPtr<FJsonObject> Anchors=ObjectField(Params,TEXT("anchors"))) { FVector2D Min,Max; if (Vector2(ObjectField(*Anchors,TEXT("minimum")),Min)||Vector2(ObjectField(*Anchors,TEXT("min")),Min)) { if (!Vector2(ObjectField(*Anchors,TEXT("maximum")),Max)&&!Vector2(ObjectField(*Anchors,TEXT("max")),Max)) Max=Min; Slot->SetAnchors(FAnchors(Min.X,Min.Y,Max.X,Max.Y)); } }
			FMargin Offsets; if (Margin(ObjectField(Params,TEXT("offsets")),Offsets)) Slot->SetOffsets(Offsets); if (Params.HasField(TEXT("auto_size"))) Slot->SetAutoSize(JsonBool(Params,TEXT("auto_size"))); if (Params.HasField(TEXT("z_order"))) Slot->SetZOrder(JsonInt(Params,TEXT("z_order")));
		}

		TSharedRef<FJsonObject> WidgetJson(UWidget* Widget)
		{
			TSharedRef<FJsonObject> Result=MakeShared<FJsonObject>(); if (!Widget) return Result; Result->SetStringField(TEXT("name"),Widget->GetName()); Result->SetStringField(TEXT("path"),Widget->GetPathName()); Result->SetStringField(TEXT("class"),Widget->GetClass()->GetPathName()); if (UPanelSlot* Slot=Widget->Slot) { Result->SetStringField(TEXT("slot_class"),Slot->GetClass()->GetPathName()); if (Slot->Parent) Result->SetStringField(TEXT("parent"),Slot->Parent->GetName()); } return Result;
		}

		TSharedRef<FJsonObject> AssetJson(UObject* Object) { TSharedRef<FJsonObject> Result=MakeShared<FJsonObject>(); if (Object) { Result->SetStringField(TEXT("name"),Object->GetName()); Result->SetStringField(TEXT("path"),Object->GetPathName()); Result->SetStringField(TEXT("class"),Object->GetClass()->GetPathName()); } return Result; }

		FObjectProperty* WidgetProperty(UWidgetBlueprint* Blueprint, UWidget* Widget)
		{
			if (!Blueprint||!Widget) return nullptr; for (UClass* Class : {Blueprint->GeneratedClass,Blueprint->SkeletonGeneratedClass}) if (Class) { FObjectProperty* Property=FindFProperty<FObjectProperty>(Class,Widget->GetFName()); if (Property&&Property->PropertyClass&&Property->PropertyClass->IsChildOf(Widget->GetClass())) return Property; } return nullptr;
		}

		FString EffectiveType(const FString& Type, const FJsonObject& Params)
		{
			if (Type != TEXT("widget.add_widget")) return Type; const FString Class=JsonString(Params,TEXT("widget_class"),JsonString(Params,TEXT("kind"))); if (Class.Contains(TEXT("TextBlock"),ESearchCase::IgnoreCase)||Class.Equals(TEXT("text"),ESearchCase::IgnoreCase)) return TEXT("widget.add_text"); if (Class.Contains(TEXT("Button"),ESearchCase::IgnoreCase)||Class.Equals(TEXT("button"),ESearchCase::IgnoreCase)) return TEXT("widget.add_button"); return Type;
		}

		class FUEPIWidgetOperation final : public IUEPIEditOperation
		{
		public:
			explicit FUEPIWidgetOperation(FUEPIEditOperationDescriptor InDescriptor):Descriptor(MoveTemp(InDescriptor)){}
			virtual FString GetOperationType() const override{return Descriptor.Name;}
			virtual FUEPIEditOperationDescriptor GetDescriptor() const override{return Descriptor;}
			virtual FUEPIEditResult Validate(const FUEPIEditContext& Context,const FJsonObject& Params) override{return Preview(Context,Params);}
			virtual FUEPIEditResult Preview(const FUEPIEditContext& Context,const FJsonObject& Params) override
			{
				const UUEPISettings* Settings=GetDefault<UUEPISettings>(); if(!Settings||!Settings->bAllowUMGEdits) return Failure(TEXT("UEPI_EDIT_CAPABILITY_DISABLED"),TEXT("UMG edits are disabled in UEPI Project Settings.")); const FString Type=EffectiveType(Descriptor.Name,Params);
				if(Type==TEXT("widget.create")){FString Path,Name,Error;if(!SplitDestination(Params,Path,Name,Error))return Failure(TEXT("UEPI_EDIT_DESTINATION_INVALID"),Error);const FString ObjectPath=FString::Printf(TEXT("%s/%s.%s"),*Path,*Name,*Name);if(LoadObject<UObject>(nullptr,*ObjectPath))return Failure(TEXT("UEPI_EDIT_TARGET_ALREADY_EXISTS"),FString::Printf(TEXT("Widget Blueprint already exists: %s"),*ObjectPath));TSharedRef<FJsonObject> Detail=MakeShared<FJsonObject>();Detail->SetStringField(TEXT("asset_path"),ObjectPath);Detail->SetStringField(TEXT("asset_class"),UWidgetBlueprint::StaticClass()->GetPathName());return Success(TEXT("Widget Blueprint creation preflight passed."),Detail);}
				if(Type==TEXT("widget.add_widget"))return Failure(TEXT("UEPI_EDIT_WIDGET_CLASS_UNSUPPORTED"),TEXT("widget.add_widget supports TextBlock and Button in this release.")); const FString AssetPath=NormalizeObjectPath(ResolveAsset(Context,Params)); UWidgetBlueprint* Blueprint=LoadObject<UWidgetBlueprint>(nullptr,*AssetPath); const bool bPlanned=Context.ResolvedAssets.FindKey(AssetPath)!=nullptr; if(!Blueprint&&!bPlanned)return Failure(TEXT("UEPI_EDIT_WIDGET_BLUEPRINT_NOT_FOUND"),FString::Printf(TEXT("Widget Blueprint was not found: %s"),*AssetPath));
				if(!Blueprint)return Success(TEXT("Planned Widget Blueprint operation preflight passed.")); if(!Blueprint->WidgetTree)return Failure(TEXT("UEPI_EDIT_WIDGET_TREE_MISSING"),TEXT("Widget Blueprint has no WidgetTree."));
				if(Type==TEXT("widget.add_text")||Type==TEXT("widget.add_button")){UPanelWidget* Parent=ParentPanel(Blueprint,Params);if(!Parent)return Failure(TEXT("UEPI_EDIT_WIDGET_PARENT_NOT_FOUND"),TEXT("Widget parent panel was not found."));const FString Name=JsonString(Params,TEXT("name"),Type==TEXT("widget.add_text")?TEXT("UEPIText"):TEXT("UEPIButton"));if(Blueprint->WidgetTree->FindWidget(FName(*Name)))return Failure(TEXT("UEPI_EDIT_TARGET_ALREADY_EXISTS"),FString::Printf(TEXT("Widget already exists: %s"),*Name));return Success(TEXT("Widget insertion preflight passed."));}
				const FString Name=Type==TEXT("widget.bind_button_to_custom_event")?JsonString(Params,TEXT("button"),JsonString(Params,TEXT("button_name"),JsonString(Params,TEXT("widget_name"),JsonString(Params,TEXT("name"))))):JsonString(Params,TEXT("widget_name"),JsonString(Params,TEXT("name")));UWidget* Widget=Name.IsEmpty()?nullptr:Blueprint->WidgetTree->FindWidget(FName(*Name));if(!Widget)return Failure(TEXT("UEPI_EDIT_WIDGET_NOT_FOUND"),FString::Printf(TEXT("Widget was not found: %s"),*Name));
				if(Type==TEXT("widget.set_slot")&&!Cast<UCanvasPanelSlot>(Widget->Slot))return Failure(TEXT("UEPI_EDIT_WIDGET_SLOT_UNSUPPORTED"),TEXT("widget.set_slot currently requires CanvasPanelSlot."));if(Type==TEXT("widget.bind_button_to_custom_event")){UButton* Button=Cast<UButton>(Widget);const FName Delegate(*JsonString(Params,TEXT("delegate"),JsonString(Params,TEXT("event"),TEXT("OnClicked"))));if(!Button||Delegate.IsNone()||!FindFProperty<FMulticastDelegateProperty>(Button->GetClass(),Delegate))return Failure(TEXT("UEPI_EDIT_WIDGET_DELEGATE_INVALID"),TEXT("Button or multicast delegate is invalid."));}return Success(TEXT("Widget operation preflight passed."));
			}

			virtual FUEPIEditResult Apply(const FUEPIEditContext& Context,const FJsonObject& Params) override
			{
				FUEPIEditResult Check=Preview(Context,Params);if(!Check.bOk)return Check;const FString Type=EffectiveType(Descriptor.Name,Params);
				if(Type==TEXT("widget.create")){FString Path,Name,Error;SplitDestination(Params,Path,Name,Error);UWidgetBlueprintFactory* Factory=NewObject<UWidgetBlueprintFactory>();Factory->BlueprintType=BPTYPE_Normal;Factory->ParentClass=UUserWidget::StaticClass();UWidgetBlueprint* Blueprint=Cast<UWidgetBlueprint>(FAssetToolsModule::GetModule().Get().CreateAsset(Name,Path,UWidgetBlueprint::StaticClass(),Factory,TEXT("UEPI")));UCanvasPanel* Canvas=EnsureCanvas(Blueprint);if(!Blueprint||!Canvas)return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"),TEXT("Failed to create Widget Blueprint with Canvas root."));FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);TSharedRef<FJsonObject> Detail=MakeShared<FJsonObject>();Detail->SetObjectField(TEXT("asset"),AssetJson(Blueprint));Detail->SetObjectField(TEXT("root_widget"),WidgetJson(Canvas));return Success(TEXT("Widget Blueprint created with a CanvasPanel root."),Detail);}
				UWidgetBlueprint* Blueprint=LoadObject<UWidgetBlueprint>(nullptr,*NormalizeObjectPath(ResolveAsset(Context,Params)));if((Type==TEXT("widget.add_text")||Type==TEXT("widget.add_button"))&&!Blueprint->WidgetTree->RootWidget)EnsureCanvas(Blueprint);
				if(Type==TEXT("widget.add_text")||Type==TEXT("widget.add_button")){UPanelWidget* Parent=ParentPanel(Blueprint,Params);const FString Name=JsonString(Params,TEXT("name"),Type==TEXT("widget.add_text")?TEXT("UEPIText"):TEXT("UEPIButton"));Blueprint->Modify();Blueprint->WidgetTree->Modify();Parent->Modify();UWidget* Added=nullptr;UTextBlock* Label=nullptr;if(Type==TEXT("widget.add_text")){UTextBlock* Text=Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),FName(*Name));Text->SetText(FText::FromString(JsonString(Params,TEXT("text"),TEXT("Text"))));Added=Text;}else{UButton* Button=Blueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(),FName(*Name));const FString LabelText=JsonString(Params,TEXT("text"),JsonString(Params,TEXT("label")));if(!LabelText.IsEmpty()){Label=Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),FName(*(Name+TEXT("_Label"))));Label->SetText(FText::FromString(LabelText));Button->SetContent(Label);}Added=Button;}if(!Added||!AddToParent(Parent,Added))return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"),TEXT("Failed to add widget to parent panel."));ApplySlot(Added,Params);FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);TSharedRef<FJsonObject> Detail=MakeShared<FJsonObject>();Detail->SetStringField(TEXT("asset"),Blueprint->GetPathName());Detail->SetObjectField(TEXT("widget"),WidgetJson(Added));if(Label)Detail->SetObjectField(TEXT("label_widget"),WidgetJson(Label));return Success(Type==TEXT("widget.add_text")?TEXT("TextBlock widget added."):TEXT("Button widget added."),Detail);}
				const FString Name=Type==TEXT("widget.bind_button_to_custom_event")?JsonString(Params,TEXT("button"),JsonString(Params,TEXT("button_name"),JsonString(Params,TEXT("widget_name"),JsonString(Params,TEXT("name"))))):JsonString(Params,TEXT("widget_name"),JsonString(Params,TEXT("name")));UWidget* Widget=Blueprint->WidgetTree->FindWidget(FName(*Name));Blueprint->Modify();Widget->Modify();
				if(Type==TEXT("widget.set_slot")){Widget->Slot->Modify();ApplySlot(Widget,Params);FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);TSharedRef<FJsonObject> Detail=MakeShared<FJsonObject>();Detail->SetStringField(TEXT("asset"),Blueprint->GetPathName());Detail->SetObjectField(TEXT("widget"),WidgetJson(Widget));return Success(TEXT("Widget CanvasPanelSlot updated."),Detail);}
				UButton* Button=CastChecked<UButton>(Widget);Button->bIsVariable=true;FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);FObjectProperty* Property=WidgetProperty(Blueprint,Button);if(!Property){FKismetEditorUtilities::CompileBlueprint(Blueprint);Property=WidgetProperty(Blueprint,Button);}if(!Property)return Failure(TEXT("UEPI_EDIT_WIDGET_PROPERTY_MISSING"),TEXT("Generated widget variable property was not found after compile."));const FName Delegate(*JsonString(Params,TEXT("delegate"),JsonString(Params,TEXT("event"),TEXT("OnClicked"))));const UK2Node_ComponentBoundEvent* Node=FKismetEditorUtilities::FindBoundEventForComponent(Blueprint,Delegate,Property->GetFName());const bool bExisting=Node!=nullptr;if(!Node){FKismetEditorUtilities::CreateNewBoundEventForClass(Button->GetClass(),Delegate,Blueprint,Property);Node=FKismetEditorUtilities::FindBoundEventForComponent(Blueprint,Delegate,Property->GetFName());}if(!Node)return Failure(TEXT("UEPI_EDIT_APPLY_FAILED"),TEXT("Failed to create bound button event."));FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);TSharedRef<FJsonObject> Detail=MakeShared<FJsonObject>();Detail->SetStringField(TEXT("asset"),Blueprint->GetPathName());Detail->SetStringField(TEXT("button"),Name);Detail->SetStringField(TEXT("delegate"),Delegate.ToString());Detail->SetBoolField(TEXT("already_bound"),bExisting);TSharedRef<FJsonObject> NodeJson=MakeShared<FJsonObject>();NodeJson->SetStringField(TEXT("node_guid"),Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));NodeJson->SetStringField(TEXT("class"),Node->GetClass()->GetPathName());Detail->SetObjectField(TEXT("node"),NodeJson);return Success(bExisting?TEXT("Button delegate was already bound."):TEXT("Button delegate bound to a ComponentBoundEvent node."),Detail);
			}
		private:FUEPIEditOperationDescriptor Descriptor;
		};
	}

	TSharedRef<IUEPIEditOperation> MakeUEPIWidgetOperation(const FUEPIEditOperationDescriptor& Descriptor){return MakeShared<WidgetOperationsPrivate::FUEPIWidgetOperation>(Descriptor);}
}
