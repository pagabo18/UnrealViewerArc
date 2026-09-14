// graph.* commands: windowed inspection, search, node creation (typed and
// generic), deletion, connections, pin defaults, insert/replace/clone macros.
#include "CommandHelpers.h"
#include "Core/AgentCommandRegistry.h"
#include "Core/PinTypeUtils.h"
#include "Serialization/GraphSerializer.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphNode_Comment.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Self.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Knot.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_CreateWidget.h"
#include "K2Node_Literal.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Timeline.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "UObject/UnrealType.h"

namespace
{
	struct FGraphTarget
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
	};

	bool ResolveTarget(const FJsonObject& Params, FGraphTarget& Out, FAgentResult& OutError, bool bGraphRequired = true)
	{
		Out.Blueprint = AgentCmd::RequireBlueprint(Params, OutError);
		if (!Out.Blueprint)
		{
			return false;
		}
		const FString GraphName = AgentJson::GetString(Params, TEXT("graph"));
		FString Error;
		Out.Graph = AgentResolver::FindGraph(Out.Blueprint, GraphName, Error);
		if (!Out.Graph && bGraphRequired)
		{
			OutError = FAgentResult::NotFound(Error);
			return false;
		}
		return true;
	}

	FString Guid(const UEdGraphNode* Node)
	{
		return Node ? Node->NodeGuid.ToString(EGuidFormats::Digits) : FString();
	}

	/** Parses "GUID.PinName" / "GUID:PinName" / "GUID" (default exec pin). */
	bool SplitPinRef(const FString& Ref, FString& OutNode, FString& OutPin)
	{
		FString R = Ref.TrimStartAndEnd();
		int32 Sep = INDEX_NONE;
		if (R.FindChar(TEXT('.'), Sep) || R.FindChar(TEXT(':'), Sep))
		{
			OutNode = R.Left(Sep);
			OutPin = R.Mid(Sep + 1);
		}
		else
		{
			OutNode = R;
			OutPin.Empty();
		}
		return !OutNode.IsEmpty();
	}

	UEdGraphPin* ResolvePinRef(const FGraphTarget& Target, const FString& Ref, EEdGraphPinDirection Direction, FString& OutError, UEdGraphNode** OutNode = nullptr)
	{
		FString NodeRef, PinRef;
		if (!SplitPinRef(Ref, NodeRef, PinRef))
		{
			OutError = FString::Printf(TEXT("Bad pin reference '%s'."), *Ref);
			return nullptr;
		}
		UEdGraphNode* Node = AgentResolver::FindNode(Target.Blueprint, Target.Graph, NodeRef, OutError);
		if (!Node)
		{
			return nullptr;
		}
		if (OutNode) { *OutNode = Node; }
		return AgentResolver::FindPin(Node, PinRef, Direction, OutError);
	}

	/** Generic node placement: outer graph, guid, default pins. Call after configuring member refs. */
	template <typename T>
	T* PlaceNode(UEdGraph* Graph, int32 X, int32 Y, TFunctionRef<void(T*)> Configure)
	{
		Graph->Modify();
		T* Node = NewObject<T>(Graph, NAME_None, RF_Transactional);
		Configure(Node);
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Graph->AddNode(Node, /*bUserAction*/ false, /*bSelectNewNode*/ false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	UEdGraphNode* PlaceGenericNode(UEdGraph* Graph, UClass* NodeClass, int32 X, int32 Y)
	{
		Graph->Modify();
		UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	/** Chooses a position: after a node, or a free spot below the graph's last node. */
	void ChoosePosition(UEdGraph* Graph, const UEdGraphNode* Anchor, int32& OutX, int32& OutY)
	{
		if (Anchor)
		{
			OutX = Anchor->NodePosX + 320;
			OutY = Anchor->NodePosY;
			return;
		}
		int32 MaxY = 0;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node) { MaxY = FMath::Max(MaxY, Node->NodePosY); }
		}
		OutX = 0;
		OutY = Graph->Nodes.Num() > 0 ? MaxY + 200 : 0;
	}

	UEdGraphPin* FirstPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const FName& Category)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction && Pin->PinType.PinCategory == Category && !Pin->bHidden)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/**
	 * Creates a node from a "type" spec. Supported types:
	 *  function_call (function=..., self=true|false), variable_get/variable_set (variable=..., class=...),
	 *  event (event=..., class=...), custom_event (name=..., inputs=[...]), branch, sequence, cast (class=...),
	 *  self, macro (macro=... e.g. ForEachLoop), reroute, comment (text=...), make_struct/break_struct (struct=...),
	 *  switch_enum (enum=...), switch_string, switch_int, spawn_actor (class=...), create_widget (class=...),
	 *  parent_call (function=...), print (text=...), delay (duration=...), raw (class=K2Node_...)
	 */
	FAgentResult CreateNodeFromSpec(const FGraphTarget& Target, const FJsonObject& Spec, FAgentContext& Context, int32 X, int32 Y, UEdGraphNode*& OutNode)
	{
		OutNode = nullptr;
		UBlueprint* Blueprint = Target.Blueprint;
		UEdGraph* Graph = Target.Graph;
		const FString Type = AgentJson::GetString(Spec, TEXT("type")).ToLower().Replace(TEXT("-"), TEXT("_"));

		if (Type == TEXT("function_call") || Type == TEXT("call") || Type == TEXT("function"))
		{
			const FString FunctionSpec = AgentJson::GetString(Spec, TEXT("function"));
			FString Error;
			UFunction* Function = AgentResolver::ResolveFunction(Blueprint, *Context.Adapter, FunctionSpec, Error);
			if (!Function)
			{
				return FAgentResult::NotFound(Error);
			}
			OutNode = PlaceNode<UK2Node_CallFunction>(Graph, X, Y, [Function](UK2Node_CallFunction* Node)
			{
				Node->SetFromFunction(Function);
			});
			return FAgentResult::Ok();
		}
		if (Type == TEXT("parent_call"))
		{
			const FString FunctionSpec = AgentJson::GetString(Spec, TEXT("function"));
			UFunction* Function = Blueprint->ParentClass ? Blueprint->ParentClass->FindFunctionByName(*FunctionSpec) : nullptr;
			if (!Function)
			{
				return FAgentResult::NotFound(FString::Printf(TEXT("Parent function '%s' not found."), *FunctionSpec));
			}
			OutNode = PlaceNode<UK2Node_CallParentFunction>(Graph, X, Y, [Function](UK2Node_CallParentFunction* Node)
			{
				Node->SetFromFunction(Function);
			});
			return FAgentResult::Ok();
		}
		if (Type == TEXT("variable_get") || Type == TEXT("get") || Type == TEXT("variable_set") || Type == TEXT("set"))
		{
			const bool bSet = Type == TEXT("variable_set") || Type == TEXT("set");
			const FString VarName = AgentJson::GetString(Spec, TEXT("variable"));
			if (VarName.IsEmpty())
			{
				return FAgentResult::BadRequest(TEXT("'variable' is required."));
			}
			const FString ClassSpec = AgentJson::GetString(Spec, TEXT("class"));
			UClass* OwnerClass = nullptr;
			if (!ClassSpec.IsEmpty() && !ClassSpec.Equals(TEXT("self"), ESearchCase::IgnoreCase))
			{
				OwnerClass = Context.Adapter->ResolveClass(ClassSpec);
				if (!OwnerClass)
				{
					return FAgentResult::NotFound(FString::Printf(TEXT("Class '%s' not found."), *ClassSpec));
				}
			}
			// Validate the variable exists (self: skeleton class; external: class)
			UClass* CheckClass = OwnerClass ? OwnerClass : (Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get());
			FProperty* Property = CheckClass ? CheckClass->FindPropertyByName(*VarName) : nullptr;
			if (!Property && !OwnerClass)
			{
				// May be a local variable of a function graph; allow if the graph declares it.
				bool bLocal = false;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
					{
						for (const FBPVariableDescription& Local : Entry->LocalVariables)
						{
							if (Local.VarName == *VarName) { bLocal = true; }
						}
					}
				}
				if (!bLocal)
				{
					return FAgentResult::NotFound(FString::Printf(TEXT("Variable '%s' not found on %s."), *VarName, *Blueprint->GetName()));
				}
			}
			auto Configure = [&](UK2Node_Variable* Node)
			{
				if (OwnerClass)
				{
					Node->VariableReference.SetExternalMember(FName(*VarName), OwnerClass);
				}
				else if (Property && Property->GetOwnerClass() && !Blueprint->GeneratedClass->IsChildOf(Property->GetOwnerClass()) )
				{
					Node->VariableReference.SetExternalMember(FName(*VarName), Property->GetOwnerClass());
				}
				else if (Property && Property->GetOwnerStruct() && !Cast<UClass>(Property->GetOwnerStruct()))
				{
					Node->VariableReference.SetLocalMember(FName(*VarName), Property->GetOwnerStruct(), FGuid());
				}
				else
				{
					Node->VariableReference.SetSelfMember(FName(*VarName));
				}
			};
			if (bSet)
			{
				OutNode = PlaceNode<UK2Node_VariableSet>(Graph, X, Y, [&](UK2Node_VariableSet* Node) { Configure(Node); });
			}
			else
			{
				OutNode = PlaceNode<UK2Node_VariableGet>(Graph, X, Y, [&](UK2Node_VariableGet* Node) { Configure(Node); });
			}
			return FAgentResult::Ok();
		}
		if (Type == TEXT("event"))
		{
			const FString EventName = AgentJson::GetString(Spec, TEXT("event"), AgentJson::GetString(Spec, TEXT("name")));
			const FString ClassSpec = AgentJson::GetString(Spec, TEXT("class"));
			UClass* EventClass = ClassSpec.IsEmpty() ? Blueprint->ParentClass.Get() : Context.Adapter->ResolveClass(ClassSpec);
			if (!EventClass)
			{
				return FAgentResult::NotFound(FString::Printf(TEXT("Class '%s' not found."), *ClassSpec));
			}
			UFunction* Function = EventClass->FindFunctionByName(*EventName);
			if (!Function || !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
			{
				return FAgentResult::NotFound(FString::Printf(TEXT("'%s' is not an overridable event on %s."), *EventName, *EventClass->GetName()));
			}
			if (const UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, EventClass, *EventName))
			{
				return FAgentResult::Error(AgentErrors::Conflict, FString::Printf(TEXT("Event %s already exists (node %s)."), *EventName, *Guid(Existing)));
			}
			int32 PosY = Y;
			UK2Node_Event* EventNode = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph, *EventName, EventClass, PosY);
			if (!EventNode)
			{
				return FAgentResult::Error(AgentErrors::Internal, TEXT("Could not add event node."));
			}
			EventNode->NodePosX = X;
			EventNode->NodePosY = Y;
			OutNode = EventNode;
			return FAgentResult::Ok();
		}
		if (Type == TEXT("custom_event"))
		{
			const FString Name = AgentJson::GetString(Spec, TEXT("name"), AgentJson::GetString(Spec, TEXT("event")));
			if (Name.IsEmpty())
			{
				return FAgentResult::BadRequest(TEXT("custom_event needs 'name'."));
			}
			UK2Node_CustomEvent* Node = PlaceNode<UK2Node_CustomEvent>(Graph, X, Y, [&Name](UK2Node_CustomEvent* N)
			{
				N->CustomFunctionName = FName(*Name);
			});
			const TArray<TSharedPtr<FJsonValue>>* Inputs = AgentJson::GetArray(Spec, TEXT("inputs"));
			if (Inputs)
			{
				for (const TSharedPtr<FJsonValue>& Item : *Inputs)
				{
					FString PinName, TypeSpec;
					const FString S = Item->Type == EJson::String ? Item->AsString() : FString();
					if (!S.Split(TEXT(":"), &PinName, &TypeSpec))
					{
						return FAgentResult::BadRequest(FString::Printf(TEXT("Input spec '%s' must be Name:type."), *S));
					}
					FEdGraphPinType PinType;
					FString TypeError;
					if (!AgentPinTypes::Parse(TypeSpec, *Context.Adapter, PinType, TypeError))
					{
						return FAgentResult::BadRequest(TypeError);
					}
					Node->CreateUserDefinedPin(FName(*PinName), PinType, EGPD_Output);
				}
			}
			OutNode = Node;
			return FAgentResult::Ok();
		}
		if (Type == TEXT("branch") || Type == TEXT("if"))
		{
			OutNode = PlaceNode<UK2Node_IfThenElse>(Graph, X, Y, [](UK2Node_IfThenElse*) {});
			return FAgentResult::Ok();
		}
		if (Type == TEXT("sequence"))
		{
			OutNode = PlaceNode<UK2Node_ExecutionSequence>(Graph, X, Y, [](UK2Node_ExecutionSequence*) {});
			const int32 Outputs = AgentJson::GetInt(Spec, TEXT("outputs"), 2);
			for (int32 Index = 2; Index < Outputs; ++Index)
			{
				Cast<UK2Node_ExecutionSequence>(OutNode)->AddPinToExecutionNode();
			}
			return FAgentResult::Ok();
		}
		if (Type == TEXT("cast"))
		{
			UClass* TargetClass = Context.Adapter->ResolveClass(AgentJson::GetString(Spec, TEXT("class")));
			if (!TargetClass)
			{
				return FAgentResult::NotFound(TEXT("cast needs a valid 'class'."));
			}
			const bool bPure = AgentJson::GetBool(Spec, TEXT("pure"), false);
			OutNode = PlaceNode<UK2Node_DynamicCast>(Graph, X, Y, [TargetClass, bPure](UK2Node_DynamicCast* Node)
			{
				Node->TargetType = TargetClass;
				Node->SetPurity(bPure);
			});
			return FAgentResult::Ok();
		}
		if (Type == TEXT("self"))
		{
			OutNode = PlaceNode<UK2Node_Self>(Graph, X, Y, [](UK2Node_Self*) {});
			return FAgentResult::Ok();
		}
		if (Type == TEXT("reroute") || Type == TEXT("knot"))
		{
			OutNode = PlaceNode<UK2Node_Knot>(Graph, X, Y, [](UK2Node_Knot*) {});
			return FAgentResult::Ok();
		}
		if (Type == TEXT("comment"))
		{
			const FString Text = AgentJson::GetString(Spec, TEXT("text"), TEXT("Comment"));
			UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(PlaceGenericNode(Graph, UEdGraphNode_Comment::StaticClass(), X, Y));
			Comment->NodeComment = Text;
			Comment->NodeWidth = AgentJson::GetInt(Spec, TEXT("width"), 400);
			Comment->NodeHeight = AgentJson::GetInt(Spec, TEXT("height"), 200);
			OutNode = Comment;
			return FAgentResult::Ok();
		}
		if (Type == TEXT("macro"))
		{
			const FString MacroName = AgentJson::GetString(Spec, TEXT("macro"), AgentJson::GetString(Spec, TEXT("name")));
			UEdGraph* MacroGraph = nullptr;
			for (UEdGraph* Candidate : Blueprint->MacroGraphs)
			{
				if (Candidate && Candidate->GetName().Equals(MacroName, ESearchCase::IgnoreCase)) { MacroGraph = Candidate; }
			}
			if (!MacroGraph)
			{
				// Standard macros live in /Engine/EditorBlueprintResources/StandardMacros
				if (UBlueprint* StandardMacros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros")))
				{
					for (UEdGraph* Candidate : StandardMacros->MacroGraphs)
					{
						if (Candidate && Candidate->GetName().Equals(MacroName, ESearchCase::IgnoreCase)) { MacroGraph = Candidate; }
					}
				}
			}
			if (!MacroGraph)
			{
				return FAgentResult::NotFound(FString::Printf(TEXT("Macro '%s' not found (blueprint macros or StandardMacros: ForEachLoop, ForLoop, WhileLoop, DoOnce, Gate, FlipFlop, IsValid...)."), *MacroName));
			}
			OutNode = PlaceNode<UK2Node_MacroInstance>(Graph, X, Y, [MacroGraph](UK2Node_MacroInstance* Node)
			{
				Node->SetMacroGraph(MacroGraph);
			});
			return FAgentResult::Ok();
		}
		if (Type == TEXT("make_struct") || Type == TEXT("break_struct"))
		{
			UScriptStruct* Struct = Context.Adapter->ResolveStruct(AgentJson::GetString(Spec, TEXT("struct")));
			if (!Struct)
			{
				return FAgentResult::NotFound(TEXT("needs a valid 'struct'."));
			}
			if (Type == TEXT("make_struct"))
			{
				OutNode = PlaceNode<UK2Node_MakeStruct>(Graph, X, Y, [Struct](UK2Node_MakeStruct* Node) { Node->StructType = Struct; });
			}
			else
			{
				OutNode = PlaceNode<UK2Node_BreakStruct>(Graph, X, Y, [Struct](UK2Node_BreakStruct* Node) { Node->StructType = Struct; });
			}
			return FAgentResult::Ok();
		}
		if (Type == TEXT("switch_enum"))
		{
			UEnum* Enum = Context.Adapter->ResolveEnum(AgentJson::GetString(Spec, TEXT("enum")));
			if (!Enum)
			{
				return FAgentResult::NotFound(TEXT("switch_enum needs a valid 'enum'."));
			}
			OutNode = PlaceNode<UK2Node_SwitchEnum>(Graph, X, Y, [Enum](UK2Node_SwitchEnum* Node) { Node->SetEnum(Enum); });
			return FAgentResult::Ok();
		}
		if (Type == TEXT("switch_string"))
		{
			TArray<FString> Cases = AgentJson::GetStringArray(Spec, TEXT("cases"));
			OutNode = PlaceNode<UK2Node_SwitchString>(Graph, X, Y, [&Cases](UK2Node_SwitchString* Node)
			{
				for (const FString& Case : Cases) { Node->PinNames.Add(FName(*Case)); }
			});
			return FAgentResult::Ok();
		}
		if (Type == TEXT("switch_int"))
		{
			OutNode = PlaceNode<UK2Node_SwitchInteger>(Graph, X, Y, [](UK2Node_SwitchInteger*) {});
			return FAgentResult::Ok();
		}
		if (Type == TEXT("spawn_actor"))
		{
			OutNode = PlaceNode<UK2Node_SpawnActorFromClass>(Graph, X, Y, [](UK2Node_SpawnActorFromClass*) {});
			const FString ClassSpec = AgentJson::GetString(Spec, TEXT("class"));
			if (!ClassSpec.IsEmpty())
			{
				if (UClass* Class = Context.Adapter->ResolveClass(ClassSpec))
				{
					if (UEdGraphPin* ClassPin = OutNode->FindPin(TEXT("Class")))
					{
						Graph->GetSchema()->TrySetDefaultObject(*ClassPin, Class);
						OutNode->ReconstructNode();
					}
				}
			}
			return FAgentResult::Ok();
		}
		if (Type == TEXT("create_widget"))
		{
			OutNode = PlaceNode<UK2Node_CreateWidget>(Graph, X, Y, [](UK2Node_CreateWidget*) {});
			const FString ClassSpec = AgentJson::GetString(Spec, TEXT("class"));
			if (!ClassSpec.IsEmpty())
			{
				if (UClass* Class = Context.Adapter->ResolveClass(ClassSpec))
				{
					if (UEdGraphPin* ClassPin = OutNode->FindPin(TEXT("Class")))
					{
						Graph->GetSchema()->TrySetDefaultObject(*ClassPin, Class);
						OutNode->ReconstructNode();
					}
				}
			}
			return FAgentResult::Ok();
		}
		if (Type == TEXT("print"))
		{
			UFunction* Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"));
			OutNode = PlaceNode<UK2Node_CallFunction>(Graph, X, Y, [Function](UK2Node_CallFunction* Node) { Node->SetFromFunction(Function); });
			if (UEdGraphPin* Pin = OutNode->FindPin(TEXT("InString")))
			{
				Graph->GetSchema()->TrySetDefaultValue(*Pin, AgentJson::GetString(Spec, TEXT("text"), TEXT("Hello")));
			}
			return FAgentResult::Ok();
		}
		if (Type == TEXT("delay"))
		{
			UFunction* Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("Delay"));
			OutNode = PlaceNode<UK2Node_CallFunction>(Graph, X, Y, [Function](UK2Node_CallFunction* Node) { Node->SetFromFunction(Function); });
			if (UEdGraphPin* Pin = OutNode->FindPin(TEXT("Duration")))
			{
				Graph->GetSchema()->TrySetDefaultValue(*Pin, FString::SanitizeFloat(AgentJson::GetNumber(Spec, TEXT("duration"), 1.0)));
			}
			return FAgentResult::Ok();
		}
		if (Type == TEXT("raw"))
		{
			const FString ClassName = AgentJson::GetString(Spec, TEXT("class"));
			UClass* NodeClass = Context.Adapter->ResolveClass(ClassName);
			if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
			{
				return FAgentResult::NotFound(FString::Printf(TEXT("'%s' is not an EdGraphNode class."), *ClassName));
			}
			OutNode = PlaceGenericNode(Graph, NodeClass, X, Y);
			return FAgentResult::Ok();
		}
		return FAgentResult::Unsupported(FString::Printf(TEXT("Unknown node type '%s'."), *Type), TEXT("Blueprint.EditGraph"));
	}

	/** Applies {"pin": "value"} defaults to a node; returns failures. */
	void ApplyPinDefaults(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Defaults, FAgentContext& Context, TArray<FString>& OutFailed)
	{
		if (!Defaults.IsValid() || !Node)
		{
			return;
		}
		const UEdGraphSchema* Schema = Node->GetGraph()->GetSchema();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Defaults->Values)
		{
			FString Error;
			UEdGraphPin* Pin = AgentResolver::FindPin(Node, Pair.Key, EGPD_Input, Error);
			if (!Pin)
			{
				OutFailed.Add(Error);
				continue;
			}
			const FString Value = Pair.Value->Type == EJson::String ? Pair.Value->AsString() : AgentJson::SerializeValue(Pair.Value);
			Node->Modify();
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
			{
				UObject* Object = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ? static_cast<UObject*>(Context.Adapter->ResolveClass(Value)) : nullptr;
				if (!Object)
				{
					FString LoadError;
					Object = AgentResolver::LoadAsset(Value, LoadError);
				}
				if (!Object && !Value.IsEmpty() && Value != TEXT("None"))
				{
					OutFailed.Add(FString::Printf(TEXT("%s: object '%s' not found."), *Pair.Key, *Value));
					continue;
				}
				Schema->TrySetDefaultObject(*Pin, Object);
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
			{
				Schema->TrySetDefaultText(*Pin, FText::FromString(Value));
			}
			else
			{
				Schema->TrySetDefaultValue(*Pin, Value);
			}
		}
	}

	/** Connects two pins with schema checks; reports what would be broken. */
	FAgentResult ConnectPins(UEdGraphPin* From, UEdGraphPin* To, bool bForce, TSharedRef<FJsonObject>& OutInfo)
	{
		const UEdGraphSchema* Schema = From->GetOwningNode()->GetGraph()->GetSchema();
		const FPinConnectionResponse Response = Schema->CanCreateConnection(From, To);
		OutInfo->SetStringField(TEXT("from"), Guid(From->GetOwningNode()) + TEXT(".") + From->PinName.ToString());
		OutInfo->SetStringField(TEXT("to"), Guid(To->GetOwningNode()) + TEXT(".") + To->PinName.ToString());
		switch (Response.Response)
		{
		case CONNECT_RESPONSE_DISALLOW:
			return FAgentResult::Error(AgentErrors::Conflict, FString::Printf(TEXT("Cannot connect %s -> %s: %s"), *From->PinName.ToString(), *To->PinName.ToString(), *Response.Message.ToString()));
		case CONNECT_RESPONSE_BREAK_OTHERS_A:
		case CONNECT_RESPONSE_BREAK_OTHERS_B:
		case CONNECT_RESPONSE_BREAK_OTHERS_AB:
		{
			const UEdGraphPin* Victim = Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A ? From : To;
			FString Existing;
			for (const UEdGraphPin* Linked : Victim->LinkedTo)
			{
				if (Linked) { Existing += (Existing.IsEmpty() ? TEXT("") : TEXT(", ")) + Guid(Linked->GetOwningNode()) + TEXT(".") + Linked->PinName.ToString(); }
			}
			if (!bForce)
			{
				TSharedPtr<FJsonObject> Details = AgentJson::Obj();
				Details->SetStringField(TEXT("existing"), Existing);
				Details->SetStringField(TEXT("hint"), TEXT("Pass force=true to replace the existing link."));
				return FAgentResult::Error(AgentErrors::Conflict, FString::Printf(TEXT("Pin %s already connected (%s)."), *Victim->PinName.ToString(), *Existing), Details);
			}
			OutInfo->SetStringField(TEXT("replaced"), Existing);
			break;
		}
		case CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE:
			OutInfo->SetBoolField(TEXT("conversion_node"), true);
			break;
		default:
			break;
		}
		From->GetOwningNode()->Modify();
		To->GetOwningNode()->Modify();
		if (!Schema->TryCreateConnection(From, To))
		{
			return FAgentResult::Error(AgentErrors::Conflict, FString::Printf(TEXT("TryCreateConnection failed for %s -> %s."), *From->PinName.ToString(), *To->PinName.ToString()));
		}
		return FAgentResult::Ok(OutInfo);
	}

	// ------------------------------------------------------------------ commands

	FAgentResult Inspect(const FJsonObject& Params, FAgentContext& Context)
	{
		FGraphTarget Target;
		FAgentResult Error;
		if (!ResolveTarget(Params, Target, Error)) { return Error; }
		AgentGraph::FGraphFilter Filter;
		Filter.AroundGuid = AgentJson::GetString(Params, TEXT("around"));
		Filter.Depth = AgentJson::GetInt(Params, TEXT("depth"), 1);
		Filter.Query = AgentJson::GetString(Params, TEXT("query"));
		Filter.Kind = AgentJson::GetString(Params, TEXT("kind"));
		Filter.Offset = FMath::Max(0, AgentJson::GetInt(Params, TEXT("offset"), 0));
		Filter.Limit = FMath::Clamp(AgentJson::GetInt(Params, TEXT("limit"), 200), 1, 5000);
		Filter.bIncludePins = AgentJson::GetBool(Params, TEXT("pins"), true);
		Filter.bIncludeHiddenPins = AgentJson::GetBool(Params, TEXT("hidden_pins"), false);
		Filter.bIncludeDefaults = AgentJson::GetBool(Params, TEXT("defaults"), true);
		return FAgentResult::Ok(AgentGraph::SerializeGraph(Target.Blueprint, Target.Graph, Filter));
	}

	FAgentResult Graphs(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UBlueprint* Blueprint = AgentCmd::RequireBlueprint(Params, Error);
		if (!Blueprint) { return Error; }
		TArray<UEdGraph*> AllGraphs;
		AgentResolver::GetAllGraphs(Blueprint, AllGraphs);
		TArray<TSharedPtr<FJsonValue>> Items;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph) { Items.Add(MakeShared<FJsonValueObject>(AgentGraph::SerializeGraphSummary(Blueprint, Graph))); }
		}
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Blueprint);
		Json->SetArrayField(TEXT("graphs"), Items);
		return FAgentResult::Ok(Json);
	}

	FAgentResult FindNodes(const FJsonObject& Params, FAgentContext& Context)
	{
		FGraphTarget Target;
		FAgentResult Error;
		if (!ResolveTarget(Params, Target, Error, /*bGraphRequired*/ false)) { return Error; }
		AgentGraph::FGraphFilter Filter;
		Filter.Query = AgentJson::GetString(Params, TEXT("query"));
		Filter.Kind = AgentJson::GetString(Params, TEXT("kind"));
		Filter.Limit = FMath::Clamp(AgentJson::GetInt(Params, TEXT("limit"), 50), 1, 1000);
		Filter.bIncludePins = AgentJson::GetBool(Params, TEXT("pins"), false);
		if (Filter.Query.IsEmpty() && Filter.Kind.IsEmpty())
		{
			return FAgentResult::BadRequest(TEXT("Provide 'query' and/or 'kind'."));
		}
		TArray<UEdGraph*> GraphsToSearch;
		if (Target.Graph && !AgentJson::GetString(Params, TEXT("graph")).IsEmpty())
		{
			GraphsToSearch.Add(Target.Graph);
		}
		else
		{
			AgentResolver::GetAllGraphs(Target.Blueprint, GraphsToSearch);
		}
		TArray<TSharedPtr<FJsonValue>> Items;
		int32 Matched = 0;
		for (UEdGraph* Graph : GraphsToSearch)
		{
			if (!Graph) { continue; }
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) { continue; }
				if (!Filter.Kind.IsEmpty() && !AgentGraph::NodeKind(Node).Equals(Filter.Kind, ESearchCase::IgnoreCase) && !Node->GetClass()->GetName().Contains(Filter.Kind))
				{
					continue;
				}
				if (!AgentGraph::NodeMatchesQuery(Node, Filter.Query))
				{
					continue;
				}
				++Matched;
				if (Items.Num() < Filter.Limit)
				{
					TSharedRef<FJsonObject> Item = AgentGraph::SerializeNode(Node, Filter.bIncludePins, false, true);
					Item->SetStringField(TEXT("graph"), Graph->GetName());
					Items.Add(MakeShared<FJsonValueObject>(Item));
				}
			}
		}
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Target.Blueprint);
		Json->SetArrayField(TEXT("nodes"), Items);
		Json->SetNumberField(TEXT("matched"), Matched);
		Json->SetBoolField(TEXT("truncated"), Matched > Items.Num());
		return FAgentResult::Ok(Json);
	}

	FAgentResult AddNode(const FJsonObject& Params, FAgentContext& Context)
	{
		FGraphTarget Target;
		FAgentResult Error;
		if (!ResolveTarget(Params, Target, Error)) { return Error; }

		// Optional: insert after/before an existing exec node
		const FString After = AgentJson::GetString(Params, TEXT("after"));
		const FString Before = AgentJson::GetString(Params, TEXT("before"));
		UEdGraphNode* AfterNode = nullptr;
		UEdGraphPin* AfterPin = nullptr;
		UEdGraphNode* BeforeNode = nullptr;
		UEdGraphPin* BeforePin = nullptr;
		FString RefError;
		if (!After.IsEmpty())
		{
			AfterPin = ResolvePinRef(Target, After, EGPD_Output, RefError, &AfterNode);
			if (!AfterPin) { return FAgentResult::NotFound(RefError); }
		}
		if (!Before.IsEmpty())
		{
			BeforePin = ResolvePinRef(Target, Before, EGPD_Input, RefError, &BeforeNode);
			if (!BeforePin) { return FAgentResult::NotFound(RefError); }
		}

		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Add node ") + AgentJson::GetString(Params, TEXT("type"))));
		Target.Blueprint->Modify();
		int32 X, Y;
		ChoosePosition(Target.Graph, AfterNode ? AfterNode : BeforeNode, X, Y);
		X = AgentJson::GetInt(Params, TEXT("x"), X);
		Y = AgentJson::GetInt(Params, TEXT("y"), Y);

		UEdGraphNode* Node = nullptr;
		FAgentResult Created = CreateNodeFromSpec(Target, Params, Context, X, Y, Node);
		if (!Created.bOk || !Node)
		{
			return Created.bOk ? FAgentResult::Error(AgentErrors::Internal, TEXT("Node creation returned null.")) : Created;
		}
		TArray<FString> Failed;
		ApplyPinDefaults(Node, AgentJson::GetObject(Params, TEXT("pins")), Context, Failed);
		if (!AgentJson::GetString(Params, TEXT("comment")).IsEmpty())
		{
			Node->NodeComment = AgentJson::GetString(Params, TEXT("comment"));
			Node->bCommentBubbleVisible = true;
		}

		TSharedRef<FJsonObject> Json = AgentGraph::SerializeNode(Node, true, false, true);
		Json->SetStringField(TEXT("graph"), Target.Graph->GetName());
		TArray<TSharedPtr<FJsonValue>> Connections;

		// Exec splicing: after.then -> [old target] becomes after.then -> new.exec ; new.then -> old target
		UEdGraphPin* NewExecIn = FirstPin(Node, EGPD_Input, UEdGraphSchema_K2::PC_Exec);
		UEdGraphPin* NewExecOut = FirstPin(Node, EGPD_Output, UEdGraphSchema_K2::PC_Exec);
		if (AfterPin && NewExecIn)
		{
			TArray<UEdGraphPin*> OldTargets = AfterPin->LinkedTo;
			AfterPin->GetOwningNode()->Modify();
			if (OldTargets.Num() > 0 && NewExecOut)
			{
				AfterPin->BreakAllPinLinks(true);
			}
			TSharedRef<FJsonObject> Info = AgentJson::Obj();
			FAgentResult R = ConnectPins(AfterPin, NewExecIn, true, Info);
			if (!R.bOk) { return R; }
			Connections.Add(MakeShared<FJsonValueObject>(Info));
			if (NewExecOut)
			{
				for (UEdGraphPin* OldTarget : OldTargets)
				{
					if (!OldTarget) { continue; }
					TSharedRef<FJsonObject> Info2 = AgentJson::Obj();
					FAgentResult R2 = ConnectPins(NewExecOut, OldTarget, true, Info2);
					if (R2.bOk) { Connections.Add(MakeShared<FJsonValueObject>(Info2)); }
				}
			}
		}
		if (BeforePin && NewExecOut)
		{
			TArray<UEdGraphPin*> OldSources = BeforePin->LinkedTo;
			if (!AfterPin && OldSources.Num() > 0 && NewExecIn)
			{
				BeforePin->GetOwningNode()->Modify();
				BeforePin->BreakAllPinLinks(true);
				for (UEdGraphPin* OldSource : OldSources)
				{
					if (!OldSource) { continue; }
					TSharedRef<FJsonObject> Info = AgentJson::Obj();
					FAgentResult R = ConnectPins(OldSource, NewExecIn, true, Info);
					if (R.bOk) { Connections.Add(MakeShared<FJsonValueObject>(Info)); }
				}
			}
			TSharedRef<FJsonObject> Info = AgentJson::Obj();
			FAgentResult R = ConnectPins(NewExecOut, BeforePin, true, Info);
			if (!R.bOk) { return R; }
			Connections.Add(MakeShared<FJsonValueObject>(Info));
		}

		// Explicit connections: {"connect": {"Target": "GUID.ReturnValue", "then": "GUID"}}  (pin on new node -> other pin ref)
		if (TSharedPtr<FJsonObject> Connect = AgentJson::GetObject(Params, TEXT("connect")))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Connect->Values)
			{
				FString PinError;
				UEdGraphPin* Mine = AgentResolver::FindPin(Node, Pair.Key, EGPD_MAX, PinError);
				if (!Mine) { Failed.Add(PinError); continue; }
				const EEdGraphPinDirection OtherDir = Mine->Direction == EGPD_Input ? EGPD_Output : EGPD_Input;
				UEdGraphPin* Other = ResolvePinRef(Target, Pair.Value->AsString(), OtherDir, PinError);
				if (!Other) { Failed.Add(PinError); continue; }
				TSharedRef<FJsonObject> Info = AgentJson::Obj();
				FAgentResult R = Mine->Direction == EGPD_Output ? ConnectPins(Mine, Other, AgentJson::GetBool(Params, TEXT("force"), false), Info) : ConnectPins(Other, Mine, AgentJson::GetBool(Params, TEXT("force"), false), Info);
				if (R.bOk) { Connections.Add(MakeShared<FJsonValueObject>(Info)); } else { Failed.Add(R.ErrorMessage); }
			}
		}

		Json->SetArrayField(TEXT("connections"), Connections);
		if (Failed.Num() > 0) { AgentJson::SetStringArray(*Json, TEXT("failed"), Failed); }
		Target.Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Target.Blueprint);
		AgentCmd::NoteEdit(Context, Target.Blueprint, TEXT("graph.add_node"));
		return FAgentResult::Ok(Json);
	}

	FAgentResult DeleteNodes(const FJsonObject& Params, FAgentContext& Context)
	{
		FGraphTarget Target;
		FAgentResult Error;
		if (!ResolveTarget(Params, Target, Error, false)) { return Error; }
		TArray<FString> Refs = AgentJson::GetStringOrArray(Params, TEXT("nodes"));
		if (Refs.Num() == 0)
		{
			const FString Single = AgentJson::GetString(Params, TEXT("node"));
			if (!Single.IsEmpty()) { Refs.Add(Single); }
		}
		if (Refs.Num() == 0)
		{
			return FAgentResult::BadRequest(TEXT("Provide 'nodes' (array) or 'node'."));
		}
		const bool bReconnect = AgentJson::GetBool(Params, TEXT("reconnect"), true);
		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(FString::Printf(TEXT("Delete %d node(s)"), Refs.Num())));
		Target.Blueprint->Modify();
		TArray<FString> Deleted, Failed, Reconnected;
		for (const FString& Ref : Refs)
		{
			FString FindError;
			UEdGraphNode* Node = AgentResolver::FindNode(Target.Blueprint, Target.Graph, Ref, FindError);
			if (!Node)
			{
				Failed.Add(FindError);
				continue;
			}
			if (!Node->CanUserDeleteNode())
			{
				Failed.Add(FString::Printf(TEXT("%s cannot be deleted (%s)."), *Ref, *AgentGraph::NodeKind(Node)));
				continue;
			}
			// Bridge exec flow around the node when it has exactly one exec in and one exec out.
			if (bReconnect)
			{
				UEdGraphPin* ExecIn = FirstPin(Node, EGPD_Input, UEdGraphSchema_K2::PC_Exec);
				UEdGraphPin* ExecOut = FirstPin(Node, EGPD_Output, UEdGraphSchema_K2::PC_Exec);
				if (ExecIn && ExecOut && ExecIn->LinkedTo.Num() > 0 && ExecOut->LinkedTo.Num() == 1)
				{
					UEdGraphPin* Downstream = ExecOut->LinkedTo[0];
					for (UEdGraphPin* Upstream : ExecIn->LinkedTo)
					{
						if (Upstream && Downstream)
						{
							Upstream->GetOwningNode()->Modify();
							Downstream->GetOwningNode()->Modify();
							Node->GetGraph()->GetSchema()->TryCreateConnection(Upstream, Downstream);
							Reconnected.Add(Guid(Upstream->GetOwningNode()) + TEXT(" -> ") + Guid(Downstream->GetOwningNode()));
						}
					}
				}
			}
			UEdGraph* Graph = Node->GetGraph();
			Graph->Modify();
			FBlueprintEditorUtils::RemoveNode(Target.Blueprint, Node, /*bDontRecompile*/ true);
			Deleted.Add(Ref);
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Target.Blueprint);
		AgentCmd::NoteEdit(Context, Target.Blueprint, TEXT("graph.delete"));
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Target.Blueprint);
		AgentJson::SetStringArray(*Json, TEXT("deleted"), Deleted);
		AgentJson::SetStringArray(*Json, TEXT("reconnected"), Reconnected);
		if (Failed.Num() > 0) { AgentJson::SetStringArray(*Json, TEXT("failed"), Failed); }
		return FAgentResult::Ok(Json);
	}

	FAgentResult Connect(const FJsonObject& Params, FAgentContext& Context)
	{
		FGraphTarget Target;
		FAgentResult Error;
		if (!ResolveTarget(Params, Target, Error, false)) { return Error; }
		// Either {from, to} or {links: [{from, to}, ...]}
		TArray<TPair<FString, FString>> Links;
		if (const TArray<TSharedPtr<FJsonValue>>* Array = AgentJson::GetArray(Params, TEXT("links")))
		{
			for (const TSharedPtr<FJsonValue>& Item : *Array)
			{
				if (Item->Type == EJson::Object)
				{
					Links.Emplace(AgentJson::GetString(*Item->AsObject(), TEXT("from")), AgentJson::GetString(*Item->AsObject(), TEXT("to")));
				}
				else if (Item->Type == EJson::Array && Item->AsArray().Num() == 2)
				{
					Links.Emplace(Item->AsArray()[0]->AsString(), Item->AsArray()[1]->AsString());
				}
			}
		}
		else
		{
			Links.Emplace(AgentJson::GetString(Params, TEXT("from")), AgentJson::GetString(Params, TEXT("to")));
		}
		if (Links.Num() == 0 || Links[0].Key.IsEmpty())
		{
			return FAgentResult::BadRequest(TEXT("Provide from/to ('GUID.Pin') or links[]."));
		}
		const bool bForce = AgentJson::GetBool(Params, TEXT("force"), false);
		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(FString::Printf(TEXT("Connect %d pin(s)"), Links.Num())));
		Target.Blueprint->Modify();
		TArray<TSharedPtr<FJsonValue>> Done;
		TArray<FString> Failed;
		for (const TPair<FString, FString>& Link : Links)
		{
			FString RefError;
			UEdGraphPin* From = ResolvePinRef(Target, Link.Key, EGPD_Output, RefError);
			if (!From) { Failed.Add(RefError); continue; }
			UEdGraphPin* To = ResolvePinRef(Target, Link.Value, EGPD_Input, RefError);
			if (!To) { Failed.Add(RefError); continue; }
			TSharedRef<FJsonObject> Info = AgentJson::Obj();
			FAgentResult R = ConnectPins(From, To, bForce, Info);
			if (R.bOk)
			{
				Done.Add(MakeShared<FJsonValueObject>(Info));
			}
			else
			{
				Failed.Add(R.ErrorMessage + (R.ErrorDetails.IsValid() ? TEXT(" ") + AgentJson::GetString(*R.ErrorDetails, TEXT("hint")) : TEXT("")));
			}
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Target.Blueprint);
		AgentCmd::NoteEdit(Context, Target.Blueprint, TEXT("graph.connect"));
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Target.Blueprint);
		Json->SetArrayField(TEXT("connected"), Done);
		if (Failed.Num() > 0) { AgentJson::SetStringArray(*Json, TEXT("failed"), Failed); }
		if (Done.Num() == 0 && Failed.Num() > 0)
		{
			return FAgentResult::Error(AgentErrors::Conflict, Failed[0], Json);
		}
		return FAgentResult::Ok(Json);
	}

	FAgentResult Disconnect(const FJsonObject& Params, FAgentContext& Context)
	{
		FGraphTarget Target;
		FAgentResult Error;
		if (!ResolveTarget(Params, Target, Error, false)) { return Error; }
		const FString PinRef = AgentJson::GetString(Params, TEXT("pin"));
		const FString FromRef = AgentJson::GetString(Params, TEXT("from"));
		const FString ToRef = AgentJson::GetString(Params, TEXT("to"));
		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Disconnect")));
		Target.Blueprint->Modify();
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Target.Blueprint);
		FString RefError;
		if (!PinRef.IsEmpty())
		{
			UEdGraphPin* Pin = ResolvePinRef(Target, PinRef, EGPD_MAX, RefError);
			if (!Pin) { return FAgentResult::NotFound(RefError); }
			const int32 Count = Pin->LinkedTo.Num();
			Pin->GetOwningNode()->Modify();
			Pin->BreakAllPinLinks(true);
			Json->SetNumberField(TEXT("broken"), Count);
		}
		else if (!FromRef.IsEmpty() && !ToRef.IsEmpty())
		{
			UEdGraphPin* From = ResolvePinRef(Target, FromRef, EGPD_Output, RefError);
			if (!From) { return FAgentResult::NotFound(RefError); }
			UEdGraphPin* To = ResolvePinRef(Target, ToRef, EGPD_Input, RefError);
			if (!To) { return FAgentResult::NotFound(RefError); }
			if (!From->LinkedTo.Contains(To))
			{
				return FAgentResult::NotFound(TEXT("Those pins are not connected."));
			}
			From->GetOwningNode()->Modify();
			To->GetOwningNode()->Modify();
			From->BreakLinkTo(To);
			Json->SetNumberField(TEXT("broken"), 1);
		}
		else
		{
			return FAgentResult::BadRequest(TEXT("Provide 'pin' (break all) or 'from'+'to'."));
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Target.Blueprint);
		AgentCmd::NoteEdit(Context, Target.Blueprint, TEXT("graph.disconnect"));
		return FAgentResult::Ok(Json);
	}

	FAgentResult SetPins(const FJsonObject& Params, FAgentContext& Context)
	{
		FGraphTarget Target;
		FAgentResult Error;
		if (!ResolveTarget(Params, Target, Error, false)) { return Error; }
		FString NodeRef;
		if (!AgentCmd::RequireString(Params, TEXT("node"), NodeRef, Error)) { return Error; }
		FString FindError;
		UEdGraphNode* Node = AgentResolver::FindNode(Target.Blueprint, Target.Graph, NodeRef, FindError);
		if (!Node) { return FAgentResult::NotFound(FindError); }
		TSharedPtr<FJsonObject> Pins = AgentJson::GetObject(Params, TEXT("pins"));
		if (!Pins.IsValid())
		{
			// single form: pin + value
			const FString Pin = AgentJson::GetString(Params, TEXT("pin"));
			if (Pin.IsEmpty()) { return FAgentResult::BadRequest(TEXT("Provide 'pins' {name: value} or 'pin'+'value'.")); }
			Pins = AgentJson::Obj();
			Pins->SetField(Pin, Params.TryGetField(TEXT("value")));
		}
		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Set pin defaults")));
		Target.Blueprint->Modify();
		TArray<FString> Failed;
		ApplyPinDefaults(Node, Pins, Context, Failed);
		if (AgentJson::Has(Params, TEXT("comment")))
		{
			Node->Modify();
			Node->NodeComment = AgentJson::GetString(Params, TEXT("comment"));
			Node->bCommentBubbleVisible = !Node->NodeComment.IsEmpty();
		}
		if (AgentJson::Has(Params, TEXT("x")) || AgentJson::Has(Params, TEXT("y")))
		{
			Node->Modify();
			Node->NodePosX = AgentJson::GetInt(Params, TEXT("x"), Node->NodePosX);
			Node->NodePosY = AgentJson::GetInt(Params, TEXT("y"), Node->NodePosY);
		}
		if (AgentJson::Has(Params, TEXT("enabled")))
		{
			Node->Modify();
			Node->SetEnabledState(AgentJson::GetBool(Params, TEXT("enabled")) ? ENodeEnabledState::Enabled : ENodeEnabledState::Disabled);
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Target.Blueprint);
		AgentCmd::NoteEdit(Context, Target.Blueprint, TEXT("graph.set_pins"));
		TSharedRef<FJsonObject> Json = AgentGraph::SerializeNode(Node, true, false, true);
		if (Failed.Num() > 0) { AgentJson::SetStringArray(*Json, TEXT("failed"), Failed); }
		return FAgentResult::Ok(Json);
	}

	FAgentResult ReplaceNode(const FJsonObject& Params, FAgentContext& Context)
	{
		FGraphTarget Target;
		FAgentResult Error;
		if (!ResolveTarget(Params, Target, Error, false)) { return Error; }
		FString NodeRef;
		if (!AgentCmd::RequireString(Params, TEXT("node"), NodeRef, Error)) { return Error; }
		FString FindError;
		UEdGraphNode* Old = AgentResolver::FindNode(Target.Blueprint, Target.Graph, NodeRef, FindError);
		if (!Old) { return FAgentResult::NotFound(FindError); }
		Target.Graph = Old->GetGraph();

		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Replace node")));
		Target.Blueprint->Modify();
		UEdGraphNode* New = nullptr;
		FAgentResult Created = CreateNodeFromSpec(Target, Params, Context, Old->NodePosX, Old->NodePosY, New);
		if (!Created.bOk || !New) { return Created.bOk ? FAgentResult::Error(AgentErrors::Internal, TEXT("Node creation returned null.")) : Created; }

		// Migrate links by pin name, then by (direction, category) order for unmatched exec/data pins.
		TArray<FString> Migrated, Dropped;
		const UEdGraphSchema* Schema = Target.Graph->GetSchema();
		TSet<UEdGraphPin*> UsedNew;
		for (UEdGraphPin* OldPin : Old->Pins)
		{
			if (!OldPin || OldPin->LinkedTo.Num() == 0) { continue; }
			UEdGraphPin* NewPin = New->FindPin(OldPin->PinName, OldPin->Direction);
			if (!NewPin || UsedNew.Contains(NewPin))
			{
				NewPin = nullptr;
				for (UEdGraphPin* Candidate : New->Pins)
				{
					if (Candidate && !UsedNew.Contains(Candidate) && Candidate->Direction == OldPin->Direction && Candidate->PinType.PinCategory == OldPin->PinType.PinCategory && !Candidate->bHidden)
					{
						NewPin = Candidate;
						break;
					}
				}
			}
			if (!NewPin)
			{
				for (UEdGraphPin* Other : OldPin->LinkedTo) { if (Other) { Dropped.Add(OldPin->PinName.ToString() + TEXT(" -> ") + Guid(Other->GetOwningNode()) + TEXT(".") + Other->PinName.ToString()); } }
				continue;
			}
			UsedNew.Add(NewPin);
			TArray<UEdGraphPin*> Others = OldPin->LinkedTo;
			for (UEdGraphPin* Other : Others)
			{
				if (!Other) { continue; }
				Other->GetOwningNode()->Modify();
				const bool bOk = OldPin->Direction == EGPD_Output ? Schema->TryCreateConnection(NewPin, Other) : Schema->TryCreateConnection(Other, NewPin);
				(bOk ? Migrated : Dropped).Add(OldPin->PinName.ToString() + TEXT(" -> ") + Guid(Other->GetOwningNode()) + TEXT(".") + Other->PinName.ToString());
			}
			// Copy defaults for unlinked input pins with same name
		}
		for (UEdGraphPin* OldPin : Old->Pins)
		{
			if (OldPin && OldPin->Direction == EGPD_Input && OldPin->LinkedTo.Num() == 0 && !OldPin->DefaultValue.IsEmpty())
			{
				if (UEdGraphPin* NewPin = New->FindPin(OldPin->PinName, EGPD_Input))
				{
					if (NewPin->LinkedTo.Num() == 0 && NewPin->PinType == OldPin->PinType)
					{
						NewPin->DefaultValue = OldPin->DefaultValue;
						NewPin->DefaultObject = OldPin->DefaultObject;
						NewPin->DefaultTextValue = OldPin->DefaultTextValue;
					}
				}
			}
		}
		FBlueprintEditorUtils::RemoveNode(Target.Blueprint, Old, true);
		Target.Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Target.Blueprint);
		AgentCmd::NoteEdit(Context, Target.Blueprint, TEXT("graph.replace"));
		TSharedRef<FJsonObject> Json = AgentGraph::SerializeNode(New, true, false, true);
		Json->SetStringField(TEXT("replaced"), NodeRef);
		AgentJson::SetStringArray(*Json, TEXT("migrated"), Migrated);
		if (Dropped.Num() > 0) { AgentJson::SetStringArray(*Json, TEXT("dropped"), Dropped); }
		return FAgentResult::Ok(Json);
	}

	FAgentResult CloneNodes(const FJsonObject& Params, FAgentContext& Context)
	{
		FGraphTarget Target;
		FAgentResult Error;
		if (!ResolveTarget(Params, Target, Error, false)) { return Error; }
		TArray<FString> Refs = AgentJson::GetStringOrArray(Params, TEXT("nodes"));
		if (Refs.Num() == 0) { return FAgentResult::BadRequest(TEXT("Provide 'nodes'.")); }
		const int32 OffsetX = AgentJson::GetInt(Params, TEXT("dx"), 0);
		const int32 OffsetY = AgentJson::GetInt(Params, TEXT("dy"), 300);
		FString TargetGraphName = AgentJson::GetString(Params, TEXT("target_graph"));

		TArray<UEdGraphNode*> Sources;
		for (const FString& Ref : Refs)
		{
			FString FindError;
			UEdGraphNode* Node = AgentResolver::FindNode(Target.Blueprint, Target.Graph, Ref, FindError);
			if (!Node) { return FAgentResult::NotFound(FindError); }
			if (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_FunctionResult>() || (Node->IsA<UK2Node_Event>() && !Node->IsA<UK2Node_CustomEvent>()))
			{
				return FAgentResult::Error(AgentErrors::Conflict, FString::Printf(TEXT("%s (%s) cannot be duplicated."), *Ref, *AgentGraph::NodeKind(Node)));
			}
			Sources.Add(Node);
		}
		UEdGraph* DestGraph = Sources[0]->GetGraph();
		if (!TargetGraphName.IsEmpty())
		{
			FString GraphError;
			DestGraph = AgentResolver::FindGraph(Target.Blueprint, TargetGraphName, GraphError);
			if (!DestGraph) { return FAgentResult::NotFound(GraphError); }
		}

		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(FString::Printf(TEXT("Clone %d node(s)"), Sources.Num())));
		Target.Blueprint->Modify();
		DestGraph->Modify();
		TMap<UEdGraphNode*, UEdGraphNode*> Map;
		TArray<TSharedPtr<FJsonValue>> Created;
		for (UEdGraphNode* Source : Sources)
		{
			UEdGraphNode* Clone = DuplicateObject<UEdGraphNode>(Source, DestGraph);
			Clone->SetFlags(RF_Transactional);
			DestGraph->AddNode(Clone, false, false);
			Clone->CreateNewGuid();
			Clone->NodePosX += OffsetX;
			Clone->NodePosY += OffsetY;
			// Drop external links; internal links are re-created below.
			for (UEdGraphPin* Pin : Clone->Pins)
			{
				if (Pin) { Pin->LinkedTo.Reset(); }
			}
			if (UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Clone))
			{
				Custom->CustomFunctionName = FName(*(Custom->CustomFunctionName.ToString() + TEXT("_Copy")));
			}
			Clone->PostPlacedNewNode();
			Map.Add(Source, Clone);
		}
		const UEdGraphSchema* Schema = DestGraph->GetSchema();
		int32 InternalLinks = 0;
		for (UEdGraphNode* Source : Sources)
		{
			UEdGraphNode* Clone = Map[Source];
			for (int32 PinIndex = 0; PinIndex < Source->Pins.Num(); ++PinIndex)
			{
				UEdGraphPin* SourcePin = Source->Pins[PinIndex];
				if (!SourcePin || SourcePin->Direction != EGPD_Output) { continue; }
				for (UEdGraphPin* Linked : SourcePin->LinkedTo)
				{
					UEdGraphNode** OtherClone = Linked ? Map.Find(Linked->GetOwningNode()) : nullptr;
					if (!OtherClone) { continue; }
					UEdGraphPin* ClonePin = Clone->FindPin(SourcePin->PinName, EGPD_Output);
					UEdGraphPin* OtherPin = (*OtherClone)->FindPin(Linked->PinName, EGPD_Input);
					if (ClonePin && OtherPin && Schema->TryCreateConnection(ClonePin, OtherPin)) { ++InternalLinks; }
				}
			}
		}
		for (const TPair<UEdGraphNode*, UEdGraphNode*>& Pair : Map)
		{
			Created.Add(MakeShared<FJsonValueObject>(AgentGraph::SerializeNode(Pair.Value, false, false, false)));
		}
		DestGraph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Target.Blueprint);
		AgentCmd::NoteEdit(Context, Target.Blueprint, TEXT("graph.clone"));
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Target.Blueprint);
		Json->SetStringField(TEXT("graph"), DestGraph->GetName());
		Json->SetArrayField(TEXT("nodes"), Created);
		Json->SetNumberField(TEXT("internal_links"), InternalLinks);
		return FAgentResult::Ok(Json);
	}

	FAgentResult LocalVariable(const FJsonObject& Params, FAgentContext& Context)
	{
		FGraphTarget Target;
		FAgentResult Error;
		if (!ResolveTarget(Params, Target, Error)) { return Error; }
		FString Name, TypeSpec;
		if (!AgentCmd::RequireString(Params, TEXT("name"), Name, Error)) { return Error; }
		if (!AgentCmd::RequireString(Params, TEXT("type"), TypeSpec, Error)) { return Error; }
		FEdGraphPinType PinType;
		FString TypeError;
		if (!AgentPinTypes::Parse(TypeSpec, *Context.Adapter, PinType, TypeError)) { return FAgentResult::BadRequest(TypeError); }
		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Add local variable ") + Name));
		Target.Blueprint->Modify();
		if (!FBlueprintEditorUtils::AddLocalVariable(Target.Blueprint, Target.Graph, FName(*Name), PinType, AgentJson::GetString(Params, TEXT("default"))))
		{
			return FAgentResult::Error(AgentErrors::Internal, TEXT("AddLocalVariable failed (is this a function graph?)."));
		}
		AgentCmd::NoteEdit(Context, Target.Blueprint, TEXT("graph.local_variable"));
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Target.Blueprint);
		Json->SetStringField(TEXT("graph"), Target.Graph->GetName());
		Json->SetStringField(TEXT("variable"), Name);
		Json->SetStringField(TEXT("type"), AgentPinTypes::ToString(PinType));
		return FAgentResult::Ok(Json);
	}
}

