// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MoonToonMeshTargets.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class IDetailsView;
class UMoonToonTool;
template <typename ItemType> class SListView;
class SMultiLineEditableTextBox;

/**
 * The MoonToon tools panel.
 *
 * Layout is target (top) -> tools list (left) -> sections + settings + run (right) -> output (bottom).
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

	FText GetTargetText() const;
	ECheckBoxState IsTargetLocked() const;
	void OnTargetLockChanged(ECheckBoxState NewState);

	// --- Sections -------------------------------------------------------------------------------

	TSharedRef<class ITableRow> OnGenerateSectionRow(FSectionPtr Item, const TSharedRef<class STableViewBase>& OwnerTable);
	FText GetSectionHintText() const;
	FReply OnSelectAllSections();
	FReply OnClearSectionSelection();
	void OnSectionSelectionChanged(FSectionPtr Item, ESelectInfo::Type SelectInfo);

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

	FText GetToolDescriptionText() const;
	FText GetRunButtonText() const;
	bool IsRunEnabled() const;
	FReply OnRunClicked();

	// --- State ----------------------------------------------------------------------------------

	/** Tools are UObjects held strongly: nothing else references them, so GC would take them. */
	TArray<TStrongObjectPtr<UMoonToonTool>> Tools;
	TArray<TWeakObjectPtr<UMoonToonTool>> ToolListItems;
	TWeakObjectPtr<UMoonToonTool> SelectedTool;

	TArray<TWeakObjectPtr<UObject>> TargetMeshes;
	bool bLockTarget = false;

	TArray<FSectionPtr> Sections;

	/** Components carrying a highlight we pushed, so it can be taken back off when things change. */
	TArray<TWeakObjectPtr<class UMeshComponent>> HighlightedComponents;
	bool bIsolateSection = false;

	TSharedPtr<SListView<TWeakObjectPtr<UMoonToonTool>>> ToolListView;
	TSharedPtr<SListView<FSectionPtr>> SectionListView;
	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<SMultiLineEditableTextBox> OutputBox;

	FDelegateHandle AssetSelectionHandle;
	FDelegateHandle LevelSelectionHandle;
};
