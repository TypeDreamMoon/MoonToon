// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MoonToonMeshTargets.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class IDetailsView;
class SMoonToonMaterialParameters;
class SSearchBox;
class SWidgetSwitcher;
class UMaterialInterface;
class UMoonToonTool;
template <typename ItemType> class SListView;
class SMultiLineEditableTextBox;

/**
 * The MoonToon tools panel.
 *
 * Layout is target bar (top) -> tools list (left) -> sections over settings (right) -> output
 * (bottom), with every divider a splitter so any one part can be given the whole panel when that is
 * what the job needs.
 *
 * The panel owns no tool logic: every entry is a UMoonToonTool whose UPROPERTYs are rendered by an
 * IDetailsView, so adding a tool never touches this file beyond one line in BuildToolList.
 */
class SMoonToonToolsPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMoonToonToolsPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMoonToonToolsPanel() override;

	/** Repaints the settings when a tool's properties were changed from outside the panel -- the
	 *  strand live-preview actor writes its dragged ellipsoid back into the tool, and the details
	 *  view has no way to know that happened. */
	virtual void Tick(const FGeometry& AllottedGeometry, const double CurrentTime, const float DeltaTime) override;

private:
	using FSectionPtr = TSharedPtr<FMoonToonSectionInfo>;

	void BuildToolList();

	// --- Target ---------------------------------------------------------------------------------

	/** Rebuilds TargetMeshes from the content browser selection, unless the target is locked. */
	void OnAssetSelectionChanged(const TArray<FAssetData>& SelectedAssets, bool bIsPrimaryBrowser);

	/** Rebuilds TargetMeshes from the meshes used by the selected actors, unless locked. */
	void OnLevelSelectionChanged(UObject* NewSelection);

	void SetTargets(TArray<TWeakObjectPtr<UObject>>&& NewTargets);

	/**
	 * Rebuilds the section list from the current target.
	 *
	 * bPreserveSelection re-selects the same material indices afterwards. Off when the target
	 * changed -- the old selection referred to a different mesh's material layout -- and on after a
	 * tool run, where wiping the selection would force it to be re-picked before running again.
	 */
	void RefreshSections(bool bPreserveSelection = false);

	UObject* GetPrimaryTarget() const;
	FText GetTargetText() const;
	FText GetTargetTooltipText() const;
	const struct FSlateBrush* GetTargetIcon() const;
	bool HasTarget() const;
	ECheckBoxState IsTargetLocked() const;
	void OnTargetLockChanged(ECheckBoxState NewState);
	FReply OnOpenTargetClicked();
	FReply OnBrowseToTargetClicked();
	FReply OnRefreshClicked();

	// --- Sections -------------------------------------------------------------------------------

	TSharedRef<class ITableRow> OnGenerateSectionRow(FSectionPtr Item, const TSharedRef<class STableViewBase>& OwnerTable);
	FText GetSectionHintText() const;
	FSlateColor GetSectionHintColor() const;
	FReply OnSelectAllSections();
	FReply OnClearSectionSelection();
	void OnSectionSelectionChanged(FSectionPtr Item, ESelectInfo::Type SelectInfo);

	/** Right-click menu over the section list. Acts on the selection, which the click already set. */
	TSharedPtr<SWidget> OnSectionContextMenu();

	/** Sections the row actions apply to. Just the selection -- see the definition for why. */
	TArray<FSectionPtr> GetSectionsForAction() const;

	// By value: these are bound as delegate payloads, which own their copy of the row list.
	void OpenMaterialsFor(TArray<FSectionPtr> InSections);
	void BrowseToMaterialsFor(TArray<FSectionPtr> InSections);
	void CopyMaterialPathsFor(TArray<FSectionPtr> InSections);

	/** Selects every section that shares a material with the current selection. */
	void SelectSectionsSharingMaterial();

	void OnSectionSearchTextChanged(const FText& NewText);

	/** Refills the visible section rows from AllSections through the search box. */
	void ApplySectionFilter();

	/** Points the given sections' slots at one material, then re-reads the list. */
	void AssignMaterialToSections(const FAssetData& AssetData, TArray<int32> MaterialIndices);

	/** Creates one instance per given section under the picked parent, and assigns them. */
	void CreateInstancesForSections(const FAssetData& AssetData, TArray<int32> MaterialIndices);

	/** Material indices behind a set of rows, which is what every mesh-side operation takes. */
	static TArray<int32> ToMaterialIndices(const TArray<FSectionPtr>& InSections);

	void BuildAssignMaterialMenu(class FMenuBuilder& MenuBuilder, TArray<int32> MaterialIndices);
	void BuildCreateInstancesMenu(class FMenuBuilder& MenuBuilder, TArray<int32> MaterialIndices);

	/** Opens one material in its asset editor. The panel's only asset-editor entry point. */
	static void OpenMaterialAsset(UMaterialInterface* Material);

	// --- Material parameters --------------------------------------------------------------------

	/** Hands the selected sections' material instances to the parameter editor. */
	void UpdateMaterialTargets();

	/** Material instances of the selected sections, deduplicated. Only constants are editable. */
	TArray<class UMaterialInstanceConstant*> GetSelectedMaterialInstances() const;

	ECheckBoxState IsRightTab(int32 TabIndex) const;
	void OnRightTabChanged(ECheckBoxState NewState, int32 TabIndex);
	FText GetMaterialsTabText() const;

	// --- Viewport highlight ---------------------------------------------------------------------

	/**
	 * Pushes the selected section onto every level component using the target mesh, through the same
	 * per-section highlight the Static/Skeletal Mesh editors use. Clears the previous highlight first.
	 */
	void RefreshSectionHighlight();

	/** Sets (or with INDEX_NONE, clears) the highlight and isolation on one component. */
	static void ApplySectionHighlight(class UMeshComponent* Component, int32 MaterialIndex, bool bIsolate);

	ECheckBoxState IsIsolateEnabled() const;
	void OnIsolateChanged(ECheckBoxState NewState);

	// --- Tools ----------------------------------------------------------------------------------

	TSharedRef<class ITableRow> OnGenerateToolRow(TWeakObjectPtr<UMoonToonTool> Item, const TSharedRef<class STableViewBase>& OwnerTable);
	void OnToolSelectionChanged(TWeakObjectPtr<UMoonToonTool> Item, ESelectInfo::Type SelectInfo);

	/** Refills FilteredTools from the search box, keeping the current tool selected when it survives. */
	void RefreshToolFilter();
	void OnToolSearchTextChanged(const FText& NewText);

	FText GetToolDescriptionText() const;
	FText GetRunButtonText() const;
	EVisibility GetDestructiveNoteVisibility() const;
	bool IsRunEnabled() const;
	FText GetRunDisabledReason() const;
	FReply OnRunClicked();

	// --- Output ---------------------------------------------------------------------------------

	FText GetRunStatusText() const;
	bool HasOutput() const;

	/** Shows a report from something outside the panel -- the Content Browser batch actions. */
	void OnExternalReport(const FString& Report);

	FReply OnCopyOutputClicked();
	FReply OnClearOutputClicked();

	// --- State ----------------------------------------------------------------------------------

	/** Tools are UObjects held strongly: nothing else references them, so GC would take them. */
	TArray<TStrongObjectPtr<UMoonToonTool>> Tools;
	TArray<TWeakObjectPtr<UMoonToonTool>> AllTools;

	/** What the list actually shows: AllTools passed through the search box. */
	TArray<TWeakObjectPtr<UMoonToonTool>> FilteredTools;
	FString ToolFilter;
	TWeakObjectPtr<UMoonToonTool> SelectedTool;

	TArray<TWeakObjectPtr<UObject>> TargetMeshes;
	bool bLockTarget = false;

	/** Every section of the target. Sections is this list passed through the search box. */
	TArray<FSectionPtr> AllSections;
	TArray<FSectionPtr> Sections;
	FString SectionFilter;

	/** Components carrying a highlight we pushed, so it can be taken back off when things change. */
	TArray<TWeakObjectPtr<class UMeshComponent>> HighlightedComponents;
	bool bIsolateSection = false;

	/** Last run's report, kept as a string so the copy button does not have to read it back out. */
	FString OutputText;

	/** One line above the output: which tool ran, over what, and how long it took. */
	FText RunStatusText;

	TSharedPtr<SSearchBox> ToolSearchBox;
	TSharedPtr<SListView<TWeakObjectPtr<UMoonToonTool>>> ToolListView;
	TSharedPtr<SListView<FSectionPtr>> SectionListView;
	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<SMultiLineEditableTextBox> OutputBox;

	/** Lower-right pane: 0 = the selected tool's settings, 1 = the selected sections' materials. */
	TSharedPtr<SWidgetSwitcher> RightSwitcher;
	TSharedPtr<SMoonToonMaterialParameters> MaterialPanel;
	int32 RightTabIndex = 0;

	FDelegateHandle AssetSelectionHandle;
	FDelegateHandle LevelSelectionHandle;
	FDelegateHandle ReportHandle;

	/** Last external-edit serial seen on the selected tool; a change means refresh the details view. */
	uint32 LastSeenToolEditSerial = 0;
};
