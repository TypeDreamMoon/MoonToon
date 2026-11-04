// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MoonToonDebugTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STreeView.h"

class ITableRow;
class SBox;
class STableViewBase;
class UMoonToonDebugSubsystem;
template <typename OptionType> class SComboBox;
namespace MoonToonDebug { struct FViewSettings; }

/** The "MoonToon 调试器" nomad tab, the Tools menu entry and the level viewport's View Modes > MoonToon submenu. */
namespace MoonToonDebugPanel
{
	extern const FName TabName;
	void RegisterTab();
	void UnregisterTab();
	/** Call inside the module's UToolMenus startup callback (under the module's owner scope). */
	void ExtendMenus();
}

/**
 * The MoonToon debugger panel: the human entry point to UMoonToonDebugSubsystem. The MCP tools and
 * UnrealBridge call the same subsystem, so "copy JSON" here is byte for byte what get_pixel returns.
 *
 * Layout: toolbar + status line on top; the view tree (four groups, searchable) and the selected view's
 * options on the left; the legend over the pages (inspector / characters / tables / validation) on the
 * right. The view state is the r.MoonToon.Debug.* console variables, so a change made from the console
 * or over MCP shows up here too.
 */
class SMoonToonDebugPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMoonToonDebugPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMoonToonDebugPanel() override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
	struct FViewNode
	{
		FString Name;			// the view name; empty for a group and for "off"
		FString Group;
		FText Label;
		FText Tooltip;
		bool bIsGroup = false;
		bool bIsOff = false;
		TArray<TSharedPtr<FViewNode>> Children;
	};
	using FViewNodePtr = TSharedPtr<FViewNode>;
	using FLegendPtr = TSharedPtr<FMoonToonRegionValue>;
	using FActorPtr = TSharedPtr<FMoonToonDebugActorInfo>;
	using FRowPtr = TSharedPtr<FMoonToonTableRow>;
	using FIssuePtr = TSharedPtr<FMoonToonValidationIssue>;

	enum class EPage : int32 { Inspector, Characters, Tables, Validation };
	enum class ETable : int32 { ConstantTable, RampAtlas, Profiles };

	// --- building -----------------------------------------------------------------------------------
	TSharedRef<SWidget> BuildToolbar();
	TSharedRef<SWidget> BuildViewPane();
	TSharedRef<SWidget> BuildViewOptions();
	TSharedRef<SWidget> BuildLegend();
	TSharedRef<SWidget> BuildPages();
	TSharedRef<SWidget> BuildCharactersPage();
	TSharedRef<SWidget> BuildTablesPage();
	TSharedRef<SWidget> BuildValidationPage();

	// --- views --------------------------------------------------------------------------------------
	void RebuildViewTree();
	void SyncTreeSelection();
	TSharedRef<ITableRow> OnGenerateViewRow(FViewNodePtr Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnGetViewChildren(FViewNodePtr Item, TArray<FViewNodePtr>& OutChildren);
	void OnViewSelected(FViewNodePtr Item, ESelectInfo::Type SelectInfo);
	void OnViewChanged();
	const FMoonToonDebugViewInfo* GetCurrentViewInfo() const;
	void ModifySettings(TFunctionRef<void(MoonToonDebug::FViewSettings&)> Change);

	// --- legend -------------------------------------------------------------------------------------
	void RefreshLegend();
	TSharedRef<ITableRow> OnGenerateLegendRow(FLegendPtr Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnLegendClicked(FLegendPtr Item);
	void OnLegendDoubleClicked(FLegendPtr Item);

	// --- inspector ----------------------------------------------------------------------------------
	void OnReportChanged();
	void RebuildInspector();
	/** PinnedValue: the pinned report's value of the same field, when there is a pin. */
	TSharedRef<SWidget> BuildFieldRow(const FMoonToonDebugField& Field, const FString* PinnedValue);
	TSharedRef<SWidget> BuildStageBars(const FMoonToonPixelReport& Report);
	FText GetStatusText() const;
	FSlateColor GetStatusColor() const;
	FText GetLockText() const;
	FReply OnCopyJson();
	FReply OnCapture();

	// --- characters / tables / validation ------------------------------------------------------------
	void RefreshActors();
	TSharedRef<ITableRow> OnGenerateActorRow(FActorPtr Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnActorSelected(FActorPtr Item, ESelectInfo::Type SelectInfo);
	void RefreshTable();
	TSharedRef<ITableRow> OnGenerateTableRow(FRowPtr Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnTableRowClicked(FRowPtr Item);
	void OnTableRowDoubleClicked(FRowPtr Item);
	void RefreshValidation();
	TSharedRef<ITableRow> OnGenerateIssueRow(FIssuePtr Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnIssueClicked(FIssuePtr Item);

	// --- helpers ------------------------------------------------------------------------------------
	static UObject* ResolveObject(const FString& Path);
	static void OpenAsset(const FString& Path);
	static void SelectActor(const FString& Path, bool bFocus);
	/** Inspects a pixel of the target viewport, locks it and shows the inspector. */
	void LockOnViewportPixel(FIntPoint ViewportPixel);
	/** The same for a render pixel (0-based in the view rect), converted with the last report's sizes. */
	void LockOnRenderPixel(FIntPoint RenderPixel);

	TWeakObjectPtr<UMoonToonDebugSubsystem> Subsystem;
	FDelegateHandle ReportChangedHandle;
	FDelegateHandle ViewChangedHandle;

	// views
	TArray<FMoonToonDebugViewInfo> ViewInfos;
	TArray<FViewNodePtr> Roots;
	TSharedPtr<STreeView<FViewNodePtr>> ViewTree;
	FString ViewFilter;
	bool bSyncingSelection = false;
	TArray<TSharedPtr<FString>> ChannelOptions;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ChannelCombo;
	TArray<TSharedPtr<FString>> NonToonOptions;

	// legend
	TArray<FLegendPtr> LegendItems;
	TSharedPtr<SListView<FLegendPtr>> LegendList;
	double LastLegendRefresh = -1000.0;
	FString LegendViewName;
	int32 LegendOverflow = 0;

	// pages
	EPage Page = EPage::Inspector;
	TSharedPtr<SBox> InspectorBox;
	bool bInspectorDirty = true;
	double LastInspectorBuild = -1000.0;
	bool bOnlyDifferences = false;
	TSet<FString> CollapsedSections;
	FString LastCaptureText;
	FString LastCapturePath;

	TArray<FActorPtr> Actors;
	TSharedPtr<SListView<FActorPtr>> ActorList;
	TSharedPtr<SBox> ActorDetailsBox;
	FActorPtr SelectedActor;
	bool bOnlyOnScreen = true;
	bool bActorsLoaded = false;

	ETable Table = ETable::ConstantTable;
	TArray<FRowPtr> TableRows;
	TSharedPtr<SListView<FRowPtr>> TableList;
	bool bTableLoaded = false;

	TArray<FIssuePtr> Issues;
	TSharedPtr<SListView<FIssuePtr>> IssueList;
	bool bIssuesLoaded = false;
	bool bGpuChecks = true;
};
