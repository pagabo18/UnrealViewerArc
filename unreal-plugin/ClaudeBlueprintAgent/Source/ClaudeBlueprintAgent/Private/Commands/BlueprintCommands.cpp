// blueprint.* commands: inspection at several detail levels, variables,
// functions, interfaces, components, compile/save/validate.
#include "CommandHelpers.h"
#include "Core/AgentCommandRegistry.h"
#include "Core/PinTypeUtils.h"
#include "Core/PropertyUtils.h"
#include "Serialization/GraphSerializer.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Editor.h"
#include "PackageTools.h"

namespace
{
	// ------------------------------------------------------------------ helpers

	FString BlueprintKind(const UBlueprint* Blueprint)
	{
		if (Blueprint->IsA<UWidgetBlueprint>()) { return TEXT("Widget"); }
		if (Blueprint->BlueprintType == BPTYPE_Interface) { return TEXT("Interface"); }
		if (Blueprint->BlueprintType == BPTYPE_FunctionLibrary) { return TEXT("FunctionLibrary"); }
		if (Blueprint->BlueprintType == BPTYPE_MacroLibrary) { return TEXT("MacroLibrary"); }
		if (Blueprint->ParentClass)
		{
			if (Blueprint->ParentClass->IsChildOf(AActor::StaticClass())) { return TEXT("Actor"); }
			if (Blueprint->ParentClass->IsChildOf(UActorComponent::StaticClass())) { return TEXT("Component"); }
		}
		return TEXT("Object");
	}

