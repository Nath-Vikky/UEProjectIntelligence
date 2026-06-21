#include "UEPITypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::ProjectIntelligence
{
namespace
{
uint32 RotateRight(uint32 Value, uint32 Bits)
{
	return (Value >> Bits) | (Value << (32 - Bits));
}

uint32 Sha256Choice(uint32 X, uint32 Y, uint32 Z)
{
	return (X & Y) ^ (~X & Z);
}

uint32 Sha256Majority(uint32 X, uint32 Y, uint32 Z)
{
	return (X & Y) ^ (X & Z) ^ (Y & Z);
}

uint32 Sha256BigSigma0(uint32 X)
{
	return RotateRight(X, 2) ^ RotateRight(X, 13) ^ RotateRight(X, 22);
}

uint32 Sha256BigSigma1(uint32 X)
{
	return RotateRight(X, 6) ^ RotateRight(X, 11) ^ RotateRight(X, 25);
}

uint32 Sha256SmallSigma0(uint32 X)
{
	return RotateRight(X, 7) ^ RotateRight(X, 18) ^ (X >> 3);
}

uint32 Sha256SmallSigma1(uint32 X)
{
	return RotateRight(X, 17) ^ RotateRight(X, 19) ^ (X >> 10);
}

FString Sha256Hex(const uint8* Data, int32 Size)
{
	static constexpr uint32 Constants[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
		0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
		0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
		0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
		0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
		0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
		0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
		0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
		0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
	};

	uint32 Hash[8] = {
		0x6a09e667,
		0xbb67ae85,
		0x3c6ef372,
		0xa54ff53a,
		0x510e527f,
		0x9b05688c,
		0x1f83d9ab,
		0x5be0cd19
	};

	TArray<uint8> Message;
	Message.Reserve(Size + 72);
	Message.Append(Data, Size);
	Message.Add(0x80);

	while ((Message.Num() % 64) != 56)
	{
		Message.Add(0);
	}

	const uint64 BitLength = static_cast<uint64>(Size) * 8;
	for (int32 Shift = 56; Shift >= 0; Shift -= 8)
	{
		Message.Add(static_cast<uint8>((BitLength >> Shift) & 0xff));
	}

	for (int32 ChunkOffset = 0; ChunkOffset < Message.Num(); ChunkOffset += 64)
	{
		uint32 Words[64];
		for (int32 Index = 0; Index < 16; ++Index)
		{
			const int32 ByteOffset = ChunkOffset + Index * 4;
			Words[Index] =
				(static_cast<uint32>(Message[ByteOffset]) << 24) |
				(static_cast<uint32>(Message[ByteOffset + 1]) << 16) |
				(static_cast<uint32>(Message[ByteOffset + 2]) << 8) |
				static_cast<uint32>(Message[ByteOffset + 3]);
		}

		for (int32 Index = 16; Index < 64; ++Index)
		{
			Words[Index] =
				Sha256SmallSigma1(Words[Index - 2]) +
				Words[Index - 7] +
				Sha256SmallSigma0(Words[Index - 15]) +
				Words[Index - 16];
		}

		uint32 A = Hash[0];
		uint32 B = Hash[1];
		uint32 C = Hash[2];
		uint32 D = Hash[3];
		uint32 E = Hash[4];
		uint32 F = Hash[5];
		uint32 G = Hash[6];
		uint32 H = Hash[7];

		for (int32 Index = 0; Index < 64; ++Index)
		{
			const uint32 T1 = H + Sha256BigSigma1(E) + Sha256Choice(E, F, G) + Constants[Index] + Words[Index];
			const uint32 T2 = Sha256BigSigma0(A) + Sha256Majority(A, B, C);
			H = G;
			G = F;
			F = E;
			E = D + T1;
			D = C;
			C = B;
			B = A;
			A = T1 + T2;
		}

		Hash[0] += A;
		Hash[1] += B;
		Hash[2] += C;
		Hash[3] += D;
		Hash[4] += E;
		Hash[5] += F;
		Hash[6] += G;
		Hash[7] += H;
	}

	static constexpr TCHAR HexDigits[] = TEXT("0123456789abcdef");
	FString Hex;
	Hex.Reserve(64);

	for (uint32 Word : Hash)
	{
		for (int32 Shift = 28; Shift >= 0; Shift -= 4)
		{
			Hex.AppendChar(HexDigits[(Word >> Shift) & 0xf]);
		}
	}

	return Hex;
}

TArray<TSharedPtr<FJsonValue>> StringArrayToJsonValues(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());

	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}

	return Result;
}

