// Engine-version adapter. All APIs that changed (or may change) between
// supported engine versions are routed through this interface so command
// code stays free of version checks.
#pragma once

#include "CoreMinimal.h"

class UObject;
class UClass;
class UBlueprint;
struct FAssetData;

class IUnrealAdapter
{
public:
	virtual ~IUnrealAdapter() = default;

	virtual FString GetAdapterName() const = 0;
	virtual FString GetEngineVersion() const = 0;

	// ---- Assets -----------------------------------------------------------
	virtual FString GetAssetClassPath(const FAssetData& Data) const = 0;
	virtual FString GetAssetClassName(const FAssetData& Data) const = 0;
	virtual bool SaveAsset(UObject* Asset, bool bOnlyIfDirty, FString& OutError) = 0;

	// ---- Validation -------------------------------------------------------
	/** Runs the DataValidation subsystem on the object. Returns false if validation reported errors. */
	virtual bool ValidateAsset(UObject* Asset, TArray<FString>& OutErrors, TArray<FString>& OutWarnings) = 0;

	// ---- Blueprint interfaces --------------------------------------------
	virtual bool ImplementInterface(UBlueprint* Blueprint, UClass* InterfaceClass) = 0;
	virtual bool RemoveInterface(UBlueprint* Blueprint, UClass* InterfaceClass) = 0;

	// ---- Images -----------------------------------------------------------
	virtual bool EncodePNG(int32 Width, int32 Height, const TArray<FColor>& Pixels, TArray64<uint8>& OutData) = 0;
	virtual bool DecodePNG(const TArray64<uint8>& Data, int32& OutWidth, int32& OutHeight, TArray<FColor>& OutPixels) = 0;

	// ---- Classes ----------------------------------------------------------
	/** Resolves "Actor", "/Script/Engine.Actor", "/Game/X/BP_Y", "/Game/X/BP_Y.BP_Y_C". */
	virtual UClass* ResolveClass(const FString& Spec) const = 0;
	virtual UScriptStruct* ResolveStruct(const FString& Spec) const = 0;
	virtual UEnum* ResolveEnum(const FString& Spec) const = 0;
};

namespace AgentAdapterFactory
{
	/** Chooses the adapter for the engine this plugin was compiled against. */
	TUniquePtr<IUnrealAdapter> Create();
}
