// widget.* commands: compact tree with style fingerprints, widget properties,
// clone/add/remove/move/set, event binding, animations, preview + layout, diff.
#include "CommandHelpers.h"
#include "Core/AgentCommandRegistry.h"
#include "Core/AgentConfig.h"
#include "Core/PropertyUtils.h"
#include "Core/PinTypeUtils.h"
#include "Serialization/GraphSerializer.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/ContentWidget.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "Editor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"

namespace
{
	// Properties that are per-instance content rather than style.
	const TSet<FName>& StyleExcludes()
	{
		static const TSet<FName> Set = {
			TEXT("Slot"), TEXT("bIsVariable"), TEXT("Navigation"), TEXT("ToolTipText"), TEXT("ToolTipWidget"),
			TEXT("Text"), TEXT("Visibility"), TEXT("RenderTransform"), TEXT("RenderTransformPivot"), TEXT("RenderOpacity"),
			TEXT("Cursor"), TEXT("bOverride_Cursor"), TEXT("AccessibleText"), TEXT("AccessibleSummaryText"), TEXT("bIsEnabled"),
			TEXT("bIsVolatile"), TEXT("Clipping"), TEXT("FlowDirectionPreference"), TEXT("bOverrideAccessibleDefaults"),
			TEXT("AccessibleBehavior"), TEXT("AccessibleSummaryBehavior"), TEXT("bCanChildrenBeAccessible"), TEXT("PixelSnapping"),
			TEXT("DesignerFlags"), TEXT("DisplayLabel"), TEXT("CategoryName"), TEXT("bHiddenInDesigner"), TEXT("bExpandedInDesigner"),
			TEXT("bLockedInDesigner"), TEXT("bIsEnabledDelegate"), TEXT("ToolTipTextDelegate"), TEXT("VisibilityDelegate"),
			TEXT("PropertyBindings"), TEXT("Brush"), TEXT("Content")
		};
		return Set;
	}
	// Brush is style for Images/Borders; only excluded from the *generic* hash when it references a subobject. Keep it.
	const TSet<FName>& InspectExcludes()
	{
		static const TSet<FName> Set = {
			TEXT("Slot"), TEXT("Navigation"), TEXT("ToolTipWidget"), TEXT("PropertyBindings"), TEXT("DesignerFlags"),
			TEXT("DisplayLabel"), TEXT("CategoryName"), TEXT("bHiddenInDesigner"), TEXT("bExpandedInDesigner"), TEXT("bLockedInDesigner"),
			TEXT("bIsEnabledDelegate"), TEXT("ToolTipTextDelegate"), TEXT("VisibilityDelegate")
		};
		return Set;
	}
	const TSet<FName>& SlotExcludes()
	{
		static const TSet<FName> Set = { TEXT("Parent"), TEXT("Content") };
		return Set;
	}