TSharedRef<FJsonObject> StringMapToJsonObject(const TMap<FString, FString>& Values)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	TArray<FString> Keys;
	Values.GetKeys(Keys);
	Keys.Sort();

	for (const FString& Key : Keys)
	{
		Object->SetStringField(Key, Values[Key]);
	}

	return Object;
}
}

TSharedRef<FJsonObject> FEvidence::ToJson() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("source_layer"), SourceLayer);
	Object->SetStringField(TEXT("path"), Path);
	Object->SetStringField(TEXT("detail"), Detail);
	return Object;
}

TSharedRef<FJsonObject> FDiagnostic::ToJson() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("code"), Code);
	Object->SetStringField(TEXT("severity"), Severity);
	Object->SetStringField(TEXT("message"), Message);
	Object->SetObjectField(TEXT("context"), StringMapToJsonObject(Context));
	return Object;
}

TSharedRef<FJsonObject> FCompleteness::ToJson() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("state"), LexToString(State));
	Object->SetArrayField(TEXT("covered"), StringArrayToJsonValues(Covered));
	Object->SetArrayField(TEXT("omitted"), StringArrayToJsonValues(Omitted));
	Object->SetArrayField(TEXT("warnings"), StringArrayToJsonValues(Warnings));
	return Object;
}

TSharedRef<FJsonObject> FEntityRecord::ToJson() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), Id);
	Object->SetStringField(TEXT("kind"), Kind);
	Object->SetStringField(TEXT("canonical_key"), CanonicalKey);
	Object->SetStringField(TEXT("display_name"), DisplayName);
	Object->SetStringField(TEXT("source_layer"), SourceLayer);
	Object->SetObjectField(TEXT("attributes"), StringMapToJsonObject(Attributes));
	Object->SetObjectField(TEXT("completeness"), Completeness.ToJson());

	TArray<TSharedPtr<FJsonValue>> DiagnosticValues;
	DiagnosticValues.Reserve(Diagnostics.Num());
	for (const FDiagnostic& Diagnostic : Diagnostics)
	{
		DiagnosticValues.Add(MakeShared<FJsonValueObject>(Diagnostic.ToJson()));
	}
	Object->SetArrayField(TEXT("diagnostics"), DiagnosticValues);

	TArray<TSharedPtr<FJsonValue>> EvidenceValues;
	EvidenceValues.Reserve(Evidence.Num());
	for (const FEvidence& EvidenceItem : Evidence)
	{
		EvidenceValues.Add(MakeShared<FJsonValueObject>(EvidenceItem.ToJson()));
	}
	Object->SetArrayField(TEXT("evidence"), EvidenceValues);

	if (Snapshot.IsValid())
	{
		Object->SetObjectField(TEXT("snapshot"), Snapshot.ToSharedRef());
	}

	return Object;
}

TSharedRef<FJsonObject> FRelationRecord::ToJson() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), Id);
	Object->SetStringField(TEXT("type"), Type);
	Object->SetStringField(TEXT("from_id"), FromId);
	Object->SetStringField(TEXT("to_id"), ToId);
	Object->SetStringField(TEXT("source_layer"), SourceLayer);
	Object->SetBoolField(TEXT("derived"), bDerived);
	Object->SetNumberField(TEXT("confidence"), Confidence);
	Object->SetObjectField(TEXT("attributes"), StringMapToJsonObject(Attributes));

	TArray<TSharedPtr<FJsonValue>> EvidenceValues;
	EvidenceValues.Reserve(Evidence.Num());
	for (const FEvidence& EvidenceItem : Evidence)
	{
		EvidenceValues.Add(MakeShared<FJsonValueObject>(EvidenceItem.ToJson()));
	}
	Object->SetArrayField(TEXT("evidence"), EvidenceValues);

	return Object;
}

TSharedRef<FJsonObject> FProjectScanResult::ToJson() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), SchemaVersion);
	Object->SetStringField(TEXT("project_id"), ProjectId);
	Object->SetStringField(TEXT("project_name"), ProjectName);
	Object->SetStringField(TEXT("project_file"), ProjectFile);
	Object->SetStringField(TEXT("engine_version"), EngineVersion);
	Object->SetStringField(TEXT("started_at_utc"), StartedAtUtc);
	Object->SetStringField(TEXT("finished_at_utc"), FinishedAtUtc);
	Object->SetObjectField(TEXT("completeness"), Completeness.ToJson());

	TArray<TSharedPtr<FJsonValue>> EntityValues;
	EntityValues.Reserve(Entities.Num());
	for (const FEntityRecord& Entity : Entities)
	{
		EntityValues.Add(MakeShared<FJsonValueObject>(Entity.ToJson()));
	}
	Object->SetArrayField(TEXT("entities"), EntityValues);

	TArray<TSharedPtr<FJsonValue>> RelationValues;
	RelationValues.Reserve(Relations.Num());
	for (const FRelationRecord& Relation : Relations)
	{
		RelationValues.Add(MakeShared<FJsonValueObject>(Relation.ToJson()));
	}
	Object->SetArrayField(TEXT("relations"), RelationValues);

	TArray<TSharedPtr<FJsonValue>> DiagnosticValues;
	DiagnosticValues.Reserve(Diagnostics.Num());
	for (const FDiagnostic& Diagnostic : Diagnostics)
	{
		DiagnosticValues.Add(MakeShared<FJsonValueObject>(Diagnostic.ToJson()));
	}
	Object->SetArrayField(TEXT("diagnostics"), DiagnosticValues);

	return Object;
}

