// Copyright Epic Games, Inc. All Rights Reserved.

#include "UEProjectIntelligence.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "UEPIEditorSubsystem.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FUEProjectIntelligenceModule"

DEFINE_LOG_CATEGORY_STATIC(LogUEProjectIntelligence, Log, All);

namespace
{
	const FName UEPIEditorTabName(TEXT("UEProjectIntelligenceDashboard"));

	FString UEPISavedDirectory()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEProjectIntelligence")));
	}

	FString UEPILastScanPath()
	{
		return FPaths::Combine(UEPISavedDirectory(), TEXT("last_scan.json"));
	}

	FString UEPISnapshotManifestPath()
	{
		return FPaths::Combine(UEPISavedDirectory(), TEXT("store"), TEXT("manifests"), TEXT("saved.json"));
	}

	FString UEPISnapshotSummary()
	{
		const FString ManifestPath = UEPISnapshotManifestPath();
		FString ManifestText;
		if (!FFileHelper::LoadFileToString(ManifestText, *ManifestPath))
		{
			return TEXT("not generated");
		}

		TSharedPtr<FJsonObject> Manifest;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ManifestText);
		if (!FJsonSerializer::Deserialize(Reader, Manifest) || !Manifest.IsValid())
		{
			return TEXT("unreadable");
		}

		int32 Generation = 0;
		Manifest->TryGetNumberField(TEXT("generation"), Generation);
		FString CreatedAtUtc;
		Manifest->TryGetStringField(TEXT("created_at_utc"), CreatedAtUtc);

		int32 EntityCount = 0;
		int32 RelationCount = 0;
		int32 DiagnosticCount = 0;
		if (Manifest->HasTypedField<EJson::Object>(TEXT("counts")))
		{
			const TSharedPtr<FJsonObject> Counts = Manifest->GetObjectField(TEXT("counts"));
			Counts->TryGetNumberField(TEXT("entities"), EntityCount);
			Counts->TryGetNumberField(TEXT("relations"), RelationCount);
			Counts->TryGetNumberField(TEXT("diagnostics"), DiagnosticCount);
		}

		return FString::Printf(
			TEXT("generation=%d created=%s entities=%d relations=%d diagnostics=%d path=%s"),
			Generation,
			CreatedAtUtc.IsEmpty() ? TEXT("-") : *CreatedAtUtc,
			EntityCount,
			RelationCount,
			DiagnosticCount,
			*ManifestPath);
	}
}

void FUEProjectIntelligenceModule::StartupModule()
{
	UE_LOG(LogUEProjectIntelligence, Log, TEXT("UE Project Intelligence editor module started."));

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		UEPIEditorTabName,
		FOnSpawnTab::CreateRaw(this, &FUEProjectIntelligenceModule::SpawnDashboardTab))
		.SetDisplayName(LOCTEXT("UEPIDashboardTabTitle", "UE Project Intelligence"))
		.SetTooltipText(LOCTEXT("UEPIDashboardTabTooltip", "Open the UE Project Intelligence dashboard."))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUEProjectIntelligenceModule::RegisterMenus));
}

void FUEProjectIntelligenceModule::ShutdownModule()
{
	if (UToolMenus::IsToolMenuUIEnabled())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UEPIEditorTabName);
	UE_LOG(LogUEProjectIntelligence, Log, TEXT("UE Project Intelligence editor module shut down."));
}

void FUEProjectIntelligenceModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	if (UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools"))
	{
		FToolMenuSection& Section = ToolsMenu->FindOrAddSection("UEProjectIntelligence");
		Section.AddMenuEntry(
			"UEPI_OpenDashboard",
			LOCTEXT("UEPIOpenDashboardMenu", "UE Project Intelligence"),
			LOCTEXT("UEPIOpenDashboardMenuTooltip", "Open the UE Project Intelligence dashboard."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
			FUIAction(FExecuteAction::CreateRaw(this, &FUEProjectIntelligenceModule::OpenDashboardTab)));
	}

	if (UToolMenu* AssetMenu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu"))
	{
		FToolMenuSection& Section = AssetMenu->FindOrAddSection("UEProjectIntelligence");
		Section.AddMenuEntry(
			"UEPI_OpenAssetDashboard",
			LOCTEXT("UEPIOpenAssetDashboardMenu", "Open in UEPI"),
			LOCTEXT("UEPIOpenAssetDashboardMenuTooltip", "Open UE Project Intelligence for the current selection."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
			FUIAction(FExecuteAction::CreateRaw(this, &FUEProjectIntelligenceModule::OpenDashboardTab)));
	}
}

void FUEProjectIntelligenceModule::OpenDashboardTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(UEPIEditorTabName);
}

TSharedRef<SDockTab> FUEProjectIntelligenceModule::SpawnDashboardTab(const FSpawnTabArgs& SpawnTabArgs)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SBorder)
			.Padding(12.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("UEPIDashboardTitle", "UE Project Intelligence"))
						.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 8.0f)
					[
						SNew(STextBlock)
						.Text(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateRaw(this, &FUEProjectIntelligenceModule::GetDashboardStatusText)))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f)
					[
						SNew(STextBlock)
						.Text(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateRaw(this, &FUEProjectIntelligenceModule::GetLastActionText)))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 8.0f)
					[
						SNew(SSeparator)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("UEPIRunScanButton", "Run Snapshot Scan"))
							.OnClicked_Raw(this, &FUEProjectIntelligenceModule::RunMetadataScan)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("UEPIWriteEventsButton", "Write Events Snapshot"))
							.OnClicked_Raw(this, &FUEProjectIntelligenceModule::WriteIncrementalEventsSnapshot)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("UEPIOpenSavedButton", "Open Saved"))
							.OnClicked_Raw(this, &FUEProjectIntelligenceModule::OpenSavedFolder)
						]
					]
				]
			]
		];
}

