// Copyright Dream Moon. All Rights Reserved.

#include "SMoonToonMaterialParameters.h"

#include "AssetThumbnail.h"
#include "ContentBrowserModule.h"
#include "AssetToolsModule.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IAssetTools.h"
#include "MoonToonMaterialPreset.h"
#include "MoonToonMaterialPresetFactory.h"
#include "MoonToonToolsUserSettings.h"
#include "ScopedTransaction.h"
#include "Textures/SlateIcon.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "IContentBrowserSingleton.h"
#include "InputCoreTypes.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "PropertyCustomizationHelpers.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Engine/Texture.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SMoonToonMaterialParameters"

namespace
{
	/** Parameters with no group of their own, kept together at the end like the material editor does. */
	const FName UngroupedName(TEXT("None"));

	/** Pseudo-group the pinned parameters are listed under, above every real one. */
	static const FName FavoritesGroupName(TEXT("Pinned"));

	FText GroupDisplayName(const FName Group)
	{
		return Group.IsNone() ? LOCTEXT("Ungrouped", "Ungrouped") : FText::FromName(Group);
	}

	/** Row heights. Texture rows are taller because they carry a thumbnail. */
	constexpr float StandardRowHeight = 22.0f;
	constexpr float TextureRowHeight = 40.0f;

	/** Thumbnails are only ever this big here, so the pool can be small. */
	constexpr int32 ThumbnailSize = 32;
}

void SMoonToonMaterialParameters::Construct(const FArguments& InArgs)
{
	ThumbnailPool = MakeShared<FAssetThumbnailPool>(24);

	ChildSlot
	[
		SNew(SVerticalBox)

		// --- Header -----------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.0f, 0.0f, 2.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SMoonToonMaterialParameters::GetSummaryText)
				.ToolTipText(this, &SMoonToonMaterialParameters::GetSummaryTooltip)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
				.ToolTipText(LOCTEXT("SetParentTooltip",
					"Re-parent every selected instance. Overrides whose parameter the new parent also "
					"has are kept; the rest are cleared, and the report lists them."))
				.OnGetMenuContent(this, &SMoonToonMaterialParameters::MakeParentMenu)
				.ButtonContent()
				[
					SNew(STextBlock).Text(LOCTEXT("SetParent", "Set Parent"))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
				.ToolTipText(LOCTEXT("PresetTooltip",
					"Named sets of parameter values, kept as assets. Unlike copying the overrides off "
					"another instance, a preset outlives the instance it came from."))
				.OnGetMenuContent(this, &SMoonToonMaterialParameters::MakePresetMenu)
				.ButtonContent()
				[
					SNew(STextBlock).Text(LOCTEXT("Preset", "Preset"))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(2.0f, 0.0f))
				.ToolTipText(LOCTEXT("ResetAllTooltip",
					"Drop every override on the selected instances, back to the parent's values."))
				.OnClicked(this, &SMoonToonMaterialParameters::OnResetAllClicked)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Refresh"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(2.0f, 0.0f))
				.ToolTipText(LOCTEXT("OpenInstancesTooltip", "Open the selected instances in the material editor."))
				.OnClicked(this, &SMoonToonMaterialParameters::OnOpenInstancesClicked)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Edit"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(2.0f, 0.0f))
				.ToolTipText(LOCTEXT("SaveTooltip", "Save the instances changed here."))
				.OnClicked(this, &SMoonToonMaterialParameters::OnSaveClicked)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Save"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]

		// --- Filter row -------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.0f, 0.0f, 2.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(SearchBox, SSearchBox)
				.HintText(LOCTEXT("ParamSearchHint", "Search parameters"))
				.OnTextChanged_Lambda([this](const FText& NewText)
				{
					Filter = NewText.ToString();
					RebuildRows();
				})
			]
			// Icon toggles rather than labelled buttons: the panel is often docked narrow, and a word
			// here was the first thing to get clipped.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.ToolTipText(LOCTEXT("OverriddenOnlyTooltip",
					"Show only parameters the instances actually override -- usually the short list "
					"that makes them different from the base material."))
				.Padding(FMargin(4.0f, 2.0f))
				.IsChecked_Lambda([this]()
				{
					return bOverriddenOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
				{
					bOverriddenOnly = (NewState == ECheckBoxState::Checked);
					RebuildRows();
				})
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Filter"))
					.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.ToolTipText(LOCTEXT("DifferingOnlyTooltip",
					"Show only parameters the selected instances disagree on -- the list of what makes "
					"them different from each other."))
				.Padding(FMargin(6.0f, 2.0f))
				// Nothing can differ when there is one instance, and a filter that always empties the
				// list is worse than no filter.
				.IsEnabled_Lambda([this]() { return GetLiveInstances().Num() > 1; })
				.IsChecked_Lambda([this]()
				{
					return bDifferingOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
				{
					bDifferingOnly = (NewState == ECheckBoxState::Checked);
					RebuildRows();
				})
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DifferingOnly", "≠"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(2.0f, 0.0f))
				.ToolTipText(LOCTEXT("ToggleAllGroupsTooltip", "Fold or unfold every group."))
				.OnClicked(this, &SMoonToonMaterialParameters::OnToggleAllGroupsClicked)
				[
					SNew(SImage)
					.Image_Lambda([this]()
					{
						return FAppStyle::GetBrush(CollapsedGroups.Num() > 0
							? "Icons.ChevronDown" : "Icons.ChevronUp");
					})
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]

		// --- Rows -------------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SAssignNew(RowListView, SListView<FRowPtr>)
				.ListItemsSource(&Rows)
				// Single, not None: the right-click menu needs to know which row it was opened over, and
			// the selection is how a table row reports that.
			.SelectionMode(ESelectionMode::Single)
			.OnContextMenuOpening(this, &SMoonToonMaterialParameters::OnRowContextMenu)
				.OnGenerateRow(this, &SMoonToonMaterialParameters::OnGenerateRow)
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.Padding(12.0f)
			[
				SNew(STextBlock)
				.Text(this, &SMoonToonMaterialParameters::GetEmptyHintText)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Justification(ETextJustify::Center)
				.AutoWrapText(true)
				.Visibility(this, &SMoonToonMaterialParameters::GetEmptyHintVisibility)
			]
		]
	];
}

