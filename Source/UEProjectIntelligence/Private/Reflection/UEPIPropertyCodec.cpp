#include "Reflection/UEPIPropertyCodec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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

		TSharedPtr<FJsonValue> TypedValue(const FString& Type, const TCHAR* Field, const TSharedPtr<FJsonValue>& Value)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type"), Type);
			Object->SetField(Field, Value.IsValid() ? Value : NullValue());
			return MakeShared<FJsonValueObject>(Object);
		}

		TSharedPtr<FJsonValue> NormalizeTypedInput(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				return Value;
			}
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			FString Type;
			if (!Object.IsValid() || !Object->TryGetStringField(TEXT("type"), Type))
			{
				return Value;
			}
			if (Type == TEXT("struct"))
			{
				if (!Object->HasTypedField<EJson::Object>(TEXT("fields")))
				{
					return nullptr;
				}
				const TSharedPtr<FJsonObject> Fields = Object->GetObjectField(TEXT("fields"));
				return Fields.IsValid() ? MakeShared<FJsonValueObject>(Fields.ToSharedRef()) : Value;
			}
			if (Type == TEXT("array") || Type == TEXT("set"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				return Object->TryGetArrayField(TEXT("items"), Items) && Items ? MakeShared<FJsonValueArray>(*Items) : Value;
			}
			if (Type == TEXT("map"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
				return Object->TryGetArrayField(TEXT("entries"), Entries) && Entries ? MakeShared<FJsonValueArray>(*Entries) : Value;
			}
			if (Type == TEXT("object") || Type == TEXT("soft_object") || Type == TEXT("class") || Type == TEXT("soft_class"))
			{
				return Object->TryGetField(TEXT("path"));
			}
			return Object->TryGetField(TEXT("value"));
		}

		FString UEPIPropertyCodecKind(const FProperty* Property)
		{
			if (CastField<FBoolProperty>(Property)) return TEXT("bool");
			if (CastField<FEnumProperty>(Property) || (CastField<FByteProperty>(Property) && CastField<FByteProperty>(Property)->Enum)) return TEXT("enum");
			if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property)) return Numeric->IsInteger() ? TEXT("integer") : TEXT("number");
			if (CastField<FNameProperty>(Property)) return TEXT("name");
			if (CastField<FTextProperty>(Property)) return TEXT("text");
			if (CastField<FStrProperty>(Property)) return TEXT("string");
			if (CastField<FSoftClassProperty>(Property)) return TEXT("soft_class");
			if (CastField<FClassProperty>(Property)) return TEXT("class");
			if (CastField<FSoftObjectProperty>(Property)) return TEXT("soft_object");
			if (CastField<FObjectPropertyBase>(Property)) return TEXT("object");
			if (CastField<FStructProperty>(Property)) return TEXT("struct");
			if (CastField<FArrayProperty>(Property)) return TEXT("array");
			if (CastField<FSetProperty>(Property)) return TEXT("set");
			if (CastField<FMapProperty>(Property)) return TEXT("map");
			return TEXT("unsupported");
		}

		TSharedRef<FJsonObject> PropertySchema(const FProperty* Property, int32 Depth, int32 MaxDepth)
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("name"), Property->GetName());
			Schema->SetStringField(TEXT("property_path"), Property->GetName());
			Schema->SetStringField(TEXT("display_name"), Property->GetDisplayNameText().ToString());
			Schema->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
			Schema->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
			Schema->SetStringField(TEXT("kind"), UEPIPropertyCodecKind(Property));
			const bool bFlagBlocked = Property->HasAnyPropertyFlags(CPF_EditConst | CPF_BlueprintReadOnly | CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_Deprecated);
			const bool bEditable = UEPIPropertyCodecKind(Property) != TEXT("unsupported") && !bFlagBlocked && Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible);
			Schema->SetBoolField(TEXT("editable"), bEditable);
			Schema->SetBoolField(TEXT("read_only"), !bEditable);
			Schema->SetStringField(TEXT("read_only_reason"), bEditable ? FString() : (bFlagBlocked ? TEXT("property_flags") : TEXT("unsupported_or_not_editable")));
			Schema->SetBoolField(TEXT("transient"), Property->HasAnyPropertyFlags(CPF_Transient));
			Schema->SetBoolField(TEXT("editor_only"), Property->HasAnyPropertyFlags(CPF_EditorOnly));
			Schema->SetArrayField(TEXT("flags"), {});
			Schema->SetStringField(TEXT("edit_condition"), Property->GetMetaData(TEXT("EditCondition")));
			Schema->SetStringField(TEXT("clamp_min"), Property->GetMetaData(TEXT("ClampMin")));
			Schema->SetStringField(TEXT("clamp_max"), Property->GetMetaData(TEXT("ClampMax")));
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
			else if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
			{
				Schema->SetStringField(TEXT("allowed_class"), SoftClassProperty->MetaClass ? SoftClassProperty->MetaClass->GetPathName() : FString());
			}
			else if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
			{
				Schema->SetStringField(TEXT("allowed_class"), ClassProperty->MetaClass ? ClassProperty->MetaClass->GetPathName() : FString());
			}
			else if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
			{
				Schema->SetStringField(TEXT("allowed_class"), SoftProperty->PropertyClass ? SoftProperty->PropertyClass->GetPathName() : FString());
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

		int32 JsonObjectInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 DefaultValue)
		{
			int32 Result = DefaultValue;
			return Object.IsValid() && Object->TryGetNumberField(Field, Result) ? Result : DefaultValue;
		}

		bool ParseSegment(const FString& Segment, FString& OutName, int32& OutIndex, FString& OutMapKey, bool& bOutHasMapKey)
		{
			OutIndex = INDEX_NONE;
			OutMapKey.Reset();
			bOutHasMapKey = false;
			int32 Brace = INDEX_NONE;
			if (Segment.FindChar(TEXT('{'), Brace))
			{
				OutName = Segment.Left(Brace);
				const FString JsonKey = Segment.Mid(Brace + 1, Segment.Len() - Brace - 2);
				TSharedPtr<FJsonValue> Parsed;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonKey);
				if (OutName.IsEmpty() || !FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid() || !Parsed->TryGetString(OutMapKey))
				{
					return false;
				}
				bOutHasMapKey = true;
				return true;
			}
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

		FString MapKeyString(const FProperty* Property, const void* ValuePtr)
		{
			if (const FStrProperty* String = CastField<FStrProperty>(Property)) return String->GetPropertyValue(ValuePtr);
			if (const FNameProperty* Name = CastField<FNameProperty>(Property)) return Name->GetPropertyValue(ValuePtr).ToString();
			if (const FTextProperty* Text = CastField<FTextProperty>(Property)) return Text->GetPropertyValue(ValuePtr).ToString();
			if (const FEnumProperty* Enum = CastField<FEnumProperty>(Property))
			{
				const int64 Raw = Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
				return Enum->GetEnum() ? Enum->GetEnum()->GetNameStringByValue(Raw) : ::LexToString(Raw);
			}
			FString Exported;
			Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
			Exported.RemoveFromStart(TEXT("\"")); Exported.RemoveFromEnd(TEXT("\""));
			return Exported;
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
			const UClass* Class = Cast<UClass>(Struct);
			const UObject* DefaultObject = Class ? Class->GetDefaultObject(false) : nullptr;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				const FProperty* Property = *It;
				if (!Property)
				{
					continue;
				}
				TSharedRef<FJsonObject> Schema = PropertySchema(Property, 0, MaxDepth);
				if (DefaultObject)
				{
					Schema->SetField(TEXT("default_value"), ReadValue(Property, Property->ContainerPtrToValuePtr<void>(DefaultObject), 0, MaxDepth));
				}
				Properties.Add(MakeShared<FJsonValueObject>(Schema));
			}
		}
		Result->SetArrayField(TEXT("properties"), Properties);
		return Result;
	}

	TSharedRef<FJsonObject> FUEPIPropertyCodec::BuildObjectSchema(const UObject* Object, int32 MaxDepth)
	{
		TSharedRef<FJsonObject> Result = BuildSchema(Object ? Object->GetClass() : nullptr, MaxDepth);
		if (!Object)
		{
			return Result;
		}
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (Result->TryGetArrayField(TEXT("properties"), Properties) && Properties)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Properties)
			{
				const TSharedPtr<FJsonObject> Schema = Value.IsValid() ? Value->AsObject() : nullptr;
				const FString Name = Schema.IsValid() ? Schema->GetStringField(TEXT("name")) : FString();
				if (const FProperty* Property = Name.IsEmpty() ? nullptr : FindFProperty<FProperty>(Object->GetClass(), *Name))
				{
					Schema->SetField(TEXT("current_value"), ReadValue(Property, Property->ContainerPtrToValuePtr<void>(Object), 0, MaxDepth));
					Schema->SetStringField(TEXT("source"), TEXT("instance"));
				}
			}
		}
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
			return TypedValue(TEXT("bool"), TEXT("value"), MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr)));
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty && ByteProperty->Enum)
		{
			return TypedValue(TEXT("enum"), TEXT("value"), MakeShared<FJsonValueString>(ByteProperty->Enum->GetNameStringByValue(ByteProperty->GetPropertyValue(ValuePtr))));
		}
		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			return TypedValue(NumericProperty->IsInteger() ? TEXT("int64") : TEXT("double"), TEXT("value"), MakeShared<FJsonValueNumber>(NumericProperty->IsInteger() ? static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr)) : NumericProperty->GetFloatingPointPropertyValue(ValuePtr)));
		}
		if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			return TypedValue(TEXT("string"), TEXT("value"), MakeShared<FJsonValueString>(StringProperty->GetPropertyValue(ValuePtr)));
		}
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return TypedValue(TEXT("name"), TEXT("value"), MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(ValuePtr).ToString()));
		}
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			return TypedValue(TEXT("text"), TEXT("value"), MakeShared<FJsonValueString>(TextProperty->GetPropertyValue(ValuePtr).ToString()));
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 Raw = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			return TypedValue(TEXT("enum"), TEXT("value"), MakeShared<FJsonValueString>(EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetNameStringByValue(Raw) : FString::Printf(TEXT("%lld"), Raw)));
		}
		if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
		{
			return TypedValue(TEXT("soft_class"), TEXT("path"), MakeShared<FJsonValueString>(SoftClassProperty->GetPropertyValue(ValuePtr).ToSoftObjectPath().ToString()));
		}
		if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
		{
			const UObject* Object = ClassProperty->GetObjectPropertyValue(ValuePtr);
			return TypedValue(TEXT("class"), TEXT("path"), Object ? MakeShared<FJsonValueString>(Object->GetPathName()) : NullValue());
		}
		if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
		{
			return TypedValue(TEXT("soft_object"), TEXT("path"), MakeShared<FJsonValueString>(SoftProperty->GetPropertyValue(ValuePtr).ToSoftObjectPath().ToString()));
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const UObject* Object = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			return TypedValue(CastField<FClassProperty>(Property) ? TEXT("class") : TEXT("object"), TEXT("path"), Object ? MakeShared<FJsonValueString>(Object->GetPathName()) : NullValue());
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				const FProperty* Field = *It;
				Object->SetField(Field->GetName(), ReadValue(Field, Field->ContainerPtrToValuePtr<void>(ValuePtr), Depth + 1, MaxDepth));
			}
			return TypedValue(TEXT("struct"), TEXT("fields"), MakeShared<FJsonValueObject>(Object));
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			TArray<TSharedPtr<FJsonValue>> Values;
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				Values.Add(ReadValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), Depth + 1, MaxDepth));
			}
			return TypedValue(TEXT("array"), TEXT("items"), MakeShared<FJsonValueArray>(Values));
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
			return TypedValue(TEXT("set"), TEXT("items"), MakeShared<FJsonValueArray>(Values));
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
			return TypedValue(TEXT("map"), TEXT("entries"), MakeShared<FJsonValueArray>(Entries));
		}
		FString Exported;
		Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
		return TypedValue(TEXT("unsupported"), TEXT("value"), MakeShared<FJsonValueString>(Exported));
	}

	bool FUEPIPropertyCodec::WriteValue(const FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, FString& OutError, int32 Depth, int32 MaxDepth)
	{
		if (!Property || !ValuePtr || !Value.IsValid() || Depth > MaxDepth)
		{
			OutError = TEXT("Invalid property, value pointer, JSON value, or property depth.");
			return false;
		}
		if (Property->HasAnyPropertyFlags(CPF_EditConst | CPF_BlueprintReadOnly | CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_Deprecated))
		{
			OutError = FString::Printf(TEXT("Property is not writable: %s"), *Property->GetName());
			return false;
		}
		const TSharedPtr<FJsonValue> Input = NormalizeTypedInput(Value);
		if (!Input.IsValid())
		{
			OutError = TEXT("Typed property value is missing its value/path/items/entries field.");
			return false;
		}
		FString Text;
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			bool Parsed = false; if (!JsonBool(Input, Parsed)) { OutError = TEXT("Expected boolean."); return false; } BoolProperty->SetPropertyValue(ValuePtr, Parsed); return true;
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty && ByteProperty->Enum)
		{
			if (!JsonString(Input, Text)) { OutError = TEXT("Expected enum name."); return false; }
			const int64 Raw = ByteProperty->Enum->GetValueByNameString(Text);
			if (Raw == INDEX_NONE) { OutError = FString::Printf(TEXT("Unknown enum value: %s"), *Text); return false; }
			ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(Raw)); return true;
		}
		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			double Parsed = 0.0; if (!JsonNumber(Input, Parsed)) { OutError = TEXT("Expected number."); return false; }
			if (NumericProperty->IsInteger()) NumericProperty->SetIntPropertyValue(ValuePtr, static_cast<int64>(Parsed)); else NumericProperty->SetFloatingPointPropertyValue(ValuePtr, Parsed); return true;
		}
		if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property)) { if (!JsonString(Input, Text)) { OutError = TEXT("Expected string."); return false; } StringProperty->SetPropertyValue(ValuePtr, Text); return true; }
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property)) { if (!JsonString(Input, Text)) { OutError = TEXT("Expected name string."); return false; } NameProperty->SetPropertyValue(ValuePtr, FName(*Text)); return true; }
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property)) { if (!JsonString(Input, Text)) { OutError = TEXT("Expected text string."); return false; } TextProperty->SetPropertyValue(ValuePtr, FText::FromString(Text)); return true; }
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			if (!JsonString(Input, Text)) { OutError = TEXT("Expected enum name."); return false; }
			const int64 Raw = EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetValueByNameString(Text) : INDEX_NONE;
			if (Raw == INDEX_NONE) { OutError = FString::Printf(TEXT("Unknown enum value: %s"), *Text); return false; }
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, Raw); return true;
		}
		if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
		{
			if (!JsonString(Input, Text)) { OutError = TEXT("Expected soft class path string."); return false; }
			const FSoftObjectPath Path(Text); UClass* Class = Cast<UClass>(Path.TryLoad());
			if (!Path.IsValid() || (Class && SoftClassProperty->MetaClass && !Class->IsChildOf(SoftClassProperty->MetaClass))) { OutError = FString::Printf(TEXT("Soft class path is invalid or class-incompatible: %s"), *Text); return false; }
			SoftClassProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(Path)); return true;
		}
		if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
		{
			if (Input->IsNull()) { ClassProperty->SetObjectPropertyValue(ValuePtr, nullptr); return true; }
			if (!JsonString(Input, Text)) { OutError = TEXT("Expected class path string."); return false; }
			UClass* Class = LoadObject<UClass>(nullptr, *Text);
			if (!Class || (ClassProperty->MetaClass && !Class->IsChildOf(ClassProperty->MetaClass))) { OutError = FString::Printf(TEXT("Class path is invalid or class-incompatible: %s"), *Text); return false; }
			ClassProperty->SetObjectPropertyValue(ValuePtr, Class); return true;
		}
		if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
		{
			if (!JsonString(Input, Text)) { OutError = TEXT("Expected soft object path string."); return false; }
			const FSoftObjectPath Path(Text);
			if (!Path.IsValid()) { OutError = FString::Printf(TEXT("Soft object path is invalid: %s"), *Text); return false; }
			if (UObject* Resolved = Path.TryLoad(); Resolved && !Resolved->IsA(SoftProperty->PropertyClass)) { OutError = FString::Printf(TEXT("Soft object path is class-incompatible: %s"), *Text); return false; }
			SoftProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(Path)); return true;
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (Input->IsNull()) { ObjectProperty->SetObjectPropertyValue(ValuePtr, nullptr); return true; }
			if (!JsonString(Input, Text)) { OutError = TEXT("Expected object path string."); return false; }
			UObject* Object = StaticLoadObject(ObjectProperty->PropertyClass, nullptr, *Text);
			if (!Object) { OutError = FString::Printf(TEXT("Object path is invalid or class-incompatible: %s"), *Text); return false; }
			ObjectProperty->SetObjectPropertyValue(ValuePtr, Object); return true;
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const TSharedPtr<FJsonObject> Object = Input->AsObject(); if (!Object) { OutError = TEXT("Expected struct object."); return false; }
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				FProperty* Field = FindFProperty<FProperty>(StructProperty->Struct, *Pair.Key);
				if (!Field || !WriteValue(Field, Field->ContainerPtrToValuePtr<void>(ValuePtr), Pair.Value, OutError, Depth + 1, MaxDepth)) return false;
			}
			return true;
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr; if (!Input->TryGetArray(Values) || !Values) { OutError = TEXT("Expected array."); return false; }
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr); Helper.EmptyValues(); Helper.AddValues(Values->Num());
			for (int32 Index = 0; Index < Values->Num(); ++Index) if (!WriteValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), (*Values)[Index], OutError, Depth + 1, MaxDepth)) return false;
			return true;
		}
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr; if (!Input->TryGetArray(Values) || !Values) { OutError = TEXT("Expected set array."); return false; }
			FScriptSetHelper Helper(SetProperty, ValuePtr); Helper.EmptyElements();
			for (const TSharedPtr<FJsonValue>& Item : *Values) { const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash(); if (!WriteValue(SetProperty->ElementProp, Helper.GetElementPtr(Index), Item, OutError, Depth + 1, MaxDepth)) return false; } Helper.Rehash(); return true;
		}
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr; if (!Input->TryGetArray(Entries) || !Entries) { OutError = TEXT("Expected map entry array."); return false; }
			FScriptMapHelper Helper(MapProperty, ValuePtr); Helper.EmptyValues();
			for (const TSharedPtr<FJsonValue>& Item : *Entries) { const TSharedPtr<FJsonObject> Entry = Item.IsValid() ? Item->AsObject() : nullptr; if (!Entry) { OutError = TEXT("Map entry must be {key,value}."); return false; } const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash(); if (!WriteValue(MapProperty->KeyProp, Helper.GetKeyPtr(Index), Entry->TryGetField(TEXT("key")), OutError, Depth + 1, MaxDepth) || !WriteValue(MapProperty->ValueProp, Helper.GetValuePtr(Index), Entry->TryGetField(TEXT("value")), OutError, Depth + 1, MaxDepth)) return false; } Helper.Rehash(); return true;
		}
		if (JsonString(Input, Text) && Property->ImportText_Direct(*Text, ValuePtr, nullptr, PPF_None)) return true;
		OutError = FString::Printf(TEXT("Unsupported typed property write: %s"), *Property->GetCPPType());
		return false;
	}

	bool FUEPIPropertyCodec::WriteValueWithMode(const FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, const FString& Mode, FString& OutError, int32 MaxDepth)
	{
		const FString NormalizedMode = Mode.IsEmpty() ? TEXT("replace") : Mode.ToLower();
		if (NormalizedMode == TEXT("replace") || NormalizedMode == TEXT("set_field"))
		{
			return WriteValue(Property, ValuePtr, Value, OutError, 0, MaxDepth);
		}
		if (NormalizedMode == TEXT("clear"))
		{
			if (const FArrayProperty* Array = CastField<FArrayProperty>(Property)) { FScriptArrayHelper(Array, ValuePtr).EmptyValues(); return true; }
			if (const FSetProperty* Set = CastField<FSetProperty>(Property)) { FScriptSetHelper(Set, ValuePtr).EmptyElements(); return true; }
			if (const FMapProperty* Map = CastField<FMapProperty>(Property)) { FScriptMapHelper(Map, ValuePtr).EmptyValues(); return true; }
			OutError = TEXT("clear is supported only for array, set, and map properties.");
			return false;
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(Array, ValuePtr);
			if (NormalizedMode == TEXT("add"))
			{
				const TSharedPtr<FJsonValue> Input = NormalizeTypedInput(Value);
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				if (Input.IsValid() && Input->TryGetArray(Items) && Items)
				{
					const int32 Start = Helper.AddValues(Items->Num());
					for (int32 Index = 0; Index < Items->Num(); ++Index) if (!WriteValue(Array->Inner, Helper.GetRawPtr(Start + Index), (*Items)[Index], OutError, 0, MaxDepth)) return false;
					return true;
				}
				const int32 Index = Helper.AddValue();
				return WriteValue(Array->Inner, Helper.GetRawPtr(Index), Value, OutError, 0, MaxDepth);
			}
			const TSharedPtr<FJsonObject> Command = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			const int32 Index = Command.IsValid() ? JsonObjectInt(Command, TEXT("index"), INDEX_NONE) : INDEX_NONE;
			if (NormalizedMode == TEXT("insert"))
			{
				const TSharedPtr<FJsonValue> Item = Command.IsValid() ? Command->TryGetField(TEXT("value")) : nullptr;
				if (Index < 0 || Index > Helper.Num() || !Item.IsValid()) { OutError = TEXT("insert requires {index,value} within the array bounds."); return false; }
				Helper.InsertValues(Index, 1); return WriteValue(Array->Inner, Helper.GetRawPtr(Index), Item, OutError, 0, MaxDepth);
			}
			if (NormalizedMode == TEXT("remove"))
			{
				if (!Helper.IsValidIndex(Index)) { OutError = TEXT("remove requires a valid {index} for an array."); return false; }
				Helper.RemoveValues(Index, 1); return true;
			}
		}
		if (const FSetProperty* Set = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper Helper(Set, ValuePtr);
			if (NormalizedMode == TEXT("add"))
			{
				const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
				if (!WriteValue(Set->ElementProp, Helper.GetElementPtr(Index), Value, OutError, 0, MaxDepth)) return false;
				Helper.Rehash(); return true;
			}
			if (NormalizedMode == TEXT("remove"))
			{
				for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
				{
					if (!Helper.IsValidIndex(Index)) continue;
					uint8* Temp = static_cast<uint8*>(FMemory_Alloca(Set->ElementProp->GetSize())); Set->ElementProp->InitializeValue(Temp);
					const bool bWritten = WriteValue(Set->ElementProp, Temp, Value, OutError, 0, MaxDepth);
					const bool bMatch = bWritten && Set->ElementProp->Identical(Helper.GetElementPtr(Index), Temp);
					Set->ElementProp->DestroyValue(Temp);
					if (!bWritten) return false;
					if (bMatch) { Helper.RemoveAt(Index); Helper.Rehash(); return true; }
				}
				OutError = TEXT("Set element was not found for remove."); return false;
			}
		}
		if (const FMapProperty* Map = CastField<FMapProperty>(Property))
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			const TSharedPtr<FJsonValue> Key = Entry.IsValid() ? Entry->TryGetField(TEXT("key")) : nullptr;
			const TSharedPtr<FJsonValue> EntryValue = Entry.IsValid() ? Entry->TryGetField(TEXT("value")) : nullptr;
			if (!Key.IsValid()) { OutError = TEXT("Map add/remove requires {key,value?}."); return false; }
			FScriptMapHelper Helper(Map, ValuePtr);
			if (NormalizedMode == TEXT("add"))
			{
				if (!EntryValue.IsValid()) { OutError = TEXT("Map add requires {key,value}."); return false; }
				const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
				if (!WriteValue(Map->KeyProp, Helper.GetKeyPtr(Index), Key, OutError, 0, MaxDepth) || !WriteValue(Map->ValueProp, Helper.GetValuePtr(Index), EntryValue, OutError, 0, MaxDepth)) return false;
				Helper.Rehash(); return true;
			}
			if (NormalizedMode == TEXT("remove"))
			{
				for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
				{
					if (!Helper.IsValidIndex(Index)) continue;
					uint8* Temp = static_cast<uint8*>(FMemory_Alloca(Map->KeyProp->GetSize())); Map->KeyProp->InitializeValue(Temp);
					const bool bWritten = WriteValue(Map->KeyProp, Temp, Key, OutError, 0, MaxDepth);
					const bool bMatch = bWritten && Map->KeyProp->Identical(Helper.GetKeyPtr(Index), Temp);
					Map->KeyProp->DestroyValue(Temp);
					if (!bWritten) return false;
					if (bMatch) { Helper.RemoveAt(Index); Helper.Rehash(); return true; }
				}
				OutError = TEXT("Map key was not found for remove."); return false;
			}
		}
		OutError = FString::Printf(TEXT("Property write mode %s is unsupported for %s."), *NormalizedMode, Property ? *Property->GetCPPType() : TEXT("<null>"));
		return false;
	}

	bool FUEPIPropertyCodec::SetPropertyPath(UObject* Object, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonValue>& OutBefore, TSharedPtr<FJsonValue>& OutAfter, FString& OutError, const FString& Mode)
	{
		if (!IsValid(Object) || PropertyPath.IsEmpty()) { OutError = TEXT("Object and property path are required."); return false; }
		TArray<FString> Segments; PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		UStruct* CurrentStruct = Object->GetClass();
		void* CurrentContainer = Object;
		for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
		{
			FString Name; int32 ArrayIndex = INDEX_NONE; FString MapKey; bool bHasMapKey = false;
			if (!ParseSegment(Segments[SegmentIndex], Name, ArrayIndex, MapKey, bHasMapKey)) { OutError = TEXT("Invalid property path segment."); return false; }
			FProperty* Property = FindFProperty<FProperty>(CurrentStruct, *Name);
			if (!Property) { OutError = FString::Printf(TEXT("Property path field was not found: %s"), *Name); return false; }
			void* PropertyPtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (ArrayIndex != INDEX_NONE)
			{
				FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property); if (!ArrayProperty) { OutError = TEXT("Indexed path segment is not an array."); return false; }
				FScriptArrayHelper Helper(ArrayProperty, PropertyPtr); if (!Helper.IsValidIndex(ArrayIndex)) { OutError = TEXT("Array path index is out of range."); return false; }
				Property = ArrayProperty->Inner; PropertyPtr = Helper.GetRawPtr(ArrayIndex);
			}
			if (bHasMapKey)
			{
				FMapProperty* MapProperty = CastField<FMapProperty>(Property); if (!MapProperty) { OutError = TEXT("Map-key path segment is not a map."); return false; }
				FScriptMapHelper Helper(MapProperty, PropertyPtr); int32 Match = INDEX_NONE;
				for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
				{
					if (Helper.IsValidIndex(Index) && MapKeyString(MapProperty->KeyProp, Helper.GetKeyPtr(Index)) == MapKey) { Match = Index; break; }
				}
				if (Match == INDEX_NONE) { OutError = FString::Printf(TEXT("Map key was not found in property path: %s"), *MapKey); return false; }
				Property = MapProperty->ValueProp; PropertyPtr = Helper.GetValuePtr(Match);
			}
			if (SegmentIndex == Segments.Num() - 1)
			{
				OutBefore = ReadValue(Property, PropertyPtr);
				if (!WriteValueWithMode(Property, PropertyPtr, Value, Mode, OutError)) return false;
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
