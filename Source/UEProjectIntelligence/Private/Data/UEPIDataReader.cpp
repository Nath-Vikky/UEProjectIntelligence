#include "UEPIDataReader.h"

#include "Curves/CurveBase.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveLinearColorAtlas.h"
#include "Curves/CurveVector.h"
#include "Curves/RichCurve.h"
#include "Engine/AssetManager.h"
#include "Engine/CompositeDataTable.h"
#include "Engine/CurveTable.h"
#include "Engine/DataAsset.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/UserDefinedStruct.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"

namespace UE::ProjectIntelligence
{
namespace
{
FString DataBool(bool bValue)
{
	return bValue ? TEXT("true") : TEXT("false");
}

FString DataGuidString(const FGuid& Guid)
{
	return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString();
}

FString PrimaryAssetCookRuleString(EPrimaryAssetCookRule Rule)
{
	if (const UEnum* Enum = StaticEnum<EPrimaryAssetCookRule>())
	{
		return Enum->GetNameStringByValue(static_cast<int64>(Rule));
	}
	return FString::FromInt(static_cast<int32>(Rule));
}

FString AssetPathString(const FTopLevelAssetPath& AssetPath)
{
	return AssetPath.IsValid() ? AssetPath.ToString() : FString();
}

FAssetBundleData DataAssetBundleData(const UDataAsset& DataAsset, const FPrimaryAssetId& PrimaryAssetId)
{
	FAssetBundleData BundleData;
	if (UAssetManager* AssetManager = UAssetManager::GetIfInitialized())
	{
		AssetManager->InitializeAssetBundlesFromMetadata(&DataAsset, BundleData);
		if (PrimaryAssetId.IsValid())
		{
			TArray<FAssetBundleEntry> ManagedEntries;
			if (AssetManager->GetAssetBundleEntries(PrimaryAssetId, ManagedEntries))
			{
				for (const FAssetBundleEntry& Entry : ManagedEntries)
				{
					if (Entry.IsValid())
					{
						BundleData.AddBundleAssets(Entry.BundleName, Entry.AssetPaths);
					}
				}
			}
		}
	}
	return BundleData;
}

int32 BundledAssetCount(const FAssetBundleData& BundleData)
{
	int32 Count = 0;
	for (const FAssetBundleEntry& Entry : BundleData.Bundles)
	{
		Count += Entry.AssetPaths.Num();
	}
	return Count;
}

TArray<TSharedPtr<FJsonValue>> AssetBundleSnapshots(const FAssetBundleData& BundleData)
{
	TArray<FAssetBundleEntry> Entries = BundleData.Bundles;
	Entries.Sort([](const FAssetBundleEntry& Left, const FAssetBundleEntry& Right)
	{
		return Left.BundleName.ToString() < Right.BundleName.ToString();
	});

	TArray<TSharedPtr<FJsonValue>> BundleValues;
	for (const FAssetBundleEntry& Entry : Entries)
	{
		TArray<FString> AssetPaths;
		for (const FTopLevelAssetPath& AssetPath : Entry.AssetPaths)
		{
			const FString Path = AssetPathString(AssetPath);
			if (!Path.IsEmpty())
			{
				AssetPaths.Add(Path);
			}
		}
		AssetPaths.Sort();

		TArray<TSharedPtr<FJsonValue>> PathValues;
		for (const FString& Path : AssetPaths)
		{
			PathValues.Add(MakeShared<FJsonValueString>(Path));
		}

		TSharedRef<FJsonObject> BundleObject = MakeShared<FJsonObject>();
		BundleObject->SetStringField(TEXT("name"), Entry.BundleName.ToString());
		BundleObject->SetNumberField(TEXT("asset_count"), PathValues.Num());
		BundleObject->SetArrayField(TEXT("asset_paths"), PathValues);
		BundleValues.Add(MakeShared<FJsonValueObject>(BundleObject));
	}
	return BundleValues;
}

FString UserDefinedStructStatusString(EUserDefinedStructureStatus Status)
{
	switch (Status)
	{
	case UDSS_UpToDate:
		return TEXT("up_to_date");
	case UDSS_Dirty:
		return TEXT("dirty");
	case UDSS_Error:
		return TEXT("error");
	case UDSS_Duplicate:
		return TEXT("duplicate");
	default:
		return TEXT("unknown");
	}
}

FString EnumCppFormString(UEnum::ECppForm Form)
{
	switch (Form)
	{
	case UEnum::ECppForm::Regular:
		return TEXT("regular");
	case UEnum::ECppForm::Namespaced:
		return TEXT("namespaced");
	case UEnum::ECppForm::EnumClass:
		return TEXT("enum_class");
	default:
		return TEXT("unknown");
	}
}

template <typename EnumType>
FString CurveEnumString(EnumType Value)
{
	if (const UEnum* Enum = StaticEnum<EnumType>())
	{
		return Enum->GetNameStringByValue(static_cast<int64>(Value));
	}
	return FString::FromInt(static_cast<int32>(Value));
}

void AddDataEvidence(FEntityRecord& Entity, const FString& Path, const FString& Detail)
{
	Entity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Path,
		Detail
	});
}

void AddDataRelation(
	const FString& ProjectId,
	const FString& Type,
	const FString& FromId,
	const FString& ToId,
	const FString& EvidencePath,
	const FString& EvidenceDetail,
	TArray<FRelationRecord>& OutRelations)
{
	FRelationRecord Relation;
	Relation.Id = MakeRelationId(ProjectId, Type, FromId, ToId);
	Relation.Type = Type;
	Relation.FromId = FromId;
	Relation.ToId = ToId;
	Relation.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Relation.bDerived = false;
	Relation.Confidence = 1.0f;
	Relation.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		EvidencePath,
		EvidenceDetail
	});
	OutRelations.Add(MoveTemp(Relation));
}

FEntityRecord* FindDataEntity(TArray<FEntityRecord>& Entities, const FString& EntityId)
{
	return Entities.FindByPredicate([&EntityId](const FEntityRecord& Entity)
	{
		return Entity.Id == EntityId;
	});
}

FString AddDataAssetEntity(
	const FString& ProjectId,
	const UDataAsset& DataAsset,
	const FPrimaryAssetId& PrimaryAssetId,
	const FAssetBundleData& BundleData,
	TArray<FEntityRecord>& OutEntities)
{
	const FString DataAssetPath = DataAsset.GetPathName();
	const FString DataAssetId = MakeStableId(ProjectId, TEXT("data_asset"), DataAssetPath);
	if (FindDataEntity(OutEntities, DataAssetId))
	{
		return DataAssetId;
	}

	FEntityRecord Entity;
	Entity.Id = DataAssetId;
	Entity.Kind = TEXT("data_asset");
	Entity.CanonicalKey = DataAssetPath;
	Entity.DisplayName = DataAsset.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("data_asset_path"), DataAssetPath);
	Entity.Attributes.Add(TEXT("data_asset_class"), DataAsset.GetClass() ? DataAsset.GetClass()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("is_primary_data_asset"), DataBool(DataAsset.IsA<UPrimaryDataAsset>()));
	Entity.Attributes.Add(TEXT("primary_asset_id"), PrimaryAssetId.IsValid() ? PrimaryAssetId.ToString() : FString());
	Entity.Attributes.Add(TEXT("primary_asset_type"), PrimaryAssetId.IsValid() ? PrimaryAssetId.PrimaryAssetType.ToString() : FString());
	Entity.Attributes.Add(TEXT("primary_asset_name"), PrimaryAssetId.IsValid() ? PrimaryAssetId.PrimaryAssetName.ToString() : FString());
	Entity.Attributes.Add(TEXT("bundle_count"), FString::FromInt(BundleData.Bundles.Num()));
	Entity.Attributes.Add(TEXT("bundled_asset_count"), FString::FromInt(BundledAssetCount(BundleData)));
	if (PrimaryAssetId.IsValid())
	{
		if (UAssetManager* AssetManager = UAssetManager::GetIfInitialized())
		{
			const FPrimaryAssetRules Rules = AssetManager->GetPrimaryAssetRules(PrimaryAssetId);
			Entity.Attributes.Add(TEXT("asset_rule_priority"), FString::FromInt(Rules.Priority));
			Entity.Attributes.Add(TEXT("asset_rule_chunk_id"), FString::FromInt(Rules.ChunkId));
			Entity.Attributes.Add(TEXT("asset_rule_cook_rule"), PrimaryAssetCookRuleString(Rules.CookRule));
		}
	}
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("data_asset_class"), TEXT("primary_asset_id"), TEXT("asset_manager_rules"), TEXT("asset_bundle_members") };
	Entity.Completeness.Omitted = { TEXT("runtime_bundle_state") };
	AddDataEvidence(Entity, DataAssetPath, TEXT("DataAsset metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return DataAssetId;
}

TSharedRef<FJsonObject> DataAssetSnapshot(
	const UDataAsset& DataAsset,
	const FString& DataAssetId,
	const FPrimaryAssetId& PrimaryAssetId,
	const FAssetBundleData& BundleData)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.data_asset.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("id"), DataAssetId);
	Object->SetStringField(TEXT("data_asset_path"), DataAsset.GetPathName());
	Object->SetStringField(TEXT("data_asset_class"), DataAsset.GetClass() ? DataAsset.GetClass()->GetPathName() : FString());
	Object->SetBoolField(TEXT("is_primary_data_asset"), DataAsset.IsA<UPrimaryDataAsset>());
	Object->SetStringField(TEXT("primary_asset_id"), PrimaryAssetId.IsValid() ? PrimaryAssetId.ToString() : FString());
	Object->SetStringField(TEXT("primary_asset_type"), PrimaryAssetId.IsValid() ? PrimaryAssetId.PrimaryAssetType.ToString() : FString());
	Object->SetStringField(TEXT("primary_asset_name"), PrimaryAssetId.IsValid() ? PrimaryAssetId.PrimaryAssetName.ToString() : FString());
	if (PrimaryAssetId.IsValid())
	{
		if (UAssetManager* AssetManager = UAssetManager::GetIfInitialized())
		{
			const FPrimaryAssetRules Rules = AssetManager->GetPrimaryAssetRules(PrimaryAssetId);
			TSharedRef<FJsonObject> RulesObject = MakeShared<FJsonObject>();
			RulesObject->SetNumberField(TEXT("priority"), Rules.Priority);
			RulesObject->SetNumberField(TEXT("chunk_id"), Rules.ChunkId);
			RulesObject->SetStringField(TEXT("cook_rule"), PrimaryAssetCookRuleString(Rules.CookRule));
			Object->SetObjectField(TEXT("asset_manager_rules"), RulesObject);
		}
	}
	Object->SetNumberField(TEXT("bundle_count"), BundleData.Bundles.Num());
	Object->SetNumberField(TEXT("bundled_asset_count"), BundledAssetCount(BundleData));
	Object->SetArrayField(TEXT("bundles"), AssetBundleSnapshots(BundleData));
	return Object;
}