	bool IsWidgetRefProperty(const FProperty* Property)
	{
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			return ObjectProperty->PropertyClass && (ObjectProperty->PropertyClass->IsChildOf(UWidget::StaticClass()) || ObjectProperty->PropertyClass->IsChildOf(UPanelSlot::StaticClass()));
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			return IsWidgetRefProperty(ArrayProperty->Inner);
		}
		return false;
	}

	UWidgetBlueprint* RequireWidgetBlueprint(const FJsonObject& Params, FAgentResult& OutError)
	{
		UBlueprint* Blueprint = AgentCmd::RequireBlueprint(Params, OutError);
		if (!Blueprint)
		{
			return nullptr;
		}
		UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint);
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			OutError = FAgentResult::BadRequest(FString::Printf(TEXT("%s is not a Widget Blueprint."), *Blueprint->GetName()));
			return nullptr;
		}
		return WidgetBlueprint;
	}

	UWidget* FindWidget(UWidgetBlueprint* WidgetBlueprint, const FString& Name, FAgentResult& OutError)
	{
		UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(FName(*Name));
		if (!Widget)
		{
			// Case-insensitive fallback
			WidgetBlueprint->WidgetTree->ForEachWidget([&](UWidget* Candidate)
			{
				if (!Widget && Candidate && Candidate->GetName().Equals(Name, ESearchCase::IgnoreCase))
				{
					Widget = Candidate;
				}
			});
		}
		if (!Widget)
		{
			OutError = FAgentResult::NotFound(FString::Printf(TEXT("Widget '%s' not found in %s."), *Name, *WidgetBlueprint->GetName()));
		}
		return Widget;
	}

	FString UniqueWidgetName(UWidgetTree* Tree, const FString& Base)
	{
		FString Name = Base;
		int32 Suffix = 1;
		while (Tree->FindWidget(FName(*Name)) || StaticFindObjectFast(UObject::StaticClass(), Tree, FName(*Name)))
		{
			Name = FString::Printf(TEXT("%s_%d"), *Base, Suffix++);
		}
		return Name;
	}

	FString TextLabel(UWidget* Widget)
	{
		if (FTextProperty* TextProperty = CastField<FTextProperty>(Widget->GetClass()->FindPropertyByName(TEXT("Text"))))
		{
			return TextProperty->GetPropertyValue_InContainer(Widget).ToString();
		}
		return FString();
	}

	uint32 StyleHash(UWidget* Widget)
	{
		uint32 Hash = AgentProps::HashProperties(Widget, StyleExcludes());
		if (Widget->Slot)
		{
			TSet<FName> Exclude = SlotExcludes();
			Exclude.Add(TEXT("LayoutData"));
			Hash = HashCombine(Hash, AgentProps::HashProperties(Widget->Slot, Exclude));
		}
		return Hash;
	}

	TSharedRef<FJsonObject> StyleProps(UWidget* Widget)
	{
		TSharedRef<FJsonObject> Json = AgentProps::ExportProperties(Widget, true, StyleExcludes());
		if (Widget->Slot)
		{
			TSet<FName> Exclude = SlotExcludes();
			Exclude.Add(TEXT("LayoutData"));
			TSharedRef<FJsonObject> SlotProps = AgentProps::ExportProperties(Widget->Slot, true, Exclude);
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SlotProps->Values)
			{
				Json->SetField(TEXT("Slot.") + Pair.Key, Pair.Value);
			}
		}
		return Json;
	}

	void WalkTree(UWidget* Widget, int32 Depth, TFunctionRef<void(UWidget*, int32 Depth, int32 Index)> Visit, int32 Index = 0)
	{
		if (!Widget)
		{
			return;
		}
		Visit(Widget, Depth, Index);
		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			for (int32 ChildIndex = 0; ChildIndex < Panel->GetChildrenCount(); ++ChildIndex)
			{
				WalkTree(Panel->GetChildAt(ChildIndex), Depth + 1, Visit, ChildIndex);
			}
		}
	}

	TArray<FString> BoundEventsFor(UWidgetBlueprint* WidgetBlueprint, const FName& WidgetName)
	{
		TArray<FString> Out;
		for (UEdGraph* Graph : WidgetBlueprint->UbergraphPages)
		{
			if (!Graph) { continue; }
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node))
				{
					if (Bound->ComponentPropertyName == WidgetName)
					{
						Out.Add(Bound->DelegatePropertyName.ToString() + TEXT("=") + Bound->NodeGuid.ToString(EGuidFormats::Digits));
					}
				}
			}
		}
		return Out;
	}

	// ------------------------------------------------------------------ tree / inspect

	FAgentResult Cmd_Tree(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UWidgetBlueprint* WidgetBlueprint = RequireWidgetBlueprint(Params, Error);
		if (!WidgetBlueprint) { return Error; }
		const bool bStyles = AgentJson::GetBool(Params, TEXT("styles"), true);
		const FString RootName = AgentJson::GetString(Params, TEXT("root"));
		const int32 MaxDepth = AgentJson::GetInt(Params, TEXT("max_depth"), 64);

		UWidget* Root = WidgetBlueprint->WidgetTree->RootWidget;
		if (!RootName.IsEmpty())
		{
			Root = FindWidget(WidgetBlueprint, RootName, Error);
			if (!Root) { return Error; }
		}

		TMap<uint32, FString> StyleIds;
		TMap<FString, TSharedPtr<FJsonObject>> Styles;
		TMap<FString, TArray<FString>> StyleMembers;
		TArray<TSharedPtr<FJsonValue>> Widgets;
		int32 Total = 0;
		WalkTree(Root, 0, [&](UWidget* Widget, int32 Depth, int32 Index)
		{
			++Total;
			if (Depth > MaxDepth) { return; }
			TSharedRef<FJsonObject> Item = AgentJson::Obj();
			Item->SetStringField(TEXT("name"), Widget->GetName());
			Item->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
			Item->SetNumberField(TEXT("depth"), Depth);
			Item->SetNumberField(TEXT("index"), Index);
			if (UPanelWidget* Parent = Widget->GetParent())
			{
				Item->SetStringField(TEXT("parent"), Parent->GetName());
			}
			if (Widget->Slot)
			{
				Item->SetStringField(TEXT("slot"), Widget->Slot->GetClass()->GetName());
			}
			if (Widget->bIsVariable) { Item->SetBoolField(TEXT("var"), true); }
			const FString Label = TextLabel(Widget);
			if (!Label.IsEmpty()) { Item->SetStringField(TEXT("text"), Label.Left(60)); }
			if (Widget->GetVisibility() != ESlateVisibility::Visible && Widget->GetVisibility() != ESlateVisibility::SelfHitTestInvisible)
			{
				Item->SetStringField(TEXT("visibility"), StaticEnum<ESlateVisibility>()->GetNameStringByValue(static_cast<int64>(Widget->GetVisibility())));
			}
			if (const UUserWidget* Sub = Cast<UUserWidget>(Widget))
			{
				if (const UClass* SubClass = Sub->GetClass())
				{
					Item->SetStringField(TEXT("class"), AgentResolver::GetClassDisplayName(SubClass));
					Item->SetBoolField(TEXT("user_widget"), true);
				}
			}
			if (bStyles && (!Widget->IsA<UPanelWidget>() || Widget->IsA<UContentWidget>()))
			{
				const uint32 Hash = StyleHash(Widget);
				FString* Existing = StyleIds.Find(Hash);
				FString StyleId;
				if (Existing)
				{
					StyleId = *Existing;
				}
				else
				{
					StyleId = FString::Printf(TEXT("S%d"), StyleIds.Num() + 1);
					StyleIds.Add(Hash, StyleId);
					TSharedRef<FJsonObject> Style = AgentJson::Obj();
					Style->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
					Style->SetObjectField(TEXT("props"), StyleProps(Widget));
					Styles.Add(StyleId, Style);
				}
				StyleMembers.FindOrAdd(StyleId).Add(Widget->GetName());
				Item->SetStringField(TEXT("style"), StyleId);
			}
			const TArray<FString> Bound = BoundEventsFor(WidgetBlueprint, Widget->GetFName());
			if (Bound.Num() > 0) { AgentJson::SetStringArray(*Item, TEXT("events"), Bound); }
			Widgets.Add(MakeShared<FJsonValueObject>(Item));
		});

		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(WidgetBlueprint);
		Json->SetStringField(TEXT("parent"), AgentResolver::GetClassDisplayName(WidgetBlueprint->ParentClass));
		Json->SetStringField(TEXT("root"), Root ? Root->GetName() : FString());
		Json->SetArrayField(TEXT("widgets"), Widgets);
		Json->SetNumberField(TEXT("total"), Total);
		if (bStyles)
		{
			TSharedRef<FJsonObject> StylesJson = AgentJson::Obj();
			for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : Styles)
			{
				AgentJson::SetStringArray(*Pair.Value, TEXT("members"), StyleMembers[Pair.Key]);
				StylesJson->SetObjectField(Pair.Key, Pair.Value);
			}
			Json->SetObjectField(TEXT("styles"), StylesJson);
		}
		TArray<FString> Animations;
		for (const UWidgetAnimation* Animation : WidgetBlueprint->Animations)
		{
			if (Animation) { Animations.Add(Animation->GetDisplayName().ToString()); }
		}
		AgentJson::SetStringArray(*Json, TEXT("animations"), Animations);
		return FAgentResult::Ok(Json);
	}

	FAgentResult Cmd_Inspect(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UWidgetBlueprint* WidgetBlueprint = RequireWidgetBlueprint(Params, Error);
		if (!WidgetBlueprint) { return Error; }
		FString Name;
		if (!AgentCmd::RequireString(Params, TEXT("widget"), Name, Error)) { return Error; }
		UWidget* Widget = FindWidget(WidgetBlueprint, Name, Error);
		if (!Widget) { return Error; }
		const bool bAll = AgentJson::GetBool(Params, TEXT("all"), false);

		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(WidgetBlueprint);
		Json->SetStringField(TEXT("widget"), Widget->GetName());
		Json->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
		Json->SetStringField(TEXT("class_spec"), AgentResolver::GetClassSpec(Widget->GetClass()));
		if (UPanelWidget* Parent = Widget->GetParent())
		{
			Json->SetStringField(TEXT("parent"), Parent->GetName());
			Json->SetNumberField(TEXT("index"), Parent->GetChildIndex(Widget));
		}
		Json->SetBoolField(TEXT("var"), Widget->bIsVariable);
		Json->SetObjectField(TEXT("props"), AgentProps::ExportProperties(Widget, !bAll, InspectExcludes()));
		if (Widget->Slot)
		{
			TSharedRef<FJsonObject> Slot = AgentJson::Obj();
			Slot->SetStringField(TEXT("class"), Widget->Slot->GetClass()->GetName());
			Slot->SetObjectField(TEXT("props"), AgentProps::ExportProperties(Widget->Slot, !bAll, SlotExcludes()));
			Json->SetObjectField(TEXT("slot"), Slot);
		}
		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			TArray<FString> Children;
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				if (UWidget* Child = Panel->GetChildAt(Index)) { Children.Add(Child->GetName()); }
			}
			AgentJson::SetStringArray(*Json, TEXT("children"), Children);
		}
		TArray<FString> Events;
		for (TFieldIterator<FMulticastDelegateProperty> It(Widget->GetClass()); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_BlueprintAssignable)) { Events.Add(It->GetName()); }
		}
		AgentJson::SetStringArray(*Json, TEXT("events"), Events);
		AgentJson::SetStringArray(*Json, TEXT("bound"), BoundEventsFor(WidgetBlueprint, Widget->GetFName()));
		return FAgentResult::Ok(Json);
	}

	// ------------------------------------------------------------------ edits

	bool ApplyProperties(UObject* Target, const TSharedPtr<FJsonObject>& Props, TArray<FString>& OutChanged, TArray<FString>& OutFailed)
	{
		if (!Props.IsValid() || !Target)
		{
			return true;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Props->Values)
		{
			const FString Value = Pair.Value->Type == EJson::String ? Pair.Value->AsString() : AgentJson::SerializeValue(Pair.Value);
			FString SetError;
			if (AgentProps::SetValue(Target, Pair.Key, Value, SetError))
			{
				OutChanged.Add(Pair.Key);
			}
			else
			{
				OutFailed.Add(Pair.Key + TEXT(": ") + SetError);
			}
		}
		return OutFailed.Num() == 0;
	}

	void FinishWidgetEdit(UWidgetBlueprint* WidgetBlueprint, FAgentContext& Context, const FString& What)
	{
		WidgetBlueprint->WidgetTree->Modify();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		AgentCmd::NoteEdit(Context, WidgetBlueprint, What);
	}

	void CopyWidgetStyle(UWidget* Source, UWidget* Target, bool bIncludeSlot)
	{
		TSet<FName> Exclude = StyleExcludes();
		Exclude.Remove(TEXT("Brush")); // brushes are style
		AgentProps::CopyProperties(Source, Target, Exclude, [](const FProperty* Property)
		{
			return AgentProps::IsEditable(Property) && !IsWidgetRefProperty(Property);
		});
		if (bIncludeSlot && Source->Slot && Target->Slot && Source->Slot->GetClass() == Target->Slot->GetClass())
		{
			TSet<FName> SlotExclude = SlotExcludes();
			if (Source->Slot->IsA<UCanvasPanelSlot>())
			{
				SlotExclude.Add(TEXT("LayoutData"));
			}
			Target->Slot->Modify();
			AgentProps::CopyProperties(Source->Slot, Target->Slot, SlotExclude, [](const FProperty* Property) { return !IsWidgetRefProperty(Property); });
			Target->Slot->SynchronizeProperties();
		}
		Target->SynchronizeProperties();
	}

	UWidget* CloneWidgetRecursive(UWidgetTree* Tree, UWidget* Source, const FString& RootName, bool bRoot)
	{
		const FString Name = bRoot ? UniqueWidgetName(Tree, RootName) : UniqueWidgetName(Tree, Source->GetName());
		UWidget* Clone = NewObject<UWidget>(Tree, Source->GetClass(), FName(*Name), RF_Transactional);
		TSet<FName> Exclude = { TEXT("Slot") };
		AgentProps::CopyProperties(Source, Clone, Exclude, [](const FProperty* Property) { return !IsWidgetRefProperty(Property); });
		// Instanced sub-objects (e.g. Navigation) must not be shared with the source.
		for (TFieldIterator<FObjectProperty> It(Clone->GetClass()); It; ++It)
		{
			UObject* Value = It->GetObjectPropertyValue_InContainer(Clone);
			if (Value && Value->GetOuter() == Source)
			{
				It->SetObjectPropertyValue_InContainer(Clone, DuplicateObject<UObject>(Value, Clone));
			}
		}
		Clone->bIsVariable = Source->bIsVariable;
		if (UPanelWidget* SourcePanel = Cast<UPanelWidget>(Source))
		{
			UPanelWidget* ClonePanel = Cast<UPanelWidget>(Clone);
			for (int32 Index = 0; Index < SourcePanel->GetChildrenCount(); ++Index)
			{
				UWidget* SourceChild = SourcePanel->GetChildAt(Index);
				if (!SourceChild) { continue; }
				UWidget* CloneChild = CloneWidgetRecursive(Tree, SourceChild, FString(), false);
				UPanelSlot* NewSlot = ClonePanel->AddChild(CloneChild);
				if (NewSlot && SourceChild->Slot && NewSlot->GetClass() == SourceChild->Slot->GetClass())
				{
					AgentProps::CopyProperties(SourceChild->Slot, NewSlot, SlotExcludes(), [](const FProperty* Property) { return !IsWidgetRefProperty(Property); });
					NewSlot->SynchronizeProperties();
				}
			}
		}
		return Clone;
	}

	/** Resolves parent + index from params: parent/index, insert_after, insert_before. */
	bool ResolveInsertion(UWidgetBlueprint* WidgetBlueprint, const FJsonObject& Params, UWidget* DefaultSibling, UPanelWidget*& OutParent, int32& OutIndex, FAgentResult& OutError)
	{
		const FString After = AgentJson::GetString(Params, TEXT("insert_after"));
		const FString Before = AgentJson::GetString(Params, TEXT("insert_before"));
		const FString ParentName = AgentJson::GetString(Params, TEXT("parent"));
		OutParent = nullptr;
		OutIndex = -1;
		if (!After.IsEmpty() || !Before.IsEmpty())
		{
			UWidget* Sibling = FindWidget(WidgetBlueprint, After.IsEmpty() ? Before : After, OutError);
			if (!Sibling) { return false; }
			OutParent = Sibling->GetParent();
			if (!OutParent)
			{
				OutError = FAgentResult::BadRequest(TEXT("Sibling has no parent panel."));
				return false;
			}
			OutIndex = OutParent->GetChildIndex(Sibling) + (After.IsEmpty() ? 0 : 1);
			return true;
		}
		if (!ParentName.IsEmpty())
		{
			UWidget* Parent = FindWidget(WidgetBlueprint, ParentName, OutError);
			if (!Parent) { return false; }
			OutParent = Cast<UPanelWidget>(Parent);
			if (!OutParent)
			{
				OutError = FAgentResult::BadRequest(FString::Printf(TEXT("'%s' is not a panel widget."), *ParentName));
				return false;
			}
			OutIndex = AgentJson::GetInt(Params, TEXT("index"), -1);
			return true;
		}
		if (DefaultSibling && DefaultSibling->GetParent())
		{
			OutParent = DefaultSibling->GetParent();
			OutIndex = OutParent->GetChildIndex(DefaultSibling) + 1;
			return true;
		}
		OutError = FAgentResult::BadRequest(TEXT("Provide parent (+index), insert_after or insert_before."));
		return false;
	}

	UPanelSlot* InsertInto(UPanelWidget* Parent, UWidget* Widget, int32 Index)
	{
		Parent->Modify();
		if (Index < 0 || Index >= Parent->GetChildrenCount())
		{
			return Parent->AddChild(Widget);
		}
		return Parent->InsertChildAt(Index, Widget);
	}

	FAgentResult Cmd_Clone(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UWidgetBlueprint* WidgetBlueprint = RequireWidgetBlueprint(Params, Error);
		if (!WidgetBlueprint) { return Error; }
		FString SourceName, NewName;
		if (!AgentCmd::RequireString(Params, TEXT("source"), SourceName, Error)) { return Error; }
		if (!AgentCmd::RequireString(Params, TEXT("new_name"), NewName, Error)) { return Error; }
		UWidget* Source = FindWidget(WidgetBlueprint, SourceName, Error);
		if (!Source) { return Error; }
		if (WidgetBlueprint->WidgetTree->FindWidget(FName(*NewName)))
		{
			return FAgentResult::Error(AgentErrors::Conflict, FString::Printf(TEXT("Widget '%s' already exists."), *NewName));
		}
		UPanelWidget* Parent = nullptr;
		int32 Index = -1;
		if (!ResolveInsertion(WidgetBlueprint, Params, Source, Parent, Index, Error)) { return Error; }

		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Clone widget ") + SourceName + TEXT(" -> ") + NewName));
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		UWidget* Clone = CloneWidgetRecursive(WidgetBlueprint->WidgetTree, Source, NewName, true);
		UPanelSlot* NewSlot = InsertInto(Parent, Clone, Index);
		if (NewSlot && Source->Slot && NewSlot->GetClass() == Source->Slot->GetClass())
		{
			AgentProps::CopyProperties(Source->Slot, NewSlot, SlotExcludes(), [](const FProperty* Property) { return !IsWidgetRefProperty(Property); });
			if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(NewSlot))
			{
				// Avoid a perfect overlap on canvases: nudge below the source.
				FVector2D Position = CanvasSlot->GetPosition();
				Position.Y += CanvasSlot->GetSize().Y + 8.0f;
				CanvasSlot->SetPosition(Position);
			}
			NewSlot->SynchronizeProperties();
		}
		TArray<FString> Changed, Failed;
		ApplyProperties(Clone, AgentJson::GetObject(Params, TEXT("properties")), Changed, Failed);
		// Per-child overrides: {"children": {"TXT_Settings": {"Text": "Credits"}}} keyed by SOURCE child name
		if (TSharedPtr<FJsonObject> Children = AgentJson::GetObject(Params, TEXT("children")))
		{
			TArray<UWidget*> SourceOrder, CloneOrder;
			WalkTree(Source, 0, [&](UWidget* W, int32, int32) { SourceOrder.Add(W); });
			WalkTree(Clone, 0, [&](UWidget* W, int32, int32) { CloneOrder.Add(W); });
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Children->Values)
			{
				int32 Found = INDEX_NONE;
				for (int32 I = 0; I < SourceOrder.Num(); ++I)
				{
					if (SourceOrder[I]->GetName().Equals(Pair.Key, ESearchCase::IgnoreCase)) { Found = I; break; }
				}
				if (Found == INDEX_NONE || !CloneOrder.IsValidIndex(Found))
				{
					Failed.Add(Pair.Key + TEXT(": child not found in source"));
					continue;
				}
				TSharedPtr<FJsonObject> ChildProps = Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
				if (ChildProps.IsValid() && ChildProps->HasField(TEXT("rename")))
				{
					const FString ChildNew = UniqueWidgetName(WidgetBlueprint->WidgetTree, AgentJson::GetString(*ChildProps, TEXT("rename")));
					CloneOrder[Found]->Rename(*ChildNew, nullptr, REN_DontCreateRedirectors);
					ChildProps->RemoveField(TEXT("rename"));
					Changed.Add(Pair.Key + TEXT("->") + ChildNew);
				}
				TArray<FString> ChildChanged;
				ApplyProperties(CloneOrder[Found], ChildProps, ChildChanged, Failed);
				for (const FString& C : ChildChanged) { Changed.Add(CloneOrder[Found]->GetName() + TEXT(".") + C); }
			}
		}
		if (AgentJson::GetBool(Params, TEXT("rename_children"), true))
		{
			// BTN_Settings -> BTN_Credits: apply the same suffix replacement to children (TXT_Settings -> TXT_Credits)
			FString OldSuffix = SourceName, NewSuffix = NewName;
			int32 Underscore;
			if (SourceName.FindChar(TEXT('_'), Underscore)) { OldSuffix = SourceName.Mid(Underscore + 1); }
			if (NewName.FindChar(TEXT('_'), Underscore)) { NewSuffix = NewName.Mid(Underscore + 1); }
			if (!OldSuffix.IsEmpty() && OldSuffix != NewSuffix)
			{
				TArray<UWidget*> CloneWidgets;
				WalkTree(Clone, 0, [&](UWidget* W, int32 Depth, int32) { if (Depth > 0) { CloneWidgets.Add(W); } });
				for (UWidget* Child : CloneWidgets)
				{
					const FString ChildName = Child->GetName();
					if (ChildName.Contains(OldSuffix))
					{
						FString Base = ChildName.Replace(*OldSuffix, *NewSuffix);
						// Strip the _N uniqueness suffix added during cloning if the new name is free.
						int32 Pos;
						if (Base.FindLastChar(TEXT('_'), Pos) && Base.Mid(Pos + 1).IsNumeric() && !ChildName.EndsWith(Base.Mid(Pos)))
						{
							Base = Base.Left(Pos);
						}
						const FString Unique = UniqueWidgetName(WidgetBlueprint->WidgetTree, Base);
						Child->Rename(*Unique, nullptr, REN_DontCreateRedirectors);
					}
				}
			}
		}
		Clone->SynchronizeProperties();
		FinishWidgetEdit(WidgetBlueprint, Context, TEXT("widget.clone"));

		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(WidgetBlueprint);
		Json->SetStringField(TEXT("widget"), Clone->GetName());
		Json->SetStringField(TEXT("class"), Clone->GetClass()->GetName());
		Json->SetStringField(TEXT("parent"), Parent->GetName());
		Json->SetNumberField(TEXT("index"), Parent->GetChildIndex(Clone));
		TArray<FString> CloneNames;
		WalkTree(Clone, 0, [&](UWidget* W, int32 Depth, int32) { CloneNames.Add(FString::ChrN(Depth, TEXT(' ')) + W->GetName() + TEXT(":") + W->GetClass()->GetName()); });
		AgentJson::SetStringArray(*Json, TEXT("tree"), CloneNames);
		AgentJson::SetStringArray(*Json, TEXT("changed"), Changed);
		if (Failed.Num() > 0) { AgentJson::SetStringArray(*Json, TEXT("failed"), Failed); }
		return FAgentResult::Ok(Json);
	}

	FAgentResult Cmd_Add(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UWidgetBlueprint* WidgetBlueprint = RequireWidgetBlueprint(Params, Error);
		if (!WidgetBlueprint) { return Error; }
		FString ClassSpec, Name;
		if (!AgentCmd::RequireString(Params, TEXT("class"), ClassSpec, Error)) { return Error; }
		if (!AgentCmd::RequireString(Params, TEXT("name"), Name, Error)) { return Error; }
		UClass* WidgetClass = Context.Adapter->ResolveClass(ClassSpec);
		if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
		{
			return FAgentResult::NotFound(FString::Printf(TEXT("Widget class '%s' not found."), *ClassSpec));
		}
		if (WidgetBlueprint->WidgetTree->FindWidget(FName(*Name)))
		{
			return FAgentResult::Error(AgentErrors::Conflict, FString::Printf(TEXT("Widget '%s' already exists."), *Name));
		}
		UPanelWidget* Parent = nullptr;
		int32 Index = -1;
		const bool bAsRoot = AgentJson::GetBool(Params, TEXT("root"), false) || WidgetBlueprint->WidgetTree->RootWidget == nullptr;
		if (!bAsRoot && !ResolveInsertion(WidgetBlueprint, Params, nullptr, Parent, Index, Error)) { return Error; }

		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Add widget ") + Name));
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		UWidget* Widget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*Name));
		Widget->SetFlags(RF_Transactional);
		Widget->bIsVariable = AgentJson::GetBool(Params, TEXT("var"), !WidgetClass->IsChildOf(UPanelWidget::StaticClass()) || WidgetClass->IsChildOf(UContentWidget::StaticClass()));
		if (bAsRoot)
		{
			WidgetBlueprint->WidgetTree->RootWidget = Widget;
		}
		else
		{
			UPanelSlot* NewSlot = InsertInto(Parent, Widget, Index);
			TArray<FString> SlotChanged, SlotFailed;
			if (NewSlot)
			{
				ApplyProperties(NewSlot, AgentJson::GetObject(Params, TEXT("slot")), SlotChanged, SlotFailed);
			}
		}
		TArray<FString> Changed, Failed;
		ApplyProperties(Widget, AgentJson::GetObject(Params, TEXT("properties")), Changed, Failed);
		Widget->SynchronizeProperties();
		FinishWidgetEdit(WidgetBlueprint, Context, TEXT("widget.add"));
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(WidgetBlueprint);
		Json->SetStringField(TEXT("widget"), Widget->GetName());
		Json->SetStringField(TEXT("class"), WidgetClass->GetName());
		if (Parent)
		{
			Json->SetStringField(TEXT("parent"), Parent->GetName());
			Json->SetNumberField(TEXT("index"), Parent->GetChildIndex(Widget));
		}
		AgentJson::SetStringArray(*Json, TEXT("changed"), Changed);
		if (Failed.Num() > 0) { AgentJson::SetStringArray(*Json, TEXT("failed"), Failed); }
		return FAgentResult::Ok(Json);
	}

	FAgentResult Cmd_Remove(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UWidgetBlueprint* WidgetBlueprint = RequireWidgetBlueprint(Params, Error);
		if (!WidgetBlueprint) { return Error; }
		TArray<FString> Names = AgentJson::GetStringOrArray(Params, TEXT("widgets"));
		if (Names.Num() == 0) { Names.Add(AgentJson::GetString(Params, TEXT("widget"))); }
		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Remove widget(s)")));
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		TArray<FString> Removed;
		for (const FString& Name : Names)
		{
			UWidget* Widget = FindWidget(WidgetBlueprint, Name, Error);
			if (!Widget) { return Error; }
			if (UPanelWidget* Parent = Widget->GetParent())
			{
				Parent->Modify();
				Parent->RemoveChild(Widget);
			}
			else if (WidgetBlueprint->WidgetTree->RootWidget == Widget)
			{
				WidgetBlueprint->WidgetTree->RootWidget = nullptr;
			}
			TArray<UWidget*> Subtree;
			WalkTree(Widget, 0, [&](UWidget* W, int32, int32) { Subtree.Add(W); });
			for (UWidget* W : Subtree)
			{
				W->Modify();
				W->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
				W->MarkAsGarbage();
				Removed.Add(W->GetName());
			}
		}
		FinishWidgetEdit(WidgetBlueprint, Context, TEXT("widget.remove"));
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(WidgetBlueprint);
		AgentJson::SetStringArray(*Json, TEXT("removed"), Removed);
		return FAgentResult::Ok(Json);
	}

	FAgentResult Cmd_Move(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UWidgetBlueprint* WidgetBlueprint = RequireWidgetBlueprint(Params, Error);
		if (!WidgetBlueprint) { return Error; }
		FString Name;
		if (!AgentCmd::RequireString(Params, TEXT("widget"), Name, Error)) { return Error; }
		UWidget* Widget = FindWidget(WidgetBlueprint, Name, Error);
		if (!Widget) { return Error; }
		UPanelWidget* Parent = nullptr;
		int32 Index = -1;
		if (!ResolveInsertion(WidgetBlueprint, Params, nullptr, Parent, Index, Error)) { return Error; }
		if (Parent == Widget)
		{
			return FAgentResult::BadRequest(TEXT("Cannot parent a widget to itself."));
		}
		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Move widget ") + Name));
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		UPanelWidget* OldParent = Widget->GetParent();
		const bool bSameParent = OldParent == Parent;
		if (bSameParent)
		{
			const int32 Current = Parent->GetChildIndex(Widget);
			if (Index < 0 || Index >= Parent->GetChildrenCount()) { Index = Parent->GetChildrenCount() - 1; }
			else if (Index > Current) { --Index; }
			Parent->Modify();
			Parent->ShiftChild(Index, Widget);
		}
		else
		{
			if (OldParent)
			{
				OldParent->Modify();
				OldParent->RemoveChild(Widget);
			}
			InsertInto(Parent, Widget, Index);
		}
		FinishWidgetEdit(WidgetBlueprint, Context, TEXT("widget.move"));
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(WidgetBlueprint);
		Json->SetStringField(TEXT("widget"), Widget->GetName());
		Json->SetStringField(TEXT("parent"), Parent->GetName());
		Json->SetNumberField(TEXT("index"), Parent->GetChildIndex(Widget));
		return FAgentResult::Ok(Json);
	}

	FAgentResult Cmd_Set(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UWidgetBlueprint* WidgetBlueprint = RequireWidgetBlueprint(Params, Error);
		if (!WidgetBlueprint) { return Error; }
		FString Name;
		if (!AgentCmd::RequireString(Params, TEXT("widget"), Name, Error)) { return Error; }
		UWidget* Widget = FindWidget(WidgetBlueprint, Name, Error);
		if (!Widget) { return Error; }
		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Edit widget ") + Name));
		WidgetBlueprint->Modify();
		Widget->Modify();
		TArray<FString> Changed, Failed;
		ApplyProperties(Widget, AgentJson::GetObject(Params, TEXT("properties")), Changed, Failed);
		if (Widget->Slot)
		{
			TArray<FString> SlotChanged;
			Widget->Slot->Modify();
			ApplyProperties(Widget->Slot, AgentJson::GetObject(Params, TEXT("slot")), SlotChanged, Failed);
			for (const FString& C : SlotChanged) { Changed.Add(TEXT("Slot.") + C); }
			Widget->Slot->SynchronizeProperties();
		}
		if (AgentJson::Has(Params, TEXT("var")))
		{
			Widget->bIsVariable = AgentJson::GetBool(Params, TEXT("var"));
			Changed.Add(TEXT("var"));
		}
		if (AgentJson::Has(Params, TEXT("rename")))
		{
			const FString NewName = AgentJson::GetString(Params, TEXT("rename"));
			if (WidgetBlueprint->WidgetTree->FindWidget(FName(*NewName)))
			{
				return FAgentResult::Error(AgentErrors::Conflict, FString::Printf(TEXT("Widget '%s' already exists."), *NewName));
			}
			const FName OldName = Widget->GetFName();
			Widget->Rename(*NewName, nullptr, REN_DontCreateRedirectors);
			if (Widget->bIsVariable)
			{
				FBlueprintEditorUtils::ReplaceVariableReferences(WidgetBlueprint, OldName, FName(*NewName));
			}
			for (UEdGraph* Graph : WidgetBlueprint->UbergraphPages)
			{
				if (!Graph) { continue; }
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node))
					{
						if (Bound->ComponentPropertyName == OldName)
						{
							Bound->Modify();
							Bound->ComponentPropertyName = FName(*NewName);
						}
					}
				}
			}
			Changed.Add(TEXT("rename"));
		}
		Widget->SynchronizeProperties();
		FinishWidgetEdit(WidgetBlueprint, Context, TEXT("widget.set"));
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(WidgetBlueprint);
		Json->SetStringField(TEXT("widget"), Widget->GetName());
		AgentJson::SetStringArray(*Json, TEXT("changed"), Changed);
		if (Failed.Num() > 0) { AgentJson::SetStringArray(*Json, TEXT("failed"), Failed); }
		if (Failed.Num() > 0 && Changed.Num() == 0)
		{
			return FAgentResult::Error(AgentErrors::BadRequest, Failed[0], Json);
		}
		return FAgentResult::Ok(Json);
	}

	FAgentResult Cmd_CopyStyle(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UWidgetBlueprint* WidgetBlueprint = RequireWidgetBlueprint(Params, Error);
		if (!WidgetBlueprint) { return Error; }
		FString SourceName;
		if (!AgentCmd::RequireString(Params, TEXT("source"), SourceName, Error)) { return Error; }
		UWidget* Source = FindWidget(WidgetBlueprint, SourceName, Error);
		if (!Source) { return Error; }
		TArray<FString> TargetNames = AgentJson::GetStringOrArray(Params, TEXT("targets"));
		const bool bIncludeSlot = AgentJson::GetBool(Params, TEXT("slot"), true);
		const bool bRecursive = AgentJson::GetBool(Params, TEXT("recursive"), true);
		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Copy style from ") + SourceName));
		WidgetBlueprint->Modify();
		TArray<FString> Applied, Skipped;
		for (const FString& TargetName : TargetNames)
		{
			UWidget* Target = FindWidget(WidgetBlueprint, TargetName, Error);
			if (!Target) { return Error; }
			if (Target->GetClass() != Source->GetClass())
			{
				Skipped.Add(TargetName + TEXT(": class mismatch"));
				continue;
			}
			Target->Modify();
			CopyWidgetStyle(Source, Target, bIncludeSlot);
			Applied.Add(TargetName);
			if (bRecursive)
			{
				TArray<UWidget*> SourceOrder, TargetOrder;
				WalkTree(Source, 0, [&](UWidget* W, int32, int32) { SourceOrder.Add(W); });
				WalkTree(Target, 0, [&](UWidget* W, int32, int32) { TargetOrder.Add(W); });
				for (int32 I = 1; I < SourceOrder.Num() && I < TargetOrder.Num(); ++I)
				{
					if (SourceOrder[I]->GetClass() == TargetOrder[I]->GetClass())
					{
						TargetOrder[I]->Modify();
						CopyWidgetStyle(SourceOrder[I], TargetOrder[I], true);
						Applied.Add(TargetOrder[I]->GetName());
					}
				}
			}
		}
		FinishWidgetEdit(WidgetBlueprint, Context, TEXT("widget.copy_style"));
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(WidgetBlueprint);
		AgentJson::SetStringArray(*Json, TEXT("applied"), Applied);
		if (Skipped.Num() > 0) { AgentJson::SetStringArray(*Json, TEXT("skipped"), Skipped); }
		return FAgentResult::Ok(Json);
	}

	// ------------------------------------------------------------------ events

	FAgentResult Cmd_BindEvent(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UWidgetBlueprint* WidgetBlueprint = RequireWidgetBlueprint(Params, Error);
		if (!WidgetBlueprint) { return Error; }
		FString Name, EventName;
		if (!AgentCmd::RequireString(Params, TEXT("widget"), Name, Error)) { return Error; }
		if (!AgentCmd::RequireString(Params, TEXT("event"), EventName, Error)) { return Error; }
		UWidget* Widget = FindWidget(WidgetBlueprint, Name, Error);
		if (!Widget) { return Error; }
		FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(Widget->GetClass(), *EventName);
		if (!DelegateProperty)
		{
			TArray<FString> Available;
			for (TFieldIterator<FMulticastDelegateProperty> It(Widget->GetClass()); It; ++It) { Available.Add(It->GetName()); }
			return FAgentResult::NotFound(FString::Printf(TEXT("Event '%s' not found on %s. Available: %s"), *EventName, *Widget->GetClass()->GetName(), *FString::Join(Available, TEXT(", "))));
		}

		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Bind ") + Name + TEXT(".") + EventName));
		WidgetBlueprint->Modify();
		if (!Widget->bIsVariable)
		{
			Widget->Modify();
			Widget->bIsVariable = true;
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		}
		auto FindWidgetProperty = [&]() -> FObjectProperty*
		{
			if (WidgetBlueprint->SkeletonGeneratedClass)
			{
				if (FObjectProperty* P = FindFProperty<FObjectProperty>(WidgetBlueprint->SkeletonGeneratedClass, Widget->GetFName())) { return P; }
			}
			if (WidgetBlueprint->GeneratedClass)
			{
				if (FObjectProperty* P = FindFProperty<FObjectProperty>(WidgetBlueprint->GeneratedClass, Widget->GetFName())) { return P; }
			}
			return nullptr;
		};
		FObjectProperty* WidgetProperty = FindWidgetProperty();
		if (!WidgetProperty)
		{
			FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipGarbageCollection);
			WidgetProperty = FindWidgetProperty();
		}
		if (!WidgetProperty)
		{
			return FAgentResult::Error(AgentErrors::Internal, FString::Printf(TEXT("Widget '%s' has no class property even after compile."), *Name));
		}

		UEdGraph* EventGraph = AgentResolver::GetEventGraph(WidgetBlueprint);
		if (!EventGraph)
		{
			return FAgentResult::Error(AgentErrors::Internal, TEXT("No event graph."));
		}
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(WidgetBlueprint);
		Json->SetStringField(TEXT("widget"), Widget->GetName());
		Json->SetStringField(TEXT("event"), EventName);
		Json->SetStringField(TEXT("graph"), EventGraph->GetName());

		UK2Node_ComponentBoundEvent* EventNode = const_cast<UK2Node_ComponentBoundEvent*>(FKismetEditorUtilities::FindBoundEventForComponent(WidgetBlueprint, *EventName, Widget->GetFName()));
		bool bCreated = false;
		if (!EventNode)
		{
			EventGraph->Modify();
			int32 MaxY = 0;
			for (const UEdGraphNode* Node : EventGraph->Nodes) { if (Node) { MaxY = FMath::Max(MaxY, Node->NodePosY); } }
			EventNode = NewObject<UK2Node_ComponentBoundEvent>(EventGraph, NAME_None, RF_Transactional);
			EventNode->InitializeComponentBoundEventParams(WidgetProperty, DelegateProperty);
			EventNode->NodePosX = 0;
			EventNode->NodePosY = EventGraph->Nodes.Num() > 0 ? MaxY + 250 : 0;
			EventGraph->AddNode(EventNode, false, false);
			EventNode->CreateNewGuid();
			EventNode->PostPlacedNewNode();
			EventNode->AllocateDefaultPins();
			bCreated = true;
		}
		Json->SetStringField(TEXT("node"), EventNode->NodeGuid.ToString(EGuidFormats::Digits));
		Json->SetBoolField(TEXT("created"), bCreated);

		// Optional: call a function right after the event (creating it if requested).
		FString FunctionName = AgentJson::GetString(Params, TEXT("function"));
		const bool bCreateFunction = AgentJson::GetBool(Params, TEXT("create_function"), false);
		if (!FunctionName.IsEmpty())
		{
			UFunction* Function = nullptr;
			FString ResolveError;
			if (bCreateFunction)
			{
				bool bExists = false;
				for (UEdGraph* G : WidgetBlueprint->FunctionGraphs) { if (G && G->GetName() == FunctionName) { bExists = true; } }
				if (!bExists)
				{
					UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(WidgetBlueprint, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
					FBlueprintEditorUtils::AddFunctionGraph<UClass>(WidgetBlueprint, NewGraph, true, nullptr);
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
					Json->SetStringField(TEXT("function_created"), FunctionName);
				}
			}
			Function = AgentResolver::ResolveFunction(WidgetBlueprint, *Context.Adapter, FunctionName, ResolveError);
			if (!Function && bCreateFunction)
			{
				FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipGarbageCollection);
				Function = AgentResolver::ResolveFunction(WidgetBlueprint, *Context.Adapter, FunctionName, ResolveError);
			}
			if (!Function)
			{
				Json->SetStringField(TEXT("function_error"), ResolveError);
			}
			else
			{
				UEdGraphPin* Then = EventNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
				if (Then && Then->LinkedTo.Num() > 0 && !AgentJson::GetBool(Params, TEXT("force"), false))
				{
					Json->SetStringField(TEXT("function_error"), TEXT("Event already has an exec target; pass force=true to insert before it."));
				}
				else
				{
					UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(EventGraph, NAME_None, RF_Transactional);
					Call->SetFromFunction(Function);
					Call->NodePosX = EventNode->NodePosX + 320;
					Call->NodePosY = EventNode->NodePosY;
					EventGraph->AddNode(Call, false, false);
					Call->CreateNewGuid();
					Call->PostPlacedNewNode();
					Call->AllocateDefaultPins();
					UEdGraphPin* Exec = Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
					if (Then && Exec)
					{
						TArray<UEdGraphPin*> Old = Then->LinkedTo;
						Then->BreakAllPinLinks(true);
						EventGraph->GetSchema()->TryCreateConnection(Then, Exec);
						if (UEdGraphPin* CallThen = Call->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output))
						{
							for (UEdGraphPin* O : Old) { if (O) { EventGraph->GetSchema()->TryCreateConnection(CallThen, O); } }
						}
					}
					Json->SetStringField(TEXT("call_node"), Call->NodeGuid.ToString(EGuidFormats::Digits));
					Json->SetStringField(TEXT("function"), Function->GetName());
				}
			}
		}
		EventGraph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
		AgentCmd::NoteEdit(Context, WidgetBlueprint, TEXT("widget.bind_event"));
		return FAgentResult::Ok(Json);
	}

	// ------------------------------------------------------------------ animations

	FAgentResult Cmd_Animations(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UWidgetBlueprint* WidgetBlueprint = RequireWidgetBlueprint(Params, Error);
		if (!WidgetBlueprint) { return Error; }
		const FString Filter = AgentJson::GetString(Params, TEXT("animation"));
		TArray<TSharedPtr<FJsonValue>> Items;
		for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
		{
			if (!Animation) { continue; }
			const FString AnimName = Animation->GetDisplayName().ToString();
			if (!Filter.IsEmpty() && !AnimName.Equals(Filter, ESearchCase::IgnoreCase) && !Animation->GetName().Equals(Filter, ESearchCase::IgnoreCase)) { continue; }
			TSharedRef<FJsonObject> Item = AgentJson::Obj();
			Item->SetStringField(TEXT("name"), AnimName);
			const UMovieScene* Scene = Animation->GetMovieScene();
			if (Scene)
			{
				const TRange<FFrameNumber> Range = Scene->GetPlaybackRange();
				const FFrameNumber Frames = Range.GetUpperBoundValue() - Range.GetLowerBoundValue();
				Item->SetNumberField(TEXT("seconds"), Scene->GetTickResolution().AsSeconds(FFrameTime(Frames)));
				TArray<TSharedPtr<FJsonValue>> Bindings;
				for (const FMovieSceneBinding& Binding : Scene->GetBindings())
				{
					TSharedRef<FJsonObject> B = AgentJson::Obj();
					FString WidgetName;
					if (const FMovieScenePossessable* Possessable = Scene->FindPossessable(Binding.GetObjectGuid()))
					{
						WidgetName = Possessable->GetName();
					}
					for (const FWidgetAnimationBinding& WidgetBinding : Animation->AnimationBindings)
					{
						if (WidgetBinding.AnimationGuid == Binding.GetObjectGuid())
						{
							WidgetName = WidgetBinding.WidgetName.ToString();
							if (WidgetBinding.SlotWidgetName != NAME_None) { WidgetName += TEXT(" (slot of ") + WidgetBinding.SlotWidgetName.ToString() + TEXT(")"); }
						}
					}
					B->SetStringField(TEXT("widget"), WidgetName);
					TArray<FString> Tracks;
					for (const UMovieSceneTrack* Track : Binding.GetTracks())
					{
						if (Track)
						{
							Tracks.Add(Track->GetDisplayName().ToString() + FString::Printf(TEXT(" (%d sections)"), Track->GetAllSections().Num()));
						}
					}
					AgentJson::SetStringArray(*B, TEXT("tracks"), Tracks);
					Bindings.Add(MakeShared<FJsonValueObject>(B));
				}
				Item->SetArrayField(TEXT("bindings"), Bindings);
			}
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(WidgetBlueprint);
		Json->SetArrayField(TEXT("animations"), Items);
		Json->SetStringField(TEXT("note"), TEXT("Keyframe editing is not supported (capability UMG.EditAnimation)."));
		return FAgentResult::Ok(Json);
	}

	// ------------------------------------------------------------------ preview

	FAgentResult Cmd_Preview(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UWidgetBlueprint* WidgetBlueprint = RequireWidgetBlueprint(Params, Error);
		if (!WidgetBlueprint) { return Error; }
		const int32 Width = FMath::Clamp(AgentJson::GetInt(Params, TEXT("width"), 1920), 16, 8192);
		const int32 Height = FMath::Clamp(AgentJson::GetInt(Params, TEXT("height"), 1080), 16, 8192);
		const bool bImage = AgentJson::GetBool(Params, TEXT("image"), true);
		FString OutPath = AgentJson::GetString(Params, TEXT("path"));

		if (!GEditor)
		{
			return FAgentResult::Error(AgentErrors::Internal, TEXT("No editor."));
		}
		if (!WidgetBlueprint->GeneratedClass || WidgetBlueprint->Status == BS_Dirty || WidgetBlueprint->Status == BS_Unknown)
		{
			FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		}
		if (WidgetBlueprint->Status == BS_Error || !WidgetBlueprint->GeneratedClass)
		{
			return FAgentResult::Error(AgentErrors::CompileFailed, TEXT("Widget Blueprint does not compile; fix errors before previewing."));
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return FAgentResult::Error(AgentErrors::Internal, TEXT("No editor world."));
		}
		UClass* WidgetClass = WidgetBlueprint->GeneratedClass.Get();
		UUserWidget* Instance = CreateWidget<UUserWidget>(World, WidgetClass);
		if (!Instance)
		{
			return FAgentResult::Error(AgentErrors::Internal, TEXT("CreateWidget failed."));
		}
		Instance->SetDesignerFlags(EWidgetDesignFlags::Designing | EWidgetDesignFlags::Previewing);
		TSharedRef<SWidget> Slate = Instance->TakeWidget();

		FWidgetRenderer Renderer(/*bUseGammaCorrection*/ true, /*bClearTarget*/ true);
		const FVector2D DrawSize(static_cast<float>(Width), static_cast<float>(Height));
		UTextureRenderTarget2D* RenderTarget = Renderer.CreateTargetFor(DrawSize, TextureFilter::TF_Bilinear, true);
		if (!RenderTarget)
		{
			return FAgentResult::Error(AgentErrors::Internal, TEXT("Could not create render target."));
		}
		// Two passes so layout (prepass) settles before the capture.
		Renderer.DrawWidget(RenderTarget, Slate, DrawSize, 0.0f);
		Renderer.DrawWidget(RenderTarget, Slate, DrawSize, 0.016f);
		FlushRenderingCommands();

		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(WidgetBlueprint);
		Json->SetNumberField(TEXT("width"), Width);
		Json->SetNumberField(TEXT("height"), Height);

		// Layout map: absolute rects of every named widget.
		TArray<TSharedPtr<FJsonValue>> Layout;
		if (Instance->WidgetTree)
		{
			WalkTree(Instance->WidgetTree->RootWidget, 0, [&](UWidget* Widget, int32 Depth, int32)
			{
				const FGeometry& Geometry = Widget->GetCachedGeometry();
				const FVector2D Position = Geometry.GetAbsolutePosition();
				const FVector2D Size = Geometry.GetAbsoluteSize();
				TSharedRef<FJsonObject> Item = AgentJson::Obj();
				Item->SetStringField(TEXT("widget"), Widget->GetName());
				Item->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
				Item->SetNumberField(TEXT("depth"), Depth);
				Item->SetNumberField(TEXT("x"), FMath::RoundToInt(Position.X));
				Item->SetNumberField(TEXT("y"), FMath::RoundToInt(Position.Y));
				Item->SetNumberField(TEXT("w"), FMath::RoundToInt(Size.X));
				Item->SetNumberField(TEXT("h"), FMath::RoundToInt(Size.Y));
				if (!Widget->IsVisible()) { Item->SetBoolField(TEXT("hidden"), true); }
				Layout.Add(MakeShared<FJsonValueObject>(Item));
			});
		}
		Json->SetArrayField(TEXT("layout"), Layout);

		if (bImage)
		{
			TArray<FColor> Pixels;
			FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
			if (!Resource || !Resource->ReadPixels(Pixels))
			{
				return FAgentResult::Error(AgentErrors::Internal, TEXT("ReadPixels failed."));
			}
			for (FColor& Pixel : Pixels) { Pixel.A = 255; }
			TArray64<uint8> Png;
			if (!Context.Adapter->EncodePNG(Width, Height, Pixels, Png))
			{
				return FAgentResult::Error(AgentErrors::Internal, TEXT("PNG encode failed."));
			}
			if (OutPath.IsEmpty())
			{
				const FString Dir = FPaths::Combine(FAgentConfig::GetSavedDir(), TEXT("Previews"));
				IFileManager::Get().MakeDirectory(*Dir, true);
				OutPath = FPaths::Combine(Dir, FString::Printf(TEXT("%s_%s.png"), *WidgetBlueprint->GetName(), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
			}
			if (!FFileHelper::SaveArrayToFile(TArrayView64<const uint8>(Png.GetData(), Png.Num()), *OutPath))
			{
				return FAgentResult::Error(AgentErrors::Internal, FString::Printf(TEXT("Could not write %s"), *OutPath));
			}
			Json->SetStringField(TEXT("image"), FPaths::ConvertRelativePathToFull(OutPath));
			Json->SetNumberField(TEXT("bytes"), static_cast<double>(Png.Num()));
		}
		Instance->MarkAsGarbage();
		RenderTarget->MarkAsGarbage();
		return FAgentResult::Ok(Json);
	}

	FAgentResult Cmd_PreviewDiff(const FJsonObject& Params, FAgentContext& Context)
	{
		FString PathA, PathB;
		FAgentResult Error;
		if (!AgentCmd::RequireString(Params, TEXT("a"), PathA, Error)) { return Error; }
		if (!AgentCmd::RequireString(Params, TEXT("b"), PathB, Error)) { return Error; }
		const int32 Threshold = FMath::Clamp(AgentJson::GetInt(Params, TEXT("threshold"), 12), 0, 255);
		TArray64<uint8> DataA, DataB;
		if (!FFileHelper::LoadFileToArray(DataA, *PathA)) { return FAgentResult::NotFound(TEXT("Cannot read ") + PathA); }
		if (!FFileHelper::LoadFileToArray(DataB, *PathB)) { return FAgentResult::NotFound(TEXT("Cannot read ") + PathB); }
		int32 WA, HA, WB, HB;
		TArray<FColor> PixelsA, PixelsB;
		if (!Context.Adapter->DecodePNG(DataA, WA, HA, PixelsA)) { return FAgentResult::BadRequest(TEXT("a is not a decodable PNG.")); }
		if (!Context.Adapter->DecodePNG(DataB, WB, HB, PixelsB)) { return FAgentResult::BadRequest(TEXT("b is not a decodable PNG.")); }
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetNumberField(TEXT("width"), WA);
		Json->SetNumberField(TEXT("height"), HA);
		if (WA != WB || HA != HB)
		{
			Json->SetBoolField(TEXT("size_mismatch"), true);
			Json->SetNumberField(TEXT("width_b"), WB);
			Json->SetNumberField(TEXT("height_b"), HB);
			return FAgentResult::Ok(Json);
		}
		int64 Changed = 0;
		int32 MinX = WA, MinY = HA, MaxX = -1, MaxY = -1;
		for (int32 Y = 0; Y < HA; ++Y)
		{
			for (int32 X = 0; X < WA; ++X)
			{
				const FColor& A = PixelsA[Y * WA + X];
				const FColor& B = PixelsB[Y * WA + X];
				if (FMath::Abs(A.R - B.R) > Threshold || FMath::Abs(A.G - B.G) > Threshold || FMath::Abs(A.B - B.B) > Threshold)
				{
					++Changed;
					MinX = FMath::Min(MinX, X); MinY = FMath::Min(MinY, Y);
					MaxX = FMath::Max(MaxX, X); MaxY = FMath::Max(MaxY, Y);
				}
			}
		}
		const double Percent = 100.0 * static_cast<double>(Changed) / static_cast<double>(FMath::Max<int64>(1, static_cast<int64>(WA) * HA));
		Json->SetNumberField(TEXT("changed_pixels"), static_cast<double>(Changed));
		Json->SetNumberField(TEXT("diff_percent"), FMath::RoundToDouble(Percent * 100.0) / 100.0);
		if (Changed > 0)
		{
			TSharedRef<FJsonObject> Box = AgentJson::Obj();
			Box->SetNumberField(TEXT("x"), MinX);
			Box->SetNumberField(TEXT("y"), MinY);
			Box->SetNumberField(TEXT("w"), MaxX - MinX + 1);
			Box->SetNumberField(TEXT("h"), MaxY - MinY + 1);
			Json->SetObjectField(TEXT("bbox"), Box);
		}
		return FAgentResult::Ok(Json);
	}
}

void RegisterWidgetCommands(FAgentCommandRegistry& Registry)
{
	Registry.Register(TEXT("widget.tree"), TEXT("Widget hierarchy with style fingerprints."), false, &Cmd_Tree);
	Registry.Register(TEXT("widget.inspect"), TEXT("One widget's non-default properties, slot, events."), false, &Cmd_Inspect);
	Registry.Register(TEXT("widget.clone"), TEXT("Deep-clone a widget subtree (style preserved) into a parent/after a sibling."), true, &Cmd_Clone);
	Registry.Register(TEXT("widget.add"), TEXT("Add a new widget of a class under a parent."), true, &Cmd_Add);
	Registry.Register(TEXT("widget.remove"), TEXT("Remove widgets (and their subtrees)."), true, &Cmd_Remove);
	Registry.Register(TEXT("widget.move"), TEXT("Reparent/reorder a widget."), true, &Cmd_Move);
	Registry.Register(TEXT("widget.set"), TEXT("Set widget/slot properties, rename, variable flag."), true, &Cmd_Set);
	Registry.Register(TEXT("widget.copy_style"), TEXT("Copy style properties from one widget to others."), true, &Cmd_CopyStyle);
	Registry.Register(TEXT("widget.bind_event"), TEXT("Create a bound event node (e.g. OnClicked), optionally calling/creating a function."), true, &Cmd_BindEvent);
	Registry.Register(TEXT("widget.animations"), TEXT("List animations with bindings/tracks (read-only)."), false, &Cmd_Animations);
	Registry.Register(TEXT("widget.preview"), TEXT("Render the widget to PNG + layout rectangles."), false, &Cmd_Preview);
	Registry.Register(TEXT("widget.preview_diff"), TEXT("Pixel diff between two PNGs (percent + bbox)."), false, &Cmd_PreviewDiff);
}