// --- Model --------------------------------------------------------------------------------------

void SMoonToonMaterialParameters::SetInstances(const TArray<UMaterialInstanceConstant*>& InInstances)
{
	// Same set, same rows: the section selection changing to an identical material list should not
	// throw away the filter, the collapse state or the scroll position.
	bool bSame = (InInstances.Num() == Instances.Num());
	if (bSame)
	{
		for (int32 Index = 0; Index < InInstances.Num(); ++Index)
		{
			if (Instances[Index].Get() != InInstances[Index])
			{
				bSame = false;
				break;
			}
		}
	}
	if (bSame)
	{
		return;
	}

	Instances.Reset();
	for (UMaterialInstanceConstant* Instance : InInstances)
	{
		if (Instance)
		{
			Instances.Add(Instance);
		}
	}
	PendingScalars.Reset();
	Refresh();
}

void SMoonToonMaterialParameters::Refresh()
{
	RebuildParams();
	RebuildRows();
}

TArray<UMaterialInstanceConstant*> SMoonToonMaterialParameters::GetLiveInstances() const
{
	TArray<UMaterialInstanceConstant*> Live;
	for (const TWeakObjectPtr<UMaterialInstanceConstant>& Weak : Instances)
	{
		if (UMaterialInstanceConstant* Instance = Weak.Get())
		{
			Live.Add(Instance);
		}
	}
	return Live;
}

void SMoonToonMaterialParameters::RebuildParams()
{
	TArray<FMoonToonMaterialParam> Gathered;
	MoonToonMaterial::GatherSharedParameters(GetLiveInstances(), Gathered, NumSkipped);

	Params.Reset(Gathered.Num());
	for (FMoonToonMaterialParam& Param : Gathered)
	{
		Params.Add(MakeShared<FMoonToonMaterialParam>(MoveTemp(Param)));
	}
}

bool SMoonToonMaterialParameters::PassesFilter(const FMoonToonMaterialParam& Param) const
{
	if (bOverriddenOnly && Param.NumOverriding == 0)
	{
		return false;
	}
	if (bDifferingOnly && Param.bSameValue)
	{
		return false;
	}
	if (Filter.IsEmpty())
	{
		return true;
	}
	// Group as well as name: typing "diffuse" should bring up the whole Diffuse block.
	return Param.Info.Name.ToString().Contains(Filter) || Param.Group.ToString().Contains(Filter);
}

void SMoonToonMaterialParameters::RebuildRows()
{
	Rows.Reset();

	// Pinned parameters first, lifted out of their own groups rather than shown twice. Twelve of the
	// two hundred are the ones anybody touches, and they are never in the same group.
	TArray<TSharedPtr<FMoonToonMaterialParam>> Pinned;
	for (const TSharedPtr<FMoonToonMaterialParam>& Param : Params)
	{
		if (IsFavorite(Param) && PassesFilter(*Param))
		{
			Pinned.Add(Param);
		}
	}

	if (Pinned.Num() > 0)
	{
		FRowPtr Header = MakeShared<FRow>();
		Header->bIsGroup = true;
		Header->Group = FavoritesGroupName;
		Header->NumInGroup = Pinned.Num();
		Rows.Add(Header);

		if (!CollapsedGroups.Contains(FavoritesGroupName))
		{
			for (const TSharedPtr<FMoonToonMaterialParam>& Param : Pinned)
			{
				FRowPtr Row = MakeShared<FRow>();
				Row->Group = FavoritesGroupName;
				Row->Param = Param;
				Rows.Add(Row);
			}
		}
	}

	FName CurrentGroup;
	bool bHasGroup = false;
	int32 GroupHeaderIndex = INDEX_NONE;

	for (const TSharedPtr<FMoonToonMaterialParam>& Param : Params)
	{
		if (!PassesFilter(*Param) || IsFavorite(Param))
		{
			continue;
		}

		if (!bHasGroup || Param->Group != CurrentGroup)
		{
			CurrentGroup = Param->Group;
			bHasGroup = true;

			FRowPtr Header = MakeShared<FRow>();
			Header->bIsGroup = true;
			Header->Group = CurrentGroup;
			GroupHeaderIndex = Rows.Add(Header);
		}

		if (GroupHeaderIndex != INDEX_NONE)
		{
			++Rows[GroupHeaderIndex]->NumInGroup;
		}

		if (!CollapsedGroups.Contains(CurrentGroup))
		{
			FRowPtr Row = MakeShared<FRow>();
			Row->Group = CurrentGroup;
			Row->Param = Param;
			Rows.Add(Row);
		}
	}

	if (RowListView.IsValid())
	{
		RowListView->RequestListRefresh();
	}
}

