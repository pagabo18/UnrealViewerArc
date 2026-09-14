// assets.* commands: registry-backed listing/search, references, source control, creation.
#include "CommandHelpers.h"
#include "Core/AgentCommandRegistry.h"
#include "Core/AgentConfig.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "WidgetBlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "SourceControlHelpers.h"
#include "ISourceControlModule.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace
{
	FString ShortClassFromTag(const FString& TagValue)
	{
		// "/Script/Engine.Actor" -> Actor ; "/Game/X/BP_Y.BP_Y_C" -> BP_Y ; "Class'/Script/Engine.Actor'" -> Actor
		FString Value = TagValue;
		if (Value.EndsWith(TEXT("'")))
		{
			int32 Quote = INDEX_NONE;
			Value.FindChar(TEXT('\''), Quote);
			Value = Value.Mid(Quote + 1, Value.Len() - Quote - 2);
		}
		const int32 Dot = Value.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (Dot != INDEX_NONE)
		{
			Value = Value.Mid(Dot + 1);
		}
		Value.RemoveFromEnd(TEXT("_C"));
		return Value;
	}

	TArray<FString> InterfacesFromTag(const FString& TagValue)
	{
		// ((Interface=/Script/X.IFoo),(Interface=/Game/Y.BPI_Bar_C)) -> [IFoo, BPI_Bar]
		TArray<FString> Result;
		FString Rest = TagValue;
		int32 Index;
		while ((Index = Rest.Find(TEXT("Interface="))) != INDEX_NONE)
		{
			Rest = Rest.Mid(Index + 10);
			int32 End = 0;
			while (End < Rest.Len() && Rest[End] != TEXT(')') && Rest[End] != TEXT(','))
			{
				++End;
			}
			Result.Add(ShortClassFromTag(Rest.Left(End)));
			Rest = Rest.Mid(End);
		}
		return Result;
	}

	TSharedRef<FJsonObject> AssetDataToJson(const FAssetData& Data, FAgentContext& Context, bool bWithMtime)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetStringField(TEXT("path"), Data.PackageName.ToString());
		Json->SetStringField(TEXT("name"), Data.AssetName.ToString());
		Json->SetStringField(TEXT("class"), Context.Adapter->GetAssetClassName(Data));
		FString Tag;
		if (Data.GetTagValue(FBlueprintTags::ParentClassPath, Tag))
		{
			Json->SetStringField(TEXT("parent"), ShortClassFromTag(Tag));
		}
		if (Data.GetTagValue(FBlueprintTags::NativeParentClassPath, Tag))
		{
			Json->SetStringField(TEXT("native_parent"), ShortClassFromTag(Tag));
		}
		if (Data.GetTagValue(FBlueprintTags::BlueprintType, Tag))
		{
			Json->SetStringField(TEXT("bp_type"), Tag.Replace(TEXT("BPTYPE_"), TEXT("")));
		}
		if (Data.GetTagValue(FBlueprintTags::ImplementedInterfaces, Tag))
		{
			AgentJson::SetStringArray(*Json, TEXT("interfaces"), InterfacesFromTag(Tag));
		}
		if (bWithMtime)
		{
			FString Filename;
			if (FPackageName::TryConvertLongPackageNameToFilename(Data.PackageName.ToString(), Filename, FPackageName::GetAssetPackageExtension()))
			{
				const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Filename);
				Json->SetNumberField(TEXT("mtime"), static_cast<double>(Stamp.ToUnixTimestamp()));
			}
		}
		if (const UPackage* Package = FindPackage(nullptr, *Data.PackageName.ToString()))
		{
			if (Package->IsDirty())
			{
				Json->SetBoolField(TEXT("dirty"), true);
			}
		}
		return Json;
	}

	void BuildFilter(const FJsonObject& Params, FAgentContext& Context, FARFilter& Filter)
	{
		TArray<FString> Paths = AgentJson::GetStringOrArray(Params, TEXT("paths"));
		if (Paths.Num() == 0)
		{
			Paths = Context.Config->ContentRoots;
		}
		for (const FString& Path : Paths)
		{
			Filter.PackagePaths.Add(*Path);
		}
		Filter.bRecursivePaths = true;
		const FString ClassFilter = AgentJson::GetString(Params, TEXT("class"));
		if (ClassFilter.Equals(TEXT("Blueprint"), ESearchCase::IgnoreCase))
		{
			Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
		}
		else if (ClassFilter.Equals(TEXT("WidgetBlueprint"), ESearchCase::IgnoreCase) || ClassFilter.Equals(TEXT("Widget"), ESearchCase::IgnoreCase))
		{
			Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
		}
		else if (!ClassFilter.IsEmpty())
		{
			if (UClass* Class = Context.Adapter->ResolveClass(ClassFilter))
			{
				Filter.ClassPaths.Add(Class->GetClassPathName());
				Filter.bRecursiveClasses = true;
			}
		}
	}

	FAgentResult List(const FJsonObject& Params, FAgentContext& Context)
	{
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		FARFilter Filter;
		BuildFilter(Params, Context, Filter);
		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);

		const FString Contains = AgentJson::GetString(Params, TEXT("contains"));
		const double ModifiedSince = AgentJson::GetNumber(Params, TEXT("modified_since"), 0.0);
		const bool bWithMtime = AgentJson::GetBool(Params, TEXT("mtime"), ModifiedSince > 0);
		const int32 Offset = FMath::Max(0, AgentJson::GetInt(Params, TEXT("offset"), 0));
		const int32 Limit = FMath::Clamp(AgentJson::GetInt(Params, TEXT("limit"), 5000), 1, 100000);
		const bool bOnlyBlueprints = AgentJson::GetBool(Params, TEXT("blueprints_only"), false);

		Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });

		TArray<TSharedPtr<FJsonValue>> Items;
		int32 Matched = 0;
		for (const FAssetData& Data : Assets)
		{
			if (bOnlyBlueprints)
			{
				FString ParentTag;
				if (!Data.GetTagValue(FBlueprintTags::ParentClassPath, ParentTag))
				{
					continue;
				}
			}
			if (!Contains.IsEmpty() && !Data.AssetName.ToString().Contains(Contains) && !Data.PackagePath.ToString().Contains(Contains))
			{
				continue;
			}
			TSharedRef<FJsonObject> Json = AssetDataToJson(Data, Context, bWithMtime);
			if (ModifiedSince > 0)
			{
				const double Mtime = AgentJson::GetNumber(*Json, TEXT("mtime"), 0);
				if (Mtime > 0 && Mtime <= ModifiedSince && !AgentJson::GetBool(*Json, TEXT("dirty"), false))
				{
					continue;
				}
			}
			++Matched;
			if (Matched <= Offset)
			{
				continue;
			}
			if (Items.Num() >= Limit)
			{
				continue;
			}
			Items.Add(MakeShared<FJsonValueObject>(Json));
		}
		TSharedRef<FJsonObject> Result = AgentJson::Obj();
		Result->SetArrayField(TEXT("assets"), Items);
		Result->SetNumberField(TEXT("matched"), Matched);
		Result->SetNumberField(TEXT("scanned"), Assets.Num());
		Result->SetBoolField(TEXT("truncated"), Matched > Offset + Items.Num());
		return FAgentResult::Ok(Result);
	}

	FAgentResult Info(const FJsonObject& Params, FAgentContext& Context)
	{
		FString Spec;
		FAgentResult Error;
		if (!AgentCmd::RequireString(Params, TEXT("asset"), Spec, Error)) { return Error; }
		FString LoadError;
		UObject* Asset = AgentResolver::LoadAsset(Spec, LoadError);
		if (!Asset)
		{
			return FAgentResult::NotFound(LoadError);
		}
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		const FAssetData Data = Registry.GetAssetByObjectPath(FSoftObjectPath(Asset));
		TSharedRef<FJsonObject> Json = AssetDataToJson(Data, Context, true);
		Json->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
		TArray<FName> Deps, Refs;
		Registry.GetDependencies(Data.PackageName, Deps, UE::AssetRegistry::EDependencyCategory::Package);
		Registry.GetReferencers(Data.PackageName, Refs, UE::AssetRegistry::EDependencyCategory::Package);
		auto ToStrings = [](const TArray<FName>& Names)
		{
			TArray<FString> Out;
			for (const FName& Name : Names)
			{
				const FString S = Name.ToString();
				if (!S.StartsWith(TEXT("/Script/")))
				{
					Out.Add(S);
				}
			}
			Out.Sort();
			return Out;
		};
		AgentJson::SetStringArray(*Json, TEXT("dependencies"), ToStrings(Deps));
		AgentJson::SetStringArray(*Json, TEXT("referencers"), ToStrings(Refs));
		Json->SetBoolField(TEXT("dirty"), Asset->GetOutermost()->IsDirty());
		return FAgentResult::Ok(Json);
	}

	FAgentResult DependencyQuery(const FJsonObject& Params, FAgentContext& Context, bool bReferencers)
	{
		FString Spec;
		FAgentResult Error;
		if (!AgentCmd::RequireString(Params, TEXT("asset"), Spec, Error)) { return Error; }
		FString Package = Spec.StartsWith(TEXT("/")) ? AgentResolver::ToPackagePath(Spec) : FString();
		if (Package.IsEmpty())
		{
			FString LoadError;
			UObject* Asset = AgentResolver::LoadAsset(Spec, LoadError);
			if (!Asset)
			{
				return FAgentResult::NotFound(LoadError);
			}
			Package = AgentResolver::PackagePathOf(Asset);
		}
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		TArray<FName> Names;
		if (bReferencers)
		{
			Registry.GetReferencers(*Package, Names, UE::AssetRegistry::EDependencyCategory::Package);
		}
		else
		{
			Registry.GetDependencies(*Package, Names, UE::AssetRegistry::EDependencyCategory::Package);
		}
		TArray<FString> Out;
		for (const FName& Name : Names)
		{
			const FString S = Name.ToString();
			if (!S.StartsWith(TEXT("/Script/")))
			{
				Out.Add(S);
			}
		}
		Out.Sort();
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetStringField(TEXT("asset"), Package);
		AgentJson::SetStringArray(*Json, bReferencers ? TEXT("referencers") : TEXT("dependencies"), Out);
		return FAgentResult::Ok(Json);
	}

	FAgentResult Dependencies(const FJsonObject& Params, FAgentContext& Context) { return DependencyQuery(Params, Context, false); }
	FAgentResult Referencers(const FJsonObject& Params, FAgentContext& Context) { return DependencyQuery(Params, Context, true); }

	FAgentResult SourceControl(const FJsonObject& Params, FAgentContext& Context)
	{
		TArray<FString> Specs = AgentJson::GetStringOrArray(Params, TEXT("assets"));
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		ISourceControlModule& SCC = ISourceControlModule::Get();
		Json->SetStringField(TEXT("provider"), SCC.IsEnabled() ? SCC.GetProvider().GetName().ToString() : TEXT("none"));
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Spec : Specs)
		{
			FString Filename;
			const FString Package = AgentResolver::ToPackagePath(Spec);
			TSharedRef<FJsonObject> Item = AgentJson::Obj();
			Item->SetStringField(TEXT("asset"), Package);
			if (FPackageName::TryConvertLongPackageNameToFilename(Package, Filename, FPackageName::GetAssetPackageExtension()))
			{
				Item->SetBoolField(TEXT("exists"), IFileManager::Get().FileExists(*Filename));
				if (SCC.IsEnabled())
				{
					FSourceControlState State = USourceControlHelpers::QueryFileState(Filename, true);
					Item->SetBoolField(TEXT("controlled"), State.bIsSourceControlled);
					Item->SetBoolField(TEXT("checkedOut"), State.bIsCheckedOut);
					Item->SetBoolField(TEXT("checkedOutOther"), State.bIsCheckedOutOther);
					Item->SetBoolField(TEXT("modified"), State.bIsModified);
					Item->SetBoolField(TEXT("canCheckOut"), State.bCanCheckOut);
				}
				if (const UPackage* Pkg = FindPackage(nullptr, *Package))
				{
					Item->SetBoolField(TEXT("dirty"), Pkg->IsDirty());
				}
			}
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		Json->SetArrayField(TEXT("assets"), Items);
		return FAgentResult::Ok(Json);
	}

	FAgentResult CheckOut(const FJsonObject& Params, FAgentContext& Context)
	{
		TArray<FString> Specs = AgentJson::GetStringOrArray(Params, TEXT("assets"));
		TArray<FString> Done, Failed;
		for (const FString& Spec : Specs)
		{
			FString Filename;
			if (FPackageName::TryConvertLongPackageNameToFilename(AgentResolver::ToPackagePath(Spec), Filename, FPackageName::GetAssetPackageExtension())
				&& USourceControlHelpers::CheckOutFile(Filename, true))
			{
				Done.Add(Spec);
			}
			else
			{
				Failed.Add(Spec);
			}
		}
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		AgentJson::SetStringArray(*Json, TEXT("checkedOut"), Done);
		AgentJson::SetStringArray(*Json, TEXT("failed"), Failed);
		return FAgentResult::Ok(Json);
	}

	FAgentResult OpenInEditor(const FJsonObject& Params, FAgentContext& Context)
	{
		FString Spec;
		FAgentResult Error;
		if (!AgentCmd::RequireString(Params, TEXT("asset"), Spec, Error)) { return Error; }
		FString LoadError;
		UObject* Asset = AgentResolver::LoadAsset(Spec, LoadError);
		if (!Asset)
		{
			return FAgentResult::NotFound(LoadError);
		}
		if (UAssetEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
		{
			Subsystem->OpenEditorForAsset(Asset);
		}
		return FAgentResult::Ok(AgentCmd::AssetRef(Asset));
	}

	FAgentResult CreateBlueprint(const FJsonObject& Params, FAgentContext& Context)
	{
		FString Path, ParentSpec;
		FAgentResult Error;
		if (!AgentCmd::RequireString(Params, TEXT("path"), Path, Error)) { return Error; }
		ParentSpec = AgentJson::GetString(Params, TEXT("parent"), TEXT("Actor"));
		const FString Kind = AgentJson::GetString(Params, TEXT("kind"), TEXT("auto")).ToLower(); // auto|blueprint|widget

		const FString PackageName = AgentResolver::ToPackagePath(Path);
		const FString AssetName = FPackageName::GetShortName(PackageName);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			return FAgentResult::BadRequest(FString::Printf(TEXT("'%s' is not a valid package path (expected /Game/...)."), *Path));
		}
		if (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName))
		{
			return FAgentResult::Error(AgentErrors::Conflict, FString::Printf(TEXT("Asset %s already exists."), *PackageName));
		}
		UClass* ParentClass = Context.Adapter->ResolveClass(ParentSpec);
		if (!ParentClass)
		{
			return FAgentResult::NotFound(FString::Printf(TEXT("Parent class '%s' not found."), *ParentSpec));
		}
		const bool bWidget = Kind == TEXT("widget") || (Kind == TEXT("auto") && ParentClass->IsChildOf(UUserWidget::StaticClass()));

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return FAgentResult::Error(AgentErrors::Internal, TEXT("CreatePackage failed."));
		}
		Package->FullyLoad();

		const FScopedTransaction Transaction(AgentCmd::TransactionTitle(TEXT("Create Blueprint ") + AssetName));
		UBlueprint* Blueprint = nullptr;
		if (bWidget)
		{
			Blueprint = FKismetEditorUtilities::CreateBlueprint(ParentClass, Package, *AssetName, BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), TEXT("ClaudeAgent"));
			if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
			{
				if (WidgetBlueprint->WidgetTree && !WidgetBlueprint->WidgetTree->RootWidget)
				{
					const FString RootClassSpec = AgentJson::GetString(Params, TEXT("root"), TEXT("CanvasPanel"));
					UClass* RootClass = Context.Adapter->ResolveClass(RootClassSpec);
					if (RootClass && RootClass->IsChildOf(UPanelWidget::StaticClass()))
					{
						UWidget* Root = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(RootClass, *RootClassSpec);
						WidgetBlueprint->WidgetTree->RootWidget = Root;
					}
				}
			}
		}
		else
		{
			Blueprint = FKismetEditorUtilities::CreateBlueprint(ParentClass, Package, *AssetName, BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("ClaudeAgent"));
		}
		if (!Blueprint)
		{
			return FAgentResult::Error(AgentErrors::Internal, TEXT("CreateBlueprint failed (is the parent class blueprintable?)."));
		}
		FAssetRegistryModule::AssetCreated(Blueprint);
		Package->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		AgentCmd::NoteEdit(Context, Blueprint, TEXT("created"));

		TSharedRef<FJsonObject> Json = AgentCmd::AssetRef(Blueprint);
		Json->SetStringField(TEXT("parent"), AgentResolver::GetClassDisplayName(ParentClass));
		Json->SetBoolField(TEXT("widget"), bWidget);
		return FAgentResult::Ok(Json);
	}
}

void RegisterAssetCommands(FAgentCommandRegistry& Registry)
{
	Registry.Register(TEXT("assets.list"), TEXT("List assets from the registry (no loading)."), false, &List);
	Registry.Register(TEXT("assets.info"), TEXT("Asset details + dependencies/referencers."), false, &Info);
	Registry.Register(TEXT("assets.dependencies"), TEXT("Package dependencies."), false, &Dependencies);
	Registry.Register(TEXT("assets.referencers"), TEXT("Packages referencing an asset."), false, &Referencers);
	Registry.Register(TEXT("assets.source_control"), TEXT("Source control status for assets."), false, &SourceControl);
	Registry.Register(TEXT("assets.checkout"), TEXT("Check out assets in source control."), true, &CheckOut);
	Registry.Register(TEXT("assets.open"), TEXT("Open an asset in its editor."), false, &OpenInEditor);
	Registry.Register(TEXT("assets.create_blueprint"), TEXT("Create a new Blueprint or Widget Blueprint."), true, &CreateBlueprint);
}