struct FStringTableEntryRecord
{
	FString Key;
	FString SourceString;
	TMap<FString, FString> Metadata;
};

TArray<FStringTableEntryRecord> StringTableEntries(const UStringTable& StringTable)
{
	TArray<FStringTableEntryRecord> Entries;
	const FStringTableConstRef Table = StringTable.GetStringTable();
	Table->EnumerateKeysAndSourceStrings([&Entries, &Table](const FTextKey& Key, const FString& SourceString)
	{
		FStringTableEntryRecord Record;
		Record.Key = FString(Key.GetChars());
		Record.SourceString = SourceString;
		Table->EnumerateMetaData(Key, [&Record](FName MetadataId, const FString& MetadataValue)
		{
			Record.Metadata.Add(MetadataId.ToString(), MetadataValue);
			return true;
		});
		Entries.Add(MoveTemp(Record));
		return true;
	});
	Entries.Sort([](const FStringTableEntryRecord& Left, const FStringTableEntryRecord& Right)
	{
		return Left.Key < Right.Key;
	});
	return Entries;
}

int32 StringTableMetadataCount(const TArray<FStringTableEntryRecord>& Entries)
{
	int32 Count = 0;
	for (const FStringTableEntryRecord& Entry : Entries)
	{
		Count += Entry.Metadata.Num();
	}
	return Count;
}

FString AddStringTableEntity(
	const FString& ProjectId,
	const UStringTable& StringTable,
	int32 EntryCount,
	int32 MetadataCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString TablePath = StringTable.GetPathName();
	const FString TableId = MakeStableId(ProjectId, TEXT("string_table"), TablePath);
	if (FindDataEntity(OutEntities, TableId))
	{
		return TableId;
	}

	FEntityRecord Entity;
	Entity.Id = TableId;
	Entity.Kind = TEXT("string_table");
	Entity.CanonicalKey = TablePath;
	Entity.DisplayName = StringTable.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("string_table_path"), TablePath);
	Entity.Attributes.Add(TEXT("string_table_class"), StringTable.GetClass() ? StringTable.GetClass()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("table_id"), StringTable.GetStringTableId().ToString());
	Entity.Attributes.Add(TEXT("namespace"), StringTable.GetStringTable()->GetNamespace());
	Entity.Attributes.Add(TEXT("is_loaded"), DataBool(StringTable.GetStringTable()->IsLoaded()));
	Entity.Attributes.Add(TEXT("entry_count"), FString::FromInt(EntryCount));
	Entity.Attributes.Add(TEXT("metadata_count"), FString::FromInt(MetadataCount));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("string_table_entries"), TEXT("string_table_metadata") };
	Entity.Completeness.Omitted = { TEXT("localized_culture_overrides") };
	AddDataEvidence(Entity, TablePath, TEXT("StringTable entries and metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return TableId;
}

FString AddStringTableEntryEntity(
	const FString& ProjectId,
	const UStringTable& StringTable,
	const FStringTableEntryRecord& Entry,
	int32 EntryIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = FString::Printf(TEXT("%s:entry:%s"), *StringTable.GetPathName(), *Entry.Key);
	const FString EntryId = MakeStableId(ProjectId, TEXT("string_table_entry"), CanonicalKey);
	if (FindDataEntity(OutEntities, EntryId))
	{
		return EntryId;
	}

	FEntityRecord Entity;
	Entity.Id = EntryId;
	Entity.Kind = TEXT("string_table_entry");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Entry.Key;
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("string_table_path"), StringTable.GetPathName());
	Entity.Attributes.Add(TEXT("entry_key"), Entry.Key);
	Entity.Attributes.Add(TEXT("entry_index"), FString::FromInt(EntryIndex));
	Entity.Attributes.Add(TEXT("metadata_count"), FString::FromInt(Entry.Metadata.Num()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("string_table_entry_source"), TEXT("string_table_entry_metadata") };
	Entity.Completeness.Omitted = { TEXT("localized_culture_overrides") };
	AddDataEvidence(Entity, StringTable.GetPathName(), TEXT("StringTable entry read from FStringTable."));
	OutEntities.Add(MoveTemp(Entity));
	return EntryId;
}

TSharedRef<FJsonObject> StringTableEntrySnapshot(const FString& EntryId, const FStringTableEntryRecord& Entry, int32 EntryIndex)
{
	TSharedRef<FJsonObject> MetadataObject = MakeShared<FJsonObject>();
	TArray<FString> MetadataKeys;
	Entry.Metadata.GenerateKeyArray(MetadataKeys);
	MetadataKeys.Sort();
	for (const FString& MetadataKey : MetadataKeys)
	{
		MetadataObject->SetStringField(MetadataKey, Entry.Metadata[MetadataKey]);
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), EntryId);
	Object->SetNumberField(TEXT("index"), EntryIndex);
	Object->SetStringField(TEXT("key"), Entry.Key);
	Object->SetStringField(TEXT("source_string"), Entry.SourceString);
	Object->SetNumberField(TEXT("metadata_count"), Entry.Metadata.Num());
	Object->SetObjectField(TEXT("metadata"), MetadataObject);
	return Object;
}

TSharedRef<FJsonObject> StringTableSnapshot(
	const FString& ProjectId,
	const UStringTable& StringTable,
	const FString& TableId,
	const TArray<FStringTableEntryRecord>& Entries,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> EntryValues;
	for (int32 EntryIndex = 0; EntryIndex < Entries.Num(); ++EntryIndex)
	{
		const FStringTableEntryRecord& Entry = Entries[EntryIndex];
		const FString EntryId = AddStringTableEntryEntity(ProjectId, StringTable, Entry, EntryIndex, OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_string_table_entry"), TableId, EntryId, StringTable.GetPathName(), TEXT("StringTable contains this entry."), OutRelations);
		EntryValues.Add(MakeShared<FJsonValueObject>(StringTableEntrySnapshot(EntryId, Entry, EntryIndex)));
	}

	const FStringTableConstRef Table = StringTable.GetStringTable();
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.string_table.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("string_table_path"), StringTable.GetPathName());
	Object->SetStringField(TEXT("string_table_class"), StringTable.GetClass() ? StringTable.GetClass()->GetPathName() : FString());
	Object->SetStringField(TEXT("table_id"), StringTable.GetStringTableId().ToString());
	Object->SetStringField(TEXT("namespace"), Table->GetNamespace());
	Object->SetBoolField(TEXT("is_loaded"), Table->IsLoaded());
	Object->SetNumberField(TEXT("entry_count"), EntryValues.Num());
	Object->SetNumberField(TEXT("metadata_count"), StringTableMetadataCount(Entries));
	Object->SetArrayField(TEXT("entries"), EntryValues);
	return Object;
}

TArray<FProperty*> UserDefinedStructProperties(const UUserDefinedStruct& Struct)
{
	TArray<FProperty*> Properties;
	for (TFieldIterator<FProperty> PropertyIt(&Struct); PropertyIt; ++PropertyIt)
	{
		Properties.Add(*PropertyIt);
	}
	Properties.Sort([](const FProperty& Left, const FProperty& Right)
	{
		if (Left.GetOffset_ForInternal() != Right.GetOffset_ForInternal())
		{
			return Left.GetOffset_ForInternal() < Right.GetOffset_ForInternal();
		}
		return Left.GetName() < Right.GetName();
	});
	return Properties;
}

FString AddUserDefinedStructEntity(
	const FString& ProjectId,
	const UUserDefinedStruct& Struct,
	int32 FieldCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString StructPath = Struct.GetPathName();
	const FString StructId = MakeStableId(ProjectId, TEXT("user_defined_struct"), StructPath);
	if (FindDataEntity(OutEntities, StructId))
	{
		return StructId;
	}

	FEntityRecord Entity;
	Entity.Id = StructId;
	Entity.Kind = TEXT("user_defined_struct");
	Entity.CanonicalKey = StructPath;
	Entity.DisplayName = Struct.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("struct_path"), StructPath);
	Entity.Attributes.Add(TEXT("struct_class"), Struct.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("guid"), DataGuidString(Struct.Guid));
	Entity.Attributes.Add(TEXT("status"), UserDefinedStructStatusString(Struct.Status));
	Entity.Attributes.Add(TEXT("field_count"), FString::FromInt(FieldCount));
	Entity.Attributes.Add(TEXT("struct_size"), FString::FromInt(Struct.GetStructureSize()));
	Entity.Attributes.Add(TEXT("min_alignment"), FString::FromInt(Struct.GetMinAlignment()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("user_defined_struct_fields"), TEXT("field_types"), TEXT("field_layout_metadata") };
	Entity.Completeness.Omitted = { TEXT("default_instance_values"), TEXT("editor_graph_history") };
	AddDataEvidence(Entity, StructPath, TEXT("UserDefinedStruct field metadata read through public UScriptStruct reflection."));
	OutEntities.Add(MoveTemp(Entity));
	return StructId;
}