// --- Rows ---------------------------------------------------------------------------------------

TSharedRef<ITableRow> SMoonToonMaterialParameters::OnGenerateRow(FRowPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	// Parameter rows stripe, so a long list of similar-looking rows stays readable across the gap
	// between the name and its value. Group headers paint their own background and must not.
	return SNew(STableRow<FRowPtr>, OwnerTable)
		.Style(FAppStyle::Get(), Item->bIsGroup ? "TableView.Row" : "TableView.AlternatingRow")
		.Padding(FMargin(0.0f, Item->bIsGroup ? 2.0f : 0.0f))
		[
			Item->bIsGroup ? MakeGroupRow(Item) : MakeParameterRow(Item)
		];
}

TSharedRef<SWidget> SMoonToonMaterialParameters::MakeGroupRow(const FRowPtr& Item)
{
	const FName Group = Item->Group;

	// Same brushes the details panel uses for its categories, so a group here reads as one.
	return SNew(SBorder)
		.BorderImage_Lambda([this, Group]()
		{
			return FAppStyle::GetBrush(CollapsedGroups.Contains(Group)
				? "DetailsView.CollapsedCategory" : "DetailsView.CategoryTop");
		})
		.Padding(FMargin(0.0f))
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "NoBorder")
			.ContentPadding(FMargin(4.0f, 3.0f))
			.HAlign(HAlign_Fill)
			.OnClicked_Lambda([this, Group]()
			{
				if (CollapsedGroups.Contains(Group))
				{
					CollapsedGroups.Remove(Group);
				}
				else
				{
					CollapsedGroups.Add(Group);
				}
				RebuildRows();
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SImage)
					.Image_Lambda([this, Group]()
					{
						return FAppStyle::GetBrush(CollapsedGroups.Contains(Group)
							? "Icons.ChevronRight" : "Icons.ChevronDown");
					})
					.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(GroupDisplayName(Group))
					.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 2.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::AsNumber(Item->NumInGroup))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		];
}

FReply SMoonToonMaterialParameters::OnToggleAllGroupsClicked()
{
	if (CollapsedGroups.Num() > 0)
	{
		CollapsedGroups.Reset();
	}
	else
	{
		for (const TSharedPtr<FMoonToonMaterialParam>& Param : Params)
		{
			CollapsedGroups.Add(Param->Group);
		}
	}
	RebuildRows();
	return FReply::Handled();
}

TSharedRef<SWidget> SMoonToonMaterialParameters::MakeParameterRow(const FRowPtr& Item)
{
	const TSharedPtr<FMoonToonMaterialParam> Param = Item->Param;

	// The tooltip carries the authored description plus where the value is coming from -- with
	// several instances selected, "2 of 4 override this" is the thing you actually need to know.
	FText NameTooltip = FText::Format(
		LOCTEXT("ParamTooltip", "{0}{1}"),
		Param->Description.IsEmpty()
			? FText::FromName(Param->Info.Name)
			: Param->Description,
		Param->NumOverriding == 0
			? LOCTEXT("InheritedSuffix", "\n\n(inherited from the parent)")
			: (Param->IsOverriddenByAll()
				? LOCTEXT("OverriddenSuffix", "\n\n(overridden here)")
				: FText::Format(LOCTEXT("PartlyOverriddenSuffix", "\n\n(overridden on {0} of {1} instances)"),
					FText::AsNumber(Param->NumOverriding), FText::AsNumber(Param->NumInstances))));

	const bool bIsTexture = (Param->Type == EMaterialParameterType::Texture);

	return SNew(SBox)
		.MinDesiredHeight(bIsTexture ? TextureRowHeight : StandardRowHeight)
		[
			SNew(SHorizontalBox)
			// A coloured gutter down the left of any row this instance actually overrides. The checkbox
			// says the same thing, but only when you look straight at it; this is what makes an
			// overridden parameter findable while scrolling past two hundred of them.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(3.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("WhiteBrush"))
					.ColorAndOpacity_Lambda([Param]()
					{
						return Param->IsOverriddenByAll()
							? FSlateColor(FStyleColors::AccentBlue)
							: FSlateColor(FStyleColors::Warning);
					})
					.Visibility_Lambda([Param]()
					{
						return Param->NumOverriding > 0 ? EVisibility::HitTestInvisible : EVisibility::Hidden;
					})
				]
			]
			// Pin. Quiet until it is set: it earns its place on the dozen rows that have it and stays
			// out of the way on the two hundred that do not.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(3.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(1.0f, 0.0f))
				.ToolTipText(LOCTEXT("PinRowTooltip", "Pin this parameter to the top, in every material."))
				.OnClicked_Lambda([this, Param]()
				{
					ToggleFavorite(Param);
					return FReply::Handled();
				})
				[
					SNew(SImage)
					.Image_Lambda([this, Param]()
					{
						return FAppStyle::GetBrush(IsFavorite(Param) ? "Icons.Star" : "Icons.Star.Outline");
					})
					.DesiredSizeOverride(FVector2D(12.0f, 12.0f))
					.ColorAndOpacity_Lambda([this, Param]()
					{
						return IsFavorite(Param)
							? FSlateColor(FStyleColors::AccentYellow)
							: FSlateColor::UseSubduedForeground();
					})
				]
			]
			// Override state. Unchecking it is the only way back to the parent value, so it is the first
			// thing on the row rather than hidden behind a right-click.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(3.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked(this, &SMoonToonMaterialParameters::GetOverrideState, Param)
				.OnCheckStateChanged(this, &SMoonToonMaterialParameters::OnOverrideChanged, Param)
				.ToolTipText(LOCTEXT("OverrideTooltip",
					"Override this parameter on the selected instances. Unchecking returns it to the "
					"parent's value."))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.45f)
			.VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromName(Param->Info.Name))
					.ToolTipText(NameTooltip)
					// Long MoonToon parameter names are the norm; ellipsis beats a hard cut.
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					// Inherited parameters are the background noise here; the overridden ones are what
					// this instance actually says.
					.ColorAndOpacity_Lambda([Param]()
					{
						return Param->NumOverriding > 0
							? FSlateColor::UseForeground()
							: FSlateColor::UseSubduedForeground();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DiffersMarker", "≠"))
					.ToolTipText(LOCTEXT("DiffersTooltip",
						"The selected instances do not agree on this value. Editing it sets all of them."))
					.ColorAndOpacity(FSlateColor(FStyleColors::Warning))
					.Visibility(Param->bSameValue ? EVisibility::Collapsed : EVisibility::Visible)
				]
			]
			// A hairline between the two columns, the same one the details panel draws. Without it the
			// name and the value read as one run of text at a glance.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(1.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("DetailsView.GridLine"))
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.55f)
			.VAlign(VAlign_Center)
			.Padding(6.0f, 2.0f, 6.0f, 2.0f)
			[
				MakeValueWidget(Param)
			]
		];
}

