// Shared result/context types for agent commands.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FClaudeBlueprintAgentModule;
class IUnrealAdapter;
class FAgentChangeTracker;
class FAgentConfig;
class FAgentLogCapture;

/** Compact error codes shared with the MCP server (see docs/API.md). */
namespace AgentErrors
{
	static const TCHAR* BadRequest = TEXT("BAD_REQUEST");
	static const TCHAR* NotFound = TEXT("NOT_FOUND");
	static const TCHAR* Ambiguous = TEXT("AMBIGUOUS");
	static const TCHAR* Unsupported = TEXT("UNSUPPORTED");
	static const TCHAR* Conflict = TEXT("CONFLICT");
	static const TCHAR* CompileFailed = TEXT("COMPILE_FAILED");
	static const TCHAR* SaveFailed = TEXT("SAVE_FAILED");
	static const TCHAR* Internal = TEXT("INTERNAL");
	static const TCHAR* UnknownCommand = TEXT("UNKNOWN_COMMAND");
}

struct FAgentResult
{
	bool bOk = false;
	TSharedPtr<FJsonObject> Result;
	FString ErrorCode;
	FString ErrorMessage;
	TSharedPtr<FJsonObject> ErrorDetails;

	static FAgentResult Ok(const TSharedRef<FJsonObject>& InResult)
	{
		FAgentResult R;
		R.bOk = true;
		R.Result = InResult;
		return R;
	}

	static FAgentResult Ok()
	{
		return Ok(MakeShared<FJsonObject>());
	}

	static FAgentResult Error(const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr)
	{
		FAgentResult R;
		R.bOk = false;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		R.ErrorDetails = Details;
		return R;
	}

	static FAgentResult NotFound(const FString& Message)
	{
		return Error(AgentErrors::NotFound, Message);
	}

	static FAgentResult BadRequest(const FString& Message)
	{
		return Error(AgentErrors::BadRequest, Message);
	}

	static FAgentResult Unsupported(const FString& Message, const FString& CapabilityId = FString())
	{
		TSharedPtr<FJsonObject> Details;
		if (!CapabilityId.IsEmpty())
		{
			Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("capability"), CapabilityId);
		}
		return Error(AgentErrors::Unsupported, Message, Details);
	}
};

/** Services available to command handlers. */
struct FAgentContext
{
	FClaudeBlueprintAgentModule* Module = nullptr;
	IUnrealAdapter* Adapter = nullptr;
	FAgentChangeTracker* ChangeTracker = nullptr;
	FAgentConfig* Config = nullptr;
	FAgentLogCapture* LogCapture = nullptr;
	/** True while executing inside a batch (outer transaction already open). */
	bool bInBatch = false;
};

using FAgentCommandHandler = TFunction<FAgentResult(const FJsonObject& Params, FAgentContext& Context)>;

struct FAgentCommandInfo
{
	FString Name;
	FString Description;
	bool bMutating = false;
	FAgentCommandHandler Handler;
};