FString AddUserDefinedStructFieldEntity(
	const FString& ProjectId,
	const UUserDefinedStruct& Struct,
	const FProperty& Property,
	int32 FieldIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString PropertyName = Property.GetName();
	const FString CanonicalKey = FString::Printf(TEXT("%s:field:%d:%s"), *Struct.GetPathName(), FieldIndex, *PropertyName);
	const FString FieldId = MakeStableId(ProjectId, TEXT("user_defined_struct_field"), CanonicalKey);
	if (FindDataEntity(OutEntities, FieldId))
	{
		return FieldId;
	}

	FEntityRecord Entity;
	Entity.Id = FieldId;
	Entity.Kind = TEXT("user_defined_struct_field");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = PropertyName;
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("struct_path"), Struct.GetPathName());
	Entity.Attributes.Add(TEXT("field_name"), PropertyName);
	Entity.Attributes.Add(TEXT("field_index"), FString::FromInt(FieldIndex));
	Entity.Attributes.Add(TEXT("display_name"), Property.GetDisplayNameText().ToString());
	Entity.Attributes.Add(TEXT("cpp_type"), Property.GetCPPType());
	Entity.Attributes.Add(TEXT("property_class"), Property.GetClass() ? FString(Property.GetClass()->GetName()) : FString());
	Entity.Attributes.Add(TEXT("array_dim"), FString::FromInt(Property.ArrayDim));
	Entity.Attributes.Add(TEXT("property_flags"), FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(Property.GetPropertyFlags())));
	Entity.Attributes.Add(TEXT("offset_internal"), FString::FromInt(Property.GetOffset_ForInternal()));
	Entity.Attributes.Add(TEXT("size"), FString::FromInt(Property.GetSize()));
	Entity.Attributes.Add(TEXT("min_alignment"), FString::FromInt(Property.GetMinAlignment()));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("field_type"), TEXT("field_layout_metadata") };
	Entity.Completeness.Omitted = { TEXT("field_default_value") };
	AddDataEvidence(Entity, Struct.GetPathName(), TEXT("UserDefinedStruct field read from FProperty reflection metadata."));
	OutEntities.Add(MoveTemp(Entity));
	return FieldId;
}

TSharedRef<FJsonObject> UserDefinedStructFieldSnapshot(const FString& FieldId, const FProperty& Property, int32 FieldIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), FieldId);
	Object->SetNumberField(TEXT("index"), FieldIndex);
	Object->SetStringField(TEXT("name"), Property.GetName());
	Object->SetStringField(TEXT("display_name"), Property.GetDisplayNameText().ToString());
	Object->SetStringField(TEXT("cpp_type"), Property.GetCPPType());
	Object->SetStringField(TEXT("property_class"), Property.GetClass() ? FString(Property.GetClass()->GetName()) : FString());
	Object->SetNumberField(TEXT("array_dim"), Property.ArrayDim);
	Object->SetStringField(TEXT("property_flags"), FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(Property.GetPropertyFlags())));
	Object->SetNumberField(TEXT("offset_internal"), Property.GetOffset_ForInternal());
	Object->SetNumberField(TEXT("size"), Property.GetSize());
	Object->SetNumberField(TEXT("min_alignment"), Property.GetMinAlignment());
	return Object;
}

TSharedRef<FJsonObject> UserDefinedStructSnapshot(
	const FString& ProjectId,
	const UUserDefinedStruct& Struct,
	const FString& StructId,
	const TArray<FProperty*>& Properties,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> FieldValues;
	for (int32 FieldIndex = 0; FieldIndex < Properties.Num(); ++FieldIndex)
	{
		const FProperty* Property = Properties[FieldIndex];
		if (!Property)
		{
			continue;
		}

		const FString FieldId = AddUserDefinedStructFieldEntity(ProjectId, Struct, *Property, FieldIndex, OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_struct_field"), StructId, FieldId, Struct.GetPathName(), TEXT("UserDefinedStruct contains this field."), OutRelations);
		FieldValues.Add(MakeShared<FJsonValueObject>(UserDefinedStructFieldSnapshot(FieldId, *Property, FieldIndex)));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.user_defined_struct.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("struct_path"), Struct.GetPathName());
	Object->SetStringField(TEXT("struct_class"), Struct.GetClass()->GetPathName());
	Object->SetStringField(TEXT("guid"), DataGuidString(Struct.Guid));
	Object->SetStringField(TEXT("status"), UserDefinedStructStatusString(Struct.Status));
	Object->SetNumberField(TEXT("struct_size"), Struct.GetStructureSize());
	Object->SetNumberField(TEXT("min_alignment"), Struct.GetMinAlignment());
	Object->SetNumberField(TEXT("field_count"), FieldValues.Num());
	Object->SetArrayField(TEXT("fields"), FieldValues);
	return Object;
}

FString AddUserDefinedEnumEntity(
	const FString& ProjectId,
	const UUserDefinedEnum& Enum,
	int32 EntryCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString EnumPath = Enum.GetPathName();
	const FString EnumId = MakeStableId(ProjectId, TEXT("user_defined_enum"), EnumPath);
	if (FindDataEntity(OutEntities, EnumId))
	{
		return EnumId;
	}

	FEntityRecord Entity;
	Entity.Id = EnumId;
	Entity.Kind = TEXT("user_defined_enum");
	Entity.CanonicalKey = EnumPath;
	Entity.DisplayName = Enum.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("enum_path"), EnumPath);
	Entity.Attributes.Add(TEXT("enum_class"), Enum.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("cpp_form"), EnumCppFormString(Enum.GetCppForm()));
	Entity.Attributes.Add(TEXT("entry_count"), FString::FromInt(EntryCount));
#if WITH_EDITORONLY_DATA
	Entity.Attributes.Add(TEXT("description"), Enum.EnumDescription.ToString());
#endif
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("enum_entries"), TEXT("display_names") };
	Entity.Completeness.Omitted = { TEXT("editor_change_history") };
	AddDataEvidence(Entity, EnumPath, TEXT("UserDefinedEnum entries read through public UEnum API."));
	OutEntities.Add(MoveTemp(Entity));
	return EnumId;
}

bool IsEnumMaxEntry(const UEnum& Enum, int32 EntryIndex)
{
	if (EntryIndex != Enum.NumEnums() - 1)
	{
		return false;
	}
	const FString Name = Enum.GetNameStringByIndex(EntryIndex);
	return Name.EndsWith(TEXT("_MAX")) || Name.EndsWith(TEXT("::MAX"));
}

FString AddUserDefinedEnumEntryEntity(
	const FString& ProjectId,
	const UUserDefinedEnum& Enum,
	int32 EntryIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString EntryName = Enum.GetNameStringByIndex(EntryIndex);
	const FString CanonicalKey = FString::Printf(TEXT("%s:entry:%d:%s"), *Enum.GetPathName(), EntryIndex, *EntryName);
	const FString EntryId = MakeStableId(ProjectId, TEXT("user_defined_enum_entry"), CanonicalKey);
	if (FindDataEntity(OutEntities, EntryId))
	{
		return EntryId;
	}

	FEntityRecord Entity;
	Entity.Id = EntryId;
	Entity.Kind = TEXT("user_defined_enum_entry");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Enum.GetDisplayNameTextByIndex(EntryIndex).ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("enum_path"), Enum.GetPathName());
	Entity.Attributes.Add(TEXT("entry_name"), EntryName);
	Entity.Attributes.Add(TEXT("authored_name"), Enum.GetAuthoredNameStringByIndex(EntryIndex));
	Entity.Attributes.Add(TEXT("entry_index"), FString::FromInt(EntryIndex));
	Entity.Attributes.Add(TEXT("value"), FString::Printf(TEXT("%lld"), static_cast<long long>(Enum.GetValueByIndex(EntryIndex))));
	Entity.Attributes.Add(TEXT("is_hidden"), DataBool(Enum.HasMetaData(TEXT("Hidden"), EntryIndex)));
	Entity.Attributes.Add(TEXT("is_max"), DataBool(IsEnumMaxEntry(Enum, EntryIndex)));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("enum_entry_metadata") };
	Entity.Completeness.Omitted = { TEXT("editor_change_history") };
	AddDataEvidence(Entity, Enum.GetPathName(), TEXT("UserDefinedEnum entry read from UEnum metadata."));
	OutEntities.Add(MoveTemp(Entity));
	return EntryId;
}

TSharedRef<FJsonObject> UserDefinedEnumEntrySnapshot(const FString& EntryId, const UUserDefinedEnum& Enum, int32 EntryIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), EntryId);
	Object->SetNumberField(TEXT("index"), EntryIndex);
	Object->SetStringField(TEXT("name"), Enum.GetNameStringByIndex(EntryIndex));
	Object->SetStringField(TEXT("authored_name"), Enum.GetAuthoredNameStringByIndex(EntryIndex));
	Object->SetStringField(TEXT("display_name"), Enum.GetDisplayNameTextByIndex(EntryIndex).ToString());
	Object->SetStringField(TEXT("tooltip"), Enum.GetMetaData(TEXT("ToolTip"), EntryIndex));
	Object->SetNumberField(TEXT("value"), static_cast<double>(Enum.GetValueByIndex(EntryIndex)));
	Object->SetBoolField(TEXT("is_hidden"), Enum.HasMetaData(TEXT("Hidden"), EntryIndex));
	Object->SetBoolField(TEXT("is_max"), IsEnumMaxEntry(Enum, EntryIndex));
	return Object;
}

