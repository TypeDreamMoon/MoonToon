// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MoonToonMaterialLibrary.h"
#include "Widgets/SCompoundWidget.h"

class FAssetThumbnailPool;
class SSearchBox;
class UMaterialInstanceConstant;
template <typename ItemType> class SListView;

/**
 * Parameter editor for a set of material instances, inside the tools panel.
 *
 * Two things the material instance editor cannot do, and the reasons this exists: it edits one
 * instance at a time, and reaching it means leaving the mesh you are working on. Here the rows are
 * the parameters *common to every selected instance*, and every edit is written to all of them --
 * which is what "the eyes are too dark" actually means when a character has four eye materials.
 *
 * Values are written through MoonToonMaterial:: helpers rather than by touching the asset directly,
 * so a drag is a cheap uniform-expression refresh and only the release pays a real asset update.
 */
class SMoonToonMaterialParameters : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMoonToonMaterialParameters) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Points the editor at a new set of instances. Cheap when the set has not actually changed. */
	void SetInstances(const TArray<UMaterialInstanceConstant*>& InInstances);

	/** Re-reads values and override flags from the instances, keeping scroll and collapse state. */
	void Refresh();

private:
	/** One visual row: either a group header or a parameter. */
	struct FRow
	{
		bool bIsGroup = false;
		FName Group;
		int32 NumInGroup = 0;
		TSharedPtr<FMoonToonMaterialParam> Param;
	};
	using FRowPtr = TSharedPtr<FRow>;

	// --- Model ----------------------------------------------------------------------------------

	/** Re-gathers from the instances into Params. */
	void RebuildParams();

	/** Rebuilds the visible row list from Params, applying the filter and collapsed groups. */
	void RebuildRows();

	bool PassesFilter(const FMoonToonMaterialParam& Param) const;

	// --- Rows -----------------------------------------------------------------------------------

	TSharedRef<class ITableRow> OnGenerateRow(FRowPtr Item, const TSharedRef<class STableViewBase>& OwnerTable);
	TSharedRef<SWidget> MakeGroupRow(const FRowPtr& Item);
	TSharedRef<SWidget> MakeParameterRow(const FRowPtr& Item);
	TSharedRef<SWidget> MakeValueWidget(const TSharedPtr<FMoonToonMaterialParam>& Param);

	/** Every group folded, or every group open -- whichever the current state is not. */
	FReply OnToggleAllGroupsClicked();

	/** Right-click menu over a parameter row. Acts on the row the click just selected. */
	TSharedPtr<SWidget> OnRowContextMenu();

	// --- Editing --------------------------------------------------------------------------------

	/** Writes a value to every instance. bCommit publishes it; otherwise it is a drag preview. */
	void WriteValue(const TSharedPtr<FMoonToonMaterialParam>& Param, const FMaterialParameterValue& Value, bool bCommit);

	/**
	 * Opens the undo transaction a write belongs to, if one is not open already.
	 *
	 * A drag has to be captured before its first value lands, or undo restores the value it was
	 * dragged to. Everything else opens and closes one inside a single write.
	 */
	void BeginEdit(const FText& Description);
	void EndEdit();

	void CopyValue(TSharedPtr<FMoonToonMaterialParam> Param);
	void PasteValue(TSharedPtr<FMoonToonMaterialParam> Param);
	bool CanPasteValue(TSharedPtr<FMoonToonMaterialParam> Param) const;
	void ResetToParent(TSharedPtr<FMoonToonMaterialParam> Param);
	void ToggleFavorite(TSharedPtr<FMoonToonMaterialParam> Param);
	bool IsFavorite(const TSharedPtr<FMoonToonMaterialParam>& Param) const;

	ECheckBoxState GetOverrideState(TSharedPtr<FMoonToonMaterialParam> Param) const;
	void OnOverrideChanged(ECheckBoxState NewState, TSharedPtr<FMoonToonMaterialParam> Param);

	TArray<UMaterialInstanceConstant*> GetLiveInstances() const;

	// --- Header actions -------------------------------------------------------------------------

	FText GetSummaryText() const;
	FText GetSummaryTooltip() const;
	EVisibility GetEmptyHintVisibility() const;
	FText GetEmptyHintText() const;

	FReply OnOpenInstancesClicked();
	FReply OnSaveClicked();
	FReply OnResetAllClicked();
	TSharedRef<SWidget> MakeParentMenu();

	// --- Presets ----------------------------------------------------------------------------------

	TSharedRef<SWidget> MakePresetMenu();

	/** The picker behind both "apply" and "update"; bApply picks which way the values move. */
	void BuildPresetPickerMenu(class FMenuBuilder& MenuBuilder, bool bApply);
	void OnPresetPicked(const FAssetData& AssetData, bool bApply);
	void SaveAsNewPreset();

	// --- State ----------------------------------------------------------------------------------

	TArray<TWeakObjectPtr<UMaterialInstanceConstant>> Instances;

	TArray<TSharedPtr<FMoonToonMaterialParam>> Params;
	TArray<FRowPtr> Rows;

	/** Parameters dropped because only part of the selection had them. Shown in the summary. */
	int32 NumSkipped = 0;

	FString Filter;
	bool bOverriddenOnly = false;

	/** Show only parameters the selected instances disagree on. Pointless with one instance. */
	bool bDifferingOnly = false;

	TSet<FName> CollapsedGroups;

	/** Open while a slider is being dragged, so the whole drag is one undo step. */
	TUniquePtr<class FScopedTransaction> ActiveTransaction;

	/** Copy/paste buffer for a single parameter value, kept typed so a paste cannot cross types. */
	TOptional<FMaterialParameterValue> CopiedValue;

	/** Live value during a slider drag, so the spin box does not fight the getter mid-drag. */
	TMap<FName, float> PendingScalars;

	/** Shared by every texture row's asset picker; without one they cannot draw a thumbnail. */
	TSharedPtr<FAssetThumbnailPool> ThumbnailPool;

	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SListView<FRowPtr>> RowListView;
};
