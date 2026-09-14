#include "AgentServer.h"
#include "AgentJson.h"
#include "AgentLog.h"
#include "AgentConfig.h"
#include "AgentTypes.h"
#include "AgentCommandRegistry.h"
#include "AgentChangeTracker.h"
#include "AgentLogCapture.h"
#include "ClaudeBlueprintAgentModule.h"
#include "Adapters/IUnrealAdapter.h"
#include "Adapters/AgentCompat.h"

#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpPath.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HttpResultCallback.h"
#include "Async/Async.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"

FAgentServer::FAgentServer(FClaudeBlueprintAgentModule& InModule)
	: Module(InModule)
{
}

FAgentServer::~FAgentServer()
{
	Stop();
}

bool FAgentServer::Start(int32 Port)
{
	Stop();

	if (!Module.GetConfig().bAllowRemote)
	{
		// The HttpServer module reads its bind address from config at listener start.
		FString Existing;
		if (!GConfig->GetString(TEXT("HTTPServer.Listeners"), TEXT("DefaultBindAddress"), Existing, GEngineIni) || Existing.IsEmpty())
		{
			GConfig->SetString(TEXT("HTTPServer.Listeners"), TEXT("DefaultBindAddress"), TEXT("127.0.0.1"), GEngineIni);
		}
	}

	FHttpServerModule& HttpServer = FHttpServerModule::Get();
	Router = HttpServer.GetHttpRouter(static_cast<uint32>(Port));
	if (!Router.IsValid())
	{
		UE_LOG(LogClaudeAgent, Error, TEXT("Could not create HTTP router on port %d"), Port);
		return false;
	}

	RpcHandle = Router->BindRoute(FHttpPath(TEXT("/rpc")), EHttpServerRequestVerbs::VERB_POST,
		CLAUDE_AGENT_MAKE_HTTP_HANDLER([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleRpc(Request, OnComplete);
		}));

	HealthHandle = Router->BindRoute(FHttpPath(TEXT("/health")), EHttpServerRequestVerbs::VERB_GET,
		CLAUDE_AGENT_MAKE_HTTP_HANDLER([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleHealth(Request, OnComplete);
		}));

	HttpServer.StartAllListeners();
	BoundPort = Port;
	bRunning = true;
	Module.GetConfig().WriteEndpointFile();
	UE_LOG(LogClaudeAgent, Display, TEXT("Claude Blueprint Agent listening on http://127.0.0.1:%d/rpc"), Port);
	return true;
}

void FAgentServer::Stop()
{
	if (Router.IsValid())
	{
		if (RpcHandle.IsValid())
		{
			Router->UnbindRoute(RpcHandle);
			RpcHandle.Reset();
		}
		if (HealthHandle.IsValid())
		{
			Router->UnbindRoute(HealthHandle);
			HealthHandle.Reset();
		}
		Router.Reset();
	}
	if (bRunning)
	{
		Module.GetConfig().RemoveEndpointFile();
	}
	bRunning = false;
	BoundPort = 0;
}

bool FAgentServer::IsAuthorized(const FHttpServerRequest& Request) const
{
	const FString& Expected = Module.GetConfig().Token;
	if (Expected.IsEmpty())
	{
		return true;
	}
	for (const TPair<FString, TArray<FString>>& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("x-agent-token"), ESearchCase::IgnoreCase))
		{
			for (const FString& Value : Header.Value)
			{
				if (Value == Expected)
				{
					return true;
				}
			}
		}
	}
	return false;
}

void FAgentServer::Respond(const FHttpResultCallback& OnComplete, const FString& Body, int32 Code) const
{
	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	Response->Code = static_cast<EHttpServerResponseCodes>(Code);
	OnComplete(MoveTemp(Response));
}