FText FUEProjectIntelligenceModule::GetDashboardStatusText() const
{
	int32 IncrementalEventCount = 0;
	int32 PendingInvalidationCount = 0;
	FString SessionId = TEXT("-");
	FString LastHeartbeat = TEXT("-");
	FString LastAutoScan = TEXT("-");
	FString LastAutoScanMode = TEXT("-");
	FString LastAutoScanManifest = TEXT("-");
	FString LastCollectorError;
	if (GEditor)
	{
		if (const UUEPIEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UUEPIEditorSubsystem>())
		{
			FUEPICollectorStatus Status;
			Subsystem->GetCollectorStatus(Status);
			IncrementalEventCount = Status.IncrementalEvents;
			PendingInvalidationCount = Status.PendingInvalidations;
			SessionId = Status.SessionId.IsEmpty() ? TEXT("-") : Status.SessionId;
			LastHeartbeat = Status.LastHeartbeatUtc.IsEmpty() ? TEXT("-") : Status.LastHeartbeatUtc;
			LastAutoScan = Status.LastAutoScanUtc.IsEmpty() ? TEXT("-") : Status.LastAutoScanUtc;
			LastAutoScanMode = Status.LastAutoScanMode.IsEmpty() ? TEXT("-") : Status.LastAutoScanMode;
			LastAutoScanManifest = Status.LastAutoScanManifestPath.IsEmpty() ? TEXT("-") : Status.LastAutoScanManifestPath;
			LastCollectorError = Status.LastError;
		}
	}

	const FString LastScan = UEPILastScanPath();
	const FString SnapshotSummary = UEPISnapshotSummary();
	return FText::Format(
		LOCTEXT(
			"UEPIDashboardStatus",
			"Project: {0}\nSnapshot: {1}\nLive session: {2}\nLast heartbeat: {3}\nIncremental events: {4}\nPending invalidations: {5}\nLast auto scan: {6} ({7})\nLast auto manifest: {8}\nLast collector error: {9}\nLast scan artifact: {10}\nSaved directory: {11}"),
		FText::FromString(FApp::GetProjectName()),
		FText::FromString(SnapshotSummary),
		FText::FromString(SessionId),
		FText::FromString(LastHeartbeat),
		FText::AsNumber(IncrementalEventCount),
		FText::AsNumber(PendingInvalidationCount),
		FText::FromString(LastAutoScan),
		FText::FromString(LastAutoScanMode),
		FText::FromString(LastAutoScanManifest),
		FText::FromString(LastCollectorError.IsEmpty() ? FString(TEXT("-")) : LastCollectorError),
		FText::FromString(FPaths::FileExists(LastScan) ? LastScan : FString(TEXT("not generated"))),
		FText::FromString(UEPISavedDirectory()));
}

FText FUEProjectIntelligenceModule::GetLastActionText() const
{
	return FText::FromString(LastActionMessage.IsEmpty() ? FString(TEXT("Ready.")) : LastActionMessage);
}

FReply FUEProjectIntelligenceModule::RunMetadataScan()
{
	if (!GEditor)
	{
		LastActionMessage = TEXT("Editor is not available.");
		return FReply::Handled();
	}

	UUEPIEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UUEPIEditorSubsystem>();
	if (!Subsystem)
	{
		LastActionMessage = TEXT("UEPI editor subsystem is not available.");
		return FReply::Handled();
	}

	FString ReportPath;
	FString Error;
	const bool bSuccess = Subsystem->RunMetadataScan(FString(), ReportPath, Error);
	LastActionMessage = bSuccess
		? FString::Printf(TEXT("Snapshot scan wrote manifest %s"), *ReportPath)
		: FString::Printf(TEXT("Snapshot scan completed with diagnostics or error: %s"), Error.IsEmpty() ? *ReportPath : *Error);
	return FReply::Handled();
}

FReply FUEProjectIntelligenceModule::WriteIncrementalEventsSnapshot()
{
	if (!GEditor)
	{
		LastActionMessage = TEXT("Editor is not available.");
		return FReply::Handled();
	}

	const UUEPIEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UUEPIEditorSubsystem>();
	if (!Subsystem)
	{
		LastActionMessage = TEXT("UEPI editor subsystem is not available.");
		return FReply::Handled();
	}

	FString ReportPath;
	FString Error;
	const bool bSuccess = Subsystem->WriteIncrementalEventsSnapshot(FString(), ReportPath, Error);
	LastActionMessage = bSuccess
		? FString::Printf(TEXT("Incremental events snapshot wrote %s"), *ReportPath)
		: Error;
	return FReply::Handled();
}

FReply FUEProjectIntelligenceModule::OpenSavedFolder() const
{
	const FString SavedDirectory = UEPISavedDirectory();
	IFileManager::Get().MakeDirectory(*SavedDirectory, true);
	FPlatformProcess::ExploreFolder(*SavedDirectory);
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FUEProjectIntelligenceModule, UEProjectIntelligence)
