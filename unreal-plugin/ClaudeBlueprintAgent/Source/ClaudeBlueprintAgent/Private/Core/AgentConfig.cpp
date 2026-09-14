#include "AgentConfig.h"
#include "AgentJson.h"
#include "AgentLog.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"

FString FAgentConfig::GetProjectDir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
}

FString FAgentConfig::GetProjectName()
{
	return FApp::GetProjectName();
}

FString FAgentConfig::GetProjectFile()
{
	return FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
}

FString FAgentConfig::GetAgentDir()
{
	return FPaths::Combine(GetProjectDir(), TEXT(".unreal-agent"));
}

FString FAgentConfig::GetSavedDir()
{
	return FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()), TEXT("ClaudeAgent"));
}

FString FAgentConfig::GetEndpointFile()
{
	return FPaths::Combine(GetSavedDir(), TEXT("endpoint.json"));
}

FString FAgentConfig::GetConfigFile()
{
	return FPaths::Combine(GetAgentDir(), TEXT("config.json"));
}

FString FAgentConfig::GetEngineVersionString()
{
	return FEngineVersion::Current().ToString(EVersionComponent::Patch);
}

void FAgentConfig::Load()
{
	ContentRoots = { TEXT("/Game") };

	FString Text;
	const FString File = GetConfigFile();
	if (FFileHelper::LoadFileToString(Text, *File))
	{
		TSharedPtr<FJsonObject> Json = AgentJson::Parse(Text);
		if (Json.IsValid())
		{
			// Plugin section takes precedence, then flat keys (shared with the server).
			TSharedPtr<FJsonObject> Plugin = AgentJson::GetObject(*Json, TEXT("plugin"));
			const FJsonObject& Src = Plugin.IsValid() ? *Plugin : *Json;
			Port = AgentJson::GetInt(Src, TEXT("port"), AgentJson::GetInt(*Json, TEXT("port"), Port));
			Token = AgentJson::GetString(Src, TEXT("token"), Token);
			ResponseMode = AgentJson::GetString(*Json, TEXT("responseMode"), ResponseMode);
			MaxResults = AgentJson::GetInt(*Json, TEXT("maxResults"), MaxResults);
			bAutoCompile = AgentJson::GetBool(*Json, TEXT("autoCompile"), bAutoCompile);
			bAutoSave = AgentJson::GetBool(*Json, TEXT("autoSave"), bAutoSave);
			bVisualValidation = AgentJson::GetBool(*Json, TEXT("visualValidation"), bVisualValidation);
			bAllowRemote = AgentJson::GetBool(Src, TEXT("allowRemote"), bAllowRemote);
			bAutoStart = AgentJson::GetBool(Src, TEXT("autoStart"), bAutoStart);
			TArray<FString> Roots = AgentJson::GetStringArray(*Json, TEXT("contentRoots"));
			if (Roots.Num() > 0)
			{
				ContentRoots = Roots;
			}
		}
		else
		{
			UE_LOG(LogClaudeAgent, Warning, TEXT("Could not parse %s, using defaults."), *File);
		}
	}

	const FString EnvPort = FPlatformMisc::GetEnvironmentVariable(TEXT("CLAUDE_AGENT_PORT"));
	if (!EnvPort.IsEmpty() && EnvPort.IsNumeric())
	{
		Port = FCString::Atoi(*EnvPort);
	}
	const FString EnvToken = FPlatformMisc::GetEnvironmentVariable(TEXT("CLAUDE_AGENT_TOKEN"));
	if (!EnvToken.IsEmpty())
	{
		Token = EnvToken;
	}
	if (Token.IsEmpty())
	{
		// Per-session secret; only readable through the endpoint file on this machine.
		Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}
}

TSharedRef<FJsonObject> FAgentConfig::ToJson() const
{
	TSharedRef<FJsonObject> Json = AgentJson::Obj();
	Json->SetNumberField(TEXT("port"), Port);
	Json->SetStringField(TEXT("responseMode"), ResponseMode);
	Json->SetNumberField(TEXT("maxResults"), MaxResults);
	Json->SetBoolField(TEXT("autoCompile"), bAutoCompile);
	Json->SetBoolField(TEXT("autoSave"), bAutoSave);
	Json->SetBoolField(TEXT("visualValidation"), bVisualValidation);
	Json->SetBoolField(TEXT("allowRemote"), bAllowRemote);
	AgentJson::SetStringArray(*Json, TEXT("contentRoots"), ContentRoots);
	return Json;
}

void FAgentConfig::WriteEndpointFile() const
{
	TSharedRef<FJsonObject> Json = AgentJson::Obj();
	Json->SetNumberField(TEXT("port"), Port);
	Json->SetStringField(TEXT("token"), Token);
	Json->SetNumberField(TEXT("pid"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
	Json->SetStringField(TEXT("project"), GetProjectFile());
	Json->SetStringField(TEXT("projectDir"), GetProjectDir());
	Json->SetStringField(TEXT("projectName"), GetProjectName());
	Json->SetStringField(TEXT("engineVersion"), GetEngineVersionString());
	Json->SetStringField(TEXT("pluginVersion"), CLAUDE_AGENT_PLUGIN_VERSION);
	Json->SetStringField(TEXT("started"), FDateTime::UtcNow().ToIso8601());
	Json->SetStringField(TEXT("url"), FString::Printf(TEXT("http://127.0.0.1:%d"), Port));

	IFileManager::Get().MakeDirectory(*GetSavedDir(), true);
	if (!FFileHelper::SaveStringToFile(AgentJson::Serialize(Json, true), *GetEndpointFile()))
	{
		UE_LOG(LogClaudeAgent, Warning, TEXT("Could not write endpoint file %s"), *GetEndpointFile());
	}
}

void FAgentConfig::RemoveEndpointFile() const
{
	IFileManager::Get().Delete(*GetEndpointFile(), false, true, true);
}