bool FAgentServer::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedRef<FJsonObject> Json = AgentJson::Obj();
	Json->SetBoolField(TEXT("ok"), true);
	Json->SetStringField(TEXT("project"), FAgentConfig::GetProjectName());
	Json->SetStringField(TEXT("engine"), FAgentConfig::GetEngineVersionString());
	Json->SetStringField(TEXT("plugin"), CLAUDE_AGENT_PLUGIN_VERSION);
	Json->SetNumberField(TEXT("seq"), static_cast<double>(Module.GetChangeTracker().GetSequence()));
	Respond(OnComplete, AgentJson::Serialize(Json), 200);
	return true;
}

bool FAgentServer::HandleRpc(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!IsAuthorized(Request))
	{
		Respond(OnComplete, TEXT("{\"ok\":false,\"error\":{\"code\":\"UNAUTHORIZED\",\"message\":\"Missing or invalid X-Agent-Token header.\"}}"), 401);
		return true;
	}

	FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
	FString Body(Converter.Length(), Converter.Get());

	if (IsInGameThread())
	{
		Respond(OnComplete, ExecuteJson(Body), 200);
	}
	else
	{
		// Editor APIs must run on the game thread; the callback may be invoked later.
		AsyncTask(ENamedThreads::GameThread, [this, Body = MoveTemp(Body), OnComplete]()
		{
			Respond(OnComplete, ExecuteJson(Body), 200);
		});
	}
	return true;
}

FString FAgentServer::ExecuteJson(const FString& RequestJson)
{
	check(IsInGameThread());
	const double StartTime = FPlatformTime::Seconds();
	++RequestCounter;

	TSharedRef<FJsonObject> Response = AgentJson::Obj();
	TSharedPtr<FJsonObject> Request = AgentJson::Parse(RequestJson);
	if (!Request.IsValid())
	{
		Response->SetBoolField(TEXT("ok"), false);
		TSharedRef<FJsonObject> Error = AgentJson::Obj();
		Error->SetStringField(TEXT("code"), AgentErrors::BadRequest);
		Error->SetStringField(TEXT("message"), TEXT("Request body is not valid JSON."));
		Response->SetObjectField(TEXT("error"), Error);
		return AgentJson::Serialize(Response);
	}

	const FString Cmd = AgentJson::GetString(*Request, TEXT("cmd"));
	TSharedPtr<FJsonObject> Params = AgentJson::GetObject(*Request, TEXT("params"));
	if (!Params.IsValid())
	{
		Params = AgentJson::Obj();
	}
	if (AgentJson::Has(*Request, TEXT("id")))
	{
		Response->SetField(TEXT("id"), Request->TryGetField(TEXT("id")));
	}

	FAgentContext Context;
	Context.Module = &Module;
	Context.Adapter = &Module.GetAdapter();
	Context.ChangeTracker = &Module.GetChangeTracker();
	Context.Config = &Module.GetConfig();
	Context.LogCapture = &Module.GetLogCapture();

	FAgentResult Result;
	const FAgentCommandInfo* Info = Module.GetRegistry().Find(Cmd);
	if (!Info)
	{
		Result = FAgentResult::Error(AgentErrors::UnknownCommand, FString::Printf(TEXT("Unknown command '%s'."), *Cmd));
	}
	else
	{
		Result = Info->Handler(*Params, Context);
	}

	Response->SetBoolField(TEXT("ok"), Result.bOk);
	if (Result.bOk)
	{
		Response->SetObjectField(TEXT("result"), Result.Result.IsValid() ? Result.Result : AgentJson::Obj());
	}
	else
	{
		TSharedRef<FJsonObject> Error = AgentJson::Obj();
		Error->SetStringField(TEXT("code"), Result.ErrorCode);
		Error->SetStringField(TEXT("message"), Result.ErrorMessage);
		if (Result.ErrorDetails.IsValid())
		{
			Error->SetObjectField(TEXT("details"), Result.ErrorDetails);
		}
		Response->SetObjectField(TEXT("error"), Error);
	}
	Response->SetNumberField(TEXT("ms"), (FPlatformTime::Seconds() - StartTime) * 1000.0);
	Response->SetNumberField(TEXT("seq"), static_cast<double>(Module.GetChangeTracker().GetSequence()));
	return AgentJson::Serialize(Response);
}
