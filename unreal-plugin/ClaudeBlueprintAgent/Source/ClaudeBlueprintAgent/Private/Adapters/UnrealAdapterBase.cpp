#include "UnrealAdapterBase.h"
#include "AgentCompat.h"
#include "Core/AgentLog.h"

#include "Misc/EngineVersion.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/Blueprint.h"
#include "EditorAssetLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EditorValidatorSubsystem.h"
#include "Misc/DataValidation.h"
#include "Editor.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "UObject/Class.h"

FString FUnrealAdapterBase::GetEngineVersion() const
{
	return FEngineVersion::Current().ToString(EVersionComponent::Patch);
}

FString FUnrealAdapterBase::GetAssetClassPath(const FAssetData& Data) const
{
	return Data.AssetClassPath.ToString();
}

FString FUnrealAdapterBase::GetAssetClassName(const FAssetData& Data) const
{
	return Data.AssetClassPath.GetAssetName().ToString();
}

bool FUnrealAdapterBase::SaveAsset(UObject* Asset, bool bOnlyIfDirty, FString& OutError)
{
	if (!Asset)
	{
		OutError = TEXT("No asset to save.");
		return false;
	}
	// Goes through the editor pipeline (source control checkout, dirty flags, notifications).
	if (!UEditorAssetLibrary::SaveLoadedAsset(Asset, bOnlyIfDirty))
	{
		OutError = FString::Printf(TEXT("SaveLoadedAsset failed for %s"), *Asset->GetPathName());
		return false;
	}
	return true;
}

bool FUnrealAdapterBase::ValidateAsset(UObject* Asset, TArray<FString>& OutErrors, TArray<FString>& OutWarnings)
{
	if (!Asset || !GEditor)
	{
		OutErrors.Add(TEXT("No asset or editor."));
		return false;
	}
	UEditorValidatorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>();
	FDataValidationContext Context;
	EDataValidationResult Result = EDataValidationResult::NotValidated;
	if (Subsystem)
	{
		Result = Subsystem->IsObjectValidWithContext(Asset, Context);
	}
	else
	{
		Result = Asset->IsDataValid(Context);
	}
	for (const FDataValidationContext::FIssue& Issue : Context.GetIssues())
	{
		if (Issue.Severity == EMessageSeverity::Error)
		{
			OutErrors.Add(Issue.Message.ToString());
		}
		else if (Issue.Severity == EMessageSeverity::Warning)
		{
			OutWarnings.Add(Issue.Message.ToString());
		}
	}
	return Result != EDataValidationResult::Invalid && OutErrors.Num() == 0;
}

bool FUnrealAdapterBase::ImplementInterface(UBlueprint* Blueprint, UClass* InterfaceClass)
{
	if (!Blueprint || !InterfaceClass)
	{
		return false;
	}
	return FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceClass->GetClassPathName());
}

bool FUnrealAdapterBase::RemoveInterface(UBlueprint* Blueprint, UClass* InterfaceClass)
{
	if (!Blueprint || !InterfaceClass)
	{
		return false;
	}
	FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfaceClass->GetClassPathName(), /*bPreserveFunctions*/ false);
	return true;
}

bool FUnrealAdapterBase::EncodePNG(int32 Width, int32 Height, const TArray<FColor>& Pixels, TArray64<uint8>& OutData)
{
	if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
	{
		return false;
	}
	FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), OutData);
	return OutData.Num() > 0;
}

bool FUnrealAdapterBase::DecodePNG(const TArray64<uint8>& Data, int32& OutWidth, int32& OutHeight, TArray<FColor>& OutPixels)
{
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Data.GetData(), Data.Num()))
	{
		return false;
	}
	TArray64<uint8> Raw;
	if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
	{
		return false;
	}
	OutWidth = Wrapper->GetWidth();
	OutHeight = Wrapper->GetHeight();
	OutPixels.SetNumUninitialized(OutWidth * OutHeight);
	FMemory::Memcpy(OutPixels.GetData(), Raw.GetData(), FMath::Min<int64>(Raw.Num(), static_cast<int64>(OutPixels.Num()) * sizeof(FColor)));
	return true;
}