TSharedRef<SWidget> SMoonToonMaterialParameters::MakeValueWidget(const TSharedPtr<FMoonToonMaterialParam>& Param)
{
	switch (Param->Type)
	{
	case EMaterialParameterType::Scalar:
	{
		// Slider bounds come from the parameter's own metadata; typed input stays unbounded, which is
		// what the material instance editor does and what a "0..1" parameter used as a multiplier
		// occasionally needs.
		const bool bHasRange = Param->ScalarMax > Param->ScalarMin;
		const TOptional<float> SliderMin = bHasRange ? TOptional<float>(Param->ScalarMin) : TOptional<float>();
		const TOptional<float> SliderMax = bHasRange ? TOptional<float>(Param->ScalarMax) : TOptional<float>();

		return SNew(SSpinBox<float>)
			.MinValue(TOptional<float>())
			.MaxValue(TOptional<float>())
			.MinSliderValue(SliderMin)
			.MaxSliderValue(SliderMax)
			.Value_Lambda([this, Param]()
			{
				if (const float* Pending = PendingScalars.Find(Param->Info.Name))
				{
					return *Pending;
				}
				return Param->Value.Type == EMaterialParameterType::Scalar ? Param->Value.AsScalar() : 0.0f;
			})
			// Dragging writes to every instance and refreshes the viewport without dirtying anything;
			// the release is what actually commits.
			.OnValueChanged_Lambda([this, Param](float NewValue)
			{
				PendingScalars.Add(Param->Info.Name, NewValue);
				WriteValue(Param, FMaterialParameterValue(NewValue), /*bCommit=*/false);
			})
			.OnValueCommitted_Lambda([this, Param](float NewValue, ETextCommit::Type)
			{
				PendingScalars.Remove(Param->Info.Name);
				WriteValue(Param, FMaterialParameterValue(NewValue), /*bCommit=*/true);
			})
			.MinDesiredWidth(60.0f);
	}

	case EMaterialParameterType::Vector:
	{
		if (Param->bUsedAsChannelMask)
		{
			// A channel mask is one-hot, not a colour: four buttons say what it is far better than a
			// colour swatch showing pure red.
			TSharedRef<SHorizontalBox> Channels = SNew(SHorizontalBox);
			static const TCHAR* Labels[] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };
			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				Channels->AddSlot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 2.0f, 0.0f)
				[
					// The details panel's own R/G/B/A style, tinted per channel like everywhere else.
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "DetailsView.ChannelToggleButton")
					.Padding(FMargin(6.0f, 1.0f))
					.IsChecked_Lambda([Param, Channel]()
					{
						if (Param->Value.Type != EMaterialParameterType::Vector)
						{
							return ECheckBoxState::Unchecked;
						}
						const FLinearColor Mask = Param->Value.AsLinearColor();
						return Mask.Component(Channel) > 0.5f ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this, Param, Channel](ECheckBoxState)
					{
						FLinearColor Mask(0.0f, 0.0f, 0.0f, 0.0f);
						Mask.Component(Channel) = 1.0f;
						WriteValue(Param, FMaterialParameterValue(Mask), /*bCommit=*/true);
					})
					[
						SNew(STextBlock).Text(FText::FromString(Labels[Channel]))
					]
				];
			}
			return Channels;
		}

		return SNew(SBox)
			.HeightOverride(18.0f)
			.MaxDesiredWidth(160.0f)
			.HAlign(HAlign_Fill)
			[
				SNew(SColorBlock)
				.Color_Lambda([Param]()
				{
					return Param->Value.Type == EMaterialParameterType::Vector
						? Param->Value.AsLinearColor()
						: FLinearColor::Black;
				})
				.ShowBackgroundForAlpha(true)
				.AlphaDisplayMode(EColorBlockAlphaDisplayMode::Separate)
				.OnMouseButtonDown_Lambda([this, Param](const FGeometry&, const FPointerEvent& Event) -> FReply
				{
					if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
					{
						return FReply::Unhandled();
					}
					FColorPickerArgs PickerArgs(
						Param->Value.Type == EMaterialParameterType::Vector
							? Param->Value.AsLinearColor()
							: FLinearColor::Black,
						FOnLinearColorValueChanged::CreateLambda([this, Param](FLinearColor NewColor)
						{
							WriteValue(Param, FMaterialParameterValue(NewColor), /*bCommit=*/true);
						}));
					PickerArgs.bUseAlpha = true;
					PickerArgs.bOnlyRefreshOnMouseUp = true;
					PickerArgs.ParentWidget = AsShared();
					OpenColorPicker(PickerArgs);
					return FReply::Handled();
				})
			];
	}

	case EMaterialParameterType::Texture:
		// With a thumbnail pool the entry box draws the actual texture, which is the whole point:
		// "T_White_Linear" and "T_Ramp_Skin_03" are indistinguishable as names and obvious as
		// pictures. Compact size keeps the picker itself on one line next to it.
		return SNew(SObjectPropertyEntryBox)
			.AllowedClass(UTexture::StaticClass())
			.AllowClear(true)
			.DisplayThumbnail(true)
			.ThumbnailPool(ThumbnailPool)
			.ThumbnailSizeOverride(FIntPoint(ThumbnailSize, ThumbnailSize))
			.DisplayCompactSize(true)
			.DisplayUseSelected(true)
			.DisplayBrowse(true)
			.ObjectPath_Lambda([Param]()
			{
				const UTexture* Texture = Param->Value.Type == EMaterialParameterType::Texture
					? Param->Value.Texture : nullptr;
				return Texture ? Texture->GetPathName() : FString();
			})
			.OnObjectChanged_Lambda([this, Param](const FAssetData& AssetData)
			{
				UTexture* Texture = Cast<UTexture>(AssetData.GetAsset());
				WriteValue(Param, FMaterialParameterValue(Texture), /*bCommit=*/true);
			});

	case EMaterialParameterType::StaticSwitch:
		// Left-aligned: a lone checkbox centred in a wide column reads as belonging to nothing.
		return SNew(SBox)
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([Param]()
				{
					if (!Param->bSameValue)
					{
						return ECheckBoxState::Undetermined;
					}
					return (Param->Value.Type == EMaterialParameterType::StaticSwitch && Param->Value.AsStaticSwitch())
						? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, Param](ECheckBoxState NewState)
				{
					WriteValue(Param, FMaterialParameterValue(NewState == ECheckBoxState::Checked), /*bCommit=*/true);
				})
				.ToolTipText(LOCTEXT("StaticSwitchTooltip",
					"Static switch. Changing it recompiles the instance's shaders, so it is slower than "
					"the other rows."))
			];

	default:
		return SNullWidget::NullWidget;
	}
}

