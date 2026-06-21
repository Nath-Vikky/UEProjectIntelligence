// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class SDockTab;
class FSpawnTabArgs;

class FUEProjectIntelligenceModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void OpenDashboardTab();
	TSharedRef<SDockTab> SpawnDashboardTab(const FSpawnTabArgs& SpawnTabArgs);
	FText GetDashboardStatusText() const;
	FText GetLastActionText() const;
	FReply RunMetadataScan();
	FReply StartLiveWorker();
	FReply WriteIncrementalEventsSnapshot();
	FReply OpenSavedFolder() const;
	FReply OpenWebUi() const;

	FString LastActionMessage;
};
