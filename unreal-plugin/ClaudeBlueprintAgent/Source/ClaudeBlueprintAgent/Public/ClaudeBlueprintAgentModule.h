// Claude Blueprint Agent - editor module entry point.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FAgentServer;
class FAgentCommandRegistry;
class FAgentConfig;
class FAgentChangeTracker;
class FAgentLogCapture;
class IUnrealAdapter;

/**
 * Editor-only module. Owns the local HTTP server, the command registry and the
 * supporting services (config, change tracking, log capture, version adapter).
 *
 * Nothing here touches .uasset files directly: every mutation goes through the
 * regular editor APIs (Modify/transactions, Blueprint utilities, compile, save).
 */
class FClaudeBlueprintAgentModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FClaudeBlueprintAgentModule& Get();
	static bool IsAvailable();

	FAgentCommandRegistry& GetRegistry() const { return *Registry; }
	FAgentConfig& GetConfig() const { return *Config; }
	FAgentChangeTracker& GetChangeTracker() const { return *ChangeTracker; }
	FAgentLogCapture& GetLogCapture() const { return *LogCapture; }
	IUnrealAdapter& GetAdapter() const { return *Adapter; }
	FAgentServer* GetServer() const { return Server.Get(); }

	/** (Re)starts the HTTP server using the current config. */
	void StartServer();
	void StopServer();

private:
	void OnPostEngineInit();
	void RegisterConsoleCommands();

	TUniquePtr<FAgentConfig> Config;
	TUniquePtr<IUnrealAdapter> Adapter;
	TUniquePtr<FAgentChangeTracker> ChangeTracker;
	TUniquePtr<FAgentLogCapture> LogCapture;
	TUniquePtr<FAgentCommandRegistry> Registry;
	TUniquePtr<FAgentServer> Server;
	FDelegateHandle PostEngineInitHandle;
	TArray<IConsoleObject*> ConsoleObjects;
};
