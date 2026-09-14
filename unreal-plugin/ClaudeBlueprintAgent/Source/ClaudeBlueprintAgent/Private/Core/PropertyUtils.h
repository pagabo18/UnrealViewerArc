// Reflection helpers: read/write properties by dotted path, export compact
// values, and detect "differs from class default" (the basis of the compact
// widget/component listings and the style fingerprints).
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "UObject/UnrealType.h"

namespace AgentProps
{
	struct FResolved
	{
		FProperty* Property = nullptr;      // innermost property
		void* Container = nullptr;          // container holding Property
		UObject* Owner = nullptr;           // object that owns the top-level property (for edit notifications)
		FProperty* TopProperty = nullptr;   // top-level property on Owner
		const void* DefaultContainer = nullptr; // matching container on the CDO (may be null)
	};

	/** Path forms: "Padding", "Slot.Padding", "WidgetStyle.Normal.TintColor", "Slot.LayoutData.Offsets.Top". */
	bool Resolve(UObject* Root, const FString& Path, FResolved& Out, FString& OutError);

	bool GetValue(UObject* Root, const FString& Path, FString& OutValue, FString& OutError);
	bool SetValue(UObject* Root, const FString& Path, const FString& Value, FString& OutError);

	bool IsEditable(const FProperty* Property);
	bool DiffersFromDefault(const FProperty* Property, const void* Container, const void* DefaultContainer);

	/** Compact textual value; object refs become paths; structs are exported as deltas vs. defaults when available. */
	FString CompactValue(const FProperty* Property, const void* Container, const void* DefaultContainer, int32 MaxLen = 240);

	/**
	 * Exports editable properties of an object.
	 * bOnlyChanged: only properties that differ from the class default object.
	 */
	TSharedRef<FJsonObject> ExportProperties(UObject* Object, bool bOnlyChanged, const TSet<FName>& Exclude, int32 MaxValueLen = 240);

	/** Copies property values Src->Dst for all editable, non-transient properties except those excluded or filtered. */
	void CopyProperties(UObject* Src, UObject* Dst, const TSet<FName>& Exclude, TFunctionRef<bool(const FProperty*)> Filter);

	/** Stable hash of the exported property set (used for style fingerprints). */
	uint32 HashProperties(UObject* Object, const TSet<FName>& Exclude);
}