TSharedRef<FJsonObject> UserDefinedEnumSnapshot(
	const FString& ProjectId,
	const UUserDefinedEnum& Enum,
	const FString& EnumId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> EntryValues;
	for (int32 EntryIndex = 0; EntryIndex < Enum.NumEnums(); ++EntryIndex)
	{
		const FString EntryId = AddUserDefinedEnumEntryEntity(ProjectId, Enum, EntryIndex, OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_enum_entry"), EnumId, EntryId, Enum.GetPathName(), TEXT("UserDefinedEnum contains this entry."), OutRelations);
		EntryValues.Add(MakeShared<FJsonValueObject>(UserDefinedEnumEntrySnapshot(EntryId, Enum, EntryIndex)));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.user_defined_enum.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("enum_path"), Enum.GetPathName());
	Object->SetStringField(TEXT("enum_class"), Enum.GetClass()->GetPathName());
	Object->SetStringField(TEXT("cpp_form"), EnumCppFormString(Enum.GetCppForm()));
#if WITH_EDITORONLY_DATA
	Object->SetStringField(TEXT("description"), Enum.EnumDescription.ToString());
#else
	Object->SetStringField(TEXT("description"), FString());
#endif
	Object->SetNumberField(TEXT("entry_count"), EntryValues.Num());
	Object->SetArrayField(TEXT("entries"), EntryValues);
	return Object;
}

FString CurveKindString(const UObject& Asset)
{
	if (Asset.IsA<UCurveFloat>())
	{
		return TEXT("float");
	}
	if (Asset.IsA<UCurveVector>())
	{
		return TEXT("vector");
	}
	if (Asset.IsA<UCurveLinearColor>())
	{
		return TEXT("linear_color");
	}
	return TEXT("curve");
}

bool IsSupportedCurveAsset(const UObject& Asset)
{
	return Asset.IsA<UCurveFloat>() || Asset.IsA<UCurveVector>() || Asset.IsA<UCurveLinearColor>();
}

FString DataTableRowStructPath(const UDataTable& Table)
{
	const UScriptStruct* RowStruct = Table.GetRowStruct();
	return RowStruct ? RowStruct->GetPathName() : FString();
}

FString DataTableRowStructName(const UDataTable& Table)
{
	const UScriptStruct* RowStruct = Table.GetRowStruct();
	return RowStruct ? RowStruct->GetName() : FString();
}

TArray<FProperty*> DataTableProperties(const UDataTable& Table)
{
	TArray<FProperty*> Properties;
	if (const UScriptStruct* RowStruct = Table.GetRowStruct())
	{
		for (TFieldIterator<FProperty> PropertyIt(RowStruct); PropertyIt; ++PropertyIt)
		{
			Properties.Add(*PropertyIt);
		}
	}
	return Properties;
}

TArray<FName> DataTableRowNames(const UDataTable& Table)
{
	TArray<FName> RowNames;
	Table.GetRowMap().GenerateKeyArray(RowNames);
	RowNames.Sort([](const FName& Left, const FName& Right)
	{
		return Left.ToString() < Right.ToString();
	});
	return RowNames;
}

FString ExportDataTableCellValue(const FProperty& Property, const uint8* RowData)
{
	if (!RowData)
	{
		return FString();
	}

	FString Value;
	Property.ExportText_InContainer(0, Value, RowData, RowData, nullptr, PPF_None);
	if (Value.Len() > 2048)
	{
		Value = Value.Left(2048) + TEXT("...");
	}
	return Value;
}

FString ChannelNameFromEditInfo(const FRichCurveEditInfoConst& CurveInfo, int32 ChannelIndex)
{
	if (!CurveInfo.CurveName.IsNone())
	{
		return CurveInfo.CurveName.ToString();
	}
	return FString::Printf(TEXT("Channel%d"), ChannelIndex);
}

int32 CountCurveKeys(const UCurveBase& Curve)
{
	int32 KeyCount = 0;
	for (const FRichCurveEditInfoConst& CurveInfo : Curve.GetCurves())
	{
		const FRichCurve* RichCurve = static_cast<const FRichCurve*>(CurveInfo.CurveToEdit);
		if (RichCurve)
		{
			KeyCount += RichCurve->GetConstRefOfKeys().Num();
		}
	}
	return KeyCount;
}

FString AddCurveEntity(
	const FString& ProjectId,
	const UCurveBase& Curve,
	int32 ChannelCount,
	int32 KeyCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CurvePath = Curve.GetPathName();
	const FString CurveId = MakeStableId(ProjectId, TEXT("curve"), CurvePath);
	if (FindDataEntity(OutEntities, CurveId))
	{
		return CurveId;
	}

	FEntityRecord Entity;
	Entity.Id = CurveId;
	Entity.Kind = TEXT("curve");
	Entity.CanonicalKey = CurvePath;
	Entity.DisplayName = Curve.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("curve_path"), CurvePath);
	Entity.Attributes.Add(TEXT("curve_class"), Curve.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("curve_kind"), CurveKindString(Curve));
	Entity.Attributes.Add(TEXT("channel_count"), FString::FromInt(ChannelCount));
	Entity.Attributes.Add(TEXT("key_count"), FString::FromInt(KeyCount));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("curve_channels"), TEXT("curve_keys"), TEXT("curve_tangent_metadata") };
	Entity.Completeness.Omitted = { TEXT("runtime_curve_evaluation"), TEXT("compressed_curve_data") };
	AddDataEvidence(Entity, CurvePath, TEXT("Curve asset channels and keys read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return CurveId;
}

FString AddCurveAtlasEntity(
	const FString& ProjectId,
	const UCurveLinearColorAtlas& Atlas,
	TArray<FEntityRecord>& OutEntities)
{
	const FString AtlasPath = Atlas.GetPathName();
	const FString AtlasId = MakeStableId(ProjectId, TEXT("curve_linear_color_atlas"), AtlasPath);
	if (FindDataEntity(OutEntities, AtlasId))
	{
		return AtlasId;
	}

	FEntityRecord Entity;
	Entity.Id = AtlasId;
	Entity.Kind = TEXT("curve_linear_color_atlas");
	Entity.CanonicalKey = AtlasPath;
	Entity.DisplayName = Atlas.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("atlas_path"), AtlasPath);
	Entity.Attributes.Add(TEXT("atlas_class"), Atlas.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("texture_size"), FString::Printf(TEXT("%u"), Atlas.TextureSize));
	Entity.Attributes.Add(TEXT("texture_height"), FString::Printf(TEXT("%u"), Atlas.TextureHeight));
	Entity.Attributes.Add(TEXT("square_resolution"), DataBool(Atlas.bSquareResolution != 0));
	Entity.Attributes.Add(TEXT("gradient_curve_count"), FString::FromInt(Atlas.GradientCurves.Num()));
#if WITH_EDITORONLY_DATA
	Entity.Attributes.Add(TEXT("disable_all_adjustments"), DataBool(Atlas.bDisableAllAdjustments != 0));
	Entity.Attributes.Add(TEXT("dirty"), DataBool(Atlas.bIsDirty != 0));
	Entity.Attributes.Add(TEXT("has_dirty_textures"), DataBool(Atlas.bHasAnyDirtyTextures != 0));
#endif
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("curve_atlas_dimensions"), TEXT("curve_atlas_slots"), TEXT("curve_references") };
	Entity.Completeness.Omitted = { TEXT("atlas_pixel_data"), TEXT("runtime_material_bindings"), TEXT("render_resource_state") };
	AddDataEvidence(Entity, AtlasPath, TEXT("CurveLinearColorAtlas metadata and gradient curve slots read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return AtlasId;
}

FString AddCurveAtlasEntryEntity(
	const FString& ProjectId,
	const UCurveLinearColorAtlas& Atlas,
	int32 EntryIndex,
	const UCurveLinearColor* Curve,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = FString::Printf(TEXT("%s:atlas_entry:%d"), *Atlas.GetPathName(), EntryIndex);
	const FString EntryId = MakeStableId(ProjectId, TEXT("curve_atlas_entry"), CanonicalKey);
	if (FindDataEntity(OutEntities, EntryId))
	{
		return EntryId;
	}

	const uint32 TextureHeight = Atlas.TextureHeight;
	const float NormalizedPosition = TextureHeight > 0 ? (static_cast<float>(EntryIndex) + 0.5f) / static_cast<float>(TextureHeight) : 0.0f;

	FEntityRecord Entity;
	Entity.Id = EntryId;
	Entity.Kind = TEXT("curve_atlas_entry");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = Curve ? Curve->GetName() : FString::Printf(TEXT("Slot%d"), EntryIndex);
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("atlas_path"), Atlas.GetPathName());
	Entity.Attributes.Add(TEXT("slot_index"), FString::FromInt(EntryIndex));
	Entity.Attributes.Add(TEXT("normalized_position"), FString::SanitizeFloat(NormalizedPosition));
	Entity.Attributes.Add(TEXT("curve_path"), Curve ? Curve->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("curve_class"), Curve ? Curve->GetClass()->GetPathName() : FString());
	Entity.Attributes.Add(TEXT("is_null"), DataBool(Curve == nullptr));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("slot_index"), TEXT("curve_reference") };
	Entity.Completeness.Omitted = { TEXT("sampled_lut_values"), TEXT("runtime_material_parameter_usage") };
	AddDataEvidence(Entity, Atlas.GetPathName(), TEXT("Curve atlas slot read from GradientCurves array."));
	OutEntities.Add(MoveTemp(Entity));
	return EntryId;
}

TSharedRef<FJsonObject> CurveAtlasEntrySnapshot(
	const FString& EntryId,
	const UCurveLinearColorAtlas& Atlas,
	int32 EntryIndex,
	const UCurveLinearColor* Curve)
{
	const uint32 TextureHeight = Atlas.TextureHeight;
	const float NormalizedPosition = TextureHeight > 0 ? (static_cast<float>(EntryIndex) + 0.5f) / static_cast<float>(TextureHeight) : 0.0f;

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), EntryId);
	Object->SetNumberField(TEXT("index"), EntryIndex);
	Object->SetNumberField(TEXT("normalized_position"), NormalizedPosition);
	Object->SetBoolField(TEXT("is_null"), Curve == nullptr);
	Object->SetStringField(TEXT("curve_path"), Curve ? Curve->GetPathName() : FString());
	Object->SetStringField(TEXT("curve_class"), Curve ? Curve->GetClass()->GetPathName() : FString());
	Object->SetStringField(TEXT("curve_name"), Curve ? Curve->GetName() : FString());
	return Object;
}