// --- Editing ------------------------------------------------------------------------------------

void SMoonToonMaterialParameters::BeginEdit(const FText& Description)
{
	if (ActiveTransaction.IsValid())
	{
		return;
	}

	ActiveTransaction = MakeUnique<FScopedTransaction>(Description);
	// Modify before the first mutation, not after: the transaction records the state it finds.
	for (UMaterialInstanceConstant* Instance : GetLiveInstances())
	{
		Instance->Modify();
	}
}

void SMoonToonMaterialParameters::EndEdit()
{
	ActiveTransaction.Reset();
}

void SMoonToonMaterialParameters::WriteValue(
	const TSharedPtr<FMoonToonMaterialParam>& Param,
	const FMaterialParameterValue& Value,
	bool bCommit)
{
	const TArray<UMaterialInstanceConstant*> Live = GetLiveInstances();
	if (Live.Num() == 0)
	{
		return;
	}

	// A drag opens this on its first tick and keeps it until the release, so the whole drag undoes as
	// one step. A typed value or a checkbox opens and closes it inside this call.
	BeginEdit(FText::Format(
		LOCTEXT("EditParamTransaction", "Edit {0}"), FText::FromName(Param->Info.Name)));

	for (UMaterialInstanceConstant* Instance : Live)
	{
		MoonToonMaterial::ApplyValue(Instance, Param->Type, Param->Info, Value);
	}

	// The row model is what the widgets read, so it has to move with the write -- including during a
	// drag, where nothing else refreshes it.
	Param->Value = Value;
	Param->bSameValue = true;

	if (!bCommit)
	{
		MoonToonMaterial::PreviewEdits(Live);
		return;
	}

	MoonToonMaterial::FinishEdits(Live);
	EndEdit();
	Param->NumOverriding = Param->NumInstances;

	// A static switch changes which parameters exist at all, so the whole list has to come back.
	if (Param->Type == EMaterialParameterType::StaticSwitch)
	{
		Refresh();
	}
}

