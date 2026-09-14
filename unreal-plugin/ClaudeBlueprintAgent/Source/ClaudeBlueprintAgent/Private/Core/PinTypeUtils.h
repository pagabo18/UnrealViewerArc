// Compact, deterministic pin type strings:
//   bool int int64 float string name text byte exec wildcard
//   Vector Rotator (structs)  Actor (object ref)  class<Actor>  soft<Texture2D>
//   softclass<Actor>  iface<IInventory>  EItemType (enum)  delegate
//   [T] array   {T} set   {K:V} map    trailing & = by reference
#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class IUnrealAdapter;

namespace AgentPinTypes
{
	FString ToString(const FEdGraphPinType& Type);
	bool Parse(const FString& Spec, const IUnrealAdapter& Adapter, FEdGraphPinType& OutType, FString& OutError);
	bool ParseTerminal(const FString& Spec, const IUnrealAdapter& Adapter, FEdGraphTerminalType& OutType, FString& OutError);
}
