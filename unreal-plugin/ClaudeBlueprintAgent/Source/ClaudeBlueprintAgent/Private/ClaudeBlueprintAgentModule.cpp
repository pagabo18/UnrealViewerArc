#include "ClaudeBlueprintAgentModule.h"
#include "Core/AgentLog.h"
#include "Core/AgentConfig.h"
#include "Core/AgentServer.h"
#include "Core/AgentCommandRegistry.h"
#include "Core/AgentChangeTracker.h"
#include "Core/AgentLogCapture.h"
#include "Adapters/IUnrealAdapter.h"

#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY(LogClaudeAgent);

#define LOCTEXT_NAMESPACE "ClaudeBlueprintAgent"

FClaudeBlueprintAgentModule& FClaudeBlueprintAgentModule::Get()
{
	return FModuleManager::LoadModuleChecked<FClaudeBlueprintAgentModule>("ClaudeBlueprintAgent");
}

bool FClaudeBlueprintAgentModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded("ClaudeBlueprintAgent");
}

void FClaudeBlueprintAgentModule::StartupModule()
{
	Config = MakeUnique<FAgentConfig>();
	Config->Load();

	Adapter = AgentAdapterFactory::Create();
	LogCapture = MakeUnique<FAgentLogCapture>();
	LogCapture->Register();

	Registry = MakeUnique<FAgentCommandRegistry>();
	RegisterSystemCommands(*Registry);
	RegisterAssetCommands(*Registry);
	RegisterBlueprintCommands(*Registry);
	RegisterGraphCommands(*Registry);
	RegisterWidgetCommands(*Registry);
	RegisterBatchCommand(*Registry);

	ChangeTracker = MakeUnique<FAgentChangeTracker>();
	Server = MakeUnique<FAgentServer>(*this);

	if (GEditor)
	{
		OnPostEngineInit();
	}
	else
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FClaudeBlueprintAgentModule::OnPostEngineInit);
	}
	RegisterConsoleCommands();
	UE_LOG(LogClaudeAgent, Log, TEXT("ClaudeBlueprintAgent %s loaded (%d commands)."), CLAUDE_AGENT_PLUGIN_VERSION, Registry->GetAll().Num());
}

void FClaudeBlueprintAgentModule::OnPostEngineInit()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (!GEditor)
	{
		return;
	}
	ChangeTracker->Register();
	if (Config->bAutoStart)
	{
		StartServer();
	}
}

void FClaudeBlueprintAgentModule::StartServer()
{
	if (!Server.IsValid())
	{
		return;
	}
	if (!Server->Start(Config->Port))
	{
		UE_LOG(LogClaudeAgent, Error, TEXT("Failed to start agent server on port %d (set CLAUDE_AGENT_PORT or .unreal-agent/config.json to change it)."), Config->Port);
	}
}

void FClaudeBlueprintAgentModule::StopServer()
{
	if (Server.IsValid())
	{
		Server->Stop();
	}
}

void FClaudeBlueprintAgentModule::RegisterConsoleCommands()
{
	ConsoleObjects.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("ClaudeAgent.Restart"),
		TEXT("Restart the Claude Blueprint Agent HTTP server (re-reads .unreal-agent/config.json)."),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			Config->Load();
			StartServer();
		}),
		ECVF_Default));

	ConsoleObjects.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("ClaudeAgent.Stop"),
		TEXT("Stop the Claude Blueprint Agent HTTP server."),
		FConsoleCommandDelegate::CreateLambda([this]() { StopServer(); }),
		ECVF_Default));

	ConsoleObjects.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("ClaudeAgent.Exec"),
		TEXT("Execute an agent command from the console: ClaudeAgent.Exec {\"cmd\":\"system.ping\"}"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			const FString Json = FString::Join(Args, TEXT(" "));
			UE_LOG(LogClaudeAgent, Display, TEXT("%s"), *Server->ExecuteJson(Json));
		}),
		ECVF_Default));
}

void FClaudeBlueprintAgentModule::ShutdownModule()
{
	for (IConsoleObject* Object : ConsoleObjects)
	{
		IConsoleManager::Get().UnregisterConsoleObject(Object);
	}
	ConsoleObjects.Reset();
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
	}
	if (Server.IsValid())
	{
		Server->Stop();
		Server.Reset();
	}
	if (ChangeTracker.IsValid())
	{
		ChangeTracker->Unregister();
		ChangeTracker.Reset();
	}
	if (LogCapture.IsValid())
	{
		LogCapture->Unregister();
		LogCapture.Reset();
	}
	Registry.Reset();
	Adapter.Reset();
	Config.Reset();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FClaudeBlueprintAgentModule, ClaudeBlueprintAgent)
