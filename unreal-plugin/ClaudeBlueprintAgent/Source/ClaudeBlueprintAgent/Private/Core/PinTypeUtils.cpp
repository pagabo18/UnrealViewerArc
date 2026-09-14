#include "PinTypeUtils.h"
#include "Adapters/IUnrealAdapter.h"
#include "EdGraphSchema_K2.h"
#include "UObject/Class.h"

namespace AgentPinTypes
{
	static FString SubObjectName(const TWeakObjectPtr<UObject>& Object)
	{
		if (const UObject* Obj = Object.Get())
		{
			FString Name = Obj->GetName();
			if (const UClass* Class = Cast<UClass>(Obj))
			{
				(void)Class;
				// Keep Blueprint classes recognizable by their _C suffix.
			}
			return Name;
		}
		return TEXT("?");
	}

	static FString BaseToString(const FName& Category, const FName& SubCategory, const TWeakObjectPtr<UObject>& SubObject)
	{
		if (Category == UEdGraphSchema_K2::PC_Exec) { return TEXT("exec"); }
		if (Category == UEdGraphSchema_K2::PC_Boolean) { return TEXT("bool"); }
		if (Category == UEdGraphSchema_K2::PC_Int) { return TEXT("int"); }
		if (Category == UEdGraphSchema_K2::PC_Int64) { return TEXT("int64"); }
		if (Category == UEdGraphSchema_K2::PC_Real)
		{
			return SubCategory == UEdGraphSchema_K2::PC_Float ? TEXT("float32") : TEXT("float");
		}
		if (Category == UEdGraphSchema_K2::PC_String) { return TEXT("string"); }
		if (Category == UEdGraphSchema_K2::PC_Name) { return TEXT("name"); }
		if (Category == UEdGraphSchema_K2::PC_Text) { return TEXT("text"); }
		if (Category == UEdGraphSchema_K2::PC_Wildcard) { return TEXT("wildcard"); }
		if (Category == UEdGraphSchema_K2::PC_Delegate) { return TEXT("delegate"); }
		if (Category == UEdGraphSchema_K2::PC_MCDelegate) { return TEXT("mcdelegate"); }
		if (Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum)
		{
			return SubObject.IsValid() ? SubObjectName(SubObject) : TEXT("byte");
		}
		if (Category == UEdGraphSchema_K2::PC_Struct) { return SubObjectName(SubObject); }
		if (Category == UEdGraphSchema_K2::PC_Object)
		{
			if (SubCategory == UEdGraphSchema_K2::PSC_Self) { return TEXT("self"); }
			return SubObjectName(SubObject);
		}
		if (Category == UEdGraphSchema_K2::PC_Class) { return FString::Printf(TEXT("class<%s>"), *SubObjectName(SubObject)); }
		if (Category == UEdGraphSchema_K2::PC_SoftObject) { return FString::Printf(TEXT("soft<%s>"), *SubObjectName(SubObject)); }
		if (Category == UEdGraphSchema_K2::PC_SoftClass) { return FString::Printf(TEXT("softclass<%s>"), *SubObjectName(SubObject)); }
		if (Category == UEdGraphSchema_K2::PC_Interface) { return FString::Printf(TEXT("iface<%s>"), *SubObjectName(SubObject)); }
		return Category.ToString();
	}

	FString ToString(const FEdGraphPinType& Type)
	{
		const FString Inner = BaseToString(Type.PinCategory, Type.PinSubCategory, Type.PinSubCategoryObject);
		FString Result;
		switch (Type.ContainerType)
		{
		case EPinContainerType::Array:
			Result = FString::Printf(TEXT("[%s]"), *Inner);
			break;
		case EPinContainerType::Set:
			Result = FString::Printf(TEXT("{%s}"), *Inner);
			break;
		case EPinContainerType::Map:
		{
			const FString Value = BaseToString(Type.PinValueType.TerminalCategory, Type.PinValueType.TerminalSubCategory, Type.PinValueType.TerminalSubCategoryObject);
			Result = FString::Printf(TEXT("{%s:%s}"), *Inner, *Value);
			break;
		}
		default:
			Result = Inner;
			break;
		}
		if (Type.bIsReference)
		{
			Result += TEXT("&");
		}
		return Result;
	}

	static bool ExtractAngle(const FString& Spec, const TCHAR* Prefix, FString& OutInner)
	{
		const FString P(Prefix);
		if (Spec.StartsWith(P + TEXT("<"), ESearchCase::IgnoreCase) && Spec.EndsWith(TEXT(">")))
		{
			OutInner = Spec.Mid(P.Len() + 1, Spec.Len() - P.Len() - 2).TrimStartAndEnd();
			return true;
		}
		return false;
	}