void RegisterGraphCommands(FAgentCommandRegistry& Registry)
{
	Registry.Register(TEXT("graph.list"), TEXT("Graphs of a blueprint with node counts."), false, &Graphs);
	Registry.Register(TEXT("graph.inspect"), TEXT("Nodes of a graph; supports around/depth/query/kind/offset/limit."), false, &Inspect);
	Registry.Register(TEXT("graph.find_nodes"), TEXT("Search nodes across all graphs by query/kind."), false, &FindNodes);
	Registry.Register(TEXT("graph.add_node"), TEXT("Create a node (type=function_call|variable_get|...); optional after/before/pins/connect."), true, &AddNode);
	Registry.Register(TEXT("graph.delete_nodes"), TEXT("Delete nodes; bridges exec flow by default."), true, &DeleteNodes);
	Registry.Register(TEXT("graph.connect"), TEXT("Connect pins: from/to 'GUID.Pin' or links[]."), true, &Connect);
	Registry.Register(TEXT("graph.disconnect"), TEXT("Break links: pin (all) or from+to."), true, &Disconnect);
	Registry.Register(TEXT("graph.set_pins"), TEXT("Set pin defaults / comment / position / enabled on a node."), true, &SetPins);
	Registry.Register(TEXT("graph.replace_node"), TEXT("Replace a node with a new one, migrating links."), true, &ReplaceNode);
	Registry.Register(TEXT("graph.clone_nodes"), TEXT("Duplicate nodes (keeps internal links)."), true, &CloneNodes);
	Registry.Register(TEXT("graph.local_variable"), TEXT("Add a local variable to a function graph."), true, &LocalVariable);
}
