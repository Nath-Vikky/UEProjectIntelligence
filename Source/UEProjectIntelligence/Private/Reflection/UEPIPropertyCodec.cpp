#include "Reflection/UEPIPropertyCodec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace UE::ProjectIntelligence
{
	namespace
	{
		TSharedPtr<FJsonValue> NullValue()
		{
			return MakeShared<FJsonValueNull>();
		}

		TSharedRef<FJsonObject> PropertySchema(const FProperty* Property, int32 Depth, int32 MaxDepth)
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("name"), Property->GetName());
			Schema->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
			Schema->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
			Schema->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible));
			Schema->SetBoolField(TEXT("read_only"), Property->HasAnyPropertyFlags(CPF_EditConst | CPF_BlueprintReadOnly));
			Schema->SetBoolField(TEXT("transient"), Property->HasAnyPropertyFlags(CPF_Transient));
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				Schema->SetStringField(TEXT("struct_path"), StructProperty->Struct ? StructProperty->Struct->GetPathName() : FString());
				if (StructProperty->Struct && Depth < MaxDepth)
				{
					Schema->SetObjectField(TEXT("struct_schema"), FUEPIPropertyCodec::BuildSchema(StructProperty->Struct, MaxDepth - Depth - 1));
				}
			}
			else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				Schema->SetObjectField(TEXT("inner"), PropertySchema(ArrayProperty->Inner, Depth + 1, MaxDepth));
			}
			else if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
			{
				Schema->SetObjectField(TEXT("element"), PropertySchema(SetProperty->ElementProp, Depth + 1, MaxDepth));
			}
			else if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
			{
				Schema->SetObjectField(TEXT("key"), PropertySchema(MapProperty->KeyProp, Depth + 1, MaxDepth));
				Schema->SetObjectField(TEXT("value"), PropertySchema(MapProperty->ValueProp, Depth + 1, MaxDepth));
			}
			else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				Schema->SetStringField(TEXT("allowed_class"), ObjectProperty->PropertyClass ? ObjectProperty->PropertyClass->GetPathName() : FString());
			}
			else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				Schema->SetStringField(TEXT("enum_path"), EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetPathName() : FString());
			}
			return Schema;
		}

		bool JsonNumber(const TSharedPtr<FJsonValue>& Value, double& Out)
		{
			return Value.IsValid() && Value->TryGetNumber(Out);
		}

		bool JsonString(const TSharedPtr<FJsonValue>& Value, FString& Out)
		{
			return Value.IsValid() && Value->TryGetString(Out);
		}

		bool JsonBool(const TSharedPtr<FJsonValue>& Value, bool& Out)
		{
			return Value.IsValid() && Value->TryGetBool(Out);
		}

		bool ParseSegment(const FString& Segment, FString& OutName, int32& OutIndex)
		{
			OutIndex = INDEX_NONE;
			int32 Bracket = INDEX_NONE;
			if (!Segment.FindChar(TEXT('['), Bracket))
			{
				OutName = Segment;
				return !OutName.IsEmpty();
			}
			OutName = Segment.Left(Bracket);
			const FString IndexText = Segment.Mid(Bracket + 1).Replace(TEXT("]"), TEXT(""));
			return !OutName.IsEmpty() && LexTryParseString(OutIndex, *IndexText);
		}
	}

	TSharedRef<FJsonObject> FUEPIPropertyCodec::BuildSchema(const UStruct* Struct, int32 MaxDepth)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema_version"), TEXT("uepi.property-schema.v1"));
		Result->SetStringField(TEXT("type_path"), Struct ? Struct->GetPathName() : FString());
		TArray<TSharedPtr<FJsonValue>> Properties;
		if (Struct && MaxDepth >= 0)
		{
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				const FProperty* Property = *It;
				if (!Property || Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient | CPF_DuplicateTransient))
				{
					continue;
				}
				Properties.Add(MakeShared<FJsonValueObject>(PropertySchema(Property, 0, MaxDepth)));
			}
		}
		Result->SetArrayField(TEXT("properties"), Properties);
		return Result;
	}

	TSharedPtr<FJsonValue> FUEPIPropertyCodec::ReadValue(const FProperty* Property, const void* ValuePtr, int32 Depth, int32 MaxDepth)
	{
		if (!Property || !ValuePtr || Depth > MaxDepth)
		{
			return NullValue();
		}
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr));
		}
		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			return MakeShared<FJsonValueNumber>(NumericProperty->IsInteger() ? static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr)) : NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
		}
		if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			return MakeShared<FJsonValueString>(StringProperty->GetPropertyValue(ValuePtr));
		}
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(ValuePtr).ToString());
		}
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			return MakeShared<FJsonValueString>(TextProperty->GetPropertyValue(ValuePtr).ToString());
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 Raw = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetNameStringByValue(Raw) : FString::Printf(TEXT("%lld"), Raw));
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty && ByteProperty->Enum)
		{
			return MakeShared<FJsonValueString>(ByteProperty->Enum->GetNameStringByValue(ByteProperty->GetPropertyValue(ValuePtr)));
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const UObject* Object = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			return Object ? MakeShared<FJsonValueString>(Object->GetPathName()) : NullValue();
		}
		if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
		{
			return MakeShared<FJsonValueString>(SoftProperty->GetPropertyValue(ValuePtr).ToSoftObjectPath().ToString());
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				const FProperty* Field = *It;
				Object->SetField(Field->GetName(), ReadValue(Field, Field->ContainerPtrToValuePtr<void>(ValuePtr), Depth + 1, MaxDepth));
			}
			return MakeShared<FJsonValueObject>(Object);
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			TArray<TSharedPtr<FJsonValue>> Values;
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				Values.Add(ReadValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), Depth + 1, MaxDepth));
			}
			return MakeShared<FJsonValueArray>(Values);
		}
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			TArray<TSharedPtr<FJsonValue>> Values;
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (Helper.IsValidIndex(Index))
				{
					Values.Add(ReadValue(SetProperty->ElementProp, Helper.GetElementPtr(Index), Depth + 1, MaxDepth));
				}
			}
			return MakeShared<FJsonValueArray>(Values);
		}
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			TArray<TSharedPtr<FJsonValue>> Entries;
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index)) continue;
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetField(TEXT("key"), ReadValue(MapProperty->KeyProp, Helper.GetKeyPtr(Index), Depth + 1, MaxDepth));
				Entry->SetField(TEXT("value"), ReadValue(MapProperty->ValueProp, Helper.GetValuePtr(Index), Depth + 1, MaxDepth));
				Entries.Add(MakeShared<FJsonValueObject>(Entry));
			}
			return MakeShared<FJsonValueArray>(Entries);
		}
		FString Exported;
		Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(Exported);
	}

	bool FUEPIPropertyCodec::WriteValue(const FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, FString& OutError, int32 Depth, int32 MaxDepth)
	{
		if (!Property || !ValuePtr || !Value.IsValid() || Depth > MaxDepth)
		{
			OutError = TEXT("Invalid property, value pointer, JSON value, or property depth.");
			return false;
		}
		if (Property->HasAnyPropertyFlags(CPF_EditConst | CPF_BlueprintReadOnly | CPF_Transient | CPF_Deprecated))
		{
			OutError = FString::Printf(TEXT("Property is not writable: %s"), *Property->GetName());
			return false;
		}
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			bool Parsed = false; if (!JsonBool(Value, Parsed)) { OutError = TEXT("Expected boolean."); return false; } BoolProperty->SetPropertyValue(ValuePtr, Parsed); return true;
		}
		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			double Parsed = 0.0; if (!JsonNumber(Value, Parsed)) { OutError = TEXT("Expected number."); return false; }
			if (NumericProperty->IsInteger()) NumericProperty->SetIntPropertyValue(ValuePtr, static_cast<int64>(Parsed)); else NumericProperty->SetFloatingPointPropertyValue(ValuePtr, Parsed); return true;
		}
		FString Text;
		if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property)) { if (!JsonString(Value, Text)) { OutError = TEXT("Expected string."); return false; } StringProperty->SetPropertyValue(ValuePtr, Text); return true; }
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property)) { if (!JsonString(Value, Text)) { OutError = TEXT("Expected name string."); return false; } NameProperty->SetPropertyValue(ValuePtr, FName(*Text)); return true; }
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property)) { if (!JsonString(Value, Text)) { OutError = TEXT("Expected text string."); return false; } TextProperty->SetPropertyValue(ValuePtr, FText::FromString(Text)); return true; }
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			if (!JsonString(Value, Text)) { OutError = TEXT("Expected enum name."); return false; }
			const int64 Raw = EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetValueByNameString(Text) : INDEX_NONE;
			if (Raw == INDEX_NONE) { OutError = FString::Printf(TEXT("Unknown enum value: %s"), *Text); return false; }
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, Raw); return true;
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (Value->IsNull()) { ObjectProperty->SetObjectPropertyValue(ValuePtr, nullptr); return true; }
			if (!JsonString(Value, Text)) { OutError = TEXT("Expected object path string."); return false; }
			UObject* Object = StaticLoadObject(ObjectProperty->PropertyClass, nullptr, *Text);
			if (!Object) { OutError = FString::Printf(TEXT("Object path is invalid or class-incompatible: %s"), *Text); return false; }
			ObjectProperty->SetObjectPropertyValue(ValuePtr, Object); return true;
		}
		if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
		{
			if (!JsonString(Value, Text)) { OutError = TEXT("Expected soft object path string."); return false; }
			SoftProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(FSoftObjectPath(Text))); return true;
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject(); if (!Object) { OutError = TEXT("Expected struct object."); return false; }
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				FProperty* Field = FindFProperty<FProperty>(StructProperty->Struct, *Pair.Key);
				if (!Field || !WriteValue(Field, Field->ContainerPtrToValuePtr<void>(ValuePtr), Pair.Value, OutError, Depth + 1, MaxDepth)) return false;
			}
			return true;
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr; if (!Value->TryGetArray(Values) || !Values) { OutError = TEXT("Expected array."); return false; }
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr); Helper.EmptyValues(); Helper.AddValues(Values->Num());
			for (int32 Index = 0; Index < Values->Num(); ++Index) if (!WriteValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), (*Values)[Index], OutError, Depth + 1, MaxDepth)) return false;
			return true;
		}
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr; if (!Value->TryGetArray(Values) || !Values) { OutError = TEXT("Expected set array."); return false; }
			FScriptSetHelper Helper(SetProperty, ValuePtr); Helper.EmptyElements();
			for (const TSharedPtr<FJsonValue>& Item : *Values) { const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash(); if (!WriteValue(SetProperty->ElementProp, Helper.GetElementPtr(Index), Item, OutError, Depth + 1, MaxDepth)) return false; } Helper.Rehash(); return true;
		}
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr; if (!Value->TryGetArray(Entries) || !Entries) { OutError = TEXT("Expected map entry array."); return false; }
			FScriptMapHelper Helper(MapProperty, ValuePtr); Helper.EmptyValues();
			for (const TSharedPtr<FJsonValue>& Item : *Entries) { const TSharedPtr<FJsonObject> Entry = Item.IsValid() ? Item->AsObject() : nullptr; if (!Entry) { OutError = TEXT("Map entry must be {key,value}."); return false; } const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash(); if (!WriteValue(MapProperty->KeyProp, Helper.GetKeyPtr(Index), Entry->TryGetField(TEXT("key")), OutError, Depth + 1, MaxDepth) || !WriteValue(MapProperty->ValueProp, Helper.GetValuePtr(Index), Entry->TryGetField(TEXT("value")), OutError, Depth + 1, MaxDepth)) return false; } Helper.Rehash(); return true;
		}
		if (JsonString(Value, Text) && Property->ImportText_Direct(*Text, ValuePtr, nullptr, PPF_None)) return true;
		OutError = FString::Printf(TEXT("Unsupported typed property write: %s"), *Property->GetCPPType());
		return false;
	}

	bool FUEPIPropertyCodec::SetPropertyPath(UObject* Object, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonValue>& OutBefore, TSharedPtr<FJsonValue>& OutAfter, FString& OutError)
	{
		if (!IsValid(Object) || PropertyPath.IsEmpty()) { OutError = TEXT("Object and property path are required."); return false; }
		TArray<FString> Segments; PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		UStruct* CurrentStruct = Object->GetClass();
		void* CurrentContainer = Object;
		for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
		{
			FString Name; int32 ArrayIndex = INDEX_NONE;
			if (!ParseSegment(Segments[SegmentIndex], Name, ArrayIndex)) { OutError = TEXT("Invalid property path segment."); return false; }
			FProperty* Property = FindFProperty<FProperty>(CurrentStruct, *Name);
			if (!Property) { OutError = FString::Printf(TEXT("Property path field was not found: %s"), *Name); return false; }
			void* PropertyPtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (ArrayIndex != INDEX_NONE)
			{
				FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property); if (!ArrayProperty) { OutError = TEXT("Indexed path segment is not an array."); return false; }
				FScriptArrayHelper Helper(ArrayProperty, PropertyPtr); if (!Helper.IsValidIndex(ArrayIndex)) { OutError = TEXT("Array path index is out of range."); return false; }
				Property = ArrayProperty->Inner; PropertyPtr = Helper.GetRawPtr(ArrayIndex);
			}
			if (SegmentIndex == Segments.Num() - 1)
			{
				OutBefore = ReadValue(Property, PropertyPtr);
				if (!WriteValue(Property, PropertyPtr, Value, OutError)) return false;
				OutAfter = ReadValue(Property, PropertyPtr);
				return true;
			}
			if (FStructProperty* StructProperty = CastField<FStructProperty>(Property)) { CurrentStruct = StructProperty->Struct; CurrentContainer = PropertyPtr; continue; }
			if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property)) { UObject* Nested = ObjectProperty->GetObjectPropertyValue(PropertyPtr); if (!Nested) { OutError = TEXT("Nested object property is null."); return false; } CurrentStruct = Nested->GetClass(); CurrentContainer = Nested; continue; }
			OutError = TEXT("Only struct, object, and array segments may appear before the final property."); return false;
		}
		OutError = TEXT("Property path did not resolve."); return false;
	}
}
