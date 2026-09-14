// Small helpers around the engine JSON API so command code stays readable.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace AgentJson
{
	TSharedRef<FJsonObject> Obj();
	TSharedPtr<FJsonObject> Parse(const FString& Text);
	FString Serialize(const TSharedRef<FJsonObject>& Object, bool bPretty = false);
	FString SerializeValue(const TSharedPtr<FJsonValue>& Value);

	FString GetString(const FJsonObject& Object, const FString& Key, const FString& Default = FString());
	int32 GetInt(const FJsonObject& Object, const FString& Key, int32 Default = 0);
	double GetNumber(const FJsonObject& Object, const FString& Key, double Default = 0.0);
	bool GetBool(const FJsonObject& Object, const FString& Key, bool Default = false);
	bool Has(const FJsonObject& Object, const FString& Key);
	TArray<FString> GetStringArray(const FJsonObject& Object, const FString& Key);
	TSharedPtr<FJsonObject> GetObject(const FJsonObject& Object, const FString& Key);
	const TArray<TSharedPtr<FJsonValue>>* GetArray(const FJsonObject& Object, const FString& Key);

	/** Accepts either a string or an array of strings under Key. */
	TArray<FString> GetStringOrArray(const FJsonObject& Object, const FString& Key);

	void SetStringArray(FJsonObject& Object, const FString& Key, const TArray<FString>& Values);
	void SetObjectArray(FJsonObject& Object, const FString& Key, const TArray<TSharedPtr<FJsonObject>>& Values);
	TSharedRef<FJsonValueObject> Val(const TSharedRef<FJsonObject>& Object);
	TSharedRef<FJsonValueObject> Val(const TSharedPtr<FJsonObject>& Object);
	TSharedRef<FJsonValueString> Val(const FString& String);
	TSharedRef<FJsonValueNumber> Val(double Number);
}