	/** Parses a non-container type into category/subcategory/object. */
	static bool ParseBase(const FString& InSpec, const IUnrealAdapter& Adapter, FName& OutCategory, FName& OutSubCategory, UObject*& OutObject, FString& OutError)
	{
		FString Spec = InSpec.TrimStartAndEnd();
		const FString Lower = Spec.ToLower();
		OutCategory = NAME_None;
		OutSubCategory = NAME_None;
		OutObject = nullptr;

		if (Lower == TEXT("bool") || Lower == TEXT("boolean")) { OutCategory = UEdGraphSchema_K2::PC_Boolean; return true; }
		if (Lower == TEXT("int") || Lower == TEXT("int32") || Lower == TEXT("integer")) { OutCategory = UEdGraphSchema_K2::PC_Int; return true; }
		if (Lower == TEXT("int64")) { OutCategory = UEdGraphSchema_K2::PC_Int64; return true; }
		if (Lower == TEXT("float") || Lower == TEXT("double") || Lower == TEXT("real")) { OutCategory = UEdGraphSchema_K2::PC_Real; OutSubCategory = UEdGraphSchema_K2::PC_Double; return true; }
		if (Lower == TEXT("float32")) { OutCategory = UEdGraphSchema_K2::PC_Real; OutSubCategory = UEdGraphSchema_K2::PC_Float; return true; }
		if (Lower == TEXT("string")) { OutCategory = UEdGraphSchema_K2::PC_String; return true; }
		if (Lower == TEXT("name")) { OutCategory = UEdGraphSchema_K2::PC_Name; return true; }
		if (Lower == TEXT("text")) { OutCategory = UEdGraphSchema_K2::PC_Text; return true; }
		if (Lower == TEXT("byte") || Lower == TEXT("uint8")) { OutCategory = UEdGraphSchema_K2::PC_Byte; return true; }
		if (Lower == TEXT("exec")) { OutCategory = UEdGraphSchema_K2::PC_Exec; return true; }
		if (Lower == TEXT("wildcard")) { OutCategory = UEdGraphSchema_K2::PC_Wildcard; return true; }
		if (Lower == TEXT("self")) { OutCategory = UEdGraphSchema_K2::PC_Object; OutSubCategory = UEdGraphSchema_K2::PSC_Self; return true; }

		FString Inner;
		if (ExtractAngle(Spec, TEXT("class"), Inner))
		{
			OutObject = Adapter.ResolveClass(Inner);
			if (!OutObject) { OutError = FString::Printf(TEXT("Class '%s' not found."), *Inner); return false; }
			OutCategory = UEdGraphSchema_K2::PC_Class;
			return true;
		}
		if (ExtractAngle(Spec, TEXT("soft"), Inner) || ExtractAngle(Spec, TEXT("softobject"), Inner))
		{
			OutObject = Adapter.ResolveClass(Inner);
			if (!OutObject) { OutError = FString::Printf(TEXT("Class '%s' not found."), *Inner); return false; }
			OutCategory = UEdGraphSchema_K2::PC_SoftObject;
			return true;
		}
		if (ExtractAngle(Spec, TEXT("softclass"), Inner))
		{
			OutObject = Adapter.ResolveClass(Inner);
			if (!OutObject) { OutError = FString::Printf(TEXT("Class '%s' not found."), *Inner); return false; }
			OutCategory = UEdGraphSchema_K2::PC_SoftClass;
			return true;
		}
		if (ExtractAngle(Spec, TEXT("iface"), Inner) || ExtractAngle(Spec, TEXT("interface"), Inner))
		{
			OutObject = Adapter.ResolveClass(Inner);
			if (!OutObject) { OutError = FString::Printf(TEXT("Interface '%s' not found."), *Inner); return false; }
			OutCategory = UEdGraphSchema_K2::PC_Interface;
			return true;
		}
		if (ExtractAngle(Spec, TEXT("obj"), Inner) || ExtractAngle(Spec, TEXT("object"), Inner))
		{
			OutObject = Adapter.ResolveClass(Inner);
			if (!OutObject) { OutError = FString::Printf(TEXT("Class '%s' not found."), *Inner); return false; }
			OutCategory = UEdGraphSchema_K2::PC_Object;
			return true;
		}
		// prefix forms: object:X struct:X enum:X class:X
		int32 Colon = INDEX_NONE;
		if (Spec.FindChar(TEXT(':'), Colon) && !Spec.StartsWith(TEXT("/")))
		{
			const FString Kind = Spec.Left(Colon).ToLower();
			const FString Rest = Spec.Mid(Colon + 1);
			if (Kind == TEXT("object") || Kind == TEXT("obj"))
			{
				OutObject = Adapter.ResolveClass(Rest);
				OutCategory = UEdGraphSchema_K2::PC_Object;
			}
			else if (Kind == TEXT("class"))
			{
				OutObject = Adapter.ResolveClass(Rest);
				OutCategory = UEdGraphSchema_K2::PC_Class;
			}
			else if (Kind == TEXT("struct"))
			{
				OutObject = Adapter.ResolveStruct(Rest);
				OutCategory = UEdGraphSchema_K2::PC_Struct;
			}
			else if (Kind == TEXT("enum"))
			{
				OutObject = Adapter.ResolveEnum(Rest);
				OutCategory = UEdGraphSchema_K2::PC_Byte;
			}
			else if (Kind == TEXT("interface") || Kind == TEXT("iface"))
			{
				OutObject = Adapter.ResolveClass(Rest);
				OutCategory = UEdGraphSchema_K2::PC_Interface;
			}
			if (!OutObject)
			{
				OutError = FString::Printf(TEXT("Type '%s' not found."), *Spec);
				return false;
			}
			return true;
		}

		// Bare name: struct -> enum -> class
		if (UScriptStruct* Struct = Adapter.ResolveStruct(Spec))
		{
			OutObject = Struct;
			OutCategory = UEdGraphSchema_K2::PC_Struct;
			return true;
		}
		if (UEnum* Enum = Adapter.ResolveEnum(Spec))
		{
			OutObject = Enum;
			OutCategory = UEdGraphSchema_K2::PC_Byte;
			return true;
		}
		if (UClass* Class = Adapter.ResolveClass(Spec))
		{
			OutObject = Class;
			OutCategory = Class->HasAnyClassFlags(CLASS_Interface) ? UEdGraphSchema_K2::PC_Interface : UEdGraphSchema_K2::PC_Object;
			return true;
		}
		OutError = FString::Printf(TEXT("Unknown type '%s'. Examples: float, int, bool, string, Vector, Actor, class<Actor>, [float], {string:int}."), *Spec);
		return false;
	}

