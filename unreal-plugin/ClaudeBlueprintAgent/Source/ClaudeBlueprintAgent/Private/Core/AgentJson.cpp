#include "AgentJson.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"

namespace AgentJson
{
	TSharedRef<FJsonObject> Obj()
	{
		return MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> Parse(const FString& Text)
	{
		TSharedPtr<FJsonObject> Result;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Result) || !Result.IsValid())
		{
			return nullptr;
		}
		return Result;
	}

	FString Serialize(const TSharedRef<FJsonObject>& Object, bool bPretty)
	{
		FString Out;
		if (bPretty)
		{
			TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Object, Writer);
		}
		else
		{
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Object, Writer);
		}
		return Out;
	}

	FString SerializeValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("null");
		}
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Value.ToSharedRef(), FString(), Writer);
		return Out;
	}

	FString GetString(const FJsonObject& Object, const FString& Key, const FString& Default)
	{
		FString Value;
		if (Object.TryGetStringField(Key, Value))
		{
			return Value;
		}
		// Be lenient: numbers/bools passed where strings are expected.
		const TSharedPtr<FJsonValue> Raw = Object.TryGetField(Key);
		if (Raw.IsValid() && !Raw->IsNull())
		{
			if (Raw->Type == EJson::Number)
			{
				return FString::SanitizeFloat(Raw->AsNumber());
			}
			if (Raw->Type == EJson::Boolean)
			{
				return Raw->AsBool() ? TEXT("true") : TEXT("false");
			}
		}
		return Default;
	}

	int32 GetInt(const FJsonObject& Object, const FString& Key, int32 Default)
	{
		double Value;
		if (Object.TryGetNumberField(Key, Value))
		{
			return static_cast<int32>(Value);
		}
		FString Str;
		if (Object.TryGetStringField(Key, Str) && Str.IsNumeric())
		{
			return FCString::Atoi(*Str);
		}
		return Default;
	}

	double GetNumber(const FJsonObject& Object, const FString& Key, double Default)
	{
		double Value;
		if (Object.TryGetNumberField(Key, Value))
		{
			return Value;
		}
		return Default;
	}

	bool GetBool(const FJsonObject& Object, const FString& Key, bool Default)
	{
		bool Value;
		if (Object.TryGetBoolField(Key, Value))
		{
			return Value;
		}
		FString Str;
		if (Object.TryGetStringField(Key, Str))
		{
			return Str.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Str == TEXT("1");
		}
		return Default;
	}

	bool Has(const FJsonObject& Object, const FString& Key)
	{
		const TSharedPtr<FJsonValue> Raw = Object.TryGetField(Key);
		return Raw.IsValid() && !Raw->IsNull();
	}

	TArray<FString> GetStringArray(const FJsonObject& Object, const FString& Key)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Object.TryGetArrayField(Key, Array) && Array)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				if (Value.IsValid() && Value->Type == EJson::String)
				{
					Result.Add(Value->AsString());
				}
			}
		}
		return Result;
	}

	TArray<FString> GetStringOrArray(const FJsonObject& Object, const FString& Key)
	{
		FString Single;
		if (Object.TryGetStringField(Key, Single))
		{
			TArray<FString> Result;
			if (!Single.IsEmpty())
			{
				Result.Add(Single);
			}
			return Result;
		}
		return GetStringArray(Object, Key);
	}

	TSharedPtr<FJsonObject> GetObject(const FJsonObject& Object, const FString& Key)
	{
		const TSharedPtr<FJsonObject>* Result = nullptr;
		if (Object.TryGetObjectField(Key, Result) && Result)
		{
			return *Result;
		}
		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* GetArray(const FJsonObject& Object, const FString& Key)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Object.TryGetArrayField(Key, Array))
		{
			return Array;
		}
		return nullptr;
	}

	void SetStringArray(FJsonObject& Object, const FString& Key, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& V : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(V));
		}
		Object.SetArrayField(Key, Array);
	}

	void SetObjectArray(FJsonObject& Object, const FString& Key, const TArray<TSharedPtr<FJsonObject>>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const TSharedPtr<FJsonObject>& V : Values)
		{
			Array.Add(MakeShared<FJsonValueObject>(V));
		}
		Object.SetArrayField(Key, Array);
	}

	TSharedRef<FJsonValueObject> Val(const TSharedRef<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	TSharedRef<FJsonValueObject> Val(const TSharedPtr<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	TSharedRef<FJsonValueString> Val(const FString& String)
	{
		return MakeShared<FJsonValueString>(String);
	}

	TSharedRef<FJsonValueNumber> Val(double Number)
	{
		return MakeShared<FJsonValueNumber>(Number);
	}
}
