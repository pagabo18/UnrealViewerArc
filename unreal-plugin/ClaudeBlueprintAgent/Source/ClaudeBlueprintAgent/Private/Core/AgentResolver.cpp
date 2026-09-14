#include "AgentResolver.h"
#include "AgentLog.h"
#include "Adapters/IUnrealAdapter.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"

namespace AgentResolver
{
	FString ToObjectPath(const FString& PathOrPackage)
	{
		FString Path = PathOrPackage.TrimStartAndEnd();
		if (Path.IsEmpty() || !Path.StartsWith(TEXT("/")))
		{
			return Path;
		}
		// Strip export-text decoration Class'/Game/...'
		if (Path.EndsWith(TEXT("'")))
		{
			int32 Quote = INDEX_NONE;
			if (Path.FindChar(TEXT('\''), Quote))
			{
				Path = Path.Mid(Quote + 1, Path.Len() - Quote - 2);
			}
		}
		const int32 LastSlash = Path.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const int32 LastDot = Path.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (LastDot > LastSlash)
		{
			return Path;
		}
		return Path + TEXT(".") + Path.Mid(LastSlash + 1);
	}

	FString ToPackagePath(const FString& Path)
	{
		const int32 LastSlash = Path.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const int32 LastDot = Path.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (LastDot > LastSlash)
		{
			return Path.Left(LastDot);
		}
		return Path;
	}

	FString PackagePathOf(const UObject* Object)
	{
		if (!Object)
		{
			return FString();
		}
		return Object->GetOutermost()->GetName();
	}

	UObject* LoadAsset(const FString& InSpec, FString& OutError)
	{
		const FString Spec = InSpec.TrimStartAndEnd();
		if (Spec.IsEmpty())
		{
			OutError = TEXT("Empty asset reference.");
			return nullptr;
		}
		if (Spec.StartsWith(TEXT("/")))
		{
			const FString ObjectPath = ToObjectPath(Spec);
			if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
			{
				return Existing;
			}
			UObject* Loaded = LoadObject<UObject>(nullptr, *ObjectPath);
			if (!Loaded)
			{
				OutError = FString::Printf(TEXT("Asset not found: %s"), *Spec);
			}
			return Loaded;
		}

		// Short name: ask the asset registry.
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		TArray<FAssetData> Assets;
		FARFilter Filter;
		Filter.PackagePaths.Add(TEXT("/Game"));
		Filter.bRecursivePaths = true;
		Registry.GetAssets(Filter, Assets);
		TArray<FAssetData> Matches;
		for (const FAssetData& Data : Assets)
		{
			if (Data.AssetName.ToString().Equals(Spec, ESearchCase::IgnoreCase))
			{
				Matches.Add(Data);
			}
		}
		if (Matches.Num() == 0)
		{
			OutError = FString::Printf(TEXT("No asset named '%s' under /Game."), *Spec);
			return nullptr;
		}
		if (Matches.Num() > 1)
		{
			FString Candidates;
			for (const FAssetData& Data : Matches)
			{
				Candidates += TEXT(" ") + Data.PackageName.ToString();
			}
			OutError = FString::Printf(TEXT("Ambiguous asset name '%s':%s"), *Spec, *Candidates);
			return nullptr;
		}
		UObject* Loaded = Matches[0].GetAsset();
		if (!Loaded)
		{
			OutError = FString::Printf(TEXT("Failed to load %s"), *Matches[0].PackageName.ToString());
		}
		return Loaded;
	}

	UBlueprint* LoadBlueprint(const FString& Spec, FString& OutError)
	{
		UObject* Asset = LoadAsset(Spec, OutError);
		if (!Asset)
		{
			return nullptr;
		}
		UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
		if (!Blueprint)
		{
			OutError = FString::Printf(TEXT("%s is a %s, not a Blueprint."), *Spec, *Asset->GetClass()->GetName());
		}
		return Blueprint;
	}