	bool ParseTerminal(const FString& Spec, const IUnrealAdapter& Adapter, FEdGraphTerminalType& OutType, FString& OutError)
	{
		FName Category, SubCategory;
		UObject* Object = nullptr;
		if (!ParseBase(Spec, Adapter, Category, SubCategory, Object, OutError))
		{
			return false;
		}
		OutType.TerminalCategory = Category;
		OutType.TerminalSubCategory = SubCategory;
		OutType.TerminalSubCategoryObject = Object;
		return true;
	}

	bool Parse(const FString& InSpec, const IUnrealAdapter& Adapter, FEdGraphPinType& OutType, FString& OutError)
	{
		FString Spec = InSpec.TrimStartAndEnd();
		OutType = FEdGraphPinType();
		if (Spec.EndsWith(TEXT("&")))
		{
			OutType.bIsReference = true;
			Spec = Spec.LeftChop(1).TrimStartAndEnd();
		}
		FString Inner;
		if ((Spec.StartsWith(TEXT("[")) && Spec.EndsWith(TEXT("]"))))
		{
			OutType.ContainerType = EPinContainerType::Array;
			Inner = Spec.Mid(1, Spec.Len() - 2);
		}
		else if (ExtractAngle(Spec, TEXT("array"), Inner))
		{
			OutType.ContainerType = EPinContainerType::Array;
		}
		else if (Spec.StartsWith(TEXT("{")) && Spec.EndsWith(TEXT("}")))
		{
			Inner = Spec.Mid(1, Spec.Len() - 2);
			int32 Colon = INDEX_NONE;
			if (Inner.FindChar(TEXT(':'), Colon) && !Inner.StartsWith(TEXT("/")))
			{
				OutType.ContainerType = EPinContainerType::Map;
				const FString ValueSpec = Inner.Mid(Colon + 1);
				Inner = Inner.Left(Colon);
				if (!ParseTerminal(ValueSpec, Adapter, OutType.PinValueType, OutError))
				{
					return false;
				}
			}
			else
			{
				OutType.ContainerType = EPinContainerType::Set;
			}
		}
		else if (ExtractAngle(Spec, TEXT("set"), Inner))
		{
			OutType.ContainerType = EPinContainerType::Set;
		}
		else if (ExtractAngle(Spec, TEXT("map"), Inner))
		{
			OutType.ContainerType = EPinContainerType::Map;
			FString KeySpec, ValueSpec;
			if (!Inner.Split(TEXT(","), &KeySpec, &ValueSpec))
			{
				OutError = TEXT("map<K,V> needs two types.");
				return false;
			}
			Inner = KeySpec;
			if (!ParseTerminal(ValueSpec, Adapter, OutType.PinValueType, OutError))
			{
				return false;
			}
		}
		else
		{
			Inner = Spec;
		}

		FName Category, SubCategory;
		UObject* Object = nullptr;
		if (!ParseBase(Inner, Adapter, Category, SubCategory, Object, OutError))
		{
			return false;
		}
		OutType.PinCategory = Category;
		OutType.PinSubCategory = SubCategory;
		OutType.PinSubCategoryObject = Object;
		return true;
	}
}