TSharedRef<FJsonObject> CurveAtlasSnapshot(
	const FString& ProjectId,
	const UCurveLinearColorAtlas& Atlas,
	const FString& AtlasId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> EntryValues;
	for (int32 EntryIndex = 0; EntryIndex < Atlas.GradientCurves.Num(); ++EntryIndex)
	{
		const UCurveLinearColor* Curve = Atlas.GradientCurves[EntryIndex];
		const FString EntryId = AddCurveAtlasEntryEntity(ProjectId, Atlas, EntryIndex, Curve, OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_curve_atlas_entry"), AtlasId, EntryId, Atlas.GetPathName(), TEXT("Curve atlas contains this gradient slot."), OutRelations);
		if (Curve)
		{
			const FString CurveId = AddCurveEntity(ProjectId, *Curve, Curve->GetCurves().Num(), CountCurveKeys(*Curve), OutEntities);
			AddDataRelation(ProjectId, TEXT("uses_curve"), EntryId, CurveId, Atlas.GetPathName(), TEXT("Curve atlas slot references this linear color curve."), OutRelations);
		}
		EntryValues.Add(MakeShared<FJsonValueObject>(CurveAtlasEntrySnapshot(EntryId, Atlas, EntryIndex, Curve)));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.curve_linear_color_atlas.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("atlas_path"), Atlas.GetPathName());
	Object->SetStringField(TEXT("atlas_class"), Atlas.GetClass()->GetPathName());
	Object->SetNumberField(TEXT("texture_size"), Atlas.TextureSize);
	Object->SetNumberField(TEXT("texture_height"), Atlas.TextureHeight);
	Object->SetBoolField(TEXT("square_resolution"), Atlas.bSquareResolution != 0);
#if WITH_EDITORONLY_DATA
	Object->SetBoolField(TEXT("disable_all_adjustments"), Atlas.bDisableAllAdjustments != 0);
	Object->SetBoolField(TEXT("dirty"), Atlas.bIsDirty != 0);
	Object->SetBoolField(TEXT("has_dirty_textures"), Atlas.bHasAnyDirtyTextures != 0);
#else
	Object->SetBoolField(TEXT("disable_all_adjustments"), false);
	Object->SetBoolField(TEXT("dirty"), false);
	Object->SetBoolField(TEXT("has_dirty_textures"), false);
#endif
	Object->SetNumberField(TEXT("gradient_curve_count"), EntryValues.Num());
	Object->SetArrayField(TEXT("gradient_curves"), EntryValues);
	return Object;
}

FString AddDataTableEntity(
	const FString& ProjectId,
	const UDataTable& Table,
	int32 ColumnCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString TablePath = Table.GetPathName();
	const FString TableId = MakeStableId(ProjectId, TEXT("data_table"), TablePath);
	if (FindDataEntity(OutEntities, TableId))
	{
		return TableId;
	}

	FEntityRecord Entity;
	Entity.Id = TableId;
	Entity.Kind = TEXT("data_table");
	Entity.CanonicalKey = TablePath;
	Entity.DisplayName = Table.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("table_path"), TablePath);
	Entity.Attributes.Add(TEXT("table_class"), Table.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("row_struct_path"), DataTableRowStructPath(Table));
	Entity.Attributes.Add(TEXT("row_struct_name"), DataTableRowStructName(Table));
	Entity.Attributes.Add(TEXT("row_count"), FString::FromInt(Table.GetRowMap().Num()));
	Entity.Attributes.Add(TEXT("column_count"), FString::FromInt(ColumnCount));
	Entity.Attributes.Add(TEXT("strip_from_client_builds"), DataBool(Table.bStripFromClientBuilds));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("data_table_row_struct"), TEXT("data_table_columns"), TEXT("data_table_rows") };
	Entity.Completeness.Omitted = { TEXT("composite_parent_tables"), TEXT("runtime_lookup_context") };
	AddDataEvidence(Entity, TablePath, TEXT("DataTable rows and row struct metadata read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return TableId;
}

FString AddDataTableColumnEntity(
	const FString& ProjectId,
	const UDataTable& Table,
	const FProperty& Property,
	int32 ColumnIndex,
	TArray<FEntityRecord>& OutEntities)
{
	const FString PropertyName = Property.GetName();
	const FString CanonicalKey = FString::Printf(TEXT("%s:column:%d:%s"), *Table.GetPathName(), ColumnIndex, *PropertyName);
	const FString ColumnId = MakeStableId(ProjectId, TEXT("data_table_column"), CanonicalKey);
	if (FindDataEntity(OutEntities, ColumnId))
	{
		return ColumnId;
	}

	FEntityRecord Entity;
	Entity.Id = ColumnId;
	Entity.Kind = TEXT("data_table_column");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = PropertyName;
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("table_path"), Table.GetPathName());
	Entity.Attributes.Add(TEXT("column_name"), PropertyName);
	Entity.Attributes.Add(TEXT("column_index"), FString::FromInt(ColumnIndex));
	Entity.Attributes.Add(TEXT("display_name"), Property.GetDisplayNameText().ToString());
	Entity.Attributes.Add(TEXT("cpp_type"), Property.GetCPPType());
	Entity.Attributes.Add(TEXT("property_class"), Property.GetClass() ? FString(Property.GetClass()->GetName()) : FString());
	Entity.Attributes.Add(TEXT("array_dim"), FString::FromInt(Property.ArrayDim));
	Entity.Attributes.Add(TEXT("property_flags"), FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(Property.GetPropertyFlags())));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("data_table_column_metadata") };
	Entity.Completeness.Omitted = { TEXT("column_editor_metadata") };
	AddDataEvidence(Entity, Table.GetPathName(), TEXT("DataTable column read from row struct FProperty metadata."));
	OutEntities.Add(MoveTemp(Entity));
	return ColumnId;
}

FString AddDataTableRowEntity(
	const FString& ProjectId,
	const UDataTable& Table,
	const FName& RowName,
	int32 RowIndex,
	int32 FieldCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString RowNameString = RowName.ToString();
	const FString CanonicalKey = FString::Printf(TEXT("%s:row:%s"), *Table.GetPathName(), *RowNameString);
	const FString RowId = MakeStableId(ProjectId, TEXT("data_table_row"), CanonicalKey);
	if (FindDataEntity(OutEntities, RowId))
	{
		return RowId;
	}

	FEntityRecord Entity;
	Entity.Id = RowId;
	Entity.Kind = TEXT("data_table_row");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = RowNameString;
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("table_path"), Table.GetPathName());
	Entity.Attributes.Add(TEXT("row_name"), RowNameString);
	Entity.Attributes.Add(TEXT("row_index"), FString::FromInt(RowIndex));
	Entity.Attributes.Add(TEXT("field_count"), FString::FromInt(FieldCount));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("data_table_row_name"), TEXT("data_table_field_values") };
	Entity.Completeness.Omitted = { TEXT("runtime_lookup_context") };
	AddDataEvidence(Entity, Table.GetPathName(), TEXT("DataTable row read from row map."));
	OutEntities.Add(MoveTemp(Entity));
	return RowId;
}

TSharedRef<FJsonObject> DataTableColumnSnapshot(const FString& ColumnId, const FProperty& Property, int32 ColumnIndex)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), ColumnId);
	Object->SetNumberField(TEXT("index"), ColumnIndex);
	Object->SetStringField(TEXT("name"), Property.GetName());
	Object->SetStringField(TEXT("display_name"), Property.GetDisplayNameText().ToString());
	Object->SetStringField(TEXT("cpp_type"), Property.GetCPPType());
	Object->SetStringField(TEXT("property_class"), Property.GetClass() ? FString(Property.GetClass()->GetName()) : FString());
	Object->SetNumberField(TEXT("array_dim"), Property.ArrayDim);
	return Object;
}

TSharedRef<FJsonObject> DataTableRowSnapshot(
	const FString& RowId,
	const UDataTable& Table,
	const FName& RowName,
	const uint8* RowData,
	const TArray<FProperty*>& Properties,
	int32 RowIndex)
{
	TArray<TSharedPtr<FJsonValue>> FieldValues;
	for (const FProperty* Property : Properties)
	{
		if (!Property)
		{
			continue;
		}

		TSharedRef<FJsonObject> FieldObject = MakeShared<FJsonObject>();
		FieldObject->SetStringField(TEXT("column_name"), Property->GetName());
		FieldObject->SetStringField(TEXT("value"), ExportDataTableCellValue(*Property, RowData));
		FieldValues.Add(MakeShared<FJsonValueObject>(FieldObject));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), RowId);
	Object->SetNumberField(TEXT("index"), RowIndex);
	Object->SetStringField(TEXT("name"), RowName.ToString());
	Object->SetNumberField(TEXT("field_count"), FieldValues.Num());
	Object->SetArrayField(TEXT("fields"), FieldValues);
	return Object;
}

TArray<const UDataTable*> CompositeParentTables(const UCompositeDataTable& CompositeTable)
{
	TArray<UObject*> Dependencies;
	const_cast<UCompositeDataTable&>(CompositeTable).GetPreloadDependencies(Dependencies);
	TArray<const UDataTable*> ParentTables;
	for (UObject* Dependency : Dependencies)
	{
		const UDataTable* ParentTable = Cast<UDataTable>(Dependency);
		if (ParentTable && ParentTable != &CompositeTable)
		{
			ParentTables.AddUnique(ParentTable);
		}
	}
	ParentTables.Sort([](const UDataTable& Left, const UDataTable& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});
	return ParentTables;
}

