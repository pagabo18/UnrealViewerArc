// Node/pin/graph -> JSON. Filtering (neighbourhood, query, pagination) happens
// here so the wire never carries a 500-node graph when 12 nodes were asked for.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

namespace AgentGraph
{
	struct FGraphFilter
	{
		/** Only nodes within Depth hops of this node GUID (empty = whole graph). */
		FString AroundGuid;
		int32 Depth = 1;
		/** Substring matched against kind/title/member/comment (case-insensitive). */
		FString Query;
		/** Kind filter, e.g. "CallFunction", "VariableSet", "Event". */
		FString Kind;
		int32 Offset = 0;
		int32 Limit = 200;
		bool bIncludePins = true;
		bool bIncludeHiddenPins = false;
		bool bIncludeDefaults = true;
	};

	FString NodeKind(const UEdGraphNode* Node);
	FString NodeMember(const UEdGraphNode* Node);
	FString NodeMemberClass(const UEdGraphNode* Node);
	FString NodeTitle(const UEdGraphNode* Node);
	bool NodeMatchesQuery(const UEdGraphNode* Node, const FString& Query);

	TSharedRef<FJsonObject> SerializePin(const UEdGraphPin* Pin, bool bIncludeDefaults);
	TSharedRef<FJsonObject> SerializeNode(const UEdGraphNode* Node, bool bIncludePins, bool bIncludeHiddenPins, bool bIncludeDefaults);
	TSharedRef<FJsonObject> SerializeGraph(UBlueprint* Blueprint, UEdGraph* Graph, const FGraphFilter& Filter);
	TSharedRef<FJsonObject> SerializeGraphSummary(UBlueprint* Blueprint, UEdGraph* Graph);

	void CollectNeighborhood(const UEdGraphNode* Center, int32 Depth, TSet<const UEdGraphNode*>& OutNodes);
	int32 CountNodes(const UEdGraph* Graph);

	/** Function signature (from the entry/result nodes). */
	TSharedRef<FJsonObject> SerializeFunctionSignature(UEdGraph* Graph);
}
