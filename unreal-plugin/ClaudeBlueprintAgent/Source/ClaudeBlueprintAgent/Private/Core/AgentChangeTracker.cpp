#include "AgentChangeTracker.h"
#include "AgentJson.h"
#include "AgentLog.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/ObjectSaveContext.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "HAL/PlatformTime.h"
#include "Algo/Reverse.h"
#include "Modules/ModuleManager.h"

bool FAgentChangeTracker::IsProjectAssetPackage(const FString& PackageName)
{
	if (PackageName.IsEmpty() || !PackageName.StartsWith(TEXT("/")))
	{
		return false;
	}
	if (PackageName.StartsWith(TEXT("/Engine/")) || PackageName.StartsWith(TEXT("/Temp/")) || PackageName.StartsWith(TEXT("/Script/")))
	{
		return false;
	}
	if (PackageName.Contains(TEXT("/Transient")) || PackageName.Contains(TEXT("_REINST")) || PackageName.Contains(TEXT("SKEL_")))
	{
		return false;
	}
	return true;
}

FString FAgentChangeTracker::PackageOf(const UObject* Object)
{
	if (!Object)
	{
		return FString();
	}
	const UPackage* Package = Object->GetOutermost();
	return Package ? Package->GetName() : FString();
}

void FAgentChangeTracker::Register()
{
	ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddRaw(this, &FAgentChangeTracker::OnObjectModified);
	SavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(this, &FAgentChangeTracker::OnPackageSaved);
	if (GEditor)
	{
		PreCompileHandle = GEditor->OnBlueprintPreCompile().AddRaw(this, &FAgentChangeTracker::OnBlueprintPreCompile);
	}
	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	AddedHandle = Registry.OnAssetAdded().AddRaw(this, &FAgentChangeTracker::OnAssetAdded);
	RemovedHandle = Registry.OnAssetRemoved().AddRaw(this, &FAgentChangeTracker::OnAssetRemoved);
	RenamedHandle = Registry.OnAssetRenamed().AddRaw(this, &FAgentChangeTracker::OnAssetRenamed);
}

void FAgentChangeTracker::Unregister()
{
	FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
	UPackage::PackageSavedWithContextEvent.Remove(SavedHandle);
	if (GEditor)
	{
		GEditor->OnBlueprintPreCompile().Remove(PreCompileHandle);
	}
	if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
	{
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		Registry.OnAssetAdded().Remove(AddedHandle);
		Registry.OnAssetRemoved().Remove(RemovedHandle);
		Registry.OnAssetRenamed().Remove(RenamedHandle);
	}
}

void FAgentChangeTracker::Record(const FString& Type, const FString& AssetPackage, const FString& Extra)
{
	if (!IsProjectAssetPackage(AssetPackage))
	{
		return;
	}
	FChange Change;
	Change.Seq = ++Sequence;
	Change.Type = Type;
	Change.Asset = AssetPackage;
	Change.Extra = Extra;
	Change.Time = FPlatformTime::Seconds();
	Ring.Add(Change);
	if (Ring.Num() > RingCapacity)
	{
		Ring.RemoveAt(0, Ring.Num() - RingCapacity, EAllowShrinking::No);
	}
	AssetVersions.FindOrAdd(AssetPackage) = Sequence;
}

uint64 FAgentChangeTracker::GetAssetVersion(const FString& AssetPackage) const
{
	const uint64* Version = AssetVersions.Find(AssetPackage);
	return Version ? *Version : 0;
}

TSharedRef<FJsonObject> FAgentChangeTracker::GetSince(uint64 Since, int32 Max) const
{
	TSharedRef<FJsonObject> Json = AgentJson::Obj();
	TArray<TSharedPtr<FJsonValue>> Items;
	int32 Skipped = 0;
	bool bTruncated = false;
	// Coalesce repeated entries for the same asset+type within the window.
	TSet<FString> Seen;
	for (int32 Index = Ring.Num() - 1; Index >= 0; --Index)
	{
		const FChange& Change = Ring[Index];
		if (Change.Seq <= Since)
		{
			break;
		}
		const FString Key = Change.Type + TEXT("|") + Change.Asset;
		if (Seen.Contains(Key))
		{
			++Skipped;
			continue;
		}
		Seen.Add(Key);
		if (Items.Num() >= Max)
		{
			bTruncated = true;
			break;
		}
		TSharedRef<FJsonObject> Item = AgentJson::Obj();
		Item->SetNumberField(TEXT("seq"), static_cast<double>(Change.Seq));
		Item->SetStringField(TEXT("type"), Change.Type);
		Item->SetStringField(TEXT("asset"), Change.Asset);
		if (!Change.Extra.IsEmpty())
		{
			Item->SetStringField(TEXT("extra"), Change.Extra);
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	Algo::Reverse(Items);
	Json->SetArrayField(TEXT("changes"), Items);
	Json->SetNumberField(TEXT("seq"), static_cast<double>(Sequence));
	Json->SetBoolField(TEXT("truncated"), bTruncated);
	// If the caller is far behind the ring, tell it to do a full refresh.
	const bool bOverflow = Ring.Num() > 0 && Since > 0 && Since < Ring[0].Seq - 1;
	Json->SetBoolField(TEXT("overflow"), bOverflow);
	return Json;
}

void FAgentChangeTracker::OnObjectModified(UObject* Object)
{
	const FString Package = PackageOf(Object);
	if (!IsProjectAssetPackage(Package))
	{
		return;
	}
	// UObject::Modify fires per sub-object; collapse bursts for the same asset.
	const double Now = FPlatformTime::Seconds();
	if (Package == LastModifiedAsset && (Now - LastModifiedTime) < 0.25)
	{
		AssetVersions.FindOrAdd(Package) = Sequence;
		return;
	}
	LastModifiedAsset = Package;
	LastModifiedTime = Now;
	Record(TEXT("modified"), Package);
}

void FAgentChangeTracker::OnBlueprintPreCompile(UBlueprint* Blueprint)
{
	Record(TEXT("compiled"), PackageOf(Blueprint));
}

void FAgentChangeTracker::OnPackageSaved(const FString& Filename, UPackage* Package, FObjectPostSaveContext Context)
{
	if (Package)
	{
		Record(TEXT("saved"), Package->GetName());
	}
}

void FAgentChangeTracker::OnAssetAdded(const FAssetData& Data)
{
	Record(TEXT("added"), Data.PackageName.ToString());
}

void FAgentChangeTracker::OnAssetRemoved(const FAssetData& Data)
{
	Record(TEXT("removed"), Data.PackageName.ToString());
}

void FAgentChangeTracker::OnAssetRenamed(const FAssetData& Data, const FString& OldPath)
{
	Record(TEXT("renamed"), Data.PackageName.ToString(), OldPath);
}