ECheckBoxState SMoonToonMaterialParameters::GetOverrideState(TSharedPtr<FMoonToonMaterialParam> Param) const
{
	if (!Param.IsValid() || Param->NumOverriding == 0)
	{
		return ECheckBoxState::Unchecked;
	}
	return Param->IsOverriddenByAll() ? ECheckBoxState::Checked : ECheckBoxState::Undetermined;
}

void SMoonToonMaterialParameters::OnOverrideChanged(ECheckBoxState NewState, TSharedPtr<FMoonToonMaterialParam> Param)
{
	const TArray<UMaterialInstanceConstant*> Live = GetLiveInstances();
	if (!Param.IsValid() || Live.Num() == 0)
	{
		return;
	}

	BeginEdit(FText::Format(
		LOCTEXT("OverrideTransaction", "Override {0}"), FText::FromName(Param->Info.Name)));

	if (NewState == ECheckBoxState::Checked)
	{
		// Taking the override on means freezing what the parent currently says, which is the same
		// value the row is already showing.
		for (UMaterialInstanceConstant* Instance : Live)
		{
			MoonToonMaterial::ApplyValue(Instance, Param->Type, Param->Info, Param->Value);
		}
	}
	else
	{
		for (UMaterialInstanceConstant* Instance : Live)
		{
			MoonToonMaterial::ClearOverride(Instance, Param->Type, Param->Info);
		}
	}

	MoonToonMaterial::FinishEdits(Live);
	EndEdit();
	Refresh();
}

// --- Row actions --------------------------------------------------------------------------------

bool SMoonToonMaterialParameters::IsFavorite(const TSharedPtr<FMoonToonMaterialParam>& Param) const
{
	return Param.IsValid() && UMoonToonToolsUserSettings::Get().IsFavorite(Param->Info.Name);
}

void SMoonToonMaterialParameters::ToggleFavorite(TSharedPtr<FMoonToonMaterialParam> Param)
{
	if (!Param.IsValid())
	{
		return;
	}
	UMoonToonToolsUserSettings::Get().ToggleFavorite(Param->Info.Name);
	RebuildRows();
}

void SMoonToonMaterialParameters::CopyValue(TSharedPtr<FMoonToonMaterialParam> Param)
{
	if (!Param.IsValid())
	{
		return;
	}
	CopiedValue = Param->Value;

	// Also as text, because half the time the value is wanted somewhere that is not this panel --
	// a spreadsheet, a bug report, the material editor next to it.
	FString AsText;
	switch (Param->Value.Type)
	{
	case EMaterialParameterType::Scalar:
		AsText = FString::SanitizeFloat(Param->Value.AsScalar());
		break;
	case EMaterialParameterType::Vector:
		AsText = Param->Value.AsLinearColor().ToString();
		break;
	case EMaterialParameterType::Texture:
		AsText = Param->Value.Texture ? Param->Value.Texture->GetPathName() : TEXT("None");
		break;
	case EMaterialParameterType::StaticSwitch:
		AsText = Param->Value.AsStaticSwitch() ? TEXT("true") : TEXT("false");
		break;
	default:
		break;
	}
	if (!AsText.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*AsText);
	}
}

bool SMoonToonMaterialParameters::CanPasteValue(TSharedPtr<FMoonToonMaterialParam> Param) const
{
	// Same type only. A scalar pasted into a texture slot has no meaning, and the value union would
	// happily accept it.
	return Param.IsValid() && CopiedValue.IsSet() && CopiedValue->Type == Param->Type;
}

void SMoonToonMaterialParameters::PasteValue(TSharedPtr<FMoonToonMaterialParam> Param)
{
	if (CanPasteValue(Param))
	{
		WriteValue(Param, CopiedValue.GetValue(), /*bCommit=*/true);
	}
}

void SMoonToonMaterialParameters::ResetToParent(TSharedPtr<FMoonToonMaterialParam> Param)
{
	OnOverrideChanged(ECheckBoxState::Unchecked, Param);
}

