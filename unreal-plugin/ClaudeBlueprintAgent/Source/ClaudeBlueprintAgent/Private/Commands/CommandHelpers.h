// Shared helpers for command implementations.
#pragma once

#include "CoreMinimal.h"
#include "Core/AgentTypes.h"
#include "Core/AgentJson.h"
#include "Core/AgentResolver.h"
#include "Core/AgentChangeTracker.h"
#include "Adapters/IUnrealAdapter.h"
#include "ScopedTransaction.h"
#include "Engine/Blueprint.h"

#define AGENT_LOCTEXT_NS "ClaudeBlueprintAgent"

namespace AgentCmd
{
	/** Reads Params[Key] as string; empty => error result populated. */
	inline bool RequireString(const FJsonObject& Params, const TCHAR* Key, FString& Out, FAgentResult& OutError)
	{
		Out = AgentJson::GetString(Params, Key);
		if (Out.IsEmpty())
		{
			OutError = FAgentResult::BadRequest(FString::Printf(TEXT("Missing required parameter '%s'."), Key));
			return false;
		}
		return true;
	}

	inline UBlueprint* RequireBlueprint(const FJsonObject& Params, FAgentResult& OutError, const TCHAR* Key = TEXT("asset"))
	{
		FString Spec;
		if (!RequireString(Params, Key, Spec, OutError))
		{
			return nullptr;
		}
		FString Error;
		UBlueprint* Blueprint = AgentResolver::LoadBlueprint(Spec, Error);
		if (!Blueprint)
		{
			OutError = FAgentResult::NotFound(Error);
		}
		return Blueprint;
	}

	inline FText TransactionTitle(const FString& Description)
	{
		return FText::FromString(TEXT("Claude: ") + Description);
	}

	inline TSharedRef<FJsonObject> AssetRef(const UObject* Asset)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetStringField(TEXT("asset"), AgentResolver::PackagePathOf(Asset));
		Json->SetStringField(TEXT("name"), Asset ? Asset->GetName() : FString());
		return Json;
	}

	/** Records an edit against an asset in the change tracker (so the server invalidates its cache). */
	inline void NoteEdit(FAgentContext& Context, const UObject* Asset, const FString& What)
	{
		if (Context.ChangeTracker && Asset)
		{
			Context.ChangeTracker->Record(TEXT("modified"), AgentResolver::PackagePathOf(Asset), What);
		}
	}
}