	void GetAllGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& OutGraphs)
	{
		OutGraphs.Reset();
		if (Blueprint)
		{
			FBlueprintEditorUtils::GetAllGraphs(Blueprint, OutGraphs);
		}
	}

	UEdGraph* GetEventGraph(UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return nullptr;
		}
		if (UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
		{
			return Graph;
		}
		return Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
	}

	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& InGraphName, FString& OutError)
	{
		if (!Blueprint)
		{
			OutError = TEXT("No blueprint.");
			return nullptr;
		}
		const FString GraphName = InGraphName.TrimStartAndEnd();
		if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
		{
			if (UEdGraph* Graph = GetEventGraph(Blueprint))
			{
				return Graph;
			}
		}
		TArray<UEdGraph*> Graphs;
		GetAllGraphs(Blueprint, Graphs);
		UEdGraph* Partial = nullptr;
		int32 PartialCount = 0;
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			const FString Name = Graph->GetName();
			if (Name.Equals(GraphName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
			if (Name.Contains(GraphName))
			{
				Partial = Graph;
				++PartialCount;
			}
		}
		if (PartialCount == 1)
		{
			return Partial;
		}
		OutError = FString::Printf(TEXT("Graph '%s' not found in %s."), *GraphName, *Blueprint->GetName());
		return nullptr;
	}

	FString GraphKind(UBlueprint* Blueprint, UEdGraph* Graph)
	{
		if (!Blueprint || !Graph)
		{
			return TEXT("unknown");
		}
		if (Blueprint->UbergraphPages.Contains(Graph)) { return TEXT("event"); }
		if (Blueprint->FunctionGraphs.Contains(Graph)) { return TEXT("function"); }
		if (Blueprint->MacroGraphs.Contains(Graph)) { return TEXT("macro"); }
		if (Blueprint->DelegateSignatureGraphs.Contains(Graph)) { return TEXT("delegate"); }
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			if (Interface.Graphs.Contains(Graph))
			{
				return TEXT("interface");
			}
		}
		return TEXT("subgraph");
	}

	static UEdGraphNode* FindNodeInGraphRecursive(UEdGraph* Graph, const FGuid& Guid)
	{
		if (!Graph)
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == Guid)
			{
				return Node;
			}
		}
		for (UEdGraph* Sub : Graph->SubGraphs)
		{
			if (UEdGraphNode* Found = FindNodeInGraphRecursive(Sub, Guid))
			{
				return Found;
			}
		}
		return nullptr;
	}

	UEdGraphNode* FindNode(UBlueprint* Blueprint, UEdGraph* PreferredGraph, const FString& GuidStr, FString& OutError)
	{
		FGuid Guid;
		if (!FGuid::Parse(GuidStr, Guid))
		{
			OutError = FString::Printf(TEXT("'%s' is not a node GUID."), *GuidStr);
			return nullptr;
		}
		if (UEdGraphNode* Node = FindNodeInGraphRecursive(PreferredGraph, Guid))
		{
			return Node;
		}
		TArray<UEdGraph*> Graphs;
		GetAllGraphs(Blueprint, Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (UEdGraphNode* Node = FindNodeInGraphRecursive(Graph, Guid))
			{
				return Node;
			}
		}
		OutError = FString::Printf(TEXT("Node %s not found."), *GuidStr);
		return nullptr;
	}

	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& InNameOrId, EEdGraphPinDirection Direction, FString& OutError)
	{
		if (!Node)
		{
			OutError = TEXT("No node.");
			return nullptr;
		}
		const FString NameOrId = InNameOrId.TrimStartAndEnd();
		FGuid PinGuid;
		if (NameOrId.Len() >= 32 && FGuid::Parse(NameOrId, PinGuid))
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->PinId == PinGuid)
				{
					return Pin;
				}
			}
		}
		// Default exec pins: "" / "exec" / "in" -> execute ; "out" -> then
		FString Name = NameOrId;
		if (Direction == EGPD_Input && (Name.IsEmpty() || Name.Equals(TEXT("exec"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("in"), ESearchCase::IgnoreCase)))
		{
			Name = UEdGraphSchema_K2::PN_Execute.ToString();
		}
		if (Direction == EGPD_Output && (Name.IsEmpty() || Name.Equals(TEXT("exec"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("out"), ESearchCase::IgnoreCase)))
		{
			Name = UEdGraphSchema_K2::PN_Then.ToString();
		}
		UEdGraphPin* Partial = nullptr;
		int32 PartialCount = 0;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || (Direction != EGPD_MAX && Pin->Direction != Direction))
			{
				continue;
			}
			if (Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
			const FString Friendly = Pin->PinFriendlyName.IsEmpty() ? FString() : Pin->PinFriendlyName.ToString();
			if (!Friendly.IsEmpty() && Friendly.Equals(Name, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
			if (Pin->PinName.ToString().Replace(TEXT(" "), TEXT("")).Equals(Name.Replace(TEXT(" "), TEXT("")), ESearchCase::IgnoreCase))
			{
				Partial = Pin;
				++PartialCount;
			}
		}
		if (PartialCount == 1)
		{
			return Partial;
		}
		// Fallbacks for the common "the only exec pin" case.
		if (Name.Equals(UEdGraphSchema_K2::PN_Execute.ToString()) || Name.Equals(UEdGraphSchema_K2::PN_Then.ToString()))
		{
			UEdGraphPin* OnlyExec = nullptr;
			int32 ExecCount = 0;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && (Direction == EGPD_MAX || Pin->Direction == Direction) && !Pin->bHidden)
				{
					OnlyExec = Pin;
					++ExecCount;
				}
			}
			if (ExecCount == 1)
			{
				return OnlyExec;
			}
		}
		FString Available;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !Pin->bHidden && (Direction == EGPD_MAX || Pin->Direction == Direction))
			{
				Available += (Available.IsEmpty() ? TEXT("") : TEXT(", ")) + Pin->PinName.ToString();
			}
		}
		OutError = FString::Printf(TEXT("Pin '%s' not found on %s. Available: %s"), *InNameOrId, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Available);
		return nullptr;
	}

	static UFunction* FindFunctionInLibraries(const FString& Name, TArray<FString>& OutCandidates)
	{
		UFunction* Found = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				continue;
			}
			if (UFunction* Function = Class->FindFunctionByName(*Name, EIncludeSuperFlag::ExcludeSuper))
			{
				OutCandidates.Add(Class->GetName() + TEXT(".") + Name);
				Found = Function;
			}
		}
		return OutCandidates.Num() == 1 ? Found : nullptr;
	}

	UFunction* ResolveFunction(UBlueprint* Context, const IUnrealAdapter& Adapter, const FString& InSpec, FString& OutError)
	{
		FString Spec = InSpec.TrimStartAndEnd();
		if (Spec.IsEmpty())
		{
			OutError = TEXT("Empty function spec.");
			return nullptr;
		}
		// Fully qualified /Script/Module.Class:Function
		if (Spec.StartsWith(TEXT("/Script/")) && Spec.Contains(TEXT(":")))
		{
			if (UFunction* Function = FindObject<UFunction>(nullptr, *Spec))
			{
				return Function;
			}
		}
		FString ClassPart;
		FString FunctionPart = Spec;
		int32 SepIndex = INDEX_NONE;
		if (Spec.Contains(TEXT("::")))
		{
			Spec.Split(TEXT("::"), &ClassPart, &FunctionPart);
		}
		else if (Spec.StartsWith(TEXT("/")))
		{
			// "/Game/X/BP_Y.AddItem" or "/Game/X/BP_Y:AddItem"
			int32 Colon = INDEX_NONE;
			if (Spec.FindChar(TEXT(':'), Colon))
			{
				ClassPart = Spec.Left(Colon);
				FunctionPart = Spec.Mid(Colon + 1);
			}
			else
			{
				const int32 LastSlash = Spec.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				const int32 LastDot = Spec.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				if (LastDot > LastSlash)
				{
					ClassPart = Spec.Left(LastDot);
					FunctionPart = Spec.Mid(LastDot + 1);
				}
			}
		}
		else if (Spec.FindChar(TEXT('.'), SepIndex))
		{
			ClassPart = Spec.Left(SepIndex);
			FunctionPart = Spec.Mid(SepIndex + 1);
		}

		if (!ClassPart.IsEmpty())
		{
			UClass* Class = Adapter.ResolveClass(ClassPart);
			if (!Class)
			{
				// Maybe a blueprint asset whose skeleton class has the function
				FString LoadError;
				if (UBlueprint* Other = LoadBlueprint(ClassPart, LoadError))
				{
					Class = Other->SkeletonGeneratedClass ? Other->SkeletonGeneratedClass.Get() : Other->GeneratedClass.Get();
				}
			}
			if (!Class)
			{
				OutError = FString::Printf(TEXT("Class '%s' not found."), *ClassPart);
				return nullptr;
			}
			UFunction* Function = Class->FindFunctionByName(*FunctionPart);
			if (!Function)
			{
				OutError = FString::Printf(TEXT("Function '%s' not found on %s."), *FunctionPart, *Class->GetName());
			}
			return Function;
		}

		// Unqualified: self (skeleton) -> parents -> libraries
		if (Context)
		{
			UClass* SelfClass = Context->SkeletonGeneratedClass ? Context->SkeletonGeneratedClass.Get() : Context->GeneratedClass.Get();
			if (!SelfClass)
			{
				SelfClass = Context->ParentClass;
			}
			if (SelfClass)
			{
				if (UFunction* Function = SelfClass->FindFunctionByName(*FunctionPart))
				{
					return Function;
				}
			}
		}
		TArray<FString> Candidates;
		if (UFunction* Function = FindFunctionInLibraries(FunctionPart, Candidates))
		{
			return Function;
		}
		if (Candidates.Num() > 1)
		{
			OutError = FString::Printf(TEXT("Ambiguous function '%s': %s"), *FunctionPart, *FString::Join(Candidates, TEXT(", ")));
			return nullptr;
		}
		OutError = FString::Printf(TEXT("Function '%s' not found (self, parents, libraries). Use Class.Function."), *FunctionPart);
		return nullptr;
	}

	FString GetClassDisplayName(const UClass* Class)
	{
		if (!Class)
		{
			return TEXT("None");
		}
		FString Name = Class->GetName();
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	FString GetClassSpec(const UClass* Class)
	{
		if (!Class)
		{
			return FString();
		}
		if (const UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Class))
		{
			if (BPClass->ClassGeneratedBy)
			{
				return PackagePathOf(BPClass->ClassGeneratedBy);
			}
		}
		return Class->GetPathName();
	}
}
