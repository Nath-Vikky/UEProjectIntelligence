#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UE::ProjectIntelligence
{
enum class ECollectionLevel : uint8
{
	Registry,
	Reflection,
	Structural,
	Semantic,
	Evaluated
};

enum class ECompletenessState : uint8
{
	Complete,
	Partial,
	MetadataOnly,
	Failed,
	Unsupported,
	Stale,
	RuntimeContextRequired
};

enum class ESourceLayer : uint8
{
	Filesystem,
	AssetRegistry,
	UObjectReflection,
	EditorSourceGraph,
	CompiledBlueprint,
	AnimationDataModel,
	RuntimeEvaluation,
	RuntimeObserved,
	Heuristic
};

struct FEvidence
{
	FString SourceLayer;
	FString Path;
	FString Detail;

	TSharedRef<FJsonObject> ToJson() const;
};

struct FDiagnostic
{
	FString Code;
	FString Severity;
	FString Message;
	TMap<FString, FString> Context;

	TSharedRef<FJsonObject> ToJson() const;
};

struct FCompleteness
{
	ECompletenessState State = ECompletenessState::MetadataOnly;
	TArray<FString> Covered;
	TArray<FString> Omitted;
	TArray<FString> Warnings;

	TSharedRef<FJsonObject> ToJson() const;
};

struct FEntityRecord
{
	FString Id;
	FString Kind;
	FString CanonicalKey;
	FString DisplayName;
	FString SourceLayer;
	TMap<FString, FString> Attributes;
	FCompleteness Completeness;
	TArray<FDiagnostic> Diagnostics;
	TArray<FEvidence> Evidence;
	TSharedPtr<FJsonObject> Snapshot;

	TSharedRef<FJsonObject> ToJson() const;
};

struct FRelationRecord
{
	FString Id;
	FString Type;
	FString FromId;
	FString ToId;
	FString SourceLayer;
	bool bDerived = false;
	float Confidence = 1.0f;
	TMap<FString, FString> Attributes;
	TArray<FEvidence> Evidence;

	TSharedRef<FJsonObject> ToJson() const;
};

struct FScanOptions
{
	bool bIncludeGameContent = true;
	bool bIncludeProjectPluginContent = true;
	bool bIncludeEngineContent = false;
	bool bReadUObjectReflection = false;
	bool bReadBlueprintGraphs = false;
	int32 MaxAssetsPerBatch = 50;
	int32 MaxInlineCollectionItems = 1000;
	FString OutputPath;
	TArray<FString> TargetObjectPaths;
};

struct FProjectScanResult
{
	FString SchemaVersion = TEXT("uepi.scan.v1");
	FString ProjectId;
	FString ProjectName;
	FString ProjectFile;
	FString EngineVersion;
	FString StartedAtUtc;
	FString FinishedAtUtc;
	FCompleteness Completeness;
	TArray<FEntityRecord> Entities;
	TArray<FRelationRecord> Relations;
	TArray<FDiagnostic> Diagnostics;

	TSharedRef<FJsonObject> ToJson() const;
};

FString LexToString(ECollectionLevel Level);
FString LexToString(ECompletenessState State);
FString LexToString(ESourceLayer SourceLayer);
FString MakeStableId(const FString& ProjectId, const FString& EntityKind, const FString& CanonicalKey);
FString MakeRelationId(
	const FString& ProjectId,
	const FString& RelationType,
	const FString& FromId,
	const FString& ToId,
	const TMap<FString, FString>* Attributes = nullptr);
FString NormalizePathForUEPI(const FString& Path);
}
