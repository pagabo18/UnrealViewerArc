// system.* commands: ping, capabilities, changes, log, undo/redo, config.
#include "CommandHelpers.h"
#include "Core/AgentCommandRegistry.h"
#include "Core/AgentConfig.h"
#include "Core/AgentLogCapture.h"
#include "ClaudeBlueprintAgentModule.h"

#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Misc/ITransaction.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace
{
	// Capability registry (mirrors capabilities.json in the repo). Status: supported | experimental | unsupported.
	struct FCapability { const TCHAR* Id; const TCHAR* Status; };
	static const FCapability GCapabilities[] =
	{
		{ TEXT("Asset.Search"), TEXT("supported") },
		{ TEXT("Asset.References"), TEXT("supported") },
		{ TEXT("Asset.SourceControlStatus"), TEXT("supported") },
		{ TEXT("Asset.CreateBlueprint"), TEXT("supported") },
		{ TEXT("Blueprint.ReadSummary"), TEXT("supported") },
		{ TEXT("Blueprint.ReadStructure"), TEXT("supported") },
		{ TEXT("Blueprint.ReadGraph"), TEXT("supported") },
		{ TEXT("Blueprint.EditGraph"), TEXT("supported") },
		{ TEXT("Blueprint.Variables"), TEXT("supported") },
		{ TEXT("Blueprint.Functions"), TEXT("supported") },
		{ TEXT("Blueprint.Interfaces"), TEXT("supported") },
		{ TEXT("Blueprint.Components"), TEXT("supported") },
		{ TEXT("Blueprint.Compile"), TEXT("supported") },
		{ TEXT("Blueprint.Save"), TEXT("supported") },
		{ TEXT("Blueprint.Validate"), TEXT("supported") },
		{ TEXT("Blueprint.Undo"), TEXT("supported") },
		{ TEXT("Blueprint.Timelines"), TEXT("unsupported") },
		{ TEXT("Blueprint.MacroEditing"), TEXT("experimental") },
		{ TEXT("UMG.ReadTree"), TEXT("supported") },
		{ TEXT("UMG.ReadWidget"), TEXT("supported") },
		{ TEXT("UMG.StyleFingerprint"), TEXT("supported") },
		{ TEXT("UMG.CloneWidget"), TEXT("supported") },
		{ TEXT("UMG.AddWidget"), TEXT("supported") },
		{ TEXT("UMG.EditWidget"), TEXT("supported") },
		{ TEXT("UMG.MoveWidget"), TEXT("supported") },
		{ TEXT("UMG.BindEvent"), TEXT("supported") },
		{ TEXT("UMG.ReadAnimations"), TEXT("supported") },
		{ TEXT("UMG.EditAnimation"), TEXT("unsupported") },
		{ TEXT("UMG.Preview"), TEXT("supported") },
		{ TEXT("UMG.PreviewDiff"), TEXT("supported") },
		{ TEXT("Editor.ViewportScreenshot"), TEXT("experimental") },
		{ TEXT("Editor.Batch"), TEXT("supported") },
	};

	FAgentResult Ping(const FJsonObject& Params, FAgentContext& Context)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetStringField(TEXT("project"), FAgentConfig::GetProjectName());
		Json->SetStringField(TEXT("projectFile"), FAgentConfig::GetProjectFile());
		Json->SetStringField(TEXT("projectDir"), FAgentConfig::GetProjectDir());
		Json->SetStringField(TEXT("engine"), Context.Adapter->GetEngineVersion());
		Json->SetStringField(TEXT("adapter"), Context.Adapter->GetAdapterName());
		Json->SetStringField(TEXT("plugin"), CLAUDE_AGENT_PLUGIN_VERSION);
		Json->SetNumberField(TEXT("seq"), static_cast<double>(Context.ChangeTracker->GetSequence()));
		Json->SetBoolField(TEXT("pie"), GEditor && GEditor->PlayWorld != nullptr);
		ISourceControlModule& SCC = ISourceControlModule::Get();
		Json->SetStringField(TEXT("sourceControl"), SCC.IsEnabled() ? SCC.GetProvider().GetName().ToString() : TEXT("none"));
		const bool bGit = IFileManager::Get().DirectoryExists(*FPaths::Combine(FAgentConfig::GetProjectDir(), TEXT(".git")))
			|| IFileManager::Get().DirectoryExists(*FPaths::Combine(FAgentConfig::GetProjectDir(), TEXT(".."), TEXT(".git")));
		Json->SetBoolField(TEXT("git"), bGit);
		return FAgentResult::Ok(Json);
	}

	FAgentResult Capabilities(const FJsonObject& Params, FAgentContext& Context)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		TSharedRef<FJsonObject> Caps = AgentJson::Obj();
		for (const FCapability& Cap : GCapabilities)
		{
			Caps->SetStringField(Cap.Id, Cap.Status);
		}
		Json->SetObjectField(TEXT("capabilities"), Caps);
		Json->SetStringField(TEXT("plugin"), CLAUDE_AGENT_PLUGIN_VERSION);
		Json->SetStringField(TEXT("engine"), Context.Adapter->GetEngineVersion());
		AgentJson::SetStringArray(*Json, TEXT("commands"), Context.Module->GetRegistry().GetNames());
		return FAgentResult::Ok(Json);
	}

	FAgentResult Changes(const FJsonObject& Params, FAgentContext& Context)
	{
		const uint64 Since = static_cast<uint64>(AgentJson::GetNumber(Params, TEXT("since"), 0));
		const int32 Max = FMath::Clamp(AgentJson::GetInt(Params, TEXT("limit"), 200), 1, 2000);
		return FAgentResult::Ok(Context.ChangeTracker->GetSince(Since, Max));
	}

	FAgentResult Log(const FJsonObject& Params, FAgentContext& Context)
	{
		const FString Level = AgentJson::GetString(Params, TEXT("level"), TEXT("error"));
		const FString Category = AgentJson::GetString(Params, TEXT("category"));
		const FString Contains = AgentJson::GetString(Params, TEXT("contains"));
		const uint64 Since = static_cast<uint64>(AgentJson::GetNumber(Params, TEXT("since"), 0));
		const int32 Limit = FMath::Clamp(AgentJson::GetInt(Params, TEXT("limit"), 20), 1, 500);
		if (AgentJson::GetBool(Params, TEXT("clear"), false))
		{
			Context.LogCapture->Clear();
		}
		return FAgentResult::Ok(Context.LogCapture->Query(Level, Category, Contains, Since, Limit));
	}

	TSharedRef<FJsonObject> TransactionState(int32 Limit)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		if (!GEditor || !GEditor->Trans)
		{
			return Json;
		}
		UTransBuffer* Trans = Cast<UTransBuffer>(GEditor->Trans);
		if (!Trans)
		{
			return Json;
		}
		const int32 QueueLength = Trans->GetQueueLength();
		const int32 UndoCount = Trans->GetUndoCount();
		Json->SetNumberField(TEXT("queue"), QueueLength);
		Json->SetNumberField(TEXT("redoable"), UndoCount);
		TArray<TSharedPtr<FJsonValue>> Items;
		const int32 Top = QueueLength - UndoCount - 1;
		for (int32 Index = Top; Index >= 0 && Items.Num() < Limit; --Index)
		{
			const FTransaction* Transaction = Trans->GetTransaction(Index);
			if (Transaction)
			{
				Items.Add(MakeShared<FJsonValueString>(Transaction->GetContext().Title.ToString()));
			}
		}
		Json->SetArrayField(TEXT("undoStack"), Items);
		return Json;
	}

	FAgentResult Undo(const FJsonObject& Params, FAgentContext& Context)
	{
		const int32 Steps = FMath::Clamp(AgentJson::GetInt(Params, TEXT("steps"), 1), 1, 50);
		int32 Done = 0;
		TArray<FString> Titles;
		for (int32 Index = 0; Index < Steps; ++Index)
		{
			UTransBuffer* Trans = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
			if (Trans)
			{
				const int32 Top = Trans->GetQueueLength() - Trans->GetUndoCount() - 1;
				if (const FTransaction* Transaction = Top >= 0 ? Trans->GetTransaction(Top) : nullptr)
				{
					Titles.Add(Transaction->GetContext().Title.ToString());
				}
			}
			if (!GEditor || !GEditor->UndoTransaction(true))
			{
				break;
			}
			++Done;
		}
		TSharedRef<FJsonObject> Json = TransactionState(5);
		Json->SetNumberField(TEXT("undone"), Done);
		AgentJson::SetStringArray(*Json, TEXT("titles"), Titles);
		return FAgentResult::Ok(Json);
	}

	FAgentResult Redo(const FJsonObject& Params, FAgentContext& Context)
	{
		const int32 Steps = FMath::Clamp(AgentJson::GetInt(Params, TEXT("steps"), 1), 1, 50);
		int32 Done = 0;
		for (int32 Index = 0; Index < Steps; ++Index)
		{
			if (!GEditor || !GEditor->RedoTransaction())
			{
				break;
			}
			++Done;
		}
		TSharedRef<FJsonObject> Json = TransactionState(5);
		Json->SetNumberField(TEXT("redone"), Done);
		return FAgentResult::Ok(Json);
	}

	FAgentResult Transactions(const FJsonObject& Params, FAgentContext& Context)
	{
		return FAgentResult::Ok(TransactionState(FMath::Clamp(AgentJson::GetInt(Params, TEXT("limit"), 10), 1, 100)));
	}

	FAgentResult Config(const FJsonObject& Params, FAgentContext& Context)
	{
		TSharedRef<FJsonObject> Json = Context.Config->ToJson();
		Json->SetStringField(TEXT("configFile"), FAgentConfig::GetConfigFile());
		Json->SetStringField(TEXT("endpointFile"), FAgentConfig::GetEndpointFile());
		return FAgentResult::Ok(Json);
	}

	FAgentResult CollectGarbage(const FJsonObject& Params, FAgentContext& Context)
	{
		::CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		return FAgentResult::Ok();
	}
}

void RegisterSystemCommands(FAgentCommandRegistry& Registry)
{
	Registry.Register(TEXT("system.ping"), TEXT("Editor/project/plugin status."), false, &Ping);
	Registry.Register(TEXT("system.capabilities"), TEXT("Capability registry + command list."), false, &Capabilities);
	Registry.Register(TEXT("system.changes"), TEXT("Change events since a sequence number."), false, &Changes);
	Registry.Register(TEXT("system.log"), TEXT("Captured errors/warnings (counts + slice)."), false, &Log);
	Registry.Register(TEXT("system.undo"), TEXT("Undo N editor transactions."), true, &Undo);
	Registry.Register(TEXT("system.redo"), TEXT("Redo N editor transactions."), true, &Redo);
	Registry.Register(TEXT("system.transactions"), TEXT("Undo stack titles."), false, &Transactions);
	Registry.Register(TEXT("system.config"), TEXT("Effective plugin configuration."), false, &Config);
	Registry.Register(TEXT("system.gc"), TEXT("Force garbage collection."), false, &CollectGarbage);
}