TSharedRef<FJsonObject> DataTableSnapshot(
	const FString& ProjectId,
	const UDataTable& Table,
	const FString& TableId,
	const TArray<FProperty*>& Properties,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	TArray<TSharedPtr<FJsonValue>> ColumnValues;
	for (int32 ColumnIndex = 0; ColumnIndex < Properties.Num(); ++ColumnIndex)
	{
		const FProperty* Property = Properties[ColumnIndex];
		if (!Property)
		{
			continue;
		}

		const FString ColumnId = AddDataTableColumnEntity(ProjectId, Table, *Property, ColumnIndex, OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_data_table_column"), TableId, ColumnId, Table.GetPathName(), TEXT("DataTable row struct contains this column."), OutRelations);
		ColumnValues.Add(MakeShared<FJsonValueObject>(DataTableColumnSnapshot(ColumnId, *Property, ColumnIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> RowValues;
	const TMap<FName, uint8*>& RowMap = Table.GetRowMap();
	const TArray<FName> RowNames = DataTableRowNames(Table);
	for (int32 RowIndex = 0; RowIndex < RowNames.Num(); ++RowIndex)
	{
		const FName& RowName = RowNames[RowIndex];
		uint8* const* RowDataPtr = RowMap.Find(RowName);
		const uint8* RowData = RowDataPtr ? *RowDataPtr : nullptr;
		const FString RowId = AddDataTableRowEntity(ProjectId, Table, RowName, RowIndex, Properties.Num(), OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_data_table_row"), TableId, RowId, Table.GetPathName(), TEXT("DataTable contains this row."), OutRelations);
		RowValues.Add(MakeShared<FJsonValueObject>(DataTableRowSnapshot(RowId, Table, RowName, RowData, Properties, RowIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> ParentTableValues;
	if (const UCompositeDataTable* CompositeTable = Cast<UCompositeDataTable>(&Table))
	{
		const TArray<const UDataTable*> ParentTables = CompositeParentTables(*CompositeTable);
		for (int32 ParentIndex = 0; ParentIndex < ParentTables.Num(); ++ParentIndex)
		{
			const UDataTable* ParentTable = ParentTables[ParentIndex];
			if (!ParentTable)
			{
				continue;
			}

			const TArray<FProperty*> ParentProperties = DataTableProperties(*ParentTable);
			const FString ParentTableId = AddDataTableEntity(ProjectId, *ParentTable, ParentProperties.Num(), OutEntities);
			AddDataRelation(ProjectId, TEXT("composite_parent_table"), TableId, ParentTableId, Table.GetPathName(), TEXT("CompositeDataTable references this parent table."), OutRelations);

			TSharedRef<FJsonObject> ParentObject = MakeShared<FJsonObject>();
			ParentObject->SetStringField(TEXT("id"), ParentTableId);
			ParentObject->SetNumberField(TEXT("index"), ParentIndex);
			ParentObject->SetStringField(TEXT("table_path"), ParentTable->GetPathName());
			ParentObject->SetStringField(TEXT("row_struct_path"), DataTableRowStructPath(*ParentTable));
			ParentObject->SetNumberField(TEXT("row_count"), ParentTable->GetRowMap().Num());
			ParentTableValues.Add(MakeShared<FJsonValueObject>(ParentObject));
		}
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.data_table.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("table_path"), Table.GetPathName());
	Object->SetStringField(TEXT("table_class"), Table.GetClass()->GetPathName());
	Object->SetStringField(TEXT("row_struct_path"), DataTableRowStructPath(Table));
	Object->SetStringField(TEXT("row_struct_name"), DataTableRowStructName(Table));
	Object->SetNumberField(TEXT("row_count"), RowValues.Num());
	Object->SetNumberField(TEXT("column_count"), ColumnValues.Num());
	Object->SetBoolField(TEXT("strip_from_client_builds"), Table.bStripFromClientBuilds);
	Object->SetBoolField(TEXT("ignore_extra_fields"), Table.bIgnoreExtraFields);
	Object->SetBoolField(TEXT("ignore_missing_fields"), Table.bIgnoreMissingFields);
	Object->SetStringField(TEXT("import_key_field"), Table.ImportKeyField);
	Object->SetArrayField(TEXT("columns"), ColumnValues);
	Object->SetArrayField(TEXT("rows"), RowValues);
	Object->SetNumberField(TEXT("parent_table_count"), ParentTableValues.Num());
	Object->SetArrayField(TEXT("parent_tables"), ParentTableValues);
	return Object;
}

FString AddCurveChannelEntity(
	const FString& ProjectId,
	const UCurveBase& Curve,
	const FRealCurve& RealCurve,
	const FString& ChannelName,
	int32 ChannelIndex,
	int32 KeyCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = FString::Printf(TEXT("%s:channel:%d:%s"), *Curve.GetPathName(), ChannelIndex, *ChannelName);
	const FString ChannelId = MakeStableId(ProjectId, TEXT("curve_channel"), CanonicalKey);
	if (FindDataEntity(OutEntities, ChannelId))
	{
		return ChannelId;
	}

	const bool bHasDefaultValue = RealCurve.GetDefaultValue() != MAX_flt;

	FEntityRecord Entity;
	Entity.Id = ChannelId;
	Entity.Kind = TEXT("curve_channel");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = ChannelName;
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("curve_path"), Curve.GetPathName());
	Entity.Attributes.Add(TEXT("channel_name"), ChannelName);
	Entity.Attributes.Add(TEXT("channel_index"), FString::FromInt(ChannelIndex));
	Entity.Attributes.Add(TEXT("key_count"), FString::FromInt(KeyCount));
	Entity.Attributes.Add(TEXT("has_default_value"), DataBool(bHasDefaultValue));
	Entity.Attributes.Add(TEXT("default_value"), bHasDefaultValue ? FString::SanitizeFloat(RealCurve.GetDefaultValue()) : FString());
	Entity.Attributes.Add(TEXT("pre_infinity_extrap"), CurveEnumString(static_cast<ERichCurveExtrapolation>(RealCurve.PreInfinityExtrap.GetValue())));
	Entity.Attributes.Add(TEXT("post_infinity_extrap"), CurveEnumString(static_cast<ERichCurveExtrapolation>(RealCurve.PostInfinityExtrap.GetValue())));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("curve_channel_metadata"), TEXT("curve_keys") };
	Entity.Completeness.Omitted = { TEXT("runtime_curve_evaluation") };
	AddDataEvidence(Entity, Curve.GetPathName(), TEXT("Curve channel read from UCurveBase rich curve data."));
	OutEntities.Add(MoveTemp(Entity));
	return ChannelId;
}

FString AddCurveKeyEntity(
	const FString& ProjectId,
	const UCurveBase& Curve,
	const FString& ChannelName,
	int32 ChannelIndex,
	int32 KeyIndex,
	const FRichCurveKey& Key,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = FString::Printf(TEXT("%s:channel:%d:key:%d"), *Curve.GetPathName(), ChannelIndex, KeyIndex);
	const FString KeyId = MakeStableId(ProjectId, TEXT("curve_key"), CanonicalKey);
	if (FindDataEntity(OutEntities, KeyId))
	{
		return KeyId;
	}

	FEntityRecord Entity;
	Entity.Id = KeyId;
	Entity.Kind = TEXT("curve_key");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = FString::Printf(TEXT("%s[%d]"), *ChannelName, KeyIndex);
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("curve_path"), Curve.GetPathName());
	Entity.Attributes.Add(TEXT("channel_name"), ChannelName);
	Entity.Attributes.Add(TEXT("channel_index"), FString::FromInt(ChannelIndex));
	Entity.Attributes.Add(TEXT("key_index"), FString::FromInt(KeyIndex));
	Entity.Attributes.Add(TEXT("time"), FString::SanitizeFloat(Key.Time));
	Entity.Attributes.Add(TEXT("value"), FString::SanitizeFloat(Key.Value));
	Entity.Attributes.Add(TEXT("interp_mode"), CurveEnumString(static_cast<ERichCurveInterpMode>(Key.InterpMode.GetValue())));
	Entity.Attributes.Add(TEXT("tangent_mode"), CurveEnumString(static_cast<ERichCurveTangentMode>(Key.TangentMode.GetValue())));
	Entity.Attributes.Add(TEXT("tangent_weight_mode"), CurveEnumString(static_cast<ERichCurveTangentWeightMode>(Key.TangentWeightMode.GetValue())));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("curve_key_time_value"), TEXT("curve_key_interpolation"), TEXT("curve_key_tangents") };
	Entity.Completeness.Omitted = { TEXT("runtime_curve_evaluation") };
	AddDataEvidence(Entity, Curve.GetPathName(), TEXT("Curve key read from FRichCurve key data."));
	OutEntities.Add(MoveTemp(Entity));
	return KeyId;
}

TSharedRef<FJsonObject> CurveKeySnapshot(const FString& KeyId, int32 KeyIndex, const FRichCurveKey& Key)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), KeyId);
	Object->SetNumberField(TEXT("index"), KeyIndex);
	Object->SetNumberField(TEXT("time"), Key.Time);
	Object->SetNumberField(TEXT("value"), Key.Value);
	Object->SetStringField(TEXT("interp_mode"), CurveEnumString(static_cast<ERichCurveInterpMode>(Key.InterpMode.GetValue())));
	Object->SetStringField(TEXT("tangent_mode"), CurveEnumString(static_cast<ERichCurveTangentMode>(Key.TangentMode.GetValue())));
	Object->SetStringField(TEXT("tangent_weight_mode"), CurveEnumString(static_cast<ERichCurveTangentWeightMode>(Key.TangentWeightMode.GetValue())));
	Object->SetNumberField(TEXT("arrive_tangent"), Key.ArriveTangent);
	Object->SetNumberField(TEXT("arrive_tangent_weight"), Key.ArriveTangentWeight);
	Object->SetNumberField(TEXT("leave_tangent"), Key.LeaveTangent);
	Object->SetNumberField(TEXT("leave_tangent_weight"), Key.LeaveTangentWeight);
	return Object;
}

TSharedRef<FJsonObject> CurveSnapshot(
	const FString& ProjectId,
	const UCurveBase& Curve,
	const FString& CurveId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const TArray<FRichCurveEditInfoConst> CurveInfos = Curve.GetCurves();

	TArray<TSharedPtr<FJsonValue>> ChannelValues;
	int32 TotalKeyCount = 0;
	for (int32 ChannelIndex = 0; ChannelIndex < CurveInfos.Num(); ++ChannelIndex)
	{
		const FRichCurveEditInfoConst& CurveInfo = CurveInfos[ChannelIndex];
		const FRichCurve* RichCurve = static_cast<const FRichCurve*>(CurveInfo.CurveToEdit);
		if (!RichCurve)
		{
			continue;
		}

		const FString ChannelName = ChannelNameFromEditInfo(CurveInfo, ChannelIndex);
		const TArray<FRichCurveKey>& Keys = RichCurve->GetConstRefOfKeys();
		const FString ChannelId = AddCurveChannelEntity(ProjectId, Curve, *RichCurve, ChannelName, ChannelIndex, Keys.Num(), OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_curve_channel"), CurveId, ChannelId, Curve.GetPathName(), TEXT("Curve contains this channel."), OutRelations);

		float MinTime = 0.0f;
		float MaxTime = 0.0f;
		float MinValue = 0.0f;
		float MaxValue = 0.0f;
		if (RichCurve->HasAnyData())
		{
			RichCurve->GetTimeRange(MinTime, MaxTime);
			RichCurve->GetValueRange(MinValue, MaxValue);
		}

		TArray<TSharedPtr<FJsonValue>> KeyValues;
		for (int32 KeyIndex = 0; KeyIndex < Keys.Num(); ++KeyIndex)
		{
			const FRichCurveKey& Key = Keys[KeyIndex];
			const FString KeyId = AddCurveKeyEntity(ProjectId, Curve, ChannelName, ChannelIndex, KeyIndex, Key, OutEntities);
			AddDataRelation(ProjectId, TEXT("contains_curve_key"), ChannelId, KeyId, Curve.GetPathName(), TEXT("Curve channel contains this key."), OutRelations);
			KeyValues.Add(MakeShared<FJsonValueObject>(CurveKeySnapshot(KeyId, KeyIndex, Key)));
		}

		const bool bHasDefaultValue = RichCurve->GetDefaultValue() != MAX_flt;

		TSharedRef<FJsonObject> ChannelObject = MakeShared<FJsonObject>();
		ChannelObject->SetStringField(TEXT("id"), ChannelId);
		ChannelObject->SetNumberField(TEXT("index"), ChannelIndex);
		ChannelObject->SetStringField(TEXT("name"), ChannelName);
		ChannelObject->SetNumberField(TEXT("key_count"), Keys.Num());
		ChannelObject->SetBoolField(TEXT("has_default_value"), bHasDefaultValue);
		ChannelObject->SetNumberField(TEXT("default_value"), bHasDefaultValue ? RichCurve->GetDefaultValue() : 0.0f);
		ChannelObject->SetStringField(TEXT("pre_infinity_extrap"), CurveEnumString(static_cast<ERichCurveExtrapolation>(RichCurve->PreInfinityExtrap.GetValue())));
		ChannelObject->SetStringField(TEXT("post_infinity_extrap"), CurveEnumString(static_cast<ERichCurveExtrapolation>(RichCurve->PostInfinityExtrap.GetValue())));
		ChannelObject->SetNumberField(TEXT("min_time"), MinTime);
		ChannelObject->SetNumberField(TEXT("max_time"), MaxTime);
		ChannelObject->SetNumberField(TEXT("min_value"), MinValue);
		ChannelObject->SetNumberField(TEXT("max_value"), MaxValue);
		ChannelObject->SetArrayField(TEXT("keys"), KeyValues);
		ChannelValues.Add(MakeShared<FJsonValueObject>(ChannelObject));
		TotalKeyCount += Keys.Num();
	}

	float MinTime = 0.0f;
	float MaxTime = 0.0f;
	float MinValue = 0.0f;
	float MaxValue = 0.0f;
	if (TotalKeyCount > 0)
	{
		Curve.GetTimeRange(MinTime, MaxTime);
		Curve.GetValueRange(MinValue, MaxValue);
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.curve.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("curve_path"), Curve.GetPathName());
	Object->SetStringField(TEXT("curve_class"), Curve.GetClass()->GetPathName());
	Object->SetStringField(TEXT("curve_kind"), CurveKindString(Curve));
	Object->SetNumberField(TEXT("channel_count"), ChannelValues.Num());
	Object->SetNumberField(TEXT("key_count"), TotalKeyCount);
	Object->SetNumberField(TEXT("min_time"), MinTime);
	Object->SetNumberField(TEXT("max_time"), MaxTime);
	Object->SetNumberField(TEXT("min_value"), MinValue);
	Object->SetNumberField(TEXT("max_value"), MaxValue);
	Object->SetArrayField(TEXT("channels"), ChannelValues);
	return Object;
}

TArray<FName> CurveTableRowNames(const UCurveTable& Table)
{
	TArray<FName> RowNames;
	Table.GetRowMap().GenerateKeyArray(RowNames);
	RowNames.Sort([](const FName& Left, const FName& Right)
	{
		return Left.ToString() < Right.ToString();
	});
	return RowNames;
}

FString AddCurveTableEntity(
	const FString& ProjectId,
	const UCurveTable& Table,
	int32 RowCount,
	int32 KeyCount,
	TArray<FEntityRecord>& OutEntities)
{
	const FString TablePath = Table.GetPathName();
	const FString TableId = MakeStableId(ProjectId, TEXT("curve_table"), TablePath);
	if (FindDataEntity(OutEntities, TableId))
	{
		return TableId;
	}

	FEntityRecord Entity;
	Entity.Id = TableId;
	Entity.Kind = TEXT("curve_table");
	Entity.CanonicalKey = TablePath;
	Entity.DisplayName = Table.GetName();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("curve_table_path"), TablePath);
	Entity.Attributes.Add(TEXT("curve_table_class"), Table.GetClass()->GetPathName());
	Entity.Attributes.Add(TEXT("row_count"), FString::FromInt(RowCount));
	Entity.Attributes.Add(TEXT("key_count"), FString::FromInt(KeyCount));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("curve_table_rows"), TEXT("curve_table_key_counts") };
	Entity.Completeness.Omitted = { TEXT("runtime_curve_evaluation") };
	AddDataEvidence(Entity, TablePath, TEXT("CurveTable rows read through public Engine API."));
	OutEntities.Add(MoveTemp(Entity));
	return TableId;
}

FString AddCurveTableRowEntity(
	const FString& ProjectId,
	const UCurveTable& Table,
	const FName& RowName,
	int32 RowIndex,
	const FRealCurve& Curve,
	TArray<FEntityRecord>& OutEntities)
{
	const FString CanonicalKey = FString::Printf(TEXT("%s:row:%d:%s"), *Table.GetPathName(), RowIndex, *RowName.ToString());
	const FString RowId = MakeStableId(ProjectId, TEXT("curve_table_row"), CanonicalKey);
	if (FindDataEntity(OutEntities, RowId))
	{
		return RowId;
	}

	float MinTime = 0.0f;
	float MaxTime = 0.0f;
	float MinValue = 0.0f;
	float MaxValue = 0.0f;
	if (Curve.HasAnyData())
	{
		Curve.GetTimeRange(MinTime, MaxTime);
		Curve.GetValueRange(MinValue, MaxValue);
	}

	FEntityRecord Entity;
	Entity.Id = RowId;
	Entity.Kind = TEXT("curve_table_row");
	Entity.CanonicalKey = CanonicalKey;
	Entity.DisplayName = RowName.ToString();
	Entity.SourceLayer = LexToString(ESourceLayer::UObjectReflection);
	Entity.Attributes.Add(TEXT("curve_table_path"), Table.GetPathName());
	Entity.Attributes.Add(TEXT("row_name"), RowName.ToString());
	Entity.Attributes.Add(TEXT("row_index"), FString::FromInt(RowIndex));
	Entity.Attributes.Add(TEXT("key_count"), FString::FromInt(Curve.GetNumKeys()));
	Entity.Attributes.Add(TEXT("min_time"), FString::SanitizeFloat(MinTime));
	Entity.Attributes.Add(TEXT("max_time"), FString::SanitizeFloat(MaxTime));
	Entity.Attributes.Add(TEXT("min_value"), FString::SanitizeFloat(MinValue));
	Entity.Attributes.Add(TEXT("max_value"), FString::SanitizeFloat(MaxValue));
	Entity.Attributes.Add(TEXT("collection_level"), LexToString(ECollectionLevel::Structural));
	Entity.Completeness.State = ECompletenessState::Partial;
	Entity.Completeness.Covered = { TEXT("curve_table_row_metadata"), TEXT("curve_table_key_count") };
	Entity.Completeness.Omitted = { TEXT("curve_table_key_artifact"), TEXT("runtime_curve_evaluation") };
	AddDataEvidence(Entity, Table.GetPathName(), TEXT("CurveTable row metadata read from FRealCurve."));
	OutEntities.Add(MoveTemp(Entity));
	return RowId;
}

TSharedRef<FJsonObject> CurveTableRowSnapshot(const FString& RowId, const FName& RowName, int32 RowIndex, const FRealCurve& Curve)
{
	float MinTime = 0.0f;
	float MaxTime = 0.0f;
	float MinValue = 0.0f;
	float MaxValue = 0.0f;
	if (Curve.HasAnyData())
	{
		Curve.GetTimeRange(MinTime, MaxTime);
		Curve.GetValueRange(MinValue, MaxValue);
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), RowId);
	Object->SetNumberField(TEXT("index"), RowIndex);
	Object->SetStringField(TEXT("name"), RowName.ToString());
	Object->SetNumberField(TEXT("key_count"), Curve.GetNumKeys());
	Object->SetNumberField(TEXT("min_time"), MinTime);
	Object->SetNumberField(TEXT("max_time"), MaxTime);
	Object->SetNumberField(TEXT("min_value"), MinValue);
	Object->SetNumberField(TEXT("max_value"), MaxValue);
	return Object;
}

TSharedRef<FJsonObject> CurveTableSnapshot(
	const FString& ProjectId,
	const UCurveTable& Table,
	const FString& TableId,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	const TMap<FName, FRealCurve*>& RowMap = Table.GetRowMap();
	TArray<TSharedPtr<FJsonValue>> RowValues;
	const TArray<FName> RowNames = CurveTableRowNames(Table);
	for (int32 RowIndex = 0; RowIndex < RowNames.Num(); ++RowIndex)
	{
		const FName& RowName = RowNames[RowIndex];
		FRealCurve* const* CurvePtr = RowMap.Find(RowName);
		if (!CurvePtr || !*CurvePtr)
		{
			continue;
		}
		const FString RowId = AddCurveTableRowEntity(ProjectId, Table, RowName, RowIndex, **CurvePtr, OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_curve_table_row"), TableId, RowId, Table.GetPathName(), TEXT("CurveTable contains this row."), OutRelations);
		RowValues.Add(MakeShared<FJsonValueObject>(CurveTableRowSnapshot(RowId, RowName, RowIndex, **CurvePtr)));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), TEXT("uepi.curve_table.v1"));
	Object->SetStringField(TEXT("source_layer"), LexToString(ESourceLayer::UObjectReflection));
	Object->SetStringField(TEXT("curve_table_path"), Table.GetPathName());
	Object->SetStringField(TEXT("curve_table_class"), Table.GetClass()->GetPathName());
	Object->SetNumberField(TEXT("row_count"), RowValues.Num());
	Object->SetArrayField(TEXT("rows"), RowValues);
	return Object;
}
}

