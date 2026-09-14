// Resolves the loose identifiers the MCP server sends (asset paths/names,
// graph names, node GUIDs, pin names, function specs) into editor objects.
#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UFunction;
class UClass;
class IUnrealAdapter;

namespace AgentResolver
{
	/** "/Game/A/BP_X" -> "/Game/A/BP_X.BP_X"; object paths are returned unchanged. */
	FString ToObjectPath(const FString& PathOrPackage);
	/** "/Game/A/BP_X.BP_X" -> "/Game/A/BP_X". */
	FString ToPackagePath(const FString& Path);
	FString PackagePathOf(const UObject* Object);

	/** Accepts package path, object path, or a unique asset short name ("BP_Player"). */
	UObject* LoadAsset(const FString& Spec, FString& OutError);
	UBlueprint* LoadBlueprint(const FString& Spec, FString& OutError);

	void GetAllGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& OutGraphs);
	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName, FString& OutError);
	UEdGraph* GetEventGraph(UBlueprint* Blueprint);
	FString GraphKind(UBlueprint* Blueprint, UEdGraph* Graph);

	/** Node lookup by GUID (string). Searches PreferredGraph first, then all graphs (incl. sub-graphs). */
	UEdGraphNode* FindNode(UBlueprint* Blueprint, UEdGraph* PreferredGraph, const FString& Guid, FString& OutError);
	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& NameOrId, EEdGraphPinDirection Direction, FString& OutError);

	/**
	 * Function spec forms: "AddItem" (self/parent/libraries), "KismetSystemLibrary.PrintString",
	 * "/Script/Engine.KismetSystemLibrary:PrintString", "/Game/X/BP_Y.AddItem", "BP_Y.AddItem".
	 */
	UFunction* ResolveFunction(UBlueprint* Context, const IUnrealAdapter& Adapter, const FString& Spec, FString& OutError);

	FString GetClassDisplayName(const UClass* Class);
	FString GetClassSpec(const UClass* Class); // /Script/... or /Game/... path usable as input
}
