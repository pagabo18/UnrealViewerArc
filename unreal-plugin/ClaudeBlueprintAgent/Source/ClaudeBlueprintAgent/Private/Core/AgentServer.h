#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"

class FClaudeBlueprintAgentModule;
class IHttpRouter;
struct FHttpServerRequest;
struct FHttpServerResponse;
using FHttpResultCallback = TFunction<void(TUniquePtr<FHttpServerResponse>&& Response)>;

/**
 * Localhost HTTP endpoint: POST /rpc {"cmd": "...", "params": {...}}.
 * Requests are executed on the game thread (required by editor APIs).
 */
class FAgentServer
{
public:
	explicit FAgentServer(FClaudeBlueprintAgentModule& InModule);
	~FAgentServer();

	bool Start(int32 Port);
	void Stop();
	bool IsRunning() const { return bRunning; }
	int32 GetPort() const { return BoundPort; }

	/** Executes a command synchronously (used by HTTP, console commands and tests). */
	FString ExecuteJson(const FString& RequestJson);

private:
	bool HandleRpc(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool IsAuthorized(const FHttpServerRequest& Request) const;
	void Respond(const FHttpResultCallback& OnComplete, const FString& Body, int32 Code) const;

	FClaudeBlueprintAgentModule& Module;
	TSharedPtr<IHttpRouter> Router;
	FHttpRouteHandle RpcHandle;
	FHttpRouteHandle HealthHandle;
	int32 BoundPort = 0;
	bool bRunning = false;
	uint64 RequestCounter = 0;
};
