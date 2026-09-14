#include "AgentLogCapture.h"
#include "AgentJson.h"
#include "HAL/PlatformTime.h"
#include "Misc/OutputDeviceRedirector.h"
#include "HAL/PlatformOutputDevices.h"
#include "Algo/Reverse.h"
#include "Misc/CoreMisc.h"

void FAgentLogCapture::Register()
{
	if (!bRegistered && GLog)
	{
		GLog->AddOutputDevice(this);
		bRegistered = true;
	}
}

void FAgentLogCapture::Unregister()
{
	if (bRegistered && GLog)
	{
		GLog->RemoveOutputDevice(this);
		bRegistered = false;
	}
}

void FAgentLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	if (Verbosity > ELogVerbosity::Warning)
	{
		// Only errors/warnings (and fatal) are worth keeping around.
		return;
	}
	FScopeLock ScopeLock(&Lock);
	FEntry Entry;
	Entry.Seq = ++Sequence;
	Entry.Time = FPlatformTime::Seconds();
	Entry.Verbosity = Verbosity;
	Entry.Category = Category;
	Entry.Message = V;
	Ring.Add(MoveTemp(Entry));
	if (Ring.Num() > Capacity)
	{
		Ring.RemoveAt(0, Ring.Num() - Capacity, EAllowShrinking::No);
	}
}

void FAgentLogCapture::Clear()
{
	FScopeLock ScopeLock(&Lock);
	Ring.Reset();
}

TSharedRef<FJsonObject> FAgentLogCapture::Query(const FString& Level, const FString& Category, const FString& Contains, uint64 Since, int32 Limit) const
{
	FScopeLock ScopeLock(&Lock);
	const bool bErrorsOnly = Level.Equals(TEXT("error"), ESearchCase::IgnoreCase);
	const bool bWarningsAndErrors = bErrorsOnly || Level.Equals(TEXT("warning"), ESearchCase::IgnoreCase);

	int32 Errors = 0;
	int32 Warnings = 0;
	TArray<TSharedPtr<FJsonValue>> Items;
	int32 Matched = 0;
	for (int32 Index = Ring.Num() - 1; Index >= 0; --Index)
	{
		const FEntry& Entry = Ring[Index];
		if (Entry.Seq <= Since)
		{
			break;
		}
		const bool bIsError = Entry.Verbosity <= ELogVerbosity::Error;
		const bool bIsWarning = Entry.Verbosity == ELogVerbosity::Warning;
		if (bErrorsOnly && !bIsError)
		{
			continue;
		}
		if (bWarningsAndErrors && !(bIsError || bIsWarning))
		{
			continue;
		}
		if (!Category.IsEmpty() && !Entry.Category.ToString().Equals(Category, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!Contains.IsEmpty() && !Entry.Message.Contains(Contains))
		{
			continue;
		}
		++Matched;
		if (bIsError) { ++Errors; } else if (bIsWarning) { ++Warnings; }
		if (Items.Num() < Limit)
		{
			TSharedRef<FJsonObject> Item = AgentJson::Obj();
			Item->SetNumberField(TEXT("seq"), static_cast<double>(Entry.Seq));
			Item->SetStringField(TEXT("level"), bIsError ? TEXT("error") : (bIsWarning ? TEXT("warning") : TEXT("info")));
			Item->SetStringField(TEXT("category"), Entry.Category.ToString());
			Item->SetStringField(TEXT("message"), Entry.Message.Left(400));
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
	}
	Algo::Reverse(Items);
	TSharedRef<FJsonObject> Json = AgentJson::Obj();
	Json->SetNumberField(TEXT("errors"), Errors);
	Json->SetNumberField(TEXT("warnings"), Warnings);
	Json->SetNumberField(TEXT("matched"), Matched);
	Json->SetArrayField(TEXT("entries"), Items);
	Json->SetNumberField(TEXT("seq"), static_cast<double>(Sequence));
	Json->SetStringField(TEXT("logFile"), FPlatformOutputDevices::GetAbsoluteLogFilename());
	return Json;
}
