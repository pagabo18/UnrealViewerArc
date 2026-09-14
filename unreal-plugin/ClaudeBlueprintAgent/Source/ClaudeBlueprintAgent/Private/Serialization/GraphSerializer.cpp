#include "GraphSerializer.h"
#include "Core/AgentJson.h"
#include "Core/AgentResolver.h"
#include "Core/PinTypeUtils.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Composite.h"
#include "K2Node_Timeline.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Knot.h"

namespace AgentGraph
{
	FString NodeKind(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return TEXT("?");
		}
		if (Node->IsA<UEdGraphNode_Comment>()) { return TEXT("Comment"); }
		if (Node->IsA<UK2Node_IfThenElse>()) { return TEXT("Branch"); }
		if (Node->IsA<UK2Node_ExecutionSequence>()) { return TEXT("Sequence"); }
		if (Node->IsA<UK2Node_Knot>()) { return TEXT("Reroute"); }
		if (Node->IsA<UK2Node_CustomEvent>()) { return TEXT("CustomEvent"); }
		FString Name = Node->GetClass()->GetName();
		Name.RemoveFromStart(TEXT("K2Node_"));
		Name.RemoveFromStart(TEXT("EdGraphNode_"));
		return Name;
	}

	FString NodeMember(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return FString();
		}
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			return Call->FunctionReference.GetMemberName().ToString();
		}
		if (const UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node))
		{
			return Var->GetVarName().ToString();
		}
		if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Node))
		{
			return Custom->CustomFunctionName.ToString();
		}
		if (const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			return Bound->ComponentPropertyName.ToString() + TEXT(".") + Bound->DelegatePropertyName.ToString();
		}
		if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
		{
			return Event->EventReference.GetMemberName().ToString();
		}
		if (const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
		{
			return CastNode->TargetType ? AgentResolver::GetClassDisplayName(CastNode->TargetType) : FString();
		}
		if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
		{
			return Macro->GetMacroGraph() ? Macro->GetMacroGraph()->GetName() : FString();
		}
		if (const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
		{
			return Composite->BoundGraph ? Composite->BoundGraph->GetName() : FString();
		}
		if (const UK2Node_Timeline* Timeline = Cast<UK2Node_Timeline>(Node))
		{
			return Timeline->TimelineName.ToString();
		}
		if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
		{
			return Entry->GetGraph() ? Entry->GetGraph()->GetName() : FString();
		}
		if (const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			return Comment->NodeComment;
		}
		return FString();
	}

	FString NodeMemberClass(const UEdGraphNode* Node)
	{
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			if (Call->FunctionReference.IsSelfContext())
			{
				return TEXT("self");
			}
			if (const UFunction* Function = Call->GetTargetFunction())
			{
				return AgentResolver::GetClassDisplayName(Function->GetOwnerClass());
			}
			if (const UClass* Parent = Call->FunctionReference.GetMemberParentClass())
			{
				return AgentResolver::GetClassDisplayName(Parent);
			}
		}
		if (const UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node))
		{
			if (Var->VariableReference.IsSelfContext())
			{
				return TEXT("self");
			}
			if (const UClass* Parent = Var->VariableReference.GetMemberParentClass())
			{
				return AgentResolver::GetClassDisplayName(Parent);
			}
		}
		if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
		{
			if (const UClass* Parent = Event->EventReference.GetMemberParentClass())
			{
				return AgentResolver::GetClassDisplayName(Parent);
			}
		}
		return FString();
	}

	FString NodeTitle(const UEdGraphNode* Node)
	{
		return Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Replace(TEXT("\n"), TEXT(" ")) : FString();
	}

	bool NodeMatchesQuery(const UEdGraphNode* Node, const FString& Query)
	{
		if (Query.IsEmpty())
		{
			return true;
		}
		return NodeKind(Node).Contains(Query) || NodeTitle(Node).Contains(Query) || NodeMember(Node).Contains(Query)
			|| NodeMemberClass(Node).Contains(Query) || Node->NodeComment.Contains(Query);
	}

	TSharedRef<FJsonObject> SerializePin(const UEdGraphPin* Pin, bool bIncludeDefaults)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetStringField(TEXT("id"), Pin->PinId.ToString(EGuidFormats::Digits));
		Json->SetStringField(TEXT("name"), Pin->PinName.ToString());
		if (!Pin->PinFriendlyName.IsEmpty() && Pin->PinFriendlyName.ToString() != Pin->PinName.ToString())
		{
			Json->SetStringField(TEXT("label"), Pin->PinFriendlyName.ToString());
		}
		Json->SetStringField(TEXT("dir"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
		Json->SetStringField(TEXT("type"), AgentPinTypes::ToString(Pin->PinType));
		if (Pin->bHidden) { Json->SetBoolField(TEXT("hidden"), true); }
		if (Pin->bAdvancedView) { Json->SetBoolField(TEXT("adv"), true); }
		if (Pin->ParentPin) { Json->SetStringField(TEXT("parent"), Pin->ParentPin->PinName.ToString()); }
		if (bIncludeDefaults && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() == 0)
		{
			if (Pin->DefaultObject)
			{
				Json->SetStringField(TEXT("default"), Pin->DefaultObject->GetPathName());
			}
			else if (!Pin->DefaultTextValue.IsEmpty())
			{
				Json->SetStringField(TEXT("default"), Pin->DefaultTextValue.ToString());
			}
			else if (!Pin->DefaultValue.IsEmpty() && !Pin->DoesDefaultValueMatchAutogenerated())
			{
				Json->SetStringField(TEXT("default"), Pin->DefaultValue);
			}
		}
		if (Pin->LinkedTo.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Links;
			for (const UEdGraphPin* Other : Pin->LinkedTo)
			{
				if (!Other || !Other->GetOwningNodeUnchecked())
				{
					continue;
				}
				TSharedRef<FJsonObject> Link = AgentJson::Obj();
				Link->SetStringField(TEXT("node"), Other->GetOwningNode()->NodeGuid.ToString(EGuidFormats::Digits));
				Link->SetStringField(TEXT("pin"), Other->PinName.ToString());
				Links.Add(MakeShared<FJsonValueObject>(Link));
			}
			Json->SetArrayField(TEXT("links"), Links);
		}
		return Json;
	}

	TSharedRef<FJsonObject> SerializeNode(const UEdGraphNode* Node, bool bIncludePins, bool bIncludeHiddenPins, bool bIncludeDefaults)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::Digits));
		Json->SetStringField(TEXT("kind"), NodeKind(Node));
		Json->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Json->SetStringField(TEXT("title"), NodeTitle(Node));
		const FString Member = NodeMember(Node);
		if (!Member.IsEmpty()) { Json->SetStringField(TEXT("member"), Member); }
		const FString MemberClass = NodeMemberClass(Node);
		if (!MemberClass.IsEmpty()) { Json->SetStringField(TEXT("member_class"), MemberClass); }
		if (const UK2Node* K2 = Cast<UK2Node>(Node))
		{
			if (K2->IsNodePure()) { Json->SetBoolField(TEXT("pure"), true); }
		}
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			if (Call->IsLatentFunction()) { Json->SetBoolField(TEXT("latent"), true); }
		}
		Json->SetNumberField(TEXT("x"), Node->NodePosX);
		Json->SetNumberField(TEXT("y"), Node->NodePosY);
		if (!Node->NodeComment.IsEmpty() && !Node->IsA<UEdGraphNode_Comment>())
		{
			Json->SetStringField(TEXT("comment"), Node->NodeComment);
		}
		if (Node->bHasCompilerMessage && !Node->ErrorMsg.IsEmpty())
		{
			Json->SetStringField(TEXT("error"), Node->ErrorMsg);
			Json->SetNumberField(TEXT("error_type"), Node->ErrorType);
		}
		if (!Node->IsNodeEnabled())
		{
			Json->SetBoolField(TEXT("disabled"), true);
		}
		if (bIncludePins)
		{
			TArray<TSharedPtr<FJsonValue>> Pins;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || (Pin->bHidden && !bIncludeHiddenPins && Pin->LinkedTo.Num() == 0))
				{
					continue;
				}
				Pins.Add(MakeShared<FJsonValueObject>(SerializePin(Pin, bIncludeDefaults)));
			}
			Json->SetArrayField(TEXT("pins"), Pins);
		}
		return Json;
	}

	void CollectNeighborhood(const UEdGraphNode* Center, int32 Depth, TSet<const UEdGraphNode*>& OutNodes)
	{
		if (!Center)
		{
			return;
		}
		TArray<const UEdGraphNode*> Frontier;
		Frontier.Add(Center);
		OutNodes.Add(Center);
		for (int32 Level = 0; Level < Depth && Frontier.Num() > 0; ++Level)
		{
			TArray<const UEdGraphNode*> Next;
			for (const UEdGraphNode* Node : Frontier)
			{
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin)
					{
						continue;
					}
					for (const UEdGraphPin* Other : Pin->LinkedTo)
					{
						const UEdGraphNode* OtherNode = Other ? Other->GetOwningNodeUnchecked() : nullptr;
						if (OtherNode && !OutNodes.Contains(OtherNode))
						{
							OutNodes.Add(OtherNode);
							Next.Add(OtherNode);
						}
					}
				}
			}
			Frontier = MoveTemp(Next);
		}
	}

	int32 CountNodes(const UEdGraph* Graph)
	{
		return Graph ? Graph->Nodes.Num() : 0;
	}

	TSharedRef<FJsonObject> SerializeGraphSummary(UBlueprint* Blueprint, UEdGraph* Graph)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetStringField(TEXT("name"), Graph->GetName());
		Json->SetStringField(TEXT("kind"), AgentResolver::GraphKind(Blueprint, Graph));
		Json->SetNumberField(TEXT("nodes"), Graph->Nodes.Num());
		return Json;
	}

	TSharedRef<FJsonObject> SerializeFunctionSignature(UEdGraph* Graph)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		TArray<TSharedPtr<FJsonValue>> Inputs;
		TArray<TSharedPtr<FJsonValue>> Outputs;
		bool bPure = false;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				bPure = (Entry->GetExtraFlags() & FUNC_BlueprintPure) != 0;
				for (const UEdGraphPin* Pin : Entry->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && !Pin->bHidden && !Pin->ParentPin)
					{
						Inputs.Add(MakeShared<FJsonValueString>(Pin->PinName.ToString() + TEXT(":") + AgentPinTypes::ToString(Pin->PinType)));
					}
				}
			}
			else if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
			{
				if (Outputs.Num() == 0)
				{
					for (const UEdGraphPin* Pin : Result->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && !Pin->bHidden && !Pin->ParentPin)
						{
							Outputs.Add(MakeShared<FJsonValueString>(Pin->PinName.ToString() + TEXT(":") + AgentPinTypes::ToString(Pin->PinType)));
						}
					}
				}
			}
		}
		Json->SetArrayField(TEXT("inputs"), Inputs);
		Json->SetArrayField(TEXT("outputs"), Outputs);
		Json->SetBoolField(TEXT("pure"), bPure);
		return Json;
	}

	TSharedRef<FJsonObject> SerializeGraph(UBlueprint* Blueprint, UEdGraph* Graph, const FGraphFilter& Filter)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetStringField(TEXT("asset"), AgentResolver::PackagePathOf(Blueprint));
		Json->SetStringField(TEXT("graph"), Graph->GetName());
		Json->SetStringField(TEXT("kind"), AgentResolver::GraphKind(Blueprint, Graph));
		Json->SetNumberField(TEXT("total_nodes"), Graph->Nodes.Num());
		if (AgentResolver::GraphKind(Blueprint, Graph) == TEXT("function"))
		{
			Json->SetObjectField(TEXT("signature"), SerializeFunctionSignature(Graph));
		}

		// Candidate set
		TArray<const UEdGraphNode*> Candidates;
		if (!Filter.AroundGuid.IsEmpty())
		{
			FString Error;
			UEdGraphNode* Center = AgentResolver::FindNode(Blueprint, Graph, Filter.AroundGuid, Error);
			if (!Center)
			{
				Json->SetStringField(TEXT("error"), Error);
				Json->SetArrayField(TEXT("nodes"), {});
				return Json;
			}
			TSet<const UEdGraphNode*> Neighborhood;
			CollectNeighborhood(Center, FMath::Max(0, Filter.Depth), Neighborhood);
			// Preserve graph order for determinism
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Neighborhood.Contains(Node))
				{
					Candidates.Add(Node);
				}
			}
			Json->SetStringField(TEXT("around"), Filter.AroundGuid);
			Json->SetNumberField(TEXT("depth"), Filter.Depth);
		}
		else
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node)
				{
					Candidates.Add(Node);
				}
			}
		}

		// Query / kind filter
		TArray<const UEdGraphNode*> Matched;
		for (const UEdGraphNode* Node : Candidates)
		{
			if (!Filter.Kind.IsEmpty() && !NodeKind(Node).Equals(Filter.Kind, ESearchCase::IgnoreCase) && !Node->GetClass()->GetName().Contains(Filter.Kind))
			{
				continue;
			}
			if (!NodeMatchesQuery(Node, Filter.Query))
			{
				continue;
			}
			Matched.Add(Node);
		}

		// Stable order: by position (top-to-bottom, left-to-right) which mirrors reading order.
		Matched.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
		{
			if (A.NodePosY != B.NodePosY) { return A.NodePosY < B.NodePosY; }
			if (A.NodePosX != B.NodePosX) { return A.NodePosX < B.NodePosX; }
			return A.NodeGuid < B.NodeGuid;
		});

		TArray<TSharedPtr<FJsonValue>> Nodes;
		const int32 End = FMath::Min(Matched.Num(), Filter.Offset + Filter.Limit);
		for (int32 Index = Filter.Offset; Index < End; ++Index)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(SerializeNode(Matched[Index], Filter.bIncludePins, Filter.bIncludeHiddenPins, Filter.bIncludeDefaults)));
		}
		Json->SetArrayField(TEXT("nodes"), Nodes);
		Json->SetNumberField(TEXT("matched"), Matched.Num());
		Json->SetNumberField(TEXT("offset"), Filter.Offset);
		Json->SetBoolField(TEXT("truncated"), End < Matched.Num());
		return Json;
	}
}