bool FDataReader::AppendDataAsset(
	UObject& Asset,
	const FString& ProjectId,
	FEntityRecord& AssetEntity,
	TArray<FEntityRecord>& OutEntities,
	TArray<FRelationRecord>& OutRelations)
{
	if (UUserDefinedStruct* UserDefinedStruct = Cast<UUserDefinedStruct>(&Asset))
	{
		const TArray<FProperty*> Properties = UserDefinedStructProperties(*UserDefinedStruct);
		const FString StructId = AddUserDefinedStructEntity(ProjectId, *UserDefinedStruct, Properties.Num(), OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_user_defined_struct"), AssetEntity.Id, StructId, UserDefinedStruct->GetPathName(), TEXT("Asset contains the extracted UserDefinedStruct record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("user_defined_struct"), UserDefinedStructSnapshot(ProjectId, *UserDefinedStruct, StructId, Properties, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("user_defined_struct_guid"), DataGuidString(UserDefinedStruct->Guid));
		AssetEntity.Attributes.Add(TEXT("user_defined_struct_field_count"), FString::FromInt(Properties.Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("user_defined_struct_fields"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("user_defined_struct_field_types"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("user_defined_struct_default_values"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			UserDefinedStruct->GetPathName(),
			TEXT("UserDefinedStruct structure extracted through public Engine reflection API.")
		});
		return true;
	}

	if (UUserDefinedEnum* UserDefinedEnum = Cast<UUserDefinedEnum>(&Asset))
	{
		const FString EnumId = AddUserDefinedEnumEntity(ProjectId, *UserDefinedEnum, UserDefinedEnum->NumEnums(), OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_user_defined_enum"), AssetEntity.Id, EnumId, UserDefinedEnum->GetPathName(), TEXT("Asset contains the extracted UserDefinedEnum record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("user_defined_enum"), UserDefinedEnumSnapshot(ProjectId, *UserDefinedEnum, EnumId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("user_defined_enum_entry_count"), FString::FromInt(UserDefinedEnum->NumEnums()));
		AssetEntity.Attributes.Add(TEXT("user_defined_enum_cpp_form"), EnumCppFormString(UserDefinedEnum->GetCppForm()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("user_defined_enum_entries"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("user_defined_enum_display_names"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("user_defined_enum_editor_history"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			UserDefinedEnum->GetPathName(),
			TEXT("UserDefinedEnum entries extracted through public Engine UEnum API.")
		});
		return true;
	}

	if (UDataAsset* DataAsset = Cast<UDataAsset>(&Asset))
	{
		const FPrimaryAssetId PrimaryAssetId = DataAsset->GetPrimaryAssetId();
		const FAssetBundleData BundleData = DataAssetBundleData(*DataAsset, PrimaryAssetId);
		const FString DataAssetId = AddDataAssetEntity(ProjectId, *DataAsset, PrimaryAssetId, BundleData, OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_data_asset"), AssetEntity.Id, DataAssetId, DataAsset->GetPathName(), TEXT("Asset contains the extracted DataAsset record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("data_asset"), DataAssetSnapshot(*DataAsset, DataAssetId, PrimaryAssetId, BundleData));
		AssetEntity.Attributes.Add(TEXT("data_asset_class"), DataAsset->GetClass() ? DataAsset->GetClass()->GetPathName() : FString());
		AssetEntity.Attributes.Add(TEXT("is_primary_data_asset"), DataBool(DataAsset->IsA<UPrimaryDataAsset>()));
		AssetEntity.Attributes.Add(TEXT("primary_asset_id"), PrimaryAssetId.IsValid() ? PrimaryAssetId.ToString() : FString());
		AssetEntity.Attributes.Add(TEXT("primary_asset_bundle_count"), FString::FromInt(BundleData.Bundles.Num()));
		AssetEntity.Attributes.Add(TEXT("primary_asset_bundled_asset_count"), FString::FromInt(BundledAssetCount(BundleData)));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("data_asset_metadata"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("primary_asset_id"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("asset_manager_rules"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("asset_bundle_members"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_bundle_state"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			DataAsset->GetPathName(),
			TEXT("DataAsset metadata extracted through public Engine API.")
		});
		return true;
	}

	if (UStringTable* StringTable = Cast<UStringTable>(&Asset))
	{
		const TArray<FStringTableEntryRecord> Entries = StringTableEntries(*StringTable);
		const FString TableId = AddStringTableEntity(ProjectId, *StringTable, Entries.Num(), StringTableMetadataCount(Entries), OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_string_table"), AssetEntity.Id, TableId, StringTable->GetPathName(), TEXT("Asset contains the extracted StringTable record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("string_table"), StringTableSnapshot(ProjectId, *StringTable, TableId, Entries, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("string_table_id"), StringTable->GetStringTableId().ToString());
		AssetEntity.Attributes.Add(TEXT("string_table_entry_count"), FString::FromInt(Entries.Num()));
		AssetEntity.Attributes.Add(TEXT("string_table_metadata_count"), FString::FromInt(StringTableMetadataCount(Entries)));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("string_table_entries"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("string_table_metadata"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("localized_culture_overrides"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			StringTable->GetPathName(),
			TEXT("StringTable entries extracted through public Engine API.")
		});
		return true;
	}

	if (UDataTable* DataTable = Cast<UDataTable>(&Asset))
	{
		const TArray<FProperty*> Properties = DataTableProperties(*DataTable);
		const FString TableId = AddDataTableEntity(ProjectId, *DataTable, Properties.Num(), OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_data_table"), AssetEntity.Id, TableId, DataTable->GetPathName(), TEXT("Asset contains the extracted DataTable record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("data_table"), DataTableSnapshot(ProjectId, *DataTable, TableId, Properties, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("data_table_row_struct"), DataTableRowStructPath(*DataTable));
		AssetEntity.Attributes.Add(TEXT("data_table_row_count"), FString::FromInt(DataTable->GetRowMap().Num()));
		AssetEntity.Attributes.Add(TEXT("data_table_column_count"), FString::FromInt(Properties.Num()));
		if (const UCompositeDataTable* CompositeTable = Cast<UCompositeDataTable>(DataTable))
		{
			AssetEntity.Attributes.Add(TEXT("composite_parent_table_count"), FString::FromInt(CompositeParentTables(*CompositeTable).Num()));
			AssetEntity.Completeness.Covered.AddUnique(TEXT("composite_parent_tables"));
		}
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("data_table_row_struct"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("data_table_columns"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("data_table_rows"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_lookup_context"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			DataTable->GetPathName(),
			TEXT("DataTable structure extracted through public Engine API.")
		});
		return true;
	}

	if (UCurveLinearColorAtlas* Atlas = Cast<UCurveLinearColorAtlas>(&Asset))
	{
		const FString AtlasId = AddCurveAtlasEntity(ProjectId, *Atlas, OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_curve_atlas"), AssetEntity.Id, AtlasId, Atlas->GetPathName(), TEXT("Asset contains the extracted CurveLinearColorAtlas record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("curve_linear_color_atlas"), CurveAtlasSnapshot(ProjectId, *Atlas, AtlasId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("curve_atlas_texture_size"), FString::Printf(TEXT("%u"), Atlas->TextureSize));
		AssetEntity.Attributes.Add(TEXT("curve_atlas_texture_height"), FString::Printf(TEXT("%u"), Atlas->TextureHeight));
		AssetEntity.Attributes.Add(TEXT("curve_atlas_gradient_curve_count"), FString::FromInt(Atlas->GradientCurves.Num()));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("curve_atlas_dimensions"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("curve_atlas_gradient_curves"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("curve_atlas_pixel_data"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_material_bindings"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			Atlas->GetPathName(),
			TEXT("CurveLinearColorAtlas structure extracted through public Engine API.")
		});
		return true;
	}

	if (UCurveTable* CurveTable = Cast<UCurveTable>(&Asset))
	{
		int32 KeyCount = 0;
		for (const TPair<FName, FRealCurve*>& RowPair : CurveTable->GetRowMap())
		{
			if (RowPair.Value)
			{
				KeyCount += RowPair.Value->GetNumKeys();
			}
		}
		const FString TableId = AddCurveTableEntity(ProjectId, *CurveTable, CurveTable->GetRowMap().Num(), KeyCount, OutEntities);
		AddDataRelation(ProjectId, TEXT("contains_curve_table"), AssetEntity.Id, TableId, CurveTable->GetPathName(), TEXT("Asset contains the extracted CurveTable record."), OutRelations);

		if (!AssetEntity.Snapshot.IsValid())
		{
			AssetEntity.Snapshot = MakeShared<FJsonObject>();
		}
		AssetEntity.Snapshot->SetObjectField(TEXT("curve_table"), CurveTableSnapshot(ProjectId, *CurveTable, TableId, OutEntities, OutRelations));
		AssetEntity.Attributes.Add(TEXT("curve_table_row_count"), FString::FromInt(CurveTable->GetRowMap().Num()));
		AssetEntity.Attributes.Add(TEXT("curve_table_key_count"), FString::FromInt(KeyCount));
		AssetEntity.Completeness.State = ECompletenessState::Partial;
		AssetEntity.Completeness.Covered.AddUnique(TEXT("curve_table_rows"));
		AssetEntity.Completeness.Covered.AddUnique(TEXT("curve_table_key_counts"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("curve_table_key_artifact"));
		AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_curve_evaluation"));
		AssetEntity.Evidence.Add({
			LexToString(ESourceLayer::UObjectReflection),
			CurveTable->GetPathName(),
			TEXT("CurveTable structure extracted through public Engine API.")
		});
		return true;
	}

	UCurveBase* Curve = Cast<UCurveBase>(&Asset);
	if (!Curve || !IsSupportedCurveAsset(Asset))
	{
		return false;
	}

	const int32 ChannelCount = Curve->GetCurves().Num();
	const int32 KeyCount = CountCurveKeys(*Curve);
	const FString CurveId = AddCurveEntity(ProjectId, *Curve, ChannelCount, KeyCount, OutEntities);
	AddDataRelation(ProjectId, TEXT("contains_curve"), AssetEntity.Id, CurveId, Curve->GetPathName(), TEXT("Asset contains the extracted curve record."), OutRelations);

	if (!AssetEntity.Snapshot.IsValid())
	{
		AssetEntity.Snapshot = MakeShared<FJsonObject>();
	}
	AssetEntity.Snapshot->SetObjectField(TEXT("curve"), CurveSnapshot(ProjectId, *Curve, CurveId, OutEntities, OutRelations));
	AssetEntity.Attributes.Add(TEXT("curve_kind"), CurveKindString(*Curve));
	AssetEntity.Attributes.Add(TEXT("curve_channel_count"), FString::FromInt(ChannelCount));
	AssetEntity.Attributes.Add(TEXT("curve_key_count"), FString::FromInt(KeyCount));
	AssetEntity.Completeness.State = ECompletenessState::Partial;
	AssetEntity.Completeness.Covered.AddUnique(TEXT("curve_channels"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("curve_keys"));
	AssetEntity.Completeness.Covered.AddUnique(TEXT("curve_tangents"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("runtime_curve_evaluation"));
	AssetEntity.Completeness.Omitted.AddUnique(TEXT("compressed_curve_data"));
	AssetEntity.Evidence.Add({
		LexToString(ESourceLayer::UObjectReflection),
		Curve->GetPathName(),
		TEXT("Curve structure extracted through public Engine API.")
	});
	return true;
}
}