template <typename T>
T* FUnrealAdapterBase::ResolveTypeGeneric(const FString& InSpec) const
{
	FString Spec = InSpec.TrimStartAndEnd();
	if (Spec.IsEmpty())
	{
		return nullptr;
	}
	// Strip common decorations: "class:Actor", "U"/"A" prefixes are NOT stripped (they are part of C++ names only).
	int32 ColonIndex;
	if (Spec.FindChar(TEXT(':'), ColonIndex) && !Spec.StartsWith(TEXT("/")))
	{
		Spec = Spec.Mid(ColonIndex + 1);
	}
	if (Spec.EndsWith(TEXT("'")) && Spec.Contains(TEXT("'")))
	{
		// Export-text form: /Script/Engine.Actor'/Game/...' -> take inner path
		int32 First = INDEX_NONE;
		Spec.FindChar(TEXT('\''), First);
		Spec = Spec.Mid(First + 1, Spec.Len() - First - 2);
	}
	if (Spec.StartsWith(TEXT("/")))
	{
		if (T* Found = FindObject<T>(nullptr, *Spec))
		{
			return Found;
		}
		if (T* Loaded = LoadObject<T>(nullptr, *Spec))
		{
			return Loaded;
		}
		// Blueprint asset path -> generated class
		FString AssetPath = Spec;
		if (!AssetPath.Contains(TEXT(".")))
		{
			AssetPath += TEXT(".") + FPackageName::GetShortName(AssetPath);
		}
		if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath))
		{
			return Cast<T>(Blueprint->GeneratedClass);
		}
		if (!Spec.EndsWith(TEXT("_C")))
		{
			return FindObject<T>(nullptr, *(AssetPath + TEXT("_C")));
		}
		return nullptr;
	}
	// Short name: engine-wide lookup (handles native and loaded Blueprint classes).
	if (T* Found = UClass::TryFindTypeSlow<T>(Spec))
	{
		return Found;
	}
	// Blueprint short name (e.g. "BP_Player") -> "BP_Player_C" among loaded classes
	if (T* FoundC = UClass::TryFindTypeSlow<T>(Spec + TEXT("_C")))
	{
		return FoundC;
	}
	return nullptr;
}

UClass* FUnrealAdapterBase::ResolveClass(const FString& Spec) const
{
	return ResolveTypeGeneric<UClass>(Spec);
}

UScriptStruct* FUnrealAdapterBase::ResolveStruct(const FString& Spec) const
{
	return ResolveTypeGeneric<UScriptStruct>(Spec);
}

UEnum* FUnrealAdapterBase::ResolveEnum(const FString& Spec) const
{
	return ResolveTypeGeneric<UEnum>(Spec);
}

namespace AgentAdapterFactory
{
	TUniquePtr<IUnrealAdapter> Create()
	{
		TUniquePtr<IUnrealAdapter> Adapter;
#if CLAUDE_AGENT_ENGINE_MAJOR == 5 && CLAUDE_AGENT_ENGINE_MINOR == 5
		Adapter = MakeUnique<FUnrealAdapterBase>();
#elif CLAUDE_AGENT_ENGINE_MAJOR == 5 && CLAUDE_AGENT_ENGINE_MINOR == 6
		Adapter = MakeUnique<FUE56Adapter>();
#elif CLAUDE_AGENT_ENGINE_MAJOR == 5 && CLAUDE_AGENT_ENGINE_MINOR == 7
		Adapter = MakeUnique<FUE57Adapter>();
#else
		Adapter = MakeUnique<FUEFutureAdapter>();
		UE_LOG(LogClaudeAgent, Warning, TEXT("Engine %d.%d is not an explicitly supported version; using the latest adapter."), CLAUDE_AGENT_ENGINE_MAJOR, CLAUDE_AGENT_ENGINE_MINOR);
#endif
		UE_LOG(LogClaudeAgent, Log, TEXT("Using adapter %s for engine %s"), *Adapter->GetAdapterName(), *Adapter->GetEngineVersion());
		return Adapter;
	}
}