FString LexToString(ECollectionLevel Level)
{
	switch (Level)
	{
	case ECollectionLevel::Registry:
		return TEXT("L0");
	case ECollectionLevel::Reflection:
		return TEXT("L1");
	case ECollectionLevel::Structural:
		return TEXT("L2");
	case ECollectionLevel::Semantic:
		return TEXT("L3");
	case ECollectionLevel::Evaluated:
		return TEXT("L4");
	default:
		return TEXT("unknown");
	}
}

FString LexToString(ECompletenessState State)
{
	switch (State)
	{
	case ECompletenessState::Complete:
		return TEXT("complete");
	case ECompletenessState::Partial:
		return TEXT("partial");
	case ECompletenessState::MetadataOnly:
		return TEXT("metadata_only");
	case ECompletenessState::Failed:
		return TEXT("failed");
	case ECompletenessState::Unsupported:
		return TEXT("unsupported");
	case ECompletenessState::Stale:
		return TEXT("stale");
	case ECompletenessState::RuntimeContextRequired:
		return TEXT("runtime_context_required");
	default:
		return TEXT("unknown");
	}
}

FString LexToString(ESourceLayer SourceLayer)
{
	switch (SourceLayer)
	{
	case ESourceLayer::Filesystem:
		return TEXT("filesystem");
	case ESourceLayer::AssetRegistry:
		return TEXT("asset_registry");
	case ESourceLayer::UObjectReflection:
		return TEXT("uobject_reflection");
	case ESourceLayer::EditorSourceGraph:
		return TEXT("editor_source_graph");
	case ESourceLayer::CompiledBlueprint:
		return TEXT("compiled_blueprint");
	case ESourceLayer::AnimationDataModel:
		return TEXT("animation_data_model");
	case ESourceLayer::RuntimeEvaluation:
		return TEXT("runtime_evaluation");
	case ESourceLayer::RuntimeObserved:
		return TEXT("runtime_observed");
	case ESourceLayer::Heuristic:
		return TEXT("heuristic");
	default:
		return TEXT("unknown");
	}
}

FString MakeStableId(const FString& ProjectId, const FString& EntityKind, const FString& CanonicalKey)
{
	FString Input;
	Input.Reserve(ProjectId.Len() + EntityKind.Len() + CanonicalKey.Len() + 2);
	Input += ProjectId;
	Input.AppendChar(TEXT('\0'));
	Input += EntityKind;
	Input.AppendChar(TEXT('\0'));
	Input += CanonicalKey;

	FTCHARToUTF8 Utf8Input(*Input, Input.Len());
	return Sha256Hex(reinterpret_cast<const uint8*>(Utf8Input.Get()), Utf8Input.Length());
}

FString MakeRelationId(
	const FString& ProjectId,
	const FString& RelationType,
	const FString& FromId,
	const FString& ToId,
	const TMap<FString, FString>* Attributes)
{
	FString CanonicalKey;
	CanonicalKey.Reserve(RelationType.Len() + FromId.Len() + ToId.Len() + 2);
	CanonicalKey += RelationType;
	CanonicalKey.AppendChar(TEXT('\0'));
	CanonicalKey += FromId;
	CanonicalKey.AppendChar(TEXT('\0'));
	CanonicalKey += ToId;

	if (Attributes && Attributes->Num() > 0)
	{
		TArray<FString> Keys;
		Attributes->GetKeys(Keys);
		Keys.Sort();

		for (const FString& Key : Keys)
		{
			CanonicalKey.AppendChar(TEXT('\0'));
			CanonicalKey += Key;
			CanonicalKey.AppendChar(TEXT('\0'));
			CanonicalKey += (*Attributes)[Key];
		}
	}

	return MakeStableId(ProjectId, TEXT("relation"), CanonicalKey);
}

FString NormalizePathForUEPI(const FString& Path)
{
	FString Normalized = Path;
	FPaths::NormalizeFilename(Normalized);
	return Normalized;
}
}
