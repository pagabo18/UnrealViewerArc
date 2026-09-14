#pragma once

#include "CoreMinimal.h"
#include "AgentTypes.h"

/** Name -> handler map. Commands are registered by the Commands/*.cpp files. */
class FAgentCommandRegistry
{
public:
	void Register(const FString& Name, const FString& Description, bool bMutating, FAgentCommandHandler Handler);
	const FAgentCommandInfo* Find(const FString& Name) const;
	TArray<FString> GetNames() const;
	const TMap<FString, FAgentCommandInfo>& GetAll() const { return Commands; }

private:
	TMap<FString, FAgentCommandInfo> Commands;
};

// Registration entry points implemented in Commands/*.cpp
void RegisterSystemCommands(FAgentCommandRegistry& Registry);
void RegisterAssetCommands(FAgentCommandRegistry& Registry);
void RegisterBlueprintCommands(FAgentCommandRegistry& Registry);
void RegisterGraphCommands(FAgentCommandRegistry& Registry);
void RegisterWidgetCommands(FAgentCommandRegistry& Registry);
void RegisterBatchCommand(FAgentCommandRegistry& Registry);
void RegisterEditorCommands(FAgentCommandRegistry& Registry);
