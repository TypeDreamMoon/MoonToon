// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonEditor.h"

#include "MoonToonMaterialMenus.h"
#include "MoonToonSmoothNormalTool.h"
#include "SMoonToonToolsPanel.h"
#include "ToolMenus.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "FMoonToonEditorModule"

namespace
{
	const FName MoonToonToolsTabName(TEXT("MoonToonTools"));

	TSharedRef<SDockTab> SpawnToolsTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SMoonToonToolsPanel)
			];
	}

	/**
	 * A nomad tab rather than an asset editor: the tools act on whatever is selected and write back to
	 * assets, so there is nothing per-asset to keep open, and the level viewport is already the best
	 * preview for vertex colour and outline changes.
	 */
	void RegisterToolsTab()
	{
		FGlobalTabmanager::Get()
			->RegisterNomadTabSpawner(MoonToonToolsTabName, FOnSpawnTab::CreateStatic(&SpawnToolsTab))
			.SetDisplayName(LOCTEXT("ToolsTabTitle", "MoonToon Tools"))
			.SetTooltipText(LOCTEXT("ToolsTabTooltip",
				"Mesh inspection and the MoonToon vertex bakes, in one panel."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));
	}

	void AddToolsMenuEntry()
	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
		if (!Menu)
		{
			return;
		}

		FToolMenuSection& Section = Menu->FindOrAddSection(
			TEXT("MoonToon"), LOCTEXT("MoonToonSection", "MoonToon"));

		Section.AddMenuEntry(
			TEXT("MoonToonToolsPanel"),
			LOCTEXT("OpenToolsPanel", "MoonToon Tools"),
			LOCTEXT("OpenToolsPanelTooltip",
				"Mesh inspection and the MoonToon vertex bakes, in one panel."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"),
			FUIAction(FExecuteAction::CreateLambda([]
			{
				FGlobalTabmanager::Get()->TryInvokeTab(MoonToonToolsTabName);
			})));
	}

	/**
	 * Runs one of the tool entry points on a throwaway instance. The tool keeps no state between
	 * calls -- it reads the content browser selection itself -- so there is nothing to preserve.
	 */
	void RunTool(void (UMoonToonSmoothNormalTool::*Action)())
	{
		UMoonToonSmoothNormalTool* Tool = NewObject<UMoonToonSmoothNormalTool>(GetTransientPackage());
		(Tool->*Action)();
	}

	void AddMoonToonSection(const TCHAR* MenuName)
	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(FName(MenuName));
		if (!Menu)
		{
			return;
		}

		FToolMenuSection& Section = Menu->FindOrAddSection(
			TEXT("MoonToon"), LOCTEXT("MoonToonSection", "MoonToon"));

		Section.AddMenuEntry(
			TEXT("MoonToonBakeSmoothedNormal"),
			LOCTEXT("BakeSmoothedNormal", "Bake Smoothed Normal and Curvature"),
			LOCTEXT("BakeSmoothedNormalTooltip",
				"Bakes the tangent-space smoothed normal (scaled by curvature) into UV2.y / UV3.xy of "
				"every selected mesh, for every LOD. Overwrites the face-forward bake."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]
			{
				RunTool(&UMoonToonSmoothNormalTool::BakeSmoothedNormalAndCurvature);
			})));

		Section.AddMenuEntry(
			TEXT("MoonToonBakeFaceForward"),
			LOCTEXT("BakeFaceForward", "Bake Face Forward Direction"),
			LOCTEXT("BakeFaceForwardTooltip",
				"Bakes the tangent-space face-forward direction into UV2.xy / UV3.x of every selected "
				"mesh, for every LOD. Overwrites the smoothed-normal bake."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]
			{
				// Takes a defaulted argument, so it cannot go through the member-pointer helper.
				UMoonToonSmoothNormalTool* Tool = NewObject<UMoonToonSmoothNormalTool>(GetTransientPackage());
				Tool->BakeFaceForwardDirection();
			})));

		Section.AddMenuEntry(
			TEXT("MoonToonFixBuildSettings"),
			LOCTEXT("FixBuildSettings", "Fix Build Settings"),
			LOCTEXT("FixBuildSettingsTooltip",
				"Forces the LOD build settings the bakes depend on and reimports any mesh that had to "
				"be corrected."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]
			{
				RunTool(&UMoonToonSmoothNormalTool::FixBuildSettings);
			})));
	}

	void RegisterMoonToonMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(TEXT("MoonToonEditor"));
		AddMoonToonSection(TEXT("ContentBrowser.AssetContextMenu.StaticMesh"));
		AddMoonToonSection(TEXT("ContentBrowser.AssetContextMenu.SkeletalMesh"));
		RegisterMoonToonMaterialMenus();
		AddToolsMenuEntry();
	}
}

void FMoonToonEditorModule::StartupModule()
{
	// Registering through the startup callback rather than directly: UToolMenus may not exist yet
	// when an Editor-phase module loads.
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterMoonToonMenus));

	RegisterToolsTab();
}

void FMoonToonEditorModule::ShutdownModule()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(MoonToonToolsTabName);
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(TEXT("MoonToonEditor"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMoonToonEditorModule, MoonToonEditor)