TSharedPtr<SWidget> SMoonToonMaterialParameters::OnRowContextMenu()
{
	if (!RowListView.IsValid())
	{
		return nullptr;
	}

	// Right-clicking a row selected it on the way in, which is the only reason the list has selection
	// turned on at all.
	const TArray<FRowPtr> Selected = RowListView->GetSelectedItems();
	if (Selected.Num() != 1 || !Selected[0].IsValid() || Selected[0]->bIsGroup || !Selected[0]->Param.IsValid())
	{
		return nullptr;
	}
	const TSharedPtr<FMoonToonMaterialParam> Param = Selected[0]->Param;

	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	MenuBuilder.BeginSection(NAME_None, FText::FromName(Param->Info.Name));
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("CopyValueEntry", "Copy Value"),
			LOCTEXT("CopyValueTip", "Copy this value, both for pasting into another row and as text."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Clipboard"),
			FUIAction(FExecuteAction::CreateSP(this, &SMoonToonMaterialParameters::CopyValue, Param)));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("PasteValueEntry", "Paste Value"),
			LOCTEXT("PasteValueTip", "Write the copied value here, on every selected instance."),
			// No paste glyph in the editor style; import reads the same way round.
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"),
			FUIAction(
				FExecuteAction::CreateSP(this, &SMoonToonMaterialParameters::PasteValue, Param),
				FCanExecuteAction::CreateSP(this, &SMoonToonMaterialParameters::CanPasteValue, Param)));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("ResetEntry", "Reset to Parent"),
			LOCTEXT("ResetTip", "Drop the override so this parameter follows the parent again."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"),
			FUIAction(
				FExecuteAction::CreateSP(this, &SMoonToonMaterialParameters::ResetToParent, Param),
				FCanExecuteAction::CreateLambda([Param]() { return Param->NumOverriding > 0; })));

		MenuBuilder.AddMenuEntry(
			IsFavorite(Param) ? LOCTEXT("UnpinEntry", "Unpin") : LOCTEXT("PinEntry", "Pin to Top"),
			LOCTEXT("PinTip", "Keep this parameter in a group at the top, in every material, until unpinned."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), IsFavorite(Param) ? "Icons.Star" : "Icons.Star.Outline"),
			FUIAction(FExecuteAction::CreateSP(this, &SMoonToonMaterialParameters::ToggleFavorite, Param)));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("CopyNameEntry", "Copy Parameter Name"),
			LOCTEXT("CopyNameTip", "Copy the name, for a Python call or a material graph search."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Documentation"),
			FUIAction(FExecuteAction::CreateLambda([Param]()
			{
				FPlatformApplicationMisc::ClipboardCopy(*Param->Info.Name.ToString());
			})));
	}
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

// --- Header actions -----------------------------------------------------------------------------

FText SMoonToonMaterialParameters::GetSummaryText() const
{
	const TArray<UMaterialInstanceConstant*> Live = GetLiveInstances();
	if (Live.Num() == 0)
	{
		return FText::GetEmpty();
	}

	const FText Subject = Live.Num() == 1
		? FText::FromString(Live[0]->GetName())
		: FText::Format(LOCTEXT("NInstances", "{0} instances"), FText::AsNumber(Live.Num()));

	if (NumSkipped > 0)
	{
		return FText::Format(LOCTEXT("SummaryWithSkipped", "{0}  --  {1} shared parameters, {2} not on all"),
			Subject, FText::AsNumber(Params.Num()), FText::AsNumber(NumSkipped));
	}
	return FText::Format(LOCTEXT("Summary", "{0}  --  {1} parameters"),
		Subject, FText::AsNumber(Params.Num()));
}

FText SMoonToonMaterialParameters::GetSummaryTooltip() const
{
	const TArray<UMaterialInstanceConstant*> Live = GetLiveInstances();
	if (Live.Num() == 0)
	{
		return FText::GetEmpty();
	}

	FString Text;
	for (const UMaterialInstanceConstant* Instance : Live)
	{
		Text += FString::Printf(TEXT("%s\n    parent: %s\n"),
			*Instance->GetPathName(),
			Instance->Parent ? *Instance->Parent->GetName() : TEXT("<none>"));
	}
	if (NumSkipped > 0)
	{
		Text += FString::Printf(
			TEXT("\n%d parameter(s) hidden: only some of these instances have them, so there is no ")
			TEXT("one value to edit."), NumSkipped);
	}
	return FText::FromString(Text);
}

EVisibility SMoonToonMaterialParameters::GetEmptyHintVisibility() const
{
	return Rows.Num() > 0 ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
}

FText SMoonToonMaterialParameters::GetEmptyHintText() const
{
	if (GetLiveInstances().Num() == 0)
	{
		return LOCTEXT("NoInstances",
			"Select one or more sections above.\n\n"
			"Sections whose material is a material instance can be edited here, all of them at once.");
	}
	if (Params.Num() == 0)
	{
		return LOCTEXT("NoSharedParams",
			"These instances have no parameters in common.\n\n"
			"Selecting sections that share a base material gives something to edit.");
	}
	return LOCTEXT("NoMatchingParams", "Nothing matches the filter.");
}

FReply SMoonToonMaterialParameters::OnOpenInstancesClicked()
{
	if (GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			for (UMaterialInstanceConstant* Instance : GetLiveInstances())
			{
				AssetEditor->OpenEditorForAsset(Instance);
			}
		}
	}
	return FReply::Handled();
}

FReply SMoonToonMaterialParameters::OnSaveClicked()
{
	UMoonToonMaterialLibrary::SaveInstances(GetLiveInstances());
	return FReply::Handled();
}

FReply SMoonToonMaterialParameters::OnResetAllClicked()
{
	UMoonToonMaterialLibrary::ResetOverrides(GetLiveInstances(), TArray<FName>());
	Refresh();
	return FReply::Handled();
}

