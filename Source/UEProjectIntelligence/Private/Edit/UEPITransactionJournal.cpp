#include "Edit/UEPITransactionJournal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::ProjectIntelligence
{
	bool FUEPITransactionJournal::Write(const FString& TransactionId, const FString& Phase, const TArray<FString>& AffectedAssets, const TMap<FString, FString>& BackupFiles, bool bSaved, const FString& Message, FString& OutPath, FString& OutError)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema_version"), TEXT("uepi.transaction-journal.v1"));
		Root->SetStringField(TEXT("transaction_id"), TransactionId);
		Root->SetStringField(TEXT("phase"), Phase);
		Root->SetStringField(TEXT("observed_at"), FDateTime::UtcNow().ToIso8601());
		Root->SetBoolField(TEXT("saved"), bSaved);
		Root->SetStringField(TEXT("message"), Message);
		TArray<TSharedPtr<FJsonValue>> Assets;
		for (const FString& Asset : AffectedAssets) Assets.Add(MakeShared<FJsonValueString>(Asset));
		Root->SetArrayField(TEXT("affected_assets"), Assets);
		TArray<TSharedPtr<FJsonValue>> Backups;
		for (const TPair<FString, FString>& Pair : BackupFiles)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("package_file"), Pair.Key); Item->SetStringField(TEXT("backup_file"), Pair.Value); Backups.Add(MakeShared<FJsonValueObject>(Item));
		}
		Root->SetArrayField(TEXT("backups"), Backups);
		FString Text; const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text); FJsonSerializer::Serialize(Root, Writer);
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence"), TEXT("store"), TEXT("transactions"));
		IFileManager::Get().MakeDirectory(*Directory, true);
		OutPath = FPaths::Combine(Directory, FPaths::MakeValidFileName(TransactionId) + TEXT(".") + FPaths::MakeValidFileName(Phase) + TEXT(".json"));
		if (!FFileHelper::SaveStringToFile(Text, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write transaction journal: %s"), *OutPath);
			return false;
		}
		OutError.Reset(); return true;
	}
}
