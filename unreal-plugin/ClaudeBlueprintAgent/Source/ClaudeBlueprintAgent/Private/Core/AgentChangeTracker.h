#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UPackage;
struct FAssetData;
class FObjectPostSaveContext;

/**
 * Records asset-level change events (modified/compiled/saved/added/removed/renamed)
 * with a monotonically increasing sequence number. The MCP server polls
 * `changes.since` to invalidate exactly the cache entries that changed.
 */
class FAgentChangeTracker
{
public:
	struct FChange
	{
		uint64 Seq = 0;
		FString Type;
		FString Asset;      // package path, e.g. /Game/UI/WBP_MainMenu
		FString Extra;      // e.g. old name for renames
		double Time = 0.0;  // FPlatformTime::Seconds()
	};

	void Register();
	void Unregister();

	void Record(const FString& Type, const FString& AssetPackage, const FString& Extra = FString());
	uint64 GetSequence() const { return Sequence; }
	uint64 GetAssetVersion(const FString& AssetPackage) const;
	TSharedRef<FJsonObject> GetSince(uint64 Since, int32 Max) const;

	/** Whether the package path belongs to project content (as opposed to /Engine, /Temp, transient). */
	static bool IsProjectAssetPackage(const FString& PackageName);
	static FString PackageOf(const UObject* Object);

private:
	void OnObjectModified(UObject* Object);
	void OnBlueprintPreCompile(UBlueprint* Blueprint);
	void OnPackageSaved(const FString& Filename, UPackage* Package, FObjectPostSaveContext Context);
	void OnAssetAdded(const FAssetData& Data);
	void OnAssetRemoved(const FAssetData& Data);
	void OnAssetRenamed(const FAssetData& Data, const FString& OldPath);

	uint64 Sequence = 0;
	TArray<FChange> Ring;
	int32 RingCapacity = 4000;
	TMap<FString, uint64> AssetVersions;
	FString LastModifiedAsset;
	double LastModifiedTime = 0.0;

	FDelegateHandle ModifiedHandle;
	FDelegateHandle PreCompileHandle;
	FDelegateHandle SavedHandle;
	FDelegateHandle AddedHandle;
	FDelegateHandle RemovedHandle;
	FDelegateHandle RenamedHandle;
};
