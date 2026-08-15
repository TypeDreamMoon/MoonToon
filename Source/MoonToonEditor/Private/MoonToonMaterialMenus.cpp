// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonMaterialMenus.h"

#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "ContentBrowserMenuContexts.h"
#include "IAssetTools.h"
#include "MoonToonMaterialPreset.h"
#include "MoonToonMaterialPresetFactory.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IContentBrowserSingleton.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MoonToonMaterialLibrary.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "MoonToonMaterialMenus"

DEFINE_LOG_CATEGORY_STATIC(LogMoonToonMaterialMenus, Log, All);

namespace
{
	/** The selected material instances, loaded. Right-click no longer loads assets by itself. */
	TArray<UMaterialInstanceConstant*> GetSelectedInstances(const FToolMenuContext& Context)
	{
		if (const UContentBrowserAssetContextMenuContext* AssetContext =
			Context.FindContext<UContentBrowserAssetContextMenuContext>())
		{
			return AssetContext->LoadSelectedObjects<UMaterialInstanceConstant>();
		}
		return {};
	}

	/**
	 * Publishes a batch operation's report: the tools panel's output pane when it is open, the log
	 * always, and a notification so the Content Browser gives some sign that anything happened.
	 */
	void PublishReport(const FText& Title, const FString& Report)
	{
		UE_LOG(LogMoonToonMaterialMenus, Log, TEXT("[MoonToon] %s\n%s"), *Title.ToString(), *Report);
		MoonToonMaterial::OnReport().Broadcast(FString::Printf(TEXT("%s\n\n%s"), *Title.ToString(), *Report));

		FNotificationInfo Info(Title);
		Info.ExpireDuration = 4.0f;
		Info.SubText = MoonToonMaterial::OnReport().IsBound()
			? LOCTEXT("ReportInPanel", "Details in the MoonToon Tools output pane.")
			: LOCTEXT("ReportInLog", "Details in the Output Log.");
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	/** Local alias kept so the entries below read as they did before the picker was shared out. */
	TSharedRef<SWidget> MakeAssetPickerMenu(const UClass* AllowedClass, FOnAssetSelected OnSelected)
	{
		return MakeMoonToonAssetPickerMenu(AllowedClass, MoveTemp(OnSelected));
	}

	void AddMaterialInstanceEntries(UToolMenu* Menu)
	{
		FToolMenuSection& Section = Menu->FindOrAddSection(
			TEXT("MoonToon"), LOCTEXT("MoonToonSection", "MoonToon"));

		Section.AddSubMenu(
			TEXT("MoonToonSetParent"),
			LOCTEXT("SetParent", "Set Parent"),
			LOCTEXT("SetParentTooltip",
				"Re-parent every selected instance to one material. Overrides are stored by parameter "
				"name, so the ones the new parent also declares carry over; the rest are cleared and "
				"listed in the report."),
			FNewToolMenuChoice(FNewToolMenuDelegate::CreateLambda([](UToolMenu* SubMenu)
			{
				const TArray<UMaterialInstanceConstant*> Instances = GetSelectedInstances(SubMenu->Context);
				FToolMenuSection& PickerSection = SubMenu->FindOrAddSection(TEXT("Picker"));
				PickerSection.AddEntry(FToolMenuEntry::InitWidget(
					TEXT("ParentPicker"),
					MakeAssetPickerMenu(UMaterialInterface::StaticClass(),
						FOnAssetSelected::CreateLambda([Instances](const FAssetData& AssetData)
						{
							FSlateApplication::Get().DismissAllMenus();
							if (UMaterialInterface* NewParent = Cast<UMaterialInterface>(AssetData.GetAsset()))
							{
								const FString Report = UMoonToonMaterialLibrary::SetParentOnInstances(
									Instances, NewParent, /*bClearOrphanedOverrides=*/true);
								PublishReport(FText::Format(LOCTEXT("SetParentDone",
									"Re-parented {0} material instance(s)"), FText::AsNumber(Instances.Num())), Report);
							}
						})),
					FText::GetEmpty(),
					/*bNoIndent=*/true));
			})),
			/*bInOpenSubMenuOnClick=*/false,
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Link"));

		Section.AddSubMenu(
			TEXT("MoonToonCopyOverrides"),
			LOCTEXT("CopyOverrides", "Copy Overrides From"),
			LOCTEXT("CopyOverridesTooltip",
				"Copy every override from one instance onto the selected ones. Parameters the target's "
				"own parent does not declare are skipped rather than planted as dead entries."),
			FNewToolMenuChoice(FNewToolMenuDelegate::CreateLambda([](UToolMenu* SubMenu)
			{
				const TArray<UMaterialInstanceConstant*> Instances = GetSelectedInstances(SubMenu->Context);
				FToolMenuSection& PickerSection = SubMenu->FindOrAddSection(TEXT("Picker"));
				PickerSection.AddEntry(FToolMenuEntry::InitWidget(
					TEXT("SourcePicker"),
					MakeAssetPickerMenu(UMaterialInstanceConstant::StaticClass(),
						FOnAssetSelected::CreateLambda([Instances](const FAssetData& AssetData)
						{
							FSlateApplication::Get().DismissAllMenus();
							if (UMaterialInstanceConstant* Source = Cast<UMaterialInstanceConstant>(AssetData.GetAsset()))
							{
								const FString Report = UMoonToonMaterialLibrary::CopyOverrides(Source, Instances);
								PublishReport(FText::Format(LOCTEXT("CopyOverridesDone",
									"Copied overrides onto {0} instance(s)"), FText::AsNumber(Instances.Num())), Report);
							}
						})),
					FText::GetEmpty(),
					/*bNoIndent=*/true));
			})),
			/*bInOpenSubMenuOnClick=*/false,
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Duplicate"));

		Section.AddSubMenu(
			TEXT("MoonToonApplyPreset"),
			LOCTEXT("ApplyPreset", "Apply Preset"),
			LOCTEXT("ApplyPresetTooltip",
				"Write a MoonToon material preset's values onto every selected instance. Parameters the "
				"instance's own parent does not declare are skipped and listed in the report."),
			FNewToolMenuChoice(FNewToolMenuDelegate::CreateLambda([](UToolMenu* SubMenu)
			{
				const TArray<UMaterialInstanceConstant*> Instances = GetSelectedInstances(SubMenu->Context);
				FToolMenuSection& PickerSection = SubMenu->FindOrAddSection(TEXT("Picker"));
				PickerSection.AddEntry(FToolMenuEntry::InitWidget(
					TEXT("PresetPicker"),
					MakeAssetPickerMenu(UMoonToonMaterialPreset::StaticClass(),
						FOnAssetSelected::CreateLambda([Instances](const FAssetData& AssetData)
						{
							FSlateApplication::Get().DismissAllMenus();
							if (UMoonToonMaterialPreset* Preset = Cast<UMoonToonMaterialPreset>(AssetData.GetAsset()))
							{
								const FString Report = UMoonToonMaterialLibrary::ApplyPreset(Preset, Instances);
								PublishReport(FText::Format(LOCTEXT("ApplyPresetDone",
									"Applied {0} to {1} instance(s)"),
									FText::FromString(Preset->GetName()), FText::AsNumber(Instances.Num())), Report);
							}
						})),
					FText::GetEmpty(),
					/*bNoIndent=*/true));
			})),
			/*bInOpenSubMenuOnClick=*/false,
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Download"));

		Section.AddMenuEntry(
			TEXT("MoonToonSaveAsPreset"),
			LOCTEXT("SaveAsPreset", "Save as Preset"),
			LOCTEXT("SaveAsPresetTooltip",
				"Capture the overrides these instances agree on into a new preset asset, so the same "
				"look can be applied to a character imported later."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.PlusCircle"),
			FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext& Context)
			{
				const TArray<UMaterialInstanceConstant*> Instances = GetSelectedInstances(Context);
				if (Instances.Num() == 0)
				{
					return;
				}

				IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
				UMoonToonMaterialPresetFactory* Factory = NewObject<UMoonToonMaterialPresetFactory>();
				UObject* Created = AssetTools.CreateAssetWithDialog(UMoonToonMaterialPreset::StaticClass(), Factory);
				if (UMoonToonMaterialPreset* Preset = Cast<UMoonToonMaterialPreset>(Created))
				{
					const FString Report = UMoonToonMaterialLibrary::CaptureToPreset(
						Preset, Instances, /*bOverriddenOnly=*/true);
					PublishReport(FText::Format(LOCTEXT("SaveAsPresetDone",
						"Captured {0} into a preset"), FText::AsNumber(Instances.Num())), Report);
				}
			}));

		Section.AddMenuEntry(
			TEXT("MoonToonInsertMaster"),
			LOCTEXT("InsertMaster", "Insert Character Master"),
			LOCTEXT("InsertMasterTooltip",
				"Put one shared instance between these and their parent, named after the folder they "
				"live in, so the character has a single place to be tuned from. Overrides they all "
				"already agreed on move up to it; nothing about the result changes."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Link"),
			FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext& Context)
			{
				const TArray<UMaterialInstanceConstant*> Instances = GetSelectedInstances(Context);
				if (Instances.Num() == 0)
				{
					return;
				}

				// No mesh to take a name from here, so the folder is the character: VRM imports put
				// one character's instances in one folder, which is exactly the grouping wanted.
				const FString Folder = FPackageName::GetLongPackagePath(Instances[0]->GetOutermost()->GetName());
				FString CharacterName = FPaths::GetCleanFilename(Folder);

				const FString Report = UMoonToonMaterialLibrary::InsertCharacterMasters(
					Instances, CharacterName, /*DestinationFolder=*/FString(),
					/*bPromoteCommonOverrides=*/true, /*bPromoteStaticSwitches=*/false);
				PublishReport(FText::Format(LOCTEXT("InsertMasterDone",
					"Inserted a master above {0} instance(s)"), FText::AsNumber(Instances.Num())), Report);
			}));

		Section.AddMenuEntry(
			TEXT("MoonToonClearOrphans"),
			LOCTEXT("ClearOrphans", "Clear Orphaned Overrides"),
			LOCTEXT("ClearOrphansTooltip",
				"Remove overrides whose parameter no longer exists in the parent. They do nothing but "
				"still serialise, and they are what a re-parent or a renamed parameter leaves behind."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
			FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext& Context)
			{
				const TArray<UMaterialInstanceConstant*> Instances = GetSelectedInstances(Context);
				const FString Report = UMoonToonMaterialLibrary::ClearOrphanedOverrides(Instances);
				PublishReport(FText::Format(LOCTEXT("ClearOrphansDone",
					"Cleaned {0} material instance(s)"), FText::AsNumber(Instances.Num())), Report);
			}));

		Section.AddMenuEntry(
			TEXT("MoonToonResetOverrides"),
			LOCTEXT("ResetOverrides", "Reset All Overrides"),
			LOCTEXT("ResetOverridesTooltip",
				"Drop every override, so the instances become exactly their parent. Asks first: this "
				"throws away authored values and undo does not cover it."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"),
			FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext& Context)
			{
				const TArray<UMaterialInstanceConstant*> Instances = GetSelectedInstances(Context);
				if (Instances.Num() == 0)
				{
					return;
				}

				const EAppReturnType::Type Answer = FMessageDialog::Open(
					EAppMsgType::YesNo,
					FText::Format(LOCTEXT("ResetConfirm",
						"Drop every parameter override on {0} material instance(s)?\n\n"
						"They will match their parent exactly. This cannot be undone."),
						FText::AsNumber(Instances.Num())));
				if (Answer != EAppReturnType::Yes)
				{
					return;
				}

				const FString Report = UMoonToonMaterialLibrary::ResetOverrides(Instances, TArray<FName>());
				PublishReport(FText::Format(LOCTEXT("ResetDone",
					"Reset {0} material instance(s)"), FText::AsNumber(Instances.Num())), Report);
			}));

		Section.AddMenuEntry(
			TEXT("MoonToonDescribeInstances"),
			LOCTEXT("Describe", "Report Overrides"),
			LOCTEXT("DescribeTooltip",
				"List each instance's parent and overrides, marking the ones the parent no longer has."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Details"),
			FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext& Context)
			{
				const TArray<UMaterialInstanceConstant*> Instances = GetSelectedInstances(Context);
				const FString Report = UMoonToonMaterialLibrary::DescribeInstances(Instances);
				PublishReport(FText::Format(LOCTEXT("DescribeDone",
					"{0} material instance(s)"), FText::AsNumber(Instances.Num())), Report);
			}));
	}
}

TSharedRef<SWidget> MakeMoonToonAssetPickerMenu(const UClass* AllowedClass, FOnAssetSelected OnSelected)
{
	FAssetPickerConfig PickerConfig;
	PickerConfig.Filter.ClassPaths.Add(AllowedClass->GetClassPathName());
	PickerConfig.Filter.bRecursiveClasses = true;
	PickerConfig.SelectionMode = ESelectionMode::Single;
	PickerConfig.InitialAssetViewType = EAssetViewType::List;
	PickerConfig.bAllowNullSelection = false;
	PickerConfig.bFocusSearchBoxWhenOpened = true;
	PickerConfig.OnAssetSelected = MoveTemp(OnSelected);

	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

	return SNew(SBox)
		.WidthOverride(320.0f)
		.HeightOverride(400.0f)
		[
			ContentBrowserModule.Get().CreateAssetPicker(PickerConfig)
		];
}

void RegisterMoonToonMaterialMenus()
{
	if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AssetContextMenu.MaterialInstanceConstant")))
	{
		AddMaterialInstanceEntries(Menu);
	}
}

#undef LOCTEXT_NAMESPACE
