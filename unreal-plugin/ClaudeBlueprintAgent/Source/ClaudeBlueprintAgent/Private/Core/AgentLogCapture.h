#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"
#include "Dom/JsonObject.h"

/**
 * Captures warnings/errors into a bounded ring buffer. Claude only ever
 * receives counts and filtered slices; the full log stays in Saved/Logs.
 */
class FAgentLogCapture : public FOutputDevice
{
public:
	struct FEntry
	{
		uint64 Seq = 0;
		double Time = 0.0;
		ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
		FName Category;
		FString Message;
	};

	void Register();
	void Unregister();

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	/** level: "error" | "warning" | "all"; Category optional; Contains optional substring. */
	TSharedRef<FJsonObject> Query(const FString& Level, const FString& Category, const FString& Contains, uint64 Since, int32 Limit) const;
	void Clear();
	uint64 GetSequence() const { return Sequence; }

private:
	mutable FCriticalSection Lock;
	TArray<FEntry> Ring;
	int32 Capacity = 5000;
	uint64 Sequence = 0;
	bool bRegistered = false;
};
