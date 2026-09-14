#include "AgentCommandRegistry.h"

void FAgentCommandRegistry::Register(const FString& Name, const FString& Description, bool bMutating, FAgentCommandHandler Handler)
{
	FAgentCommandInfo Info;
	Info.Name = Name;
	Info.Description = Description;
	Info.bMutating = bMutating;
	Info.Handler = MoveTemp(Handler);
	Commands.Add(Name, MoveTemp(Info));
}

const FAgentCommandInfo* FAgentCommandRegistry::Find(const FString& Name) const
{
	return Commands.Find(Name);
}

TArray<FString> FAgentCommandRegistry::GetNames() const
{
	TArray<FString> Names;
	Commands.GetKeys(Names);
	Names.Sort();
	return Names;
}
