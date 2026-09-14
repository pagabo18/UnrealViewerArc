// Baseline implementation targeting UE 5.5. Later versions subclass this and
// override only what changed.
#pragma once

#include "CoreMinimal.h"
#include "IUnrealAdapter.h"

class FUnrealAdapterBase : public IUnrealAdapter
{
public:
	virtual FString GetAdapterName() const override { return TEXT("UE55"); }
	virtual FString GetEngineVersion() const override;

	virtual FString GetAssetClassPath(const FAssetData& Data) const override;
	virtual FString GetAssetClassName(const FAssetData& Data) const override;
	virtual bool SaveAsset(UObject* Asset, bool bOnlyIfDirty, FString& OutError) override;

	virtual bool ValidateAsset(UObject* Asset, TArray<FString>& OutErrors, TArray<FString>& OutWarnings) override;

	virtual bool ImplementInterface(UBlueprint* Blueprint, UClass* InterfaceClass) override;
	virtual bool RemoveInterface(UBlueprint* Blueprint, UClass* InterfaceClass) override;

	virtual bool EncodePNG(int32 Width, int32 Height, const TArray<FColor>& Pixels, TArray64<uint8>& OutData) override;
	virtual bool DecodePNG(const TArray64<uint8>& Data, int32& OutWidth, int32& OutHeight, TArray<FColor>& OutPixels) override;

	virtual UClass* ResolveClass(const FString& Spec) const override;
	virtual UScriptStruct* ResolveStruct(const FString& Spec) const override;
	virtual UEnum* ResolveEnum(const FString& Spec) const override;

protected:
	template <typename T>
	T* ResolveTypeGeneric(const FString& Spec) const;
};

/** UE 5.6: no API differences relevant to this plugin were required so far. */
class FUE56Adapter : public FUnrealAdapterBase
{
public:
	virtual FString GetAdapterName() const override { return TEXT("UE56"); }
};

/** UE 5.7: no API differences relevant to this plugin were required so far. */
class FUE57Adapter : public FUnrealAdapterBase
{
public:
	virtual FString GetAdapterName() const override { return TEXT("UE57"); }
};

/** Unknown/newer versions use the latest known adapter and log a notice. */
class FUEFutureAdapter : public FUE57Adapter
{
public:
	virtual FString GetAdapterName() const override { return TEXT("UE57+"); }
};