	TSharedRef<FJsonObject> VariableToJson(UBlueprint* Blueprint, const FBPVariableDescription& Var, bool bDetailed)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetStringField(TEXT("name"), Var.VarName.ToString());
		Json->SetStringField(TEXT("type"), AgentPinTypes::ToString(Var.VarType));
		const FString Category = Var.Category.ToString();
		if (!Category.IsEmpty() && Category != TEXT("Default"))
		{
			Json->SetStringField(TEXT("category"), Category);
		}
		if (Var.PropertyFlags & CPF_Net) { Json->SetBoolField(TEXT("replicated"), true); }
		if (Var.RepNotifyFunc != NAME_None) { Json->SetStringField(TEXT("rep_notify"), Var.RepNotifyFunc.ToString()); }
		if (!(Var.PropertyFlags & CPF_DisableEditOnInstance)) { Json->SetBoolField(TEXT("instance_editable"), true); }
		if (Var.PropertyFlags & CPF_BlueprintReadOnly) { Json->SetBoolField(TEXT("read_only"), true); }
		if (Var.HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn)) { Json->SetBoolField(TEXT("expose_on_spawn"), true); }
		if (Var.HasMetaData(FBlueprintMetadata::MD_Private)) { Json->SetBoolField(TEXT("private"), true); }
		if (bDetailed)
		{
			// Actual default lives on the generated class CDO once compiled.
			FString Default = Var.DefaultValue;
			if (Blueprint->GeneratedClass)
			{
				if (FProperty* Property = Blueprint->GeneratedClass->FindPropertyByName(Var.VarName))
				{
					const UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
					Default = AgentProps::CompactValue(Property, CDO, nullptr, 200);
				}
			}
			if (!Default.IsEmpty())
			{
				Json->SetStringField(TEXT("default"), Default);
			}
			if (Var.HasMetaData(FBlueprintMetadata::MD_Tooltip))
			{
				Json->SetStringField(TEXT("tooltip"), Var.GetMetaData(FBlueprintMetadata::MD_Tooltip));
			}
		}
		return Json;
	}

	void CollectEvents(UBlueprint* Blueprint, TArray<FString>& OutEvents)
	{
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (!Graph) { continue; }
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node))
				{
					OutEvents.Add(Bound->ComponentPropertyName.ToString() + TEXT(".") + Bound->DelegatePropertyName.ToString());
				}
				else if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Node))
				{
					OutEvents.Add(Custom->CustomFunctionName.ToString() + TEXT(" (custom)"));
				}
				else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
				{
					OutEvents.Add(Event->EventReference.GetMemberName().ToString());
				}
			}
		}
		OutEvents.Sort();
	}

	TSharedRef<FJsonObject> ComponentsToJson(UBlueprint* Blueprint, bool bWithProperties)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		TArray<TSharedPtr<FJsonValue>> Items;
		// Native components from the parent CDO
		if (Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(AActor::StaticClass()))
		{
			if (const AActor* CDO = Cast<AActor>(Blueprint->ParentClass->GetDefaultObject()))
			{
				TArray<UActorComponent*> Native;
				CDO->GetComponents(Native);
				for (const UActorComponent* Component : Native)
				{
					TSharedRef<FJsonObject> Item = AgentJson::Obj();
					Item->SetStringField(TEXT("name"), Component->GetName());
					Item->SetStringField(TEXT("class"), Component->GetClass()->GetName());
					Item->SetBoolField(TEXT("native"), true);
					Items.Add(MakeShared<FJsonValueObject>(Item));
				}
			}
		}
		if (Blueprint->SimpleConstructionScript)
		{
			for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (!Node) { continue; }
				TSharedRef<FJsonObject> Item = AgentJson::Obj();
				Item->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
				Item->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : (Node->ComponentTemplate ? Node->ComponentTemplate->GetClass()->GetName() : TEXT("?")));
				FString Parent;
				if (Node->ParentComponentOrVariableName != NAME_None)
				{
					Parent = Node->ParentComponentOrVariableName.ToString();
				}
				else
				{
					for (const USCS_Node* Other : Blueprint->SimpleConstructionScript->GetAllNodes())
					{
						if (Other && Other->GetChildNodes().Contains(Node))
						{
							Parent = Other->GetVariableName().ToString();
							break;
						}
					}
				}
				if (!Parent.IsEmpty()) { Item->SetStringField(TEXT("parent"), Parent); }
				if (bWithProperties && Node->ComponentTemplate)
				{
					TSet<FName> Exclude = { TEXT("AttachParent"), TEXT("AttachChildren"), TEXT("CreationMethod"), TEXT("UCSModifiedProperties") };
					Item->SetObjectField(TEXT("props"), AgentProps::ExportProperties(Node->ComponentTemplate, true, Exclude));
				}
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Json->SetArrayField(TEXT("components"), Items);
		return Json;
	}

	void ComputeGraphCounts(UBlueprint* Blueprint, int32& OutNodes)
	{
		OutNodes = 0;
		TArray<UEdGraph*> Graphs;
		AgentResolver::GetAllGraphs(Blueprint, Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			OutNodes += Graph ? Graph->Nodes.Num() : 0;
		}
	}

	TArray<FString> Referencers(UBlueprint* Blueprint, int32 Max)
	{
		TArray<FString> Out;
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		TArray<FName> Refs;
		Registry.GetReferencers(*AgentResolver::PackagePathOf(Blueprint), Refs, UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName& Ref : Refs)
		{
			const FString S = Ref.ToString();
			if (!S.StartsWith(TEXT("/Script/")))
			{
				Out.Add(S);
			}
		}
		Out.Sort();
		if (Out.Num() > Max)
		{
			Out.SetNum(Max);
		}
		return Out;
	}

	TArray<FString> Dependencies(UBlueprint* Blueprint, int32 Max)
	{
		TArray<FString> Out;
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		TArray<FName> Deps;
		Registry.GetDependencies(*AgentResolver::PackagePathOf(Blueprint), Deps, UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName& Dep : Deps)
		{
			const FString S = Dep.ToString();
			if (!S.StartsWith(TEXT("/Script/")))
			{
				Out.Add(S);
			}
		}
		Out.Sort();
		if (Out.Num() > Max)
		{
			Out.SetNum(Max);
		}
		return Out;
	}

	TSharedRef<FJsonObject> CompileResultToJson(UBlueprint* Blueprint, FCompilerResultsLog& Results)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetStringField(TEXT("asset"), AgentResolver::PackagePathOf(Blueprint));
		const bool bOk = Blueprint->Status != BS_Error && Results.NumErrors == 0;
		Json->SetBoolField(TEXT("ok"), bOk);
		Json->SetNumberField(TEXT("errors"), Results.NumErrors);
		Json->SetNumberField(TEXT("warnings"), Results.NumWarnings);
		FString Status;
		switch (Blueprint->Status)
		{
		case BS_UpToDate: Status = TEXT("UpToDate"); break;
		case BS_UpToDateWithWarnings: Status = TEXT("UpToDateWithWarnings"); break;
		case BS_Error: Status = TEXT("Error"); break;
		case BS_Dirty: Status = TEXT("Dirty"); break;
		default: Status = TEXT("Unknown"); break;
		}
		Json->SetStringField(TEXT("status"), Status);

		TArray<TSharedPtr<FJsonValue>> Messages;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (Severity > EMessageSeverity::Warning)
			{
				continue;
			}
			TSharedRef<FJsonObject> Item = AgentJson::Obj();
			Item->SetStringField(TEXT("level"), Severity == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
			Item->SetStringField(TEXT("text"), Message->ToText().ToString().Left(400));
			Messages.Add(MakeShared<FJsonValueObject>(Item));
			if (Messages.Num() >= 40)
			{
				break;
			}
		}
		Json->SetArrayField(TEXT("messages"), Messages);

		// Node-level annotations (reliable across engine versions).
		TArray<TSharedPtr<FJsonValue>> NodeErrors;
		TArray<UEdGraph*> Graphs;
		AgentResolver::GetAllGraphs(Blueprint, Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph) { continue; }
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->bHasCompilerMessage && Node->ErrorType <= EMessageSeverity::Warning)
				{
					TSharedRef<FJsonObject> Item = AgentJson::Obj();
					Item->SetStringField(TEXT("graph"), Graph->GetName());
					Item->SetStringField(TEXT("node"), Node->NodeGuid.ToString(EGuidFormats::Digits));
					Item->SetStringField(TEXT("kind"), AgentGraph::NodeKind(Node));
					Item->SetStringField(TEXT("title"), AgentGraph::NodeTitle(Node));
					Item->SetStringField(TEXT("level"), Node->ErrorType == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
					Item->SetStringField(TEXT("text"), Node->ErrorMsg.Left(300));
					NodeErrors.Add(MakeShared<FJsonValueObject>(Item));
					if (NodeErrors.Num() >= 40)
					{
						break;
					}
				}
			}
		}
		Json->SetArrayField(TEXT("node_errors"), NodeErrors);
		return Json;
	}

	FAgentResult CompileOne(UBlueprint* Blueprint, FAgentContext& Context, TSharedRef<FJsonObject>& OutJson)
	{
		FCompilerResultsLog Results;
		Results.bLogDetailedResults = false;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);
		OutJson = CompileResultToJson(Blueprint, Results);
		return FAgentResult::Ok(OutJson);
	}

	// ------------------------------------------------------------------ inspection

	FAgentResult Summary(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UBlueprint* Blueprint = AgentCmd::RequireBlueprint(Params, Error);
		if (!Blueprint) { return Error; }

		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Blueprint);
		Json->SetStringField(TEXT("kind"), BlueprintKind(Blueprint));
		Json->SetStringField(TEXT("parent"), AgentResolver::GetClassDisplayName(Blueprint->ParentClass));
		Json->SetStringField(TEXT("parent_spec"), AgentResolver::GetClassSpec(Blueprint->ParentClass));
		Json->SetNumberField(TEXT("functions"), Blueprint->FunctionGraphs.Num());
		Json->SetNumberField(TEXT("variables"), Blueprint->NewVariables.Num());
		Json->SetNumberField(TEXT("macros"), Blueprint->MacroGraphs.Num());
		Json->SetNumberField(TEXT("event_graphs"), Blueprint->UbergraphPages.Num());
		TArray<FString> Events;
		CollectEvents(Blueprint, Events);
		Json->SetNumberField(TEXT("events"), Events.Num());
		int32 Nodes = 0;
		ComputeGraphCounts(Blueprint, Nodes);
		Json->SetNumberField(TEXT("nodes"), Nodes);
		TArray<FString> Interfaces;
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			Interfaces.Add(AgentResolver::GetClassDisplayName(Interface.Interface));
		}
		AgentJson::SetStringArray(*Json, TEXT("interfaces"), Interfaces);
		if (Blueprint->SimpleConstructionScript)
		{
			Json->SetNumberField(TEXT("components"), Blueprint->SimpleConstructionScript->GetAllNodes().Num());
		}
		if (const UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
		{
			int32 WidgetCount = 0;
			if (WidgetBlueprint->WidgetTree)
			{
				WidgetBlueprint->WidgetTree->ForEachWidget([&WidgetCount](UWidget*) { ++WidgetCount; });
			}
			Json->SetNumberField(TEXT("widgets"), WidgetCount);
			Json->SetNumberField(TEXT("animations"), WidgetBlueprint->Animations.Num());
		}
		AgentJson::SetStringArray(*Json, TEXT("referencers"), Referencers(Blueprint, 12));
		AgentJson::SetStringArray(*Json, TEXT("dependencies"), Dependencies(Blueprint, 12));
		Json->SetStringField(TEXT("status"), Blueprint->Status == BS_Error ? TEXT("Error") : (Blueprint->Status == BS_Dirty ? TEXT("Dirty") : TEXT("OK")));
		Json->SetBoolField(TEXT("dirty"), Blueprint->GetOutermost()->IsDirty());
		return FAgentResult::Ok(Json);
	}

	FAgentResult Structure(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UBlueprint* Blueprint = AgentCmd::RequireBlueprint(Params, Error);
		if (!Blueprint) { return Error; }
		const bool bDetailed = AgentJson::GetBool(Params, TEXT("detailed"), true);

		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Blueprint);
		Json->SetStringField(TEXT("kind"), BlueprintKind(Blueprint));
		Json->SetStringField(TEXT("parent"), AgentResolver::GetClassDisplayName(Blueprint->ParentClass));

		// Functions with signatures
		TArray<TSharedPtr<FJsonValue>> Functions;
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (!Graph) { continue; }
			TSharedRef<FJsonObject> Item = AgentGraph::SerializeFunctionSignature(Graph);
			Item->SetStringField(TEXT("name"), Graph->GetName());
			Item->SetNumberField(TEXT("nodes"), Graph->Nodes.Num());
			Functions.Add(MakeShared<FJsonValueObject>(Item));
		}
		// Interface function graphs
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			for (UEdGraph* Graph : Interface.Graphs)
			{
				if (!Graph) { continue; }
				TSharedRef<FJsonObject> Item = AgentGraph::SerializeFunctionSignature(Graph);
				Item->SetStringField(TEXT("name"), Graph->GetName());
				Item->SetNumberField(TEXT("nodes"), Graph->Nodes.Num());
				Item->SetStringField(TEXT("interface"), AgentResolver::GetClassDisplayName(Interface.Interface));
				Functions.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Json->SetArrayField(TEXT("functions"), Functions);

		TArray<TSharedPtr<FJsonValue>> Macros;
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (Graph) { Macros.Add(MakeShared<FJsonValueObject>(AgentGraph::SerializeGraphSummary(Blueprint, Graph))); }
		}
		Json->SetArrayField(TEXT("macros"), Macros);

		TArray<TSharedPtr<FJsonValue>> EventGraphs;
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph) { EventGraphs.Add(MakeShared<FJsonValueObject>(AgentGraph::SerializeGraphSummary(Blueprint, Graph))); }
		}
		Json->SetArrayField(TEXT("event_graphs"), EventGraphs);

		TArray<FString> Events;
		CollectEvents(Blueprint, Events);
		AgentJson::SetStringArray(*Json, TEXT("events"), Events);

		TArray<TSharedPtr<FJsonValue>> Variables;
		for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			Variables.Add(MakeShared<FJsonValueObject>(VariableToJson(Blueprint, Var, bDetailed)));
		}
		Json->SetArrayField(TEXT("variables"), Variables);

		TArray<FString> Interfaces;
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			Interfaces.Add(AgentResolver::GetClassDisplayName(Interface.Interface));
		}
		AgentJson::SetStringArray(*Json, TEXT("interfaces"), Interfaces);

		TArray<TSharedPtr<FJsonValue>> Delegates;
		for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
		{
			if (Graph) { Delegates.Add(MakeShared<FJsonValueString>(Graph->GetName())); }
		}
		Json->SetArrayField(TEXT("delegates"), Delegates);

		Json->SetObjectField(TEXT("components"), ComponentsToJson(Blueprint, false));
		AgentJson::SetStringArray(*Json, TEXT("referencers"), Referencers(Blueprint, 30));
		AgentJson::SetStringArray(*Json, TEXT("dependencies"), Dependencies(Blueprint, 30));
		return FAgentResult::Ok(Json);
	}

	/** Compact cross-reference entry used by the server-side index. */
	FAgentResult IndexEntry(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UBlueprint* Blueprint = AgentCmd::RequireBlueprint(Params, Error);
		if (!Blueprint) { return Error; }

		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Blueprint);
		Json->SetStringField(TEXT("kind"), BlueprintKind(Blueprint));
		Json->SetStringField(TEXT("parent"), AgentResolver::GetClassDisplayName(Blueprint->ParentClass));

		TArray<FString> Functions, Variables, Events, Interfaces, Components, Widgets, Macros;
		TSet<FString> Calls, Reads, Writes;
		for (UEdGraph* Graph : Blueprint->FunctionGraphs) { if (Graph) { Functions.Add(Graph->GetName()); } }
		for (UEdGraph* Graph : Blueprint->MacroGraphs) { if (Graph) { Macros.Add(Graph->GetName()); } }
		for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			Variables.Add(Var.VarName.ToString() + TEXT(":") + AgentPinTypes::ToString(Var.VarType));
		}
		CollectEvents(Blueprint, Events);
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			Interfaces.Add(AgentResolver::GetClassDisplayName(Interface.Interface));
			for (UEdGraph* Graph : Interface.Graphs) { if (Graph) { Functions.Add(Graph->GetName()); } }
		}
		if (Blueprint->SimpleConstructionScript)
		{
			for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (Node)
				{
					Components.Add(Node->GetVariableName().ToString() + TEXT(":") + (Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("?")));
				}
			}
		}
		if (const UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
		{
			if (WidgetBlueprint->WidgetTree)
			{
				WidgetBlueprint->WidgetTree->ForEachWidget([&Widgets](UWidget* Widget)
				{
					if (Widget) { Widgets.Add(Widget->GetName() + TEXT(":") + Widget->GetClass()->GetName()); }
				});
			}
		}
		TArray<UEdGraph*> Graphs;
		AgentResolver::GetAllGraphs(Blueprint, Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph) { continue; }
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
				{
					Calls.Add(AgentGraph::NodeMemberClass(Call) + TEXT(".") + Call->FunctionReference.GetMemberName().ToString());
				}
				else if (const UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node))
				{
					Writes.Add(AgentGraph::NodeMemberClass(Set) + TEXT(".") + Set->GetVarName().ToString());
				}
				else if (const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node))
				{
					Reads.Add(AgentGraph::NodeMemberClass(Get) + TEXT(".") + Get->GetVarName().ToString());
				}
			}
		}
		AgentJson::SetStringArray(*Json, TEXT("functions"), Functions);
		AgentJson::SetStringArray(*Json, TEXT("macros"), Macros);
		AgentJson::SetStringArray(*Json, TEXT("variables"), Variables);
		AgentJson::SetStringArray(*Json, TEXT("events"), Events);
		AgentJson::SetStringArray(*Json, TEXT("interfaces"), Interfaces);
		AgentJson::SetStringArray(*Json, TEXT("components"), Components);
		AgentJson::SetStringArray(*Json, TEXT("widgets"), Widgets);
		AgentJson::SetStringArray(*Json, TEXT("calls"), Calls.Array());
		AgentJson::SetStringArray(*Json, TEXT("reads"), Reads.Array());
		AgentJson::SetStringArray(*Json, TEXT("writes"), Writes.Array());
		AgentJson::SetStringArray(*Json, TEXT("dependencies"), Dependencies(Blueprint, 200));
		Json->SetNumberField(TEXT("version"), static_cast<double>(Context.ChangeTracker->GetAssetVersion(AgentResolver::PackagePathOf(Blueprint))));
		return FAgentResult::Ok(Json);
	}

	FAgentResult Components(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UBlueprint* Blueprint = AgentCmd::RequireBlueprint(Params, Error);
		if (!Blueprint) { return Error; }
		TSharedRef<FJsonObject> Json = ComponentsToJson(Blueprint, AgentJson::GetBool(Params, TEXT("properties"), true));
		Json->SetStringField(TEXT("asset"), AgentResolver::PackagePathOf(Blueprint));
		return FAgentResult::Ok(Json);
	}

	// ------------------------------------------------------------------ variables

	FAgentResult VariableOp(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UBlueprint* Blueprint = AgentCmd::RequireBlueprint(Params, Error);
		if (!Blueprint) { return Error; }
		FString Op, Name;
		if (!AgentCmd::RequireString(Params, TEXT("op"), Op, Error)) { return Error; }
		if (!AgentCmd::RequireString(Params, TEXT("name"), Name, Error)) { return Error; }
		Op = Op.ToLower();
		const FName VarName(*Name);
		const int32 ExistingIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);

		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Blueprint);
		Json->SetStringField(TEXT("op"), Op);
		Json->SetStringField(TEXT("variable"), Name);

		if (Op == TEXT("add"))
		{
			if (ExistingIndex != INDEX_NONE)
			{
				return FAgentResult::Error(AgentErrors::Conflict, FString::Printf(TEXT("Variable '%s' already exists."), *Name));
			}
			FString TypeSpec;
			if (!AgentCmd::RequireString(Params, TEXT("type"), TypeSpec, Error)) { return Error; }
			FEdGraphPinType PinType;
			FString TypeError;
			if (!AgentPinTypes::Parse(TypeSpec, *Context.Adapter, PinType, TypeError))
			{
				return FAgentResult::BadRequest(TypeError);
			}
			const FString Default = AgentJson::GetString(Params, TEXT("default"));
			const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Add variable ") + Name));
			Blueprint->Modify();
			if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType, Default))
			{
				return FAgentResult::Error(AgentErrors::Internal, TEXT("AddMemberVariable failed (invalid name or type?)."));
			}
			Json->SetStringField(TEXT("type"), AgentPinTypes::ToString(PinType));
			// fallthrough to apply optional flags
		}
		else if (ExistingIndex == INDEX_NONE)
		{
			return FAgentResult::NotFound(FString::Printf(TEXT("Variable '%s' not found on %s (only Blueprint-declared variables can be edited)."), *Name, *Blueprint->GetName()));
		}

		if (Op == TEXT("remove") || Op == TEXT("delete"))
		{
			const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Remove variable ") + Name));
			Blueprint->Modify();
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);
			AgentCmd::NoteEdit(Context, Blueprint, TEXT("variable.remove"));
			return FAgentResult::Ok(Json);
		}
		if (Op == TEXT("rename"))
		{
			FString NewName;
			if (!AgentCmd::RequireString(Params, TEXT("new_name"), NewName, Error)) { return Error; }
			const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Rename variable ") + Name));
			Blueprint->Modify();
			FBlueprintEditorUtils::RenameMemberVariable(Blueprint, VarName, FName(*NewName));
			Json->SetStringField(TEXT("variable"), NewName);
			AgentCmd::NoteEdit(Context, Blueprint, TEXT("variable.rename"));
			return FAgentResult::Ok(Json);
		}

		// add (continued) / modify
		{
			const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Modify variable ") + Name));
			Blueprint->Modify();
			const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
			if (Index == INDEX_NONE)
			{
				return FAgentResult::Error(AgentErrors::Internal, TEXT("Variable vanished."));
			}
			TArray<FString> Changed;
			if (Op == TEXT("modify") && AgentJson::Has(Params, TEXT("type")))
			{
				FEdGraphPinType PinType;
				FString TypeError;
				if (!AgentPinTypes::Parse(AgentJson::GetString(Params, TEXT("type")), *Context.Adapter, PinType, TypeError))
				{
					return FAgentResult::BadRequest(TypeError);
				}
				FBlueprintEditorUtils::ChangeMemberVariableType(Blueprint, VarName, PinType);
				Changed.Add(TEXT("type"));
			}
			FBPVariableDescription& Var = Blueprint->NewVariables[Index];
			if (AgentJson::Has(Params, TEXT("category")))
			{
				FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VarName, nullptr, FText::FromString(AgentJson::GetString(Params, TEXT("category"))), true);
				Changed.Add(TEXT("category"));
			}
			if (AgentJson::Has(Params, TEXT("tooltip")))
			{
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_Tooltip, AgentJson::GetString(Params, TEXT("tooltip")));
				Changed.Add(TEXT("tooltip"));
			}
			if (AgentJson::Has(Params, TEXT("replicated")))
			{
				if (AgentJson::GetBool(Params, TEXT("replicated"))) { Var.PropertyFlags |= CPF_Net; } else { Var.PropertyFlags &= ~CPF_Net; Var.PropertyFlags &= ~CPF_RepNotify; Var.RepNotifyFunc = NAME_None; }
				Changed.Add(TEXT("replicated"));
			}
			if (AgentJson::Has(Params, TEXT("rep_notify")))
			{
				const FString Func = AgentJson::GetString(Params, TEXT("rep_notify"));
				Var.RepNotifyFunc = FName(*Func);
				if (Func.IsEmpty()) { Var.PropertyFlags &= ~CPF_RepNotify; } else { Var.PropertyFlags |= CPF_Net | CPF_RepNotify; }
				Changed.Add(TEXT("rep_notify"));
			}
			if (AgentJson::Has(Params, TEXT("instance_editable")))
			{
				if (AgentJson::GetBool(Params, TEXT("instance_editable"))) { Var.PropertyFlags &= ~CPF_DisableEditOnInstance; } else { Var.PropertyFlags |= CPF_DisableEditOnInstance; }
				Changed.Add(TEXT("instance_editable"));
			}
			if (AgentJson::Has(Params, TEXT("read_only")))
			{
				if (AgentJson::GetBool(Params, TEXT("read_only"))) { Var.PropertyFlags |= CPF_BlueprintReadOnly; } else { Var.PropertyFlags &= ~CPF_BlueprintReadOnly; }
				Changed.Add(TEXT("read_only"));
			}
			if (AgentJson::Has(Params, TEXT("expose_on_spawn")))
			{
				if (AgentJson::GetBool(Params, TEXT("expose_on_spawn")))
				{
					Var.SetMetaData(FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
					Var.PropertyFlags |= CPF_ExposeOnSpawn;
				}
				else
				{
					Var.RemoveMetaData(FBlueprintMetadata::MD_ExposeOnSpawn);
					Var.PropertyFlags &= ~CPF_ExposeOnSpawn;
				}
				Changed.Add(TEXT("expose_on_spawn"));
			}
			if (AgentJson::Has(Params, TEXT("private")))
			{
				if (AgentJson::GetBool(Params, TEXT("private"))) { Var.SetMetaData(FBlueprintMetadata::MD_Private, TEXT("true")); } else { Var.RemoveMetaData(FBlueprintMetadata::MD_Private); }
				Changed.Add(TEXT("private"));
			}
			if (Op == TEXT("modify") && AgentJson::Has(Params, TEXT("default")))
			{
				const FString Default = AgentJson::GetString(Params, TEXT("default"));
				Var.DefaultValue = Default;
				if (Blueprint->GeneratedClass)
				{
					if (FProperty* Property = Blueprint->GeneratedClass->FindPropertyByName(VarName))
					{
						UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
						FString SetError;
						if (!AgentProps::SetValue(CDO, Name, Default, SetError))
						{
							return FAgentResult::BadRequest(SetError);
						}
					}
				}
				Changed.Add(TEXT("default"));
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			AgentJson::SetStringArray(*Json, TEXT("changed"), Changed);
		}
		AgentCmd::NoteEdit(Context, Blueprint, TEXT("variable.") + Op);
		return FAgentResult::Ok(Json);
	}

	// ------------------------------------------------------------------ functions

	UK2Node_FunctionEntry* FindEntry(UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	FAgentResult FunctionOp(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UBlueprint* Blueprint = AgentCmd::RequireBlueprint(Params, Error);
		if (!Blueprint) { return Error; }
		FString Op, Name;
		if (!AgentCmd::RequireString(Params, TEXT("op"), Op, Error)) { return Error; }
		if (!AgentCmd::RequireString(Params, TEXT("name"), Name, Error)) { return Error; }
		Op = Op.ToLower();

		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Blueprint);
		Json->SetStringField(TEXT("op"), Op);
		Json->SetStringField(TEXT("function"), Name);

		FString FindError;
		UEdGraph* Graph = nullptr;
		for (UEdGraph* Candidate : Blueprint->FunctionGraphs)
		{
			if (Candidate && Candidate->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				Graph = Candidate;
				break;
			}
		}

		if (Op == TEXT("create"))
		{
			if (Graph)
			{
				return FAgentResult::Error(AgentErrors::Conflict, FString::Printf(TEXT("Function '%s' already exists."), *Name));
			}
			const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Create function ") + Name));
			Blueprint->Modify();

			UFunction* ParentFunction = nullptr;
			if (Blueprint->ParentClass)
			{
				ParentFunction = Blueprint->ParentClass->FindFunctionByName(*Name);
			}
			if (ParentFunction && AgentJson::GetBool(Params, TEXT("override"), true))
			{
				if (ParentFunction->HasAnyFunctionFlags(FUNC_BlueprintEvent))
				{
					// Overridable events go into the event graph as event nodes.
					UEdGraph* EventGraph = AgentResolver::GetEventGraph(Blueprint);
					int32 PosY = 0;
					UK2Node_Event* EventNode = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, EventGraph, *Name, Blueprint->ParentClass, PosY);
					if (!EventNode)
					{
						return FAgentResult::Error(AgentErrors::Internal, TEXT("Could not add event node."));
					}
					EventNode->bOverrideFunction = true;
					Json->SetStringField(TEXT("event_node"), EventNode->NodeGuid.ToString(EGuidFormats::Digits));
					Json->SetStringField(TEXT("graph"), EventGraph ? EventGraph->GetName() : FString());
					Json->SetBoolField(TEXT("override"), true);
					FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
					AgentCmd::NoteEdit(Context, Blueprint, TEXT("function.override"));
					return FAgentResult::Ok(Json);
				}
				Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, *Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, /*bIsUserCreated*/ false, ParentFunction);
				Json->SetBoolField(TEXT("override"), true);
			}
			else
			{
				Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, *Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated*/ true, nullptr);
			}
			Json->SetStringField(TEXT("graph"), Graph->GetName());
			// fallthrough to signature edits
		}
		else if (!Graph)
		{
			return FAgentResult::NotFound(FString::Printf(TEXT("Function '%s' not found on %s."), *Name, *Blueprint->GetName()));
		}

		if (Op == TEXT("delete") || Op == TEXT("remove"))
		{
			const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Delete function ") + Name));
			Blueprint->Modify();
			FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Default);
			AgentCmd::NoteEdit(Context, Blueprint, TEXT("function.delete"));
			return FAgentResult::Ok(Json);
		}
		if (Op == TEXT("rename"))
		{
			FString NewName;
			if (!AgentCmd::RequireString(Params, TEXT("new_name"), NewName, Error)) { return Error; }
			const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Rename function ") + Name));
			Blueprint->Modify();
			FBlueprintEditorUtils::RenameGraph(Graph, NewName);
			Json->SetStringField(TEXT("function"), NewName);
			AgentCmd::NoteEdit(Context, Blueprint, TEXT("function.rename"));
			return FAgentResult::Ok(Json);
		}

		// create (continued) / modify: inputs, outputs, pure, category
		{
			const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Modify function ") + Name));
			Blueprint->Modify();
			Graph->Modify();
			UK2Node_FunctionEntry* Entry = FindEntry(Graph);
			if (!Entry)
			{
				return FAgentResult::Error(AgentErrors::Internal, TEXT("Function graph has no entry node."));
			}
			TArray<FString> Changed;
			auto AddPins = [&](const TCHAR* Key, bool bInputs) -> FAgentResult
			{
				const TArray<TSharedPtr<FJsonValue>>* Items = AgentJson::GetArray(Params, Key);
				if (!Items) { return FAgentResult::Ok(); }
				UK2Node_EditablePinBase* Target = Entry;
				if (!bInputs)
				{
					Target = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
				}
				if (!Target)
				{
					return FAgentResult::Error(AgentErrors::Internal, TEXT("Could not create function result node."));
				}
				Target->Modify();
				for (const TSharedPtr<FJsonValue>& Item : *Items)
				{
					FString PinName, TypeSpec, Default;
					if (Item->Type == EJson::String)
					{
						// "Name:type"
						const FString Spec = Item->AsString();
						if (!Spec.Split(TEXT(":"), &PinName, &TypeSpec))
						{
							return FAgentResult::BadRequest(FString::Printf(TEXT("Pin spec '%s' must be Name:type."), *Spec));
						}
					}
					else if (Item->Type == EJson::Object)
					{
						const TSharedPtr<FJsonObject> Obj = Item->AsObject();
						PinName = AgentJson::GetString(*Obj, TEXT("name"));
						TypeSpec = AgentJson::GetString(*Obj, TEXT("type"));
						Default = AgentJson::GetString(*Obj, TEXT("default"));
					}
					FEdGraphPinType PinType;
					FString TypeError;
					if (!AgentPinTypes::Parse(TypeSpec, *Context.Adapter, PinType, TypeError))
					{
						return FAgentResult::BadRequest(TypeError);
					}
					UEdGraphPin* Pin = Target->CreateUserDefinedPin(FName(*PinName), PinType, bInputs ? EGPD_Output : EGPD_Input);
					if (!Pin)
					{
						return FAgentResult::Error(AgentErrors::Internal, FString::Printf(TEXT("Could not add pin '%s'."), *PinName));
					}
					if (!Default.IsEmpty())
					{
						Pin->DefaultValue = Default;
					}
					Changed.Add(FString(bInputs ? TEXT("+in ") : TEXT("+out ")) + PinName);
				}
				return FAgentResult::Ok();
			};
			FAgentResult PinResult = AddPins(TEXT("inputs"), true);
			if (!PinResult.bOk) { return PinResult; }
			PinResult = AddPins(TEXT("outputs"), false);
			if (!PinResult.bOk) { return PinResult; }

			TArray<FString> RemovePins = AgentJson::GetStringArray(Params, TEXT("remove_pins"));
			for (const FString& PinName : RemovePins)
			{
				bool bRemoved = false;
				if (UEdGraphPin* Pin = Entry->FindPin(FName(*PinName)))
				{
					bRemoved = Entry->RemoveUserDefinedPinByName(FName(*PinName));
				}
				if (!bRemoved)
				{
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
						{
							if (Result->RemoveUserDefinedPinByName(FName(*PinName)))
							{
								bRemoved = true;
							}
						}
					}
				}
				if (bRemoved) { Changed.Add(TEXT("-pin ") + PinName); }
			}
			if (AgentJson::Has(Params, TEXT("pure")))
			{
				if (AgentJson::GetBool(Params, TEXT("pure"))) { Entry->AddExtraFlags(FUNC_BlueprintPure); } else { Entry->ClearExtraFlags(FUNC_BlueprintPure); }
				Changed.Add(TEXT("pure"));
			}
			if (AgentJson::Has(Params, TEXT("category")))
			{
				Entry->MetaData.Category = FText::FromString(AgentJson::GetString(Params, TEXT("category")));
				Changed.Add(TEXT("category"));
			}
			if (AgentJson::Has(Params, TEXT("tooltip")))
			{
				Entry->MetaData.ToolTip = FText::FromString(AgentJson::GetString(Params, TEXT("tooltip")));
				Changed.Add(TEXT("tooltip"));
			}
			if (AgentJson::Has(Params, TEXT("call_in_editor")))
			{
				Entry->MetaData.bCallInEditor = AgentJson::GetBool(Params, TEXT("call_in_editor"));
				Changed.Add(TEXT("call_in_editor"));
			}
			if (AgentJson::Has(Params, TEXT("access")))
			{
				const FString Access = AgentJson::GetString(Params, TEXT("access")).ToLower();
				Entry->ClearExtraFlags(FUNC_Public | FUNC_Protected | FUNC_Private);
				if (Access == TEXT("public")) { Entry->AddExtraFlags(FUNC_Public); }
				else if (Access == TEXT("protected")) { Entry->AddExtraFlags(FUNC_Protected); }
				else if (Access == TEXT("private")) { Entry->AddExtraFlags(FUNC_Private); }
				Changed.Add(TEXT("access"));
			}
			Entry->ReconstructNode();
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			AgentJson::SetStringArray(*Json, TEXT("changed"), Changed);
			Json->SetObjectField(TEXT("signature"), AgentGraph::SerializeFunctionSignature(Graph));
			Json->SetStringField(TEXT("entry_node"), Entry->NodeGuid.ToString(EGuidFormats::Digits));
		}
		AgentCmd::NoteEdit(Context, Blueprint, TEXT("function.") + Op);
		return FAgentResult::Ok(Json);
	}

	// ------------------------------------------------------------------ interfaces

	FAgentResult InterfaceOp(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UBlueprint* Blueprint = AgentCmd::RequireBlueprint(Params, Error);
		if (!Blueprint) { return Error; }
		FString Op, InterfaceSpec;
		if (!AgentCmd::RequireString(Params, TEXT("op"), Op, Error)) { return Error; }
		if (!AgentCmd::RequireString(Params, TEXT("interface"), InterfaceSpec, Error)) { return Error; }
		UClass* Interface = Context.Adapter->ResolveClass(InterfaceSpec);
		if (!Interface || !Interface->HasAnyClassFlags(CLASS_Interface))
		{
			return FAgentResult::NotFound(FString::Printf(TEXT("Interface '%s' not found."), *InterfaceSpec));
		}
		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(Op + TEXT(" interface ") + Interface->GetName()));
		Blueprint->Modify();
		bool bOk = false;
		if (Op.Equals(TEXT("add"), ESearchCase::IgnoreCase))
		{
			bOk = Context.Adapter->ImplementInterface(Blueprint, Interface);
		}
		else if (Op.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
		{
			bOk = Context.Adapter->RemoveInterface(Blueprint, Interface);
		}
		else
		{
			return FAgentResult::BadRequest(TEXT("op must be add|remove."));
		}
		if (!bOk)
		{
			return FAgentResult::Error(AgentErrors::Internal, TEXT("Interface operation failed (already implemented / not implemented?)."));
		}
		AgentCmd::NoteEdit(Context, Blueprint, TEXT("interface.") + Op);
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Blueprint);
		Json->SetStringField(TEXT("op"), Op);
		Json->SetStringField(TEXT("interface"), AgentResolver::GetClassDisplayName(Interface));
		return FAgentResult::Ok(Json);
	}

	// ------------------------------------------------------------------ components

	FAgentResult ComponentOp(const FJsonObject& Params, FAgentContext& Context)
	{
		FAgentResult Error;
		UBlueprint* Blueprint = AgentCmd::RequireBlueprint(Params, Error);
		if (!Blueprint) { return Error; }
		FString Op, Name;
		if (!AgentCmd::RequireString(Params, TEXT("op"), Op, Error)) { return Error; }
		if (!AgentCmd::RequireString(Params, TEXT("name"), Name, Error)) { return Error; }
		Op = Op.ToLower();
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			return FAgentResult::Unsupported(TEXT("This Blueprint has no construction script (not an Actor Blueprint)."), TEXT("Blueprint.Components"));
		}
		USCS_Node* Existing = SCS->FindSCSNode(FName(*Name));
		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Blueprint);
		Json->SetStringField(TEXT("op"), Op);
		Json->SetStringField(TEXT("component"), Name);

		if (Op == TEXT("add"))
		{
			if (Existing)
			{
				return FAgentResult::Error(AgentErrors::Conflict, FString::Printf(TEXT("Component '%s' already exists."), *Name));
			}
			FString ClassSpec;
			if (!AgentCmd::RequireString(Params, TEXT("class"), ClassSpec, Error)) { return Error; }
			UClass* ComponentClass = Context.Adapter->ResolveClass(ClassSpec);
			if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
			{
				return FAgentResult::NotFound(FString::Printf(TEXT("Component class '%s' not found."), *ClassSpec));
			}
			const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Add component ") + Name));
			Blueprint->Modify();
			SCS->Modify();
			USCS_Node* Node = SCS->CreateNode(ComponentClass, FName(*Name));
			const FString ParentName = AgentJson::GetString(Params, TEXT("parent"));
			USCS_Node* ParentNode = ParentName.IsEmpty() ? nullptr : SCS->FindSCSNode(FName(*ParentName));
			if (ParentNode)
			{
				ParentNode->AddChildNode(Node);
			}
			else
			{
				SCS->AddNode(Node);
			}
			Existing = Node;
			// fallthrough to property set
		}
		else if (!Existing)
		{
			return FAgentResult::NotFound(FString::Printf(TEXT("Component '%s' not found (Blueprint-added components only)."), *Name));
		}

		if (Op == TEXT("remove") || Op == TEXT("delete"))
		{
			const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Remove component ") + Name));
			Blueprint->Modify();
			SCS->Modify();
			SCS->RemoveNodeAndPromoteChildren(Existing);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			AgentCmd::NoteEdit(Context, Blueprint, TEXT("component.remove"));
			return FAgentResult::Ok(Json);
		}

		{
			const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Modify component ") + Name));
			TArray<FString> Changed;
			TArray<FString> Failed;
			if (TSharedPtr<FJsonObject> Props = AgentJson::GetObject(Params, TEXT("properties")))
			{
				if (Existing->ComponentTemplate)
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Props->Values)
					{
						FString SetError;
						const FString Value = Pair.Value->Type == EJson::String ? Pair.Value->AsString() : AgentJson::SerializeValue(Pair.Value);
						if (AgentProps::SetValue(Existing->ComponentTemplate, Pair.Key, Value, SetError))
						{
							Changed.Add(Pair.Key);
						}
						else
						{
							Failed.Add(Pair.Key + TEXT(": ") + SetError);
						}
					}
				}
			}
			if (AgentJson::Has(Params, TEXT("parent")) && Op == TEXT("modify"))
			{
				// Re-parenting: detach from current parent, attach to new
				const FString ParentName = AgentJson::GetString(Params, TEXT("parent"));
				USCS_Node* NewParent = ParentName.IsEmpty() ? nullptr : SCS->FindSCSNode(FName(*ParentName));
				for (USCS_Node* Other : SCS->GetAllNodes())
				{
					if (Other && Other->GetChildNodes().Contains(Existing))
					{
						Other->Modify();
						Other->RemoveChildNode(Existing);
					}
				}
				if (SCS->GetRootNodes().Contains(Existing))
				{
					SCS->RemoveNode(Existing);
				}
				if (NewParent) { NewParent->AddChildNode(Existing); } else { SCS->AddNode(Existing); }
				Changed.Add(TEXT("parent"));
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			AgentJson::SetStringArray(*Json, TEXT("changed"), Changed);
			if (Failed.Num() > 0) { AgentJson::SetStringArray(*Json, TEXT("failed"), Failed); }
		}
		AgentCmd::NoteEdit(Context, Blueprint, TEXT("component.") + Op);
		return FAgentResult::Ok(Json);
	}

	// ------------------------------------------------------------------ build

	FAgentResult Compile(const FJsonObject& Params, FAgentContext& Context)
	{
		TArray<FString> Specs = AgentJson::GetStringOrArray(Params, TEXT("assets"));
		if (Specs.Num() == 0)
		{
			const FString Single = AgentJson::GetString(Params, TEXT("asset"));
			if (!Single.IsEmpty()) { Specs.Add(Single); }
		}
		if (Specs.Num() == 0)
		{
			return FAgentResult::BadRequest(TEXT("Provide 'assets' (array) or 'asset'."));
		}
		const bool bSave = AgentJson::GetBool(Params, TEXT("save"), false);
		const bool bValidate = AgentJson::GetBool(Params, TEXT("validate"), false);

		TArray<TSharedPtr<FJsonValue>> Results;
		int32 Succeeded = 0;
		for (const FString& Spec : Specs)
		{
			FString LoadError;
			UBlueprint* Blueprint = AgentResolver::LoadBlueprint(Spec, LoadError);
			TSharedRef<FJsonObject> Item = AgentJson::Obj();
			if (!Blueprint)
			{
				Item->SetStringField(TEXT("asset"), Spec);
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), LoadError);
				Results.Add(MakeShared<FJsonValueObject>(Item));
				continue;
			}
			CompileOne(Blueprint, Context, Item);
			const bool bOk = AgentJson::GetBool(*Item, TEXT("ok"), false);
			if (bOk) { ++Succeeded; }
			if (bValidate)
			{
				TArray<FString> Errors, Warnings;
				const bool bValid = Context.Adapter->ValidateAsset(Blueprint, Errors, Warnings);
				TSharedRef<FJsonObject> Validation = AgentJson::Obj();
				Validation->SetBoolField(TEXT("ok"), bValid);
				AgentJson::SetStringArray(*Validation, TEXT("errors"), Errors);
				AgentJson::SetStringArray(*Validation, TEXT("warnings"), Warnings);
				Item->SetObjectField(TEXT("validation"), Validation);
			}
			if (bSave && bOk)
			{
				FString SaveError;
				const bool bSaved = Context.Adapter->SaveAsset(Blueprint, false, SaveError);
				Item->SetBoolField(TEXT("saved"), bSaved);
				if (!bSaved) { Item->SetStringField(TEXT("save_error"), SaveError); }
			}
			Results.Add(MakeShared<FJsonValueObject>(Item));
		}
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetArrayField(TEXT("results"), Results);
		Json->SetNumberField(TEXT("succeeded"), Succeeded);
		Json->SetNumberField(TEXT("total"), Specs.Num());
		return FAgentResult::Ok(Json);
	}

	FAgentResult Save(const FJsonObject& Params, FAgentContext& Context)
	{
		TArray<FString> Specs = AgentJson::GetStringOrArray(Params, TEXT("assets"));
		if (Specs.Num() == 0)
		{
			const FString Single = AgentJson::GetString(Params, TEXT("asset"));
			if (!Single.IsEmpty()) { Specs.Add(Single); }
		}
		const bool bOnlyIfDirty = AgentJson::GetBool(Params, TEXT("only_if_dirty"), true);
		TArray<TSharedPtr<FJsonValue>> Results;
		int32 Saved = 0;
		for (const FString& Spec : Specs)
		{
			FString LoadError;
			UObject* Asset = AgentResolver::LoadAsset(Spec, LoadError);
			TSharedRef<FJsonObject> Item = AgentJson::Obj();
			Item->SetStringField(TEXT("asset"), Asset ? AgentResolver::PackagePathOf(Asset) : Spec);
			if (!Asset)
			{
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), LoadError);
			}
			else
			{
				FString SaveError;
				const bool bWasDirty = Asset->GetOutermost()->IsDirty();
				const bool bOk = Context.Adapter->SaveAsset(Asset, bOnlyIfDirty, SaveError);
				Item->SetBoolField(TEXT("ok"), bOk);
				Item->SetBoolField(TEXT("was_dirty"), bWasDirty);
				if (!bOk) { Item->SetStringField(TEXT("error"), SaveError); } else { ++Saved; }
			}
			Results.Add(MakeShared<FJsonValueObject>(Item));
		}
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetArrayField(TEXT("results"), Results);
		Json->SetNumberField(TEXT("saved"), Saved);
		Json->SetNumberField(TEXT("total"), Specs.Num());
		return FAgentResult::Ok(Json);
	}

	FAgentResult Validate(const FJsonObject& Params, FAgentContext& Context)
	{
		TArray<FString> Specs = AgentJson::GetStringOrArray(Params, TEXT("assets"));
		if (Specs.Num() == 0)
		{
			const FString Single = AgentJson::GetString(Params, TEXT("asset"));
			if (!Single.IsEmpty()) { Specs.Add(Single); }
		}
		TArray<TSharedPtr<FJsonValue>> Results;
		int32 Passed = 0;
		for (const FString& Spec : Specs)
		{
			FString LoadError;
			UObject* Asset = AgentResolver::LoadAsset(Spec, LoadError);
			TSharedRef<FJsonObject> Item = AgentJson::Obj();
			Item->SetStringField(TEXT("asset"), Asset ? AgentResolver::PackagePathOf(Asset) : Spec);
			if (!Asset)
			{
				Item->SetBoolField(TEXT("ok"), false);
				Item->SetStringField(TEXT("error"), LoadError);
			}
			else
			{
				TArray<FString> Errors, Warnings;
				const bool bOk = Context.Adapter->ValidateAsset(Asset, Errors, Warnings);
				Item->SetBoolField(TEXT("ok"), bOk);
				AgentJson::SetStringArray(*Item, TEXT("errors"), Errors);
				AgentJson::SetStringArray(*Item, TEXT("warnings"), Warnings);
				if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
				{
					Item->SetStringField(TEXT("status"), Blueprint->Status == BS_Error ? TEXT("Error") : TEXT("OK"));
				}
				if (bOk) { ++Passed; }
			}
			Results.Add(MakeShared<FJsonValueObject>(Item));
		}
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetArrayField(TEXT("results"), Results);
		Json->SetNumberField(TEXT("passed"), Passed);
		Json->SetNumberField(TEXT("total"), Specs.Num());
		return FAgentResult::Ok(Json);
	}

	FAgentResult Reload(const FJsonObject& Params, FAgentContext& Context)
	{
		// Re-read from disk: discards unsaved in-memory changes for the asset.
		FAgentResult Error;
		UBlueprint* Blueprint = AgentCmd::RequireBlueprint(Params, Error);
		if (!Blueprint) { return Error; }
		UPackage* Package = Blueprint->GetOutermost();
		TArray<UPackage*> Packages = { Package };
		FText ErrorText;
		const bool bOk = UPackageTools::ReloadPackages(Packages);
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetStringField(TEXT("asset"), Package->GetName());
		Json->SetBoolField(TEXT("ok"), bOk);
		AgentCmd::NoteEdit(Context, Blueprint, TEXT("reloaded"));
		return FAgentResult::Ok(Json);
	}
}

