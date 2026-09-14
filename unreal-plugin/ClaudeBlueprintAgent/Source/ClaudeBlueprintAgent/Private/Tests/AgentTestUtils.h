// Helpers for editor automation tests: run commands through the same code
// path the HTTP server uses and parse the JSON response.
#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "ClaudeBlueprintAgentModule.h"
#include "Core/AgentServer.h"
#include "Core/AgentJson.h"

namespace AgentTest
{
	static const TCHAR* TestFolder = TEXT("/Game/__ClaudeAgentTests");

	/** Executes a single-quoted JSON request (single quotes become double quotes). */
	inline TSharedPtr<FJsonObject> Exec(FAutomationTestBase& Test, const FString& SingleQuotedJson, bool bExpectOk = true)
	{
		const FString Json = SingleQuotedJson.Replace(TEXT("'"), TEXT("\""));
		FAgentServer* Server = FClaudeBlueprintAgentModule::Get().GetServer();
		if (!Server)
		{
			Test.AddError(TEXT("Agent server not available."));
			return nullptr;
		}
		const FString Response = Server->ExecuteJson(Json);
		TSharedPtr<FJsonObject> Parsed = AgentJson::Parse(Response);
		if (!Parsed.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("Unparseable response for %s: %s"), *Json, *Response));
			return nullptr;
		}
		const bool bOk = AgentJson::GetBool(*Parsed, TEXT("ok"), false);
		if (bExpectOk && !bOk)
		{
			Test.AddError(FString::Printf(TEXT("Command failed: %s -> %s"), *Json, *Response));
		}
		if (!bExpectOk && bOk)
		{
			Test.AddError(FString::Printf(TEXT("Command unexpectedly succeeded: %s"), *Json));
		}
		return Parsed;
	}

	inline TSharedPtr<FJsonObject> Result(const TSharedPtr<FJsonObject>& Response)
	{
		return Response.IsValid() ? AgentJson::GetObject(*Response, TEXT("result")) : nullptr;
	}

	inline FString Str(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		return Object.IsValid() ? AgentJson::GetString(*Object, Key) : FString();
	}

	inline int32 Int(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		return Object.IsValid() ? AgentJson::GetInt(*Object, Key) : -1;
	}
}
