#pragma once

#include "CoreMinimal.h"

/**
 * Runtime configuration. Loaded from <Project>/.unreal-agent/config.json
 * (all keys optional) and overridable via environment variables.
 *
 * The endpoint file (<Project>/Saved/ClaudeAgent/endpoint.json) is how the
 * MCP server discovers the port and the per-session token.
 */
class FAgentConfig
{
public:
	int32 Port = 8766;
	FString Token;
	FString ResponseMode = TEXT("compact");
	int32 MaxResults = 20;
	bool bAutoCompile = true;
	bool bAutoSave = false;
	bool bVisualValidation = true;
	bool bAllowRemote = false;
	bool bAutoStart = true;
	/** Package roots that are indexed/searched. Defaults to /Game. */
	TArray<FString> ContentRoots;

	void Load();
	void WriteEndpointFile() const;
	void RemoveEndpointFile() const;

	static FString GetProjectDir();
	static FString GetProjectName();
	static FString GetProjectFile();
	static FString GetAgentDir();      // <Project>/.unreal-agent
	static FString GetSavedDir();      // <Project>/Saved/ClaudeAgent
	static FString GetEndpointFile();
	static FString GetConfigFile();
	static FString GetEngineVersionString();

	TSharedRef<class FJsonObject> ToJson() const;
};