void RegisterBlueprintCommands(FAgentCommandRegistry& Registry)
{
	Registry.Register(TEXT("blueprint.summary"), TEXT("Counts, parent, interfaces, refs (< 300 tokens)."), false, &Summary);
	Registry.Register(TEXT("blueprint.structure"), TEXT("Functions, variables, graphs, components, events."), false, &Structure);
	Registry.Register(TEXT("blueprint.index_entry"), TEXT("Cross-reference entry (calls/reads/writes) for the index."), false, &IndexEntry);
	Registry.Register(TEXT("blueprint.components"), TEXT("Components with non-default properties."), false, &Components);
	Registry.Register(TEXT("blueprint.variable"), TEXT("op=add|remove|rename|modify a member variable."), true, &VariableOp);
	Registry.Register(TEXT("blueprint.function"), TEXT("op=create|delete|rename|modify a function (inputs/outputs/pure)."), true, &FunctionOp);
	Registry.Register(TEXT("blueprint.interface"), TEXT("op=add|remove an implemented interface."), true, &InterfaceOp);
	Registry.Register(TEXT("blueprint.component"), TEXT("op=add|remove|modify a component (class, parent, properties)."), true, &ComponentOp);
	Registry.Register(TEXT("blueprint.compile"), TEXT("Compile assets; optional save/validate."), true, &Compile);
	Registry.Register(TEXT("blueprint.save"), TEXT("Save assets through the editor pipeline."), true, &Save);
	Registry.Register(TEXT("blueprint.validate"), TEXT("Run DataValidation on assets."), false, &Validate);
	Registry.Register(TEXT("blueprint.reload"), TEXT("Reload asset from disk (discard unsaved changes)."), true, &Reload);
}
