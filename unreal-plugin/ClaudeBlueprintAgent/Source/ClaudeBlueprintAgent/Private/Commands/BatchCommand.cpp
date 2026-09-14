// batch: run several commands inside ONE editor transaction. With atomic=true
// (default) a failing operation rolls the whole batch back via undo.
#include "CommandHelpers.h"
#include "Core/AgentCommandRegistry.h"
#include "ClaudeBlueprintAgentModule.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"

namespace
{
	FAgentResult Cmd_Batch(const FJsonObject& Params, FAgentContext& Context)
	{
		const TArray<TSharedPtr<FJsonValue>>* Ops = AgentJson::GetArray(Params, TEXT("ops"));
		if (!Ops || Ops->Num() == 0)
		{
			return FAgentResult::BadRequest(TEXT("'ops' must be a non-empty array of {cmd, params}."));
		}
		const bool bAtomic = AgentJson::GetBool(Params, TEXT("atomic"), true);
		const bool bStopOnError = AgentJson::GetBool(Params, TEXT("stop_on_error"), true);
		const FString Title = AgentJson::GetString(Params, TEXT("title"), FString::Printf(TEXT("Batch (%d ops)"), Ops->Num()));
		const FAgentCommandRegistry& Registry = Context.Module->GetRegistry();

		UTransBuffer* Trans = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
		const int32 QueueBefore = Trans ? Trans->GetQueueLength() : 0;

		TArray<TSharedPtr<FJsonValue>> Results;
		int32 Succeeded = 0;
		int32 Failed = 0;
		bool bAborted = false;
		{
			const FScopedTransaction Transaction(AgentCmd::TransactionTitle(Title));
			FAgentContext Inner = Context;
			Inner.bInBatch = true;
			for (int32 Index = 0; Index < Ops->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Op = (*Ops)[Index]->Type == EJson::Object ? (*Ops)[Index]->AsObject() : nullptr;
				TSharedRef<FJsonObject> Item = AgentJson::Obj();
				Item->SetNumberField(TEXT("index"), Index);
				FAgentResult R;
				if (!Op.IsValid())
				{
					R = FAgentResult::BadRequest(TEXT("op must be an object."));
				}
				else
				{
					const FString Cmd = AgentJson::GetString(*Op, TEXT("cmd"));
					Item->SetStringField(TEXT("cmd"), Cmd);
					const FAgentCommandInfo* Info = Registry.Find(Cmd);
					if (!Info)
					{
						R = FAgentResult::Error(AgentErrors::UnknownCommand, TEXT("Unknown command ") + Cmd);
					}
					else if (Cmd == TEXT("batch"))
					{
						R = FAgentResult::BadRequest(TEXT("Nested batches are not allowed."));
					}
					else
					{
						TSharedPtr<FJsonObject> OpParams = AgentJson::GetObject(*Op, TEXT("params"));
						R = Info->Handler(OpParams.IsValid() ? *OpParams : *AgentJson::Obj(), Inner);
					}
				}
				Item->SetBoolField(TEXT("ok"), R.bOk);
				if (R.bOk)
				{
					++Succeeded;
					if (R.Result.IsValid()) { Item->SetObjectField(TEXT("result"), R.Result); }
				}
				else
				{
					++Failed;
					TSharedRef<FJsonObject> Error = AgentJson::Obj();
					Error->SetStringField(TEXT("code"), R.ErrorCode);
					Error->SetStringField(TEXT("message"), R.ErrorMessage);
					if (R.ErrorDetails.IsValid()) { Error->SetObjectField(TEXT("details"), R.ErrorDetails); }
					Item->SetObjectField(TEXT("error"), Error);
				}
				Results.Add(MakeShared<FJsonValueObject>(Item));
				if (!R.bOk && (bAtomic || bStopOnError))
				{
					bAborted = true;
					break;
				}
			}
		}

		bool bRolledBack = false;
		if (bAborted && bAtomic && Trans && GEditor)
		{
			// Only undo if our transaction actually made it into the queue (empty transactions are dropped).
			if (Trans->GetQueueLength() > QueueBefore)
			{
				bRolledBack = GEditor->UndoTransaction(/*bCanRedo*/ false);
			}
		}

		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetArrayField(TEXT("results"), Results);
		Json->SetNumberField(TEXT("succeeded"), Succeeded);
		Json->SetNumberField(TEXT("failed"), Failed);
		Json->SetNumberField(TEXT("total"), Ops->Num());
		Json->SetBoolField(TEXT("aborted"), bAborted);
		Json->SetBoolField(TEXT("rolled_back"), bRolledBack);
		return FAgentResult::Ok(Json);
	}
}

void RegisterBatchCommand(FAgentCommandRegistry& Registry)
{
	Registry.Register(TEXT("batch"), TEXT("Run ops[] in one transaction; atomic=true rolls back on failure."), true, &Cmd_Batch);
}
