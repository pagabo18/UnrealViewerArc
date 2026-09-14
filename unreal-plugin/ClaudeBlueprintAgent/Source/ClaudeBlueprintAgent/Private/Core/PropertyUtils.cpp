#include "PropertyUtils.h"
#include "AgentJson.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/TextProperty.h"
#include "UObject/EnumProperty.h"
#include "UObject/Class.h"
#include "Misc/OutputDeviceNull.h"

namespace AgentProps
{
	bool IsEditable(const FProperty* Property)
	{
		if (!Property)
		{
			return false;
		}
		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient | CPF_TextExportTransient))
		{
			return false;
		}
		if (CastField<FMulticastDelegateProperty>(Property) || CastField<FDelegateProperty>(Property))
		{
			return false;
		}
		return Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible);
	}

	static FString ObjectRefString(const UObject* Value, const UObject* Root)
	{
		if (!Value)
		{
			return TEXT("None");
		}
		if (Root && Value->IsIn(Root))
		{
			return FString::Printf(TEXT("(subobject %s)"), *Value->GetClass()->GetName());
		}
		if (const UClass* Class = Cast<UClass>(Value))
		{
			FString Name = Class->GetName();
			return Name;
		}
		return Value->GetPathName();
	}

	FString CompactValue(const FProperty* Property, const void* Container, const void* DefaultContainer, int32 MaxLen)
	{
		if (!Property || !Container)
		{
			return FString();
		}
		FString Out;
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const UObject* Value = ObjectProperty->GetObjectPropertyValue_InContainer(Container);
			Out = ObjectRefString(Value, nullptr);
		}
		else if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			Out = TextProperty->GetPropertyValue_InContainer(Container).ToString();
		}
		else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const void* ValuePtr = EnumProperty->ContainerPtrToValuePtr<void>(Container);
			const int64 Value = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			Out = EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetNameStringByValue(Value) : FString::FromInt(static_cast<int32>(Value));
		}
		else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			const uint8 Value = ByteProperty->GetPropertyValue_InContainer(Container);
			Out = ByteProperty->Enum ? ByteProperty->Enum->GetNameStringByValue(Value) : FString::FromInt(Value);
		}
		else
		{
			// Structs/arrays/scalars: engine export text. Delta export when defaults are known.
			const void* Delta = DefaultContainer ? DefaultContainer : nullptr;
			if (!Property->ExportText_InContainer(0, Out, Container, Delta, nullptr, PPF_None))
			{
				Property->ExportText_InContainer(0, Out, Container, nullptr, nullptr, PPF_None);
			}
			// Strip export-text class decoration on object references inside structs: Class'/Path' -> /Path
			// (kept simple: only collapses the common pattern).
			int32 Quote = INDEX_NONE;
			while (Out.FindChar(TEXT('\''), Quote))
			{
				const int32 Start = Out.Find(TEXT("/Script/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd, Quote);
				if (Start == INDEX_NONE || Start > Quote)
				{
					break;
				}
				const int32 End = Out.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Quote + 1);
				if (End == INDEX_NONE)
				{
					break;
				}
				const FString Path = Out.Mid(Quote + 1, End - Quote - 1);
				Out = Out.Left(Start) + Path + Out.Mid(End + 1);
			}
		}
		if (Out.Len() > MaxLen)
		{
			Out = Out.Left(MaxLen - 3) + TEXT("...");
		}
		return Out;
	}

	bool DiffersFromDefault(const FProperty* Property, const void* Container, const void* DefaultContainer)
	{
		if (!Property || !Container)
		{
			return false;
		}
		if (!DefaultContainer)
		{
			return true;
		}
		return !Property->Identical_InContainer(Container, DefaultContainer, 0, PPF_None);
	}

	bool Resolve(UObject* Root, const FString& Path, FResolved& Out, FString& OutError)
	{
		if (!Root)
		{
			OutError = TEXT("No object.");
			return false;
		}
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			OutError = TEXT("Empty property path.");
			return false;
		}

		UObject* Owner = Root;
		UStruct* CurrentStruct = Root->GetClass();
		void* Container = Root;
		const void* DefaultContainer = Root->GetClass()->GetDefaultObject();
		FProperty* TopProperty = nullptr;

		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FString& Segment = Segments[Index];
			FProperty* Property = CurrentStruct->FindPropertyByName(*Segment);
			if (!Property)
			{
				// Case-insensitive / display-name fallback
				for (TFieldIterator<FProperty> It(CurrentStruct); It; ++It)
				{
					if (It->GetName().Equals(Segment, ESearchCase::IgnoreCase) || It->GetDisplayNameText().ToString().Replace(TEXT(" "), TEXT("")).Equals(Segment, ESearchCase::IgnoreCase))
					{
						Property = *It;
						break;
					}
				}
			}
			if (!Property)
			{
				OutError = FString::Printf(TEXT("Property '%s' not found on %s."), *Segment, *CurrentStruct->GetName());
				return false;
			}
			if (Container == Owner)
			{
				TopProperty = Property;
			}
			const bool bLast = Index == Segments.Num() - 1;
			if (bLast)
			{
				Out.Property = Property;
				Out.Container = Container;
				Out.Owner = Owner;
				Out.TopProperty = TopProperty;
				Out.DefaultContainer = DefaultContainer;
				return true;
			}
			// Descend
			if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				Container = StructProperty->ContainerPtrToValuePtr<void>(Container);
				DefaultContainer = DefaultContainer ? StructProperty->ContainerPtrToValuePtr<void>(DefaultContainer) : nullptr;
				CurrentStruct = StructProperty->Struct;
			}
			else if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
			{
				UObject* Next = ObjectProperty->GetObjectPropertyValue_InContainer(Container);
				if (!Next)
				{
					OutError = FString::Printf(TEXT("'%s' is null."), *Segment);
					return false;
				}
				Owner = Next;
				Container = Next;
				DefaultContainer = Next->GetClass()->GetDefaultObject();
				CurrentStruct = Next->GetClass();
				TopProperty = nullptr;
			}
			else
			{
				OutError = FString::Printf(TEXT("Cannot descend into '%s' (%s)."), *Segment, *Property->GetClass()->GetName());
				return false;
			}
		}
		return false;
	}

	bool GetValue(UObject* Root, const FString& Path, FString& OutValue, FString& OutError)
	{
		FResolved Resolved;
		if (!Resolve(Root, Path, Resolved, OutError))
		{
			return false;
		}
		OutValue = CompactValue(Resolved.Property, Resolved.Container, nullptr, 4000);
		return true;
	}

	bool SetValue(UObject* Root, const FString& Path, const FString& Value, FString& OutError)
	{
		FResolved Resolved;
		if (!Resolve(Root, Path, Resolved, OutError))
		{
			return false;
		}
		FProperty* Property = Resolved.Property;
		if (Property->HasAnyPropertyFlags(CPF_EditConst))
		{
			OutError = FString::Printf(TEXT("'%s' is read-only."), *Path);
			return false;
		}

		Resolved.Owner->Modify();
		Resolved.Owner->PreEditChange(Resolved.TopProperty);

		FString Text = Value;
		// Friendly booleans / enums
		if (CastField<FBoolProperty>(Property))
		{
			const FString Lower = Text.ToLower();
			Text = (Lower == TEXT("true") || Lower == TEXT("1") || Lower == TEXT("yes")) ? TEXT("True") : TEXT("False");
		}
		else if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			if (EnumProperty->GetEnum() && !Text.Contains(TEXT("::")))
			{
				const int64 Idx = EnumProperty->GetEnum()->GetValueByNameString(Text);
				if (Idx == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("'%s' is not a value of %s."), *Text, *EnumProperty->GetEnum()->GetName());
					return false;
				}
			}
		}

		FOutputDeviceNull Null;
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Resolved.Container);
		const TCHAR* Result = Property->ImportText_Direct(*Text, ValuePtr, Resolved.Owner, PPF_None, &Null);
		if (!Result)
		{
			OutError = FString::Printf(TEXT("Could not parse '%s' for %s (%s)."), *Value, *Path, *Property->GetCPPType());
			return false;
		}

		FPropertyChangedEvent Event(Resolved.TopProperty, EPropertyChangeType::ValueSet);
		Resolved.Owner->PostEditChangeProperty(Event);
		return true;
	}

	TSharedRef<FJsonObject> ExportProperties(UObject* Object, bool bOnlyChanged, const TSet<FName>& Exclude, int32 MaxValueLen)
	{
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		if (!Object)
		{
			return Json;
		}
		const UObject* CDO = Object->GetClass()->GetDefaultObject();
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (!IsEditable(Property) || Exclude.Contains(Property->GetFName()))
			{
				continue;
			}
			if (bOnlyChanged && !DiffersFromDefault(Property, Object, CDO))
			{
				continue;
			}
			Json->SetStringField(Property->GetName(), CompactValue(Property, Object, CDO, MaxValueLen));
		}
		return Json;
	}

	void CopyProperties(UObject* Src, UObject* Dst, const TSet<FName>& Exclude, TFunctionRef<bool(const FProperty*)> Filter)
	{
		if (!Src || !Dst)
		{
			return;
		}
		UClass* Class = Src->GetClass();
		if (!Dst->GetClass()->IsChildOf(Class))
		{
			Class = Dst->GetClass();
			if (!Src->GetClass()->IsChildOf(Class))
			{
				return;
			}
		}
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated))
			{
				continue;
			}
			if (Exclude.Contains(Property->GetFName()) || !Filter(Property))
			{
				continue;
			}
			Property->CopyCompleteValue_InContainer(Dst, Src);
		}
	}

	uint32 HashProperties(UObject* Object, const TSet<FName>& Exclude)
	{
		if (!Object)
		{
			return 0;
		}
		const UObject* CDO = Object->GetClass()->GetDefaultObject();
		FString Blob = Object->GetClass()->GetName();
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (!IsEditable(Property) || Exclude.Contains(Property->GetFName()))
			{
				continue;
			}
			if (!DiffersFromDefault(Property, Object, CDO))
			{
				continue;
			}
			Blob += TEXT("|") + Property->GetName() + TEXT("=") + CompactValue(Property, Object, CDO, 4000);
		}
		return FCrc::StrCrc32(*Blob);
	}
}