TSharedRef<SWidget> SMoonToonMaterialParameters::MakeParentMenu()
{
	FAssetPickerConfig PickerConfig;
	PickerConfig.Filter.ClassPaths.Add(UMaterialInterface::StaticClass()->GetClassPathName());
	PickerConfig.Filter.bRecursiveClasses = true;
	PickerConfig.SelectionMode = ESelectionMode::Single;
	PickerConfig.InitialAssetViewType = EAssetViewType::List;
	PickerConfig.bAllowNullSelection = false;
	PickerConfig.bFocusSearchBoxWhenOpened = true;
	PickerConfig.OnAssetSelected = FOnAssetSelected::CreateLambda([this](const FAssetData& AssetData)
	{
		FSlateApplication::Get().DismissAllMenus();

		UMaterialInterface* NewParent = Cast<UMaterialInterface>(AssetData.GetAsset());
		if (!NewParent)
		{
			return;
		}
		UMoonToonMaterialLibrary::SetParentOnInstances(
			GetLiveInstances(), NewParent, /*bClearOrphanedOverrides=*/true);
		Refresh();
	});

	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

	return SNew(SBox)
		.WidthOverride(320.0f)
		.HeightOverride(400.0f)
		[
			ContentBrowserModule.Get().CreateAssetPicker(PickerConfig)
		];
}

TSharedRef<SWidget> SMoonToonMaterialParameters::MakePresetMenu()
{
	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);

	MenuBuilder.BeginSection(NAME_None, LOCTEXT("PresetSection", "Preset"));
	{
		MenuBuilder.AddSubMenu(
			LOCTEXT("ApplyPreset", "Apply Preset..."),
			LOCTEXT("ApplyPresetTip",
				"Write a preset's values onto every selected instance. Parameters this material does "
				"not have are skipped and named in the report."),
			FNewMenuDelegate::CreateSP(this, &SMoonToonMaterialParameters::BuildPresetPickerMenu, /*bApply=*/true),
			/*bInOpenSubMenuOnClick=*/false,
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Download"));

		MenuBuilder.AddSubMenu(
			LOCTEXT("UpdatePreset", "Update Preset..."),
			LOCTEXT("UpdatePresetTip",
				"Replace an existing preset's contents with what the selected instances say now."),
			FNewMenuDelegate::CreateSP(this, &SMoonToonMaterialParameters::BuildPresetPickerMenu, /*bApply=*/false),
			/*bInOpenSubMenuOnClick=*/false,
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Save"));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("NewPreset", "Save as New Preset..."),
			LOCTEXT("NewPresetTip",
				"Capture the overridden values the selected instances agree on into a new preset asset."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.PlusCircle"),
			FUIAction(FExecuteAction::CreateSP(this, &SMoonToonMaterialParameters::SaveAsNewPreset)));
	}
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

void SMoonToonMaterialParameters::BuildPresetPickerMenu(FMenuBuilder& MenuBuilder, bool bApply)
{
	FAssetPickerConfig PickerConfig;
	PickerConfig.Filter.ClassPaths.Add(UMoonToonMaterialPreset::StaticClass()->GetClassPathName());
	PickerConfig.Filter.bRecursiveClasses = true;
	PickerConfig.SelectionMode = ESelectionMode::Single;
	PickerConfig.InitialAssetViewType = EAssetViewType::List;
	PickerConfig.bAllowNullSelection = false;
	PickerConfig.bFocusSearchBoxWhenOpened = true;
	PickerConfig.OnAssetSelected = FOnAssetSelected::CreateSP(
		this, &SMoonToonMaterialParameters::OnPresetPicked, bApply);

	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

	MenuBuilder.AddWidget(
		SNew(SBox)
		.WidthOverride(320.0f)
		.HeightOverride(400.0f)
		[
			ContentBrowserModule.Get().CreateAssetPicker(PickerConfig)
		],
		FText::GetEmpty(),
		/*bNoIndent=*/true);
}

void SMoonToonMaterialParameters::OnPresetPicked(const FAssetData& AssetData, bool bApply)
{
	FSlateApplication::Get().DismissAllMenus();

	UMoonToonMaterialPreset* Preset = Cast<UMoonToonMaterialPreset>(AssetData.GetAsset());
	const TArray<UMaterialInstanceConstant*> Live = GetLiveInstances();
	if (!Preset || Live.Num() == 0)
	{
		return;
	}

	// Both directions report through the same channel the Content Browser actions use, so the result
	// lands in the panel's output pane rather than nowhere.
	const FString Report = bApply
		? UMoonToonMaterialLibrary::ApplyPreset(Preset, Live)
		: UMoonToonMaterialLibrary::CaptureToPreset(Preset, Live, /*bOverriddenOnly=*/true);
	MoonToonMaterial::OnReport().Broadcast(Report);

	if (bApply)
	{
		Refresh();
	}
}

void SMoonToonMaterialParameters::SaveAsNewPreset()
{
	const TArray<UMaterialInstanceConstant*> Live = GetLiveInstances();
	if (Live.Num() == 0)
	{
		return;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UMoonToonMaterialPresetFactory* Factory = NewObject<UMoonToonMaterialPresetFactory>();

	// The dialog decides the name and the folder; anything else would guess at both.
	UObject* Created = AssetTools.CreateAssetWithDialog(UMoonToonMaterialPreset::StaticClass(), Factory);
	if (UMoonToonMaterialPreset* Preset = Cast<UMoonToonMaterialPreset>(Created))
	{
		const FString Report = UMoonToonMaterialLibrary::CaptureToPreset(Preset, Live, /*bOverriddenOnly=*/true);
		MoonToonMaterial::OnReport().Broadcast(Report);
	}
}

#undef LOCTEXT_NAMESPACE
