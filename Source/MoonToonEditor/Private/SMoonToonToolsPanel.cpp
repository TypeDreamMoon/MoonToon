// Copyright Dream Moon. All Rights Reserved.

#include "SMoonToonToolsPanel.h"

#include "ContentBrowserModule.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "IContentBrowserSingleton.h"
#include "Engine/Selection.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IDetailsView.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "Framework/Application/SlateApplication.h"
#include "MoonToonBakeTools.h"
#include "MoonToonCharacterMasterTool.h"
#include "MoonToonMaterialLibrary.h"
#include "MoonToonMaterialMenus.h"
#include "MoonToonMeshInfoTool.h"
#include "MoonToonNormalPreviewTool.h"
#include "MoonToonOutlineAlphaTool.h"
#include "MoonToonOutlineSetupTool.h"
#include "MoonToonStrandPreviewActor.h"
#include "MoonToonStrandTangentTool.h"
#include "MoonToonTool.h"
#include "PropertyEditorModule.h"
#include "SMoonToonMaterialParameters.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateIconFinder.h"
#include "Styling/StyleColors.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Textures/SlateIcon.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SMoonToonToolsPanel"

DEFINE_LOG_CATEGORY_STATIC(LogMoonToonTools, Log, All);

namespace MoonToonToolsPanel
{
	/** Section list columns. The row widget and the header have to agree on these names. */
	static const FName ColumnIndex(TEXT("Index"));
	static const FName ColumnSlot(TEXT("Slot"));
	static const FName ColumnMaterial(TEXT("Material"));
	static const FName ColumnTriangles(TEXT("Triangles"));
	static const FName ColumnOpen(TEXT("Open"));

	/** Pulls every static/skeletal mesh asset referenced by an actor's components. */
	static void CollectMeshesFromActor(const AActor* Actor, TArray<TWeakObjectPtr<UObject>>& OutMeshes)
	{
		if (!Actor)
		{
			return;
		}

		Actor->ForEachComponent<UMeshComponent>(/*bIncludeFromChildActors=*/true,
			[&OutMeshes](const UMeshComponent* Component)
			{
				UObject* Mesh = nullptr;
				if (const USkeletalMeshComponent* SkeletalComponent = Cast<USkeletalMeshComponent>(Component))
				{
					Mesh = SkeletalComponent->GetSkeletalMeshAsset();
				}
				else if (const UStaticMeshComponent* StaticComponent = Cast<UStaticMeshComponent>(Component))
				{
					Mesh = StaticComponent->GetStaticMesh();
				}

				if (Mesh && !OutMeshes.Contains(Mesh))
				{
					OutMeshes.Add(Mesh);
				}
			});
	}

	/** The mesh asset a component renders, or null for component types this panel does not handle. */
	static UObject* GetComponentMesh(const UMeshComponent* Component)
	{
		if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
		{
			return Skinned->GetSkinnedAsset();
		}
		if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			return Static->GetStaticMesh();
		}
		return nullptr;
	}

	/** A borderless icon button, the size the rest of the editor uses for row and toolbar actions. */
	static TSharedRef<SWidget> MakeIconButton(
		const FName IconName,
		const FText& ToolTip,
		FOnClicked OnClicked,
		const TAttribute<bool>& IsEnabled = true)
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(FMargin(2.0f, 0.0f))
			.ToolTipText(ToolTip)
			.IsEnabled(IsEnabled)
			.OnClicked(OnClicked)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(IconName))
				.ColorAndOpacity(FSlateColor::UseForeground())
			];
	}
}

/**
 * One row of the section list.
 *
 * Multi-column so the four numbers line up under a header the user can resize, and so the material
 * cell can carry its own affordance: the button opens the material without going through the row's
 * selection, because a click that lands on a button never reaches the row underneath.
 */
class SMoonToonSectionRow : public SMultiColumnTableRow<TSharedPtr<FMoonToonSectionInfo>>
{
public:
	DECLARE_DELEGATE_OneParam(FOnOpenMaterial, UMaterialInterface*);

	SLATE_BEGIN_ARGS(SMoonToonSectionRow) {}
		SLATE_ARGUMENT(TSharedPtr<FMoonToonSectionInfo>, Section)
		SLATE_EVENT(FOnOpenMaterial, OnOpenMaterial)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
	{
		Section = InArgs._Section;
		OnOpenMaterial = InArgs._OnOpenMaterial;

		SMultiColumnTableRow::Construct(FTableRowArgs().Padding(FMargin(0.0f, 1.0f)), OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Section.IsValid())
		{
			return SNullWidget::NullWidget;
		}

		if (ColumnName == MoonToonToolsPanel::ColumnIndex)
		{
			return SNew(SBox)
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Right)
				.Padding(FMargin(4.0f, 2.0f))
				[
					SNew(STextBlock)
					.Text(FText::AsNumber(Section->MaterialIndex))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				];
		}

		if (ColumnName == MoonToonToolsPanel::ColumnSlot)
		{
			const bool bHasSlotName = !Section->SlotName.IsNone();
			return SNew(SBox)
				.VAlign(VAlign_Center)
				.Padding(FMargin(6.0f, 2.0f))
				[
					SNew(STextBlock)
					.Text(bHasSlotName
						? FText::FromName(Section->SlotName)
						: LOCTEXT("UnnamedSlot", "<no slot>"))
					// A section with no slot name is geometry pointing past the material array, which
					// GetSections surfaces on purpose rather than dropping. Say so quietly.
					.ColorAndOpacity(bHasSlotName ? FSlateColor::UseForeground() : FSlateColor(FStyleColors::Warning))
				];
		}

		if (ColumnName == MoonToonToolsPanel::ColumnMaterial)
		{
			const UMaterialInterface* Material = Section->Material.Get();
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(6.0f, 2.0f, 3.0f, 2.0f))
				[
					SNew(SImage)
					.Image(Material
						? FSlateIconFinder::FindIconBrushForClass(Material->GetClass())
						: FAppStyle::GetBrush("Icons.Denied"))
					.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Section->MaterialName))
					.ToolTipText(Material
						? FText::FromString(Material->GetPathName())
						: LOCTEXT("NoMaterialTooltip", "This slot has no material assigned."))
					.ColorAndOpacity(Material ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground())
				];
		}

		if (ColumnName == MoonToonToolsPanel::ColumnTriangles)
		{
			return SNew(SBox)
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Right)
				.Padding(FMargin(4.0f, 2.0f))
				[
					SNew(STextBlock)
					.Text(FText::AsNumber(Section->NumTriangles))
					// Zero triangles means the slot exists but this LOD has no geometry in it.
					.ColorAndOpacity(Section->NumTriangles > 0
						? FSlateColor::UseSubduedForeground()
						: FSlateColor(FStyleColors::Warning))
				];
		}

		if (ColumnName == MoonToonToolsPanel::ColumnOpen)
		{
			return SNew(SBox)
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Center)
				[
					MoonToonToolsPanel::MakeIconButton(
						TEXT("Icons.Edit"),
						LOCTEXT("OpenMaterialTooltip", "Open this material. Double-clicking the row does the same."),
						FOnClicked::CreateSP(this, &SMoonToonSectionRow::HandleOpenClicked),
						Section->Material.IsValid())
				];
		}

		return SNullWidget::NullWidget;
	}

private:
	FReply HandleOpenClicked()
	{
		if (Section.IsValid())
		{
			OnOpenMaterial.ExecuteIfBound(Section->Material.Get());
		}
		return FReply::Handled();
	}

	TSharedPtr<FMoonToonSectionInfo> Section;
	FOnOpenMaterial OnOpenMaterial;
};

void SMoonToonToolsPanel::Construct(const FArguments& InArgs)
{
	BuildToolList();

	FPropertyEditorModule& PropertyModule =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bAllowSearch = false;
	DetailsArgs.bHideSelectionTip = true;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsView = PropertyModule.CreateDetailView(DetailsArgs);

	// Follow both selection surfaces: assets picked in the content browser, and meshes used by
	// actors picked in the level. The second is how a character is usually reached in practice.
	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	AssetSelectionHandle = ContentBrowserModule.GetOnAssetSelectionChanged().AddSP(
		this, &SMoonToonToolsPanel::OnAssetSelectionChanged);
	LevelSelectionHandle = USelection::SelectionChangedEvent.AddSP(
		this, &SMoonToonToolsPanel::OnLevelSelectionChanged);

	// The Content Browser batch actions have nowhere to print a report; this panel does.
	ReportHandle = MoonToonMaterial::OnReport().AddSP(this, &SMoonToonToolsPanel::OnExternalReport);

	ChildSlot
	[
		SNew(SVerticalBox)

		// --- Target bar -------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f, 4.0f, 4.0f, 2.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
			.Padding(FMargin(8.0f, 5.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SImage)
					.Image(this, &SMoonToonToolsPanel::GetTargetIcon)
					.DesiredSizeOverride(FVector2D(16.0f, 16.0f))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SMoonToonToolsPanel::GetTargetText)
					.ToolTipText(this, &SMoonToonToolsPanel::GetTargetTooltipText)
					.Font(FAppStyle::GetFontStyle("BoldFont"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					MoonToonToolsPanel::MakeIconButton(
						TEXT("Icons.BrowseContent"),
						LOCTEXT("BrowseTargetTooltip", "Show this mesh in the Content Browser."),
						FOnClicked::CreateSP(this, &SMoonToonToolsPanel::OnBrowseToTargetClicked),
						TAttribute<bool>(this, &SMoonToonToolsPanel::HasTarget))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					MoonToonToolsPanel::MakeIconButton(
						TEXT("Icons.Edit"),
						LOCTEXT("OpenTargetTooltip", "Open this mesh in its asset editor."),
						FOnClicked::CreateSP(this, &SMoonToonToolsPanel::OnOpenTargetClicked),
						TAttribute<bool>(this, &SMoonToonToolsPanel::HasTarget))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					MoonToonToolsPanel::MakeIconButton(
						TEXT("Icons.Refresh"),
						LOCTEXT("RefreshTooltip",
							"Re-read the section list. Needed after changing material slots elsewhere -- "
							"the panel follows the selection, not the asset."),
						FOnClicked::CreateSP(this, &SMoonToonToolsPanel::OnRefreshClicked),
						TAttribute<bool>(this, &SMoonToonToolsPanel::HasTarget))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
					.IsChecked(this, &SMoonToonToolsPanel::IsTargetLocked)
					.OnCheckStateChanged(this, &SMoonToonToolsPanel::OnTargetLockChanged)
					.ToolTipText(LOCTEXT("LockTooltip",
						"Stop following the selection, so the target survives clicking around the level "
						"or the Content Browser."))
					.Padding(FMargin(5.0f, 2.0f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							SNew(SImage)
							.Image_Lambda([this]()
							{
								return FAppStyle::GetBrush(bLockTarget ? "Icons.Lock" : "Icons.Unlock");
							})
							.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock).Text(LOCTEXT("Lock", "Lock"))
						]
					]
				]
			]
		]

		// --- Body over output -------------------------------------------------------------------
		//
		// Everything below the target bar is splitters, because which part needs the room changes
		// with the job: reading a 30-section character wants the section list, tuning the strand
		// ellipsoid wants the settings, and reading a mesh-info report wants the output.
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(4.0f, 2.0f, 4.0f, 4.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Vertical)
			.PhysicalSplitterHandleSize(2.0f)

			+ SSplitter::Slot()
			.Value(0.68f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Horizontal)
				.PhysicalSplitterHandleSize(2.0f)

				// --- Tools ----------------------------------------------------------------------
				+ SSplitter::Slot()
				.Value(0.26f)
				.MinSize(120.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(FMargin(2.0f))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(2.0f, 2.0f, 2.0f, 4.0f)
						[
							SAssignNew(ToolSearchBox, SSearchBox)
							.HintText(LOCTEXT("ToolSearchHint", "Search tools"))
							.OnTextChanged(this, &SMoonToonToolsPanel::OnToolSearchTextChanged)
						]
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SAssignNew(ToolListView, SListView<TWeakObjectPtr<UMoonToonTool>>)
							.ListItemsSource(&FilteredTools)
							.SelectionMode(ESelectionMode::Single)
							.OnGenerateRow(this, &SMoonToonToolsPanel::OnGenerateToolRow)
							.OnSelectionChanged(this, &SMoonToonToolsPanel::OnToolSelectionChanged)
						]
					]
				]

				+ SSplitter::Slot()
				.Value(0.74f)
				[
					SNew(SSplitter)
					.Orientation(Orient_Vertical)
					.PhysicalSplitterHandleSize(2.0f)

					// --- Sections -----------------------------------------------------------
					+ SSplitter::Slot()
					.Value(0.4f)
					.MinSize(80.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						.Padding(FMargin(4.0f, 3.0f, 4.0f, 4.0f))
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(2.0f, 0.0f, 0.0f, 4.0f)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								.Padding(0.0f, 0.0f, 8.0f, 0.0f)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("SectionsTitle", "Sections"))
									.Font(FAppStyle::GetFontStyle("BoldFont"))
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								.Padding(0.0f, 0.0f, 8.0f, 0.0f)
								[
									SNew(STextBlock)
									.Text(this, &SMoonToonToolsPanel::GetSectionHintText)
									.ColorAndOpacity(this, &SMoonToonToolsPanel::GetSectionHintColor)
									.ToolTipText(LOCTEXT("SectionsTooltip",
										"The section filter handed to the tool. Selecting nothing runs over "
										"every section.\n\nRight-click a row to assign a material, make "
										"instances, or open the one it has; double-click opens it.\n\nThe "
										"viewport highlight can only show one section at a time, so with "
										"several selected it shows the last one clicked."))
								]
								+ SHorizontalBox::Slot()
								.FillWidth(1.0f)
								.VAlign(VAlign_Center)
								.Padding(0.0f, 0.0f, 6.0f, 0.0f)
								[
									SNew(SSearchBox)
									.HintText(LOCTEXT("SectionSearchHint", "Filter slots"))
									.ToolTipText(LOCTEXT("SectionSearchTooltip",
										"Narrows the list by slot name or material name. Selecting nothing still "
										"means every section, filtered or not -- use All to select what is shown."))
									.OnTextChanged(this, &SMoonToonToolsPanel::OnSectionSearchTextChanged)
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								.Padding(0.0f, 0.0f, 6.0f, 0.0f)
								[
									SNew(SCheckBox)
									.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
									.IsChecked(this, &SMoonToonToolsPanel::IsIsolateEnabled)
									.OnCheckStateChanged(this, &SMoonToonToolsPanel::OnIsolateChanged)
									.ToolTipText(LOCTEXT("IsolateTooltip",
										"Hide every other section in the viewport instead of just tinting the "
										"selected one. Easier to read on a dense character."))
									.Padding(FMargin(5.0f, 2.0f))
									[
										SNew(STextBlock).Text(LOCTEXT("Isolate", "Isolate"))
									]
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								[
									SNew(SButton)
									.ButtonStyle(FAppStyle::Get(), "SimpleButton")
									.ContentPadding(FMargin(4.0f, 1.0f))
									.ToolTipText(LOCTEXT("SelectAllTooltip", "Select every section."))
									.OnClicked(this, &SMoonToonToolsPanel::OnSelectAllSections)
									[
										SNew(STextBlock)
										.Text(LOCTEXT("SelectAll", "All"))
										.ColorAndOpacity(FSlateColor::UseForeground())
									]
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								[
									SNew(SButton)
									.ButtonStyle(FAppStyle::Get(), "SimpleButton")
									.ContentPadding(FMargin(4.0f, 1.0f))
									.ToolTipText(LOCTEXT("ClearSelTooltip",
										"Clear the selection, which means every section again."))
									.OnClicked(this, &SMoonToonToolsPanel::OnClearSectionSelection)
									[
										SNew(STextBlock)
										.Text(LOCTEXT("ClearSel", "None"))
										.ColorAndOpacity(FSlateColor::UseForeground())
									]
								]
							]
							+ SVerticalBox::Slot()
							.FillHeight(1.0f)
							[
								SNew(SOverlay)
								+ SOverlay::Slot()
								[
									SAssignNew(SectionListView, SListView<FSectionPtr>)
									.ListItemsSource(&Sections)
									.SelectionMode(ESelectionMode::Multi)
									.OnGenerateRow(this, &SMoonToonToolsPanel::OnGenerateSectionRow)
									.OnSelectionChanged(this, &SMoonToonToolsPanel::OnSectionSelectionChanged)
									.OnContextMenuOpening(this, &SMoonToonToolsPanel::OnSectionContextMenu)
									.OnMouseButtonDoubleClick_Lambda([this](FSectionPtr Item)
									{
										if (Item.IsValid())
										{
											OpenMaterialAsset(Item->Material.Get());
										}
									})
									.HeaderRow(
										SNew(SHeaderRow)
										+ SHeaderRow::Column(MoonToonToolsPanel::ColumnIndex)
										.DefaultLabel(LOCTEXT("ColumnIndex", "#"))
										.DefaultTooltip(LOCTEXT("ColumnIndexTooltip",
											"Material index. This is what the tools filter on, not the row order."))
										.FixedWidth(34.0f)
										.HAlignHeader(HAlign_Right)
										+ SHeaderRow::Column(MoonToonToolsPanel::ColumnSlot)
										.DefaultLabel(LOCTEXT("ColumnSlot", "Slot"))
										.FillWidth(0.4f)
										+ SHeaderRow::Column(MoonToonToolsPanel::ColumnMaterial)
										.DefaultLabel(LOCTEXT("ColumnMaterial", "Material"))
										.FillWidth(0.6f)
										+ SHeaderRow::Column(MoonToonToolsPanel::ColumnTriangles)
										.DefaultLabel(LOCTEXT("ColumnTriangles", "Tris"))
										.FixedWidth(64.0f)
										.HAlignHeader(HAlign_Right)
										+ SHeaderRow::Column(MoonToonToolsPanel::ColumnOpen)
										.DefaultLabel(FText::GetEmpty())
										.FixedWidth(24.0f))
								]
								+ SOverlay::Slot()
								.HAlign(HAlign_Center)
								.VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("NoSections",
										"No sections -- select a Static or Skeletal Mesh, or an actor using one."))
									.ColorAndOpacity(FSlateColor::UseSubduedForeground())
									.Visibility_Lambda([this]()
									{
										return Sections.Num() > 0 ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
									})
								]
							]
						]
					]

					// --- Tool settings / material parameters --------------------------------
					//
					// Two panes rather than a third splitter slot: they are alternatives, not things
					// to look at together. Running a bake and dialling in a material instance are
					// separate jobs, and either one wants the whole height while it is happening.
					+ SSplitter::Slot()
					.Value(0.6f)
					.MinSize(120.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						.Padding(FMargin(4.0f))
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 0.0f, 0.0f, 5.0f)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.AutoWidth()
								[
									SNew(SCheckBox)
									.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
									.Padding(FMargin(8.0f, 3.0f))
									.IsChecked(this, &SMoonToonToolsPanel::IsRightTab, 0)
									.OnCheckStateChanged(this, &SMoonToonToolsPanel::OnRightTabChanged, 0)
									[
										SNew(STextBlock).Text(LOCTEXT("ToolTab", "Tool"))
									]
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.Padding(3.0f, 0.0f, 0.0f, 0.0f)
								[
									SNew(SCheckBox)
									.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
									.Padding(FMargin(8.0f, 3.0f))
									.ToolTipText(LOCTEXT("MaterialsTabTooltip",
										"Edit the material instances of the selected sections -- all of "
										"them at once, on the parameters they have in common."))
									.IsChecked(this, &SMoonToonToolsPanel::IsRightTab, 1)
									.OnCheckStateChanged(this, &SMoonToonToolsPanel::OnRightTabChanged, 1)
									[
										SNew(STextBlock).Text(this, &SMoonToonToolsPanel::GetMaterialsTabText)
									]
								]
							]
							+ SVerticalBox::Slot()
							.FillHeight(1.0f)
							[
								SAssignNew(RightSwitcher, SWidgetSwitcher)
								.WidgetIndex_Lambda([this]() { return RightTabIndex; })

								+ SWidgetSwitcher::Slot()
								[
									SNew(SVerticalBox)
									+ SVerticalBox::Slot()
									.AutoHeight()
									.Padding(2.0f, 0.0f, 2.0f, 6.0f)
									[
										SNew(STextBlock)
										.Text(this, &SMoonToonToolsPanel::GetToolDescriptionText)
										.AutoWrapText(true)
										.ColorAndOpacity(FSlateColor::UseSubduedForeground())
									]
									+ SVerticalBox::Slot()
									.FillHeight(1.0f)
									[
										DetailsView.ToSharedRef()
									]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 6.0f, 0.0f, 2.0f)
							[
								SNew(SHorizontalBox)
								.Visibility(this, &SMoonToonToolsPanel::GetDestructiveNoteVisibility)
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								.Padding(2.0f, 0.0f, 4.0f, 0.0f)
								[
									SNew(SImage)
									.Image(FAppStyle::GetBrush("Icons.Warning"))
									.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
								]
								+ SHorizontalBox::Slot()
								.FillWidth(1.0f)
								.VAlign(VAlign_Center)
								[
									// Undo really does not cover these -- writing mesh import data
									// rebuilds the asset. Say it next to the button rather than in a
									// comment nobody reads.
									SNew(STextBlock)
									.Text(LOCTEXT("DestructiveNote",
										"Writes to the asset. Undo will not take it back; re-run to correct."))
									.ColorAndOpacity(FSlateColor::UseSubduedForeground())
									.AutoWrapText(true)
								]
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 4.0f, 0.0f, 0.0f)
							[
								SNew(SButton)
								.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
								.HAlign(HAlign_Center)
								.ContentPadding(FMargin(0.0f, 6.0f))
								.IsEnabled(this, &SMoonToonToolsPanel::IsRunEnabled)
								.ToolTipText(this, &SMoonToonToolsPanel::GetRunDisabledReason)
								.OnClicked(this, &SMoonToonToolsPanel::OnRunClicked)
								[
									SNew(STextBlock)
									.Text(this, &SMoonToonToolsPanel::GetRunButtonText)
									.Font(FAppStyle::GetFontStyle("BoldFont"))
								]
							]
								]

								+ SWidgetSwitcher::Slot()
								[
									SAssignNew(MaterialPanel, SMoonToonMaterialParameters)
								]
							]
						]
					]
				]
			]

			// --- Output -------------------------------------------------------------------------
			+ SSplitter::Slot()
			.Value(0.32f)
			.MinSize(60.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(FMargin(4.0f, 3.0f, 4.0f, 4.0f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(2.0f, 0.0f, 0.0f, 3.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OutputTitle", "Output"))
							.Font(FAppStyle::GetFontStyle("BoldFont"))
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(this, &SMoonToonToolsPanel::GetRunStatusText)
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							MoonToonToolsPanel::MakeIconButton(
								TEXT("Icons.Clipboard"),
								LOCTEXT("CopyOutputTooltip", "Copy the report to the clipboard."),
								FOnClicked::CreateSP(this, &SMoonToonToolsPanel::OnCopyOutputClicked),
								TAttribute<bool>(this, &SMoonToonToolsPanel::HasOutput))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							MoonToonToolsPanel::MakeIconButton(
								TEXT("Icons.Delete"),
								LOCTEXT("ClearOutputTooltip", "Clear the report."),
								FOnClicked::CreateSP(this, &SMoonToonToolsPanel::OnClearOutputClicked),
								TAttribute<bool>(this, &SMoonToonToolsPanel::HasOutput))
						]
					]
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						SAssignNew(OutputBox, SMultiLineEditableTextBox)
						.IsReadOnly(true)
						.AlwaysShowScrollbars(true)
						// Monospace: the mesh-info report is a column-aligned table.
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
						.Text(LOCTEXT("OutputPlaceholder",
							"Select a mesh, pick a tool, press the button.\n\n"
							"Leaving the section list empty means every section."))
					]
				]
			]
		]
	];

	RefreshToolFilter();

	// Seed from whatever is already selected, so the panel is not blank on open.
	OnLevelSelectionChanged(nullptr);

	if (FilteredTools.Num() > 0)
	{
		ToolListView->SetSelection(FilteredTools[0]);
	}
}

SMoonToonToolsPanel::~SMoonToonToolsPanel()
{
	// Closing the panel must not leave a section tinted or the rest of the mesh hidden.
	for (const TWeakObjectPtr<UMeshComponent>& Weak : HighlightedComponents)
	{
		if (UMeshComponent* Component = Weak.Get())
		{
			ApplySectionHighlight(Component, INDEX_NONE, false);
		}
	}
	HighlightedComponents.Reset();

	if (FContentBrowserModule* ContentBrowserModule =
		FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser")))
	{
		ContentBrowserModule->GetOnAssetSelectionChanged().Remove(AssetSelectionHandle);
	}
	USelection::SelectionChangedEvent.Remove(LevelSelectionHandle);
	MoonToonMaterial::OnReport().Remove(ReportHandle);
}

void SMoonToonToolsPanel::BuildToolList()
{
	// Registration order is display order. Adding a tool is this one line plus its own class.
	auto AddTool = [this](UClass* ToolClass)
	{
		UMoonToonTool* Tool = NewObject<UMoonToonTool>(GetTransientPackage(), ToolClass);
		Tools.Emplace(Tool);
		AllTools.Add(Tool);
	};

	AddTool(UMoonToonMeshInfoTool::StaticClass());
	AddTool(UMoonToonNormalPreviewTool::StaticClass());
	AddTool(UMoonToonOutlineSetupTool::StaticClass());
	AddTool(UMoonToonOutlineAlphaTool::StaticClass());
	AddTool(UMoonToonBakeSmoothNormalTool::StaticClass());
	AddTool(UMoonToonBakeFaceForwardTool::StaticClass());
	AddTool(UMoonToonStrandTangentTool::StaticClass());
	AddTool(UMoonToonCharacterMasterTool::StaticClass());
	AddTool(UMoonToonFixBuildSettingsTool::StaticClass());
}

// --- Target -------------------------------------------------------------------------------------

void SMoonToonToolsPanel::OnAssetSelectionChanged(const TArray<FAssetData>& SelectedAssets, bool bIsPrimaryBrowser)
{
	if (bLockTarget)
	{
		return;
	}

	TArray<TWeakObjectPtr<UObject>> NewTargets;
	for (const FAssetData& AssetData : SelectedAssets)
	{
		// Only load what is already in memory: selecting a folder full of meshes should not force
		// every one of them to load just because the panel is open.
		if (UObject* Asset = AssetData.FastGetAsset(/*bLoad=*/false))
		{
			if (MoonToonMesh::IsSupportedMesh(Asset))
			{
				NewTargets.Add(Asset);
			}
		}
		else if (AssetData.IsValid())
		{
			const FTopLevelAssetPath ClassPath = AssetData.AssetClassPath;
			if (ClassPath == UStaticMesh::StaticClass()->GetClassPathName()
				|| ClassPath == USkeletalMesh::StaticClass()->GetClassPathName())
			{
				if (UObject* Loaded = AssetData.GetAsset())
				{
					NewTargets.Add(Loaded);
				}
			}
		}
	}

	if (NewTargets.Num() > 0)
	{
		SetTargets(MoveTemp(NewTargets));
	}
}

void SMoonToonToolsPanel::OnLevelSelectionChanged(UObject* NewSelection)
{
	if (bLockTarget || !GEditor)
	{
		return;
	}

	TArray<TWeakObjectPtr<UObject>> NewTargets;
	if (USelection* ActorSelection = GEditor->GetSelectedActors())
	{
		for (FSelectionIterator It(*ActorSelection); It; ++It)
		{
			AActor* Actor = Cast<AActor>(*It);
			// The strand live-preview actor gets selected on purpose (its transform is the
			// ellipsoid gizmo), but its shell is the engine's basic sphere -- following it would
			// retarget the panel onto /Engine/BasicShapes/Sphere and point every tool at it.
			if (Actor && Actor->IsA<AMoonToonStrandPreviewActor>())
			{
				continue;
			}
			MoonToonToolsPanel::CollectMeshesFromActor(Actor, NewTargets);
		}
	}

	if (NewTargets.Num() > 0)
	{
		SetTargets(MoveTemp(NewTargets));
	}
}

void SMoonToonToolsPanel::SetTargets(TArray<TWeakObjectPtr<UObject>>&& NewTargets)
{
	TargetMeshes = MoveTemp(NewTargets);
	RefreshSections();
}

void SMoonToonToolsPanel::RefreshSections(bool bPreserveSelection)
{
	// Remember the selection as material indices before the rows are thrown away.
	TSet<int32> PreviousSelection;
	if (bPreserveSelection && SectionListView.IsValid())
	{
		for (const FSectionPtr& Section : SectionListView->GetSelectedItems())
		{
			PreviousSelection.Add(Section->MaterialIndex);
		}
	}

	// Sections are listed for the first target only. Multi-select is supported for running tools,
	// but a section list merged across meshes with different material layouts would be a lie.
	AllSections.Reset();
	if (UObject* Mesh = GetPrimaryTarget())
	{
		TArray<FMoonToonSectionInfo> SectionInfos;
		MoonToonMesh::GetSections(Mesh, 0, SectionInfos);
		for (const FMoonToonSectionInfo& Info : SectionInfos)
		{
			AllSections.Add(MakeShared<FMoonToonSectionInfo>(Info));
		}
	}
	ApplySectionFilter();

	if (SectionListView.IsValid())
	{
		SectionListView->ClearSelection();
		SectionListView->RequestListRefresh();

		// Re-select by material index rather than by row pointer: RefreshSections rebuilds the
		// FMoonToonSectionInfo objects every time, so the old shared pointers are dead here.
		if (bPreserveSelection)
		{
			for (const FSectionPtr& Section : Sections)
			{
				if (PreviousSelection.Contains(Section->MaterialIndex))
				{
					SectionListView->SetItemSelection(Section, true);
				}
			}
		}
	}
	RefreshSectionHighlight();
	UpdateMaterialTargets();
}

void SMoonToonToolsPanel::ApplySectionFilter()
{
	Sections.Reset();
	for (const FSectionPtr& Section : AllSections)
	{
		// Slot and material both: on a VRM character the slot is the Japanese or Chinese name from the
		// DCC and the material is the MI named after it, and either one is what somebody types.
		if (SectionFilter.IsEmpty()
			|| Section->SlotName.ToString().Contains(SectionFilter)
			|| Section->MaterialName.Contains(SectionFilter))
		{
			Sections.Add(Section);
		}
	}
}

void SMoonToonToolsPanel::OnSectionSearchTextChanged(const FText& NewText)
{
	SectionFilter = NewText.ToString();

	// Re-filter without re-reading the mesh: on a sixty-four section character, re-reading import
	// data on every keystroke is the difference between a search box and a stutter. The rows survive,
	// so the selection survives with them.
	ApplySectionFilter();
	if (SectionListView.IsValid())
	{
		SectionListView->RequestListRefresh();
	}
}

TArray<int32> SMoonToonToolsPanel::ToMaterialIndices(const TArray<FSectionPtr>& InSections)
{
	TArray<int32> Indices;
	for (const FSectionPtr& Section : InSections)
	{
		if (Section.IsValid())
		{
			Indices.AddUnique(Section->MaterialIndex);
		}
	}
	return Indices;
}

void SMoonToonToolsPanel::AssignMaterialToSections(const FAssetData& AssetData, TArray<int32> MaterialIndices)
{
	FSlateApplication::Get().DismissAllMenus();

	UObject* Mesh = GetPrimaryTarget();
	UMaterialInterface* Material = Cast<UMaterialInterface>(AssetData.GetAsset());
	if (!Mesh || !Material || MaterialIndices.Num() == 0)
	{
		return;
	}

	OnExternalReport(UMoonToonMaterialLibrary::AssignMaterialToMeshSections(Mesh, MaterialIndices, Material));
	RunStatusText = FText::Format(LOCTEXT("AssignStatus", "Assigned {0} to {1} section(s)"),
		FText::FromString(Material->GetName()), FText::AsNumber(MaterialIndices.Num()));
	RefreshSections(/*bPreserveSelection=*/true);
}

void SMoonToonToolsPanel::CreateInstancesForSections(const FAssetData& AssetData, TArray<int32> MaterialIndices)
{
	FSlateApplication::Get().DismissAllMenus();

	UObject* Mesh = GetPrimaryTarget();
	UMaterialInterface* Parent = Cast<UMaterialInterface>(AssetData.GetAsset());
	if (!Mesh || !Parent || MaterialIndices.Num() == 0)
	{
		return;
	}

	OnExternalReport(UMoonToonMaterialLibrary::CreateInstancesForMeshSections(
		Mesh, MaterialIndices, Parent, /*DestinationFolder=*/FString(), /*bReplaceExistingInstances=*/false));
	RunStatusText = FText::Format(LOCTEXT("CreateInstancesStatus", "Created instances under {0}"),
		FText::FromString(Parent->GetName()));
	RefreshSections(/*bPreserveSelection=*/true);
}

UObject* SMoonToonToolsPanel::GetPrimaryTarget() const
{
	return TargetMeshes.Num() > 0 ? TargetMeshes[0].Get() : nullptr;
}

bool SMoonToonToolsPanel::HasTarget() const
{
	return GetPrimaryTarget() != nullptr;
}

FText SMoonToonToolsPanel::GetTargetText() const
{
	if (TargetMeshes.Num() == 0)
	{
		return LOCTEXT("NoTarget", "<nothing selected>");
	}

	const UObject* First = GetPrimaryTarget();
	if (TargetMeshes.Num() == 1)
	{
		return First ? FText::FromString(First->GetName()) : LOCTEXT("StaleTarget", "<target was unloaded>");
	}

	return FText::Format(LOCTEXT("MultiTarget", "{0}  (+{1} more)"),
		First ? FText::FromString(First->GetName()) : LOCTEXT("Unknown", "?"),
		FText::AsNumber(TargetMeshes.Num() - 1));
}

FText SMoonToonToolsPanel::GetTargetTooltipText() const
{
	const UObject* First = GetPrimaryTarget();
	if (!First)
	{
		return LOCTEXT("TargetTooltip",
			"Follows the Content Browser selection, and the meshes used by actors selected in the "
			"level. Lock to keep the current target while selecting something else.");
	}

	if (TargetMeshes.Num() == 1)
	{
		return FText::FromString(First->GetPathName());
	}

	// With several targets the tools run over all of them, but only the first one's sections are
	// listed -- worth spelling out, since the section list looks like it covers everything.
	FString Paths;
	for (const TWeakObjectPtr<UObject>& Target : TargetMeshes)
	{
		if (const UObject* Mesh = Target.Get())
		{
			Paths += Mesh->GetPathName() + LINE_TERMINATOR;
		}
	}
	return FText::Format(LOCTEXT("MultiTargetTooltip",
		"Tools run over all {0} meshes; the section list below is the first one's.\n\n{1}"),
		FText::AsNumber(TargetMeshes.Num()),
		FText::FromString(Paths));
}

const FSlateBrush* SMoonToonToolsPanel::GetTargetIcon() const
{
	const UObject* First = GetPrimaryTarget();
	return First
		? FSlateIconFinder::FindIconBrushForClass(First->GetClass())
		: FAppStyle::GetBrush("Icons.Denied");
}

ECheckBoxState SMoonToonToolsPanel::IsTargetLocked() const
{
	return bLockTarget ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SMoonToonToolsPanel::OnTargetLockChanged(ECheckBoxState NewState)
{
	bLockTarget = (NewState == ECheckBoxState::Checked);
}

FReply SMoonToonToolsPanel::OnOpenTargetClicked()
{
	if (UObject* Mesh = GetPrimaryTarget())
	{
		if (GEditor)
		{
			if (UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				AssetEditor->OpenEditorForAsset(Mesh);
			}
		}
	}
	return FReply::Handled();
}

FReply SMoonToonToolsPanel::OnBrowseToTargetClicked()
{
	TArray<UObject*> Assets;
	for (const TWeakObjectPtr<UObject>& Target : TargetMeshes)
	{
		if (UObject* Mesh = Target.Get())
		{
			Assets.Add(Mesh);
		}
	}

	if (Assets.Num() > 0)
	{
		FContentBrowserModule& ContentBrowserModule =
			FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		ContentBrowserModule.Get().SyncBrowserToAssets(Assets);
	}
	return FReply::Handled();
}

FReply SMoonToonToolsPanel::OnRefreshClicked()
{
	RefreshSections(/*bPreserveSelection=*/true);
	return FReply::Handled();
}

// --- Sections -----------------------------------------------------------------------------------

TSharedRef<ITableRow> SMoonToonToolsPanel::OnGenerateSectionRow(FSectionPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SMoonToonSectionRow, OwnerTable)
		.Section(Item)
		.OnOpenMaterial_Static(&SMoonToonToolsPanel::OpenMaterialAsset);
}

FText SMoonToonToolsPanel::GetSectionHintText() const
{
	if (Sections.Num() == 0)
	{
		return FText::GetEmpty();
	}

	const int32 NumSelected = SectionListView.IsValid() ? SectionListView->GetNumItemsSelected() : 0;
	if (NumSelected == 0)
	{
		return FText::Format(LOCTEXT("SectionsAll", "LOD 0  --  all {0}"), FText::AsNumber(Sections.Num()));
	}
	return FText::Format(LOCTEXT("SectionsSome", "LOD 0  --  {0} of {1} selected"),
		FText::AsNumber(NumSelected), FText::AsNumber(Sections.Num()));
}

FSlateColor SMoonToonToolsPanel::GetSectionHintColor() const
{
	// A filter that is actually on is worth noticing before pressing a button that writes.
	const int32 NumSelected = SectionListView.IsValid() ? SectionListView->GetNumItemsSelected() : 0;
	return NumSelected > 0 ? FSlateColor(FStyleColors::AccentBlue) : FSlateColor::UseSubduedForeground();
}

FReply SMoonToonToolsPanel::OnSelectAllSections()
{
	if (SectionListView.IsValid())
	{
		SectionListView->SetItemSelection(Sections, true);
	}
	return FReply::Handled();
}

FReply SMoonToonToolsPanel::OnClearSectionSelection()
{
	if (SectionListView.IsValid())
	{
		SectionListView->ClearSelection();
	}
	return FReply::Handled();
}

void SMoonToonToolsPanel::OnSectionSelectionChanged(FSectionPtr Item, ESelectInfo::Type SelectInfo)
{
	RefreshSectionHighlight();
	UpdateMaterialTargets();
}

// --- Material parameters ------------------------------------------------------------------------

TArray<UMaterialInstanceConstant*> SMoonToonToolsPanel::GetSelectedMaterialInstances() const
{
	TArray<UMaterialInstanceConstant*> Result;
	if (!SectionListView.IsValid())
	{
		return Result;
	}

	for (const FSectionPtr& Section : SectionListView->GetSelectedItems())
	{
		// Only constants: a plain UMaterial has nothing to override, and a dynamic instance is not
		// an asset. Sections carrying those simply contribute nothing to the editor.
		if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Section->Material.Get()))
		{
			Result.AddUnique(Instance);
		}
	}
	return Result;
}

void SMoonToonToolsPanel::UpdateMaterialTargets()
{
	if (MaterialPanel.IsValid())
	{
		MaterialPanel->SetInstances(GetSelectedMaterialInstances());
	}
}

ECheckBoxState SMoonToonToolsPanel::IsRightTab(int32 TabIndex) const
{
	return RightTabIndex == TabIndex ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SMoonToonToolsPanel::OnRightTabChanged(ECheckBoxState NewState, int32 TabIndex)
{
	// Radio behaviour: clicking the active tab again must not switch it off and leave neither pane.
	if (NewState == ECheckBoxState::Checked)
	{
		RightTabIndex = TabIndex;
	}
}

FText SMoonToonToolsPanel::GetMaterialsTabText() const
{
	const int32 NumInstances = GetSelectedMaterialInstances().Num();
	return NumInstances > 0
		? FText::Format(LOCTEXT("MaterialsTabN", "Materials ({0})"), FText::AsNumber(NumInstances))
		: LOCTEXT("MaterialsTab", "Materials");
}

TArray<SMoonToonToolsPanel::FSectionPtr> SMoonToonToolsPanel::GetSectionsForAction() const
{
	// Deliberately NOT the "empty means everything" rule Run uses. A right-click on blank space
	// under the rows would then offer to open every material on the character at once, which is a
	// lot of windows for a mis-aimed click. No selection, no menu.
	return SectionListView.IsValid() ? SectionListView->GetSelectedItems() : TArray<FSectionPtr>();
}

TSharedPtr<SWidget> SMoonToonToolsPanel::OnSectionContextMenu()
{
	// Right-clicking a row already made it the selection (STableRow selects on the press), so the
	// menu can just act on the selection and stay honest about how many rows it covers.
	const TArray<FSectionPtr> Targets = GetSectionsForAction();
	if (Targets.Num() == 0)
	{
		return nullptr;
	}

	int32 NumWithMaterial = 0;
	for (const FSectionPtr& Section : Targets)
	{
		if (Section.IsValid() && Section->Material.IsValid())
		{
			++NumWithMaterial;
		}
	}

	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	MenuBuilder.BeginSection(NAME_None, FText::Format(
		LOCTEXT("SectionMenuHeading", "{0} section(s)"), FText::AsNumber(Targets.Num())));
	{
		// First entry, because it is the one that does not leave the panel.
		MenuBuilder.AddMenuEntry(
			LOCTEXT("EditParametersHere", "Edit Parameters Here"),
			LOCTEXT("EditParametersHereTip",
				"Edit these instances in the panel's Materials tab, all of them together."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Adjust"),
			FUIAction(
				FExecuteAction::CreateLambda([this]()
				{
					RightTabIndex = 1;
					UpdateMaterialTargets();
				}),
				FCanExecuteAction::CreateLambda([this]()
				{
					return GetSelectedMaterialInstances().Num() > 0;
				})));

		MenuBuilder.AddMenuEntry(
			NumWithMaterial > 1
				? FText::Format(LOCTEXT("OpenMaterialsMulti", "Open {0} Materials"), FText::AsNumber(NumWithMaterial))
				: LOCTEXT("OpenMaterial", "Open Material"),
			LOCTEXT("OpenMaterialTip", "Open the assigned material in the material editor."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
			FUIAction(
				FExecuteAction::CreateSP(this, &SMoonToonToolsPanel::OpenMaterialsFor, Targets),
				FCanExecuteAction::CreateLambda([NumWithMaterial]() { return NumWithMaterial > 0; })));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("BrowseMaterial", "Browse to Material"),
			LOCTEXT("BrowseMaterialTip", "Show the material in the Content Browser."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.BrowseContent"),
			FUIAction(
				FExecuteAction::CreateSP(this, &SMoonToonToolsPanel::BrowseToMaterialsFor, Targets),
				FCanExecuteAction::CreateLambda([NumWithMaterial]() { return NumWithMaterial > 0; })));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("CopyMaterialPath", "Copy Material Path"),
			LOCTEXT("CopyMaterialPathTip", "Copy the material's object path to the clipboard."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Clipboard"),
			FUIAction(
				FExecuteAction::CreateSP(this, &SMoonToonToolsPanel::CopyMaterialPathsFor, Targets),
				FCanExecuteAction::CreateLambda([NumWithMaterial]() { return NumWithMaterial > 0; })));
	}
	MenuBuilder.EndSection();

	MenuBuilder.EndSection();

	// Editing the slot itself, as opposed to the material in it. Only meaningful with a live mesh,
	// and only on the mesh whose sections are listed -- the first target.
	MenuBuilder.BeginSection(NAME_None, LOCTEXT("SlotHeading", "Slots"));
	{
		const TArray<int32> Indices = ToMaterialIndices(Targets);

		MenuBuilder.AddSubMenu(
			FText::Format(LOCTEXT("AssignMaterial", "Assign Material to {0} Slot(s)"), FText::AsNumber(Indices.Num())),
			LOCTEXT("AssignMaterialTip",
				"Point these material slots at one material. Undoable, unlike the mesh bakes."),
			FNewMenuDelegate::CreateSP(this, &SMoonToonToolsPanel::BuildAssignMaterialMenu, Indices),
			/*bInOpenSubMenuOnClick=*/false,
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Use"));

		MenuBuilder.AddSubMenu(
			FText::Format(LOCTEXT("CreateInstances", "Create Instances for {0} Slot(s)"), FText::AsNumber(Indices.Num())),
			LOCTEXT("CreateInstancesTip",
				"Make one material instance per slot, named after the slot, under a parent you pick, and "
				"assign them. Slots that already use an instance are left alone."),
			FNewMenuDelegate::CreateSP(this, &SMoonToonToolsPanel::BuildCreateInstancesMenu, Indices),
			/*bInOpenSubMenuOnClick=*/false,
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.PlusCircle"));
	}
	MenuBuilder.EndSection();

	MenuBuilder.BeginSection(NAME_None, LOCTEXT("SelectionHeading", "Selection"));
	{
		// One material usually covers several slots on a character (all the MASK bits, both eyes),
		// and those are exactly the slots a tool wants to run over together.
		MenuBuilder.AddMenuEntry(
			LOCTEXT("SelectSameMaterial", "Select Sections Sharing This Material"),
			LOCTEXT("SelectSameMaterialTip",
				"Add every other section using the same material to the selection."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Filter"),
			FUIAction(FExecuteAction::CreateSP(this, &SMoonToonToolsPanel::SelectSectionsSharingMaterial)));
	}
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

void SMoonToonToolsPanel::BuildAssignMaterialMenu(FMenuBuilder& MenuBuilder, TArray<int32> MaterialIndices)
{
	MenuBuilder.AddWidget(
		MakeMoonToonAssetPickerMenu(UMaterialInterface::StaticClass(),
			FOnAssetSelected::CreateSP(this, &SMoonToonToolsPanel::AssignMaterialToSections, MaterialIndices)),
		FText::GetEmpty(),
		/*bNoIndent=*/true);
}

void SMoonToonToolsPanel::BuildCreateInstancesMenu(FMenuBuilder& MenuBuilder, TArray<int32> MaterialIndices)
{
	MenuBuilder.AddWidget(
		MakeMoonToonAssetPickerMenu(UMaterialInterface::StaticClass(),
			FOnAssetSelected::CreateSP(this, &SMoonToonToolsPanel::CreateInstancesForSections, MaterialIndices)),
		FText::GetEmpty(),
		/*bNoIndent=*/true);
}

void SMoonToonToolsPanel::OpenMaterialAsset(UMaterialInterface* Material)
{
	if (!Material || !GEditor)
	{
		return;
	}
	if (UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
	{
		AssetEditor->OpenEditorForAsset(Material);
	}
}

void SMoonToonToolsPanel::OpenMaterialsFor(TArray<FSectionPtr> InSections)
{
	// Distinct: a character's slots share materials, and opening the same asset five times just
	// raises the same tab five times.
	TSet<UMaterialInterface*> Opened;
	for (const FSectionPtr& Section : InSections)
	{
		if (!Section.IsValid())
		{
			continue;
		}
		if (UMaterialInterface* Material = Section->Material.Get())
		{
			bool bAlready = false;
			Opened.Add(Material, &bAlready);
			if (!bAlready)
			{
				OpenMaterialAsset(Material);
			}
		}
	}
}

void SMoonToonToolsPanel::BrowseToMaterialsFor(TArray<FSectionPtr> InSections)
{
	TArray<UObject*> Assets;
	for (const FSectionPtr& Section : InSections)
	{
		if (Section.IsValid())
		{
			if (UMaterialInterface* Material = Section->Material.Get())
			{
				Assets.AddUnique(Material);
			}
		}
	}

	if (Assets.Num() > 0)
	{
		FContentBrowserModule& ContentBrowserModule =
			FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		ContentBrowserModule.Get().SyncBrowserToAssets(Assets);
	}
}

void SMoonToonToolsPanel::CopyMaterialPathsFor(TArray<FSectionPtr> InSections)
{
	TArray<FString> Paths;
	for (const FSectionPtr& Section : InSections)
	{
		if (Section.IsValid())
		{
			if (const UMaterialInterface* Material = Section->Material.Get())
			{
				Paths.AddUnique(Material->GetPathName());
			}
		}
	}

	if (Paths.Num() > 0)
	{
		FPlatformApplicationMisc::ClipboardCopy(*FString::Join(Paths, LINE_TERMINATOR));
	}
}

void SMoonToonToolsPanel::SelectSectionsSharingMaterial()
{
	if (!SectionListView.IsValid())
	{
		return;
	}

	TSet<const UMaterialInterface*> Materials;
	for (const FSectionPtr& Section : SectionListView->GetSelectedItems())
	{
		if (Section.IsValid() && Section->Material.IsValid())
		{
			Materials.Add(Section->Material.Get());
		}
	}

	if (Materials.Num() == 0)
	{
		return;
	}

	for (const FSectionPtr& Section : Sections)
	{
		if (Section.IsValid() && Materials.Contains(Section->Material.Get()))
		{
			SectionListView->SetItemSelection(Section, true);
		}
	}
}

ECheckBoxState SMoonToonToolsPanel::IsIsolateEnabled() const
{
	return bIsolateSection ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SMoonToonToolsPanel::OnIsolateChanged(ECheckBoxState NewState)
{
	bIsolateSection = (NewState == ECheckBoxState::Checked);
	RefreshSectionHighlight();
}

// --- Viewport highlight -------------------------------------------------------------------------

void SMoonToonToolsPanel::ApplySectionHighlight(UMeshComponent* Component, int32 MaterialIndex, bool bIsolate)
{
	const int32 PreviewIndex = bIsolate ? MaterialIndex : INDEX_NONE;

	// Each setter reattaches the component, which on a heavy skeletal mesh means tearing down and
	// rebuilding its render state. RefreshSectionHighlight clears then re-applies on every change, so
	// without these guards a click that ends up on the same section still paid four reattaches.
	if (USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
	{
		if (Skinned->GetSelectedEditorMaterial() != MaterialIndex)
		{
			Skinned->SetSelectedEditorMaterial(MaterialIndex);
		}
		if (Skinned->GetMaterialPreview() != PreviewIndex)
		{
			Skinned->SetMaterialPreview(PreviewIndex);
		}
	}
	else if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
	{
		// The static-mesh side exposes the selection as a bare transient UPROPERTY with no setter,
		// so this one needs the render state invalidated by hand.
		if (Static->SelectedEditorMaterial != MaterialIndex)
		{
			Static->SelectedEditorMaterial = MaterialIndex;
			Static->MarkRenderStateDirty();
		}
		if (Static->MaterialIndexPreview != PreviewIndex)
		{
			Static->SetMaterialPreview(PreviewIndex);
		}
	}
}

void SMoonToonToolsPanel::RefreshSectionHighlight()
{
	// Always clear first. The target, the selection, or the level can all change underneath us, and a
	// highlight left on a component we have stopped tracking would never come off.
	for (const TWeakObjectPtr<UMeshComponent>& Weak : HighlightedComponents)
	{
		if (UMeshComponent* Component = Weak.Get())
		{
			ApplySectionHighlight(Component, INDEX_NONE, false);
		}
	}
	HighlightedComponents.Reset();

	if (!SectionListView.IsValid() || !GEditor)
	{
		return;
	}

	const TArray<FSectionPtr> SelectedSections = SectionListView->GetSelectedItems();
	if (SelectedSections.Num() == 0)
	{
		return;
	}

	// The engine's per-section highlight is a single index per component, not a set, so with several
	// sections selected only one can be shown. Use the last one, which is the one just clicked.
	const int32 MaterialIndex = SelectedSections.Last()->MaterialIndex;

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}

	// Find it by mesh asset rather than by the actor that happens to be selected: the target can come
	// from the Content Browser, and several actors may share the mesh. All of them light up.
	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		TInlineComponentArray<UMeshComponent*> Components(*ActorIt);
		for (UMeshComponent* Component : Components)
		{
			UObject* ComponentMesh = MoonToonToolsPanel::GetComponentMesh(Component);
			if (!ComponentMesh)
			{
				continue;
			}
			const bool bIsTarget = TargetMeshes.ContainsByPredicate(
				[ComponentMesh](const TWeakObjectPtr<UObject>& Target) { return Target.Get() == ComponentMesh; });
			if (!bIsTarget)
			{
				continue;
			}

			ApplySectionHighlight(Component, MaterialIndex, bIsolateSection);
			HighlightedComponents.Add(Component);
		}
	}
}

// --- Tools --------------------------------------------------------------------------------------

TSharedRef<ITableRow> SMoonToonToolsPanel::OnGenerateToolRow(TWeakObjectPtr<UMoonToonTool> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const UMoonToonTool* Tool = Item.Get();
	return SNew(STableRow<TWeakObjectPtr<UMoonToonTool>>, OwnerTable)
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 4.0f, 6.0f, 4.0f)
			[
				SNew(SImage)
				// Never NAME_None: a missing brush name is a logged error every repaint.
				.Image(FAppStyle::GetBrush(Tool ? Tool->GetToolIconName() : FName(TEXT("Icons.Adjust"))))
				.DesiredSizeOverride(FVector2D(16.0f, 16.0f))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(0.0f, 4.0f, 6.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(Tool ? Tool->GetToolName() : FText::GetEmpty())
				.ToolTipText(Tool ? Tool->GetToolDescription() : FText::GetEmpty())
			]
		];
}

void SMoonToonToolsPanel::OnToolSelectionChanged(TWeakObjectPtr<UMoonToonTool> Item, ESelectInfo::Type SelectInfo)
{
	// Filtering rebuilds the list, and an item leaving it reports a null selection. Keep showing the
	// tool that was set up rather than blanking the settings the moment the search box is typed in.
	if (!Item.IsValid() && SelectInfo == ESelectInfo::Direct)
	{
		return;
	}

	SelectedTool = Item;
	if (DetailsView.IsValid())
	{
		DetailsView->SetObject(Item.Get());
	}
	LastSeenToolEditSerial = Item.IsValid() ? Item->ExternalEditSerial : 0;
}

void SMoonToonToolsPanel::OnToolSearchTextChanged(const FText& NewText)
{
	ToolFilter = NewText.ToString();
	RefreshToolFilter();
}

void SMoonToonToolsPanel::RefreshToolFilter()
{
	FilteredTools.Reset();
	for (const TWeakObjectPtr<UMoonToonTool>& Weak : AllTools)
	{
		const UMoonToonTool* Tool = Weak.Get();
		if (!Tool)
		{
			continue;
		}
		// Description as well as name: "alpha" should find the outline tool even though the word is
		// only in its blurb.
		const bool bMatches = ToolFilter.IsEmpty()
			|| Tool->GetToolName().ToString().Contains(ToolFilter)
			|| Tool->GetToolDescription().ToString().Contains(ToolFilter);
		if (bMatches)
		{
			FilteredTools.Add(Weak);
		}
	}

	if (ToolListView.IsValid())
	{
		ToolListView->RequestListRefresh();
		if (SelectedTool.IsValid() && FilteredTools.Contains(SelectedTool))
		{
			ToolListView->SetSelection(SelectedTool, ESelectInfo::Direct);
		}
	}
}

void SMoonToonToolsPanel::Tick(const FGeometry& AllottedGeometry, const double CurrentTime, const float DeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, CurrentTime, DeltaTime);

	const UMoonToonTool* Tool = SelectedTool.Get();
	if (Tool && Tool->ExternalEditSerial != LastSeenToolEditSerial)
	{
		LastSeenToolEditSerial = Tool->ExternalEditSerial;
		if (DetailsView.IsValid())
		{
			DetailsView->ForceRefresh();
		}
	}
}

FText SMoonToonToolsPanel::GetToolDescriptionText() const
{
	const UMoonToonTool* Tool = SelectedTool.Get();
	return Tool ? Tool->GetToolDescription() : FText::GetEmpty();
}

FText SMoonToonToolsPanel::GetRunButtonText() const
{
	const UMoonToonTool* Tool = SelectedTool.Get();
	return Tool ? Tool->GetRunLabel() : LOCTEXT("NoTool", "Run");
}

EVisibility SMoonToonToolsPanel::GetDestructiveNoteVisibility() const
{
	const UMoonToonTool* Tool = SelectedTool.Get();
	return (Tool && Tool->IsDestructive()) ? EVisibility::Visible : EVisibility::Collapsed;
}

bool SMoonToonToolsPanel::IsRunEnabled() const
{
	return SelectedTool.IsValid() && HasTarget();
}

FText SMoonToonToolsPanel::GetRunDisabledReason() const
{
	if (!SelectedTool.IsValid())
	{
		return LOCTEXT("RunNoTool", "Pick a tool on the left first.");
	}
	if (!HasTarget())
	{
		return LOCTEXT("RunNoTarget",
			"Nothing to run on. Select a Static or Skeletal Mesh in the Content Browser, or an actor "
			"using one in the level.");
	}

	const int32 NumSelected = SectionListView.IsValid() ? SectionListView->GetNumItemsSelected() : 0;
	return NumSelected > 0
		? FText::Format(LOCTEXT("RunOnSome", "Run over the {0} selected section(s)."), FText::AsNumber(NumSelected))
		: LOCTEXT("RunOnAll", "Run over every section.");
}

FReply SMoonToonToolsPanel::OnRunClicked()
{
	UMoonToonTool* Tool = SelectedTool.Get();
	if (!Tool)
	{
		return FReply::Handled();
	}

	FMoonToonToolContext Context;
	Context.Meshes = TargetMeshes;

	if (SectionListView.IsValid())
	{
		for (const FSectionPtr& Section : SectionListView->GetSelectedItems())
		{
			Context.SectionMaterialIndices.Add(Section->MaterialIndex);
		}
	}

	// Phase timings, so a slow run can be attributed instead of guessed at. Look for [MoonToon] in
	// the Output Log after pressing the button.
	const double RunStart = FPlatformTime::Seconds();

	// No progress dialog, and no FScopedTransaction.
	//
	// The dialog was the whole perceived hang: a run completes inside one game-thread frame, so
	// nothing repaints the dialog while it works. It painted 0%, froze there for the entire run, then
	// vanished -- a sub-second operation wearing a hung progress bar. Even delayed it only postponed
	// the same freeze. Running with no dialog at all is both faster and more honest; the report
	// lands in the output pane the moment it finishes.
	//
	// The transaction is absent for a different reason: writing mesh import data rebuilds the asset,
	// which undo cannot restore, so a transaction would promise a rollback that does not work. Every
	// writing tool is re-runnable, and that is the real recovery path.
	FString Result = Tool->Run(Context);
	const double RunEnd = FPlatformTime::Seconds();

	OutputText = MoveTemp(Result);
	if (OutputBox.IsValid())
	{
		OutputBox->SetText(FText::FromString(OutputText));
	}

	// The header line answers "what am I looking at" for a report that scrolled, and "did it just
	// run" for one that happens to be identical to the last.
	RunStatusText = FText::Format(
		LOCTEXT("RunStatus", "{0}  --  {1}, {2}  --  {3}"),
		Tool->GetToolName(),
		Context.Meshes.Num() > 1
			? FText::Format(LOCTEXT("RunStatusMeshes", "{0} meshes"), FText::AsNumber(Context.Meshes.Num()))
			: FText::FromString(GetPrimaryTarget() ? GetPrimaryTarget()->GetName() : TEXT("?")),
		Context.SectionMaterialIndices.Num() > 0
			? FText::Format(LOCTEXT("RunStatusSections", "{0} section(s)"),
				FText::AsNumber(Context.SectionMaterialIndices.Num()))
			: LOCTEXT("RunStatusAllSections", "all sections"),
		FText::FromString(FString::Printf(TEXT("%.2f s"), RunEnd - RunStart)));

	// Only rebuild the section list when the tool could actually have changed it. Rebuilding re-reads
	// the mesh import data and re-applies the viewport highlight, which reattaches the component --
	// far too much to pay after a tool that only rewrote a vertex channel.
	if (Tool->InvalidatesSectionList())
	{
		RefreshSections(/*bPreserveSelection=*/true);
	}

	const double End = FPlatformTime::Seconds();
	UE_LOG(LogMoonToonTools, Log, TEXT("[MoonToon] %s: tool %.3fs | output+refresh %.3fs | total %.3fs"),
		*Tool->GetToolName().ToString(),
		RunEnd - RunStart,
		End - RunEnd,
		End - RunStart);

	return FReply::Handled();
}

// --- Output -------------------------------------------------------------------------------------

FText SMoonToonToolsPanel::GetRunStatusText() const
{
	return RunStatusText;
}

bool SMoonToonToolsPanel::HasOutput() const
{
	return !OutputText.IsEmpty();
}

void SMoonToonToolsPanel::OnExternalReport(const FString& Report)
{
	OutputText = Report;
	if (OutputBox.IsValid())
	{
		OutputBox->SetText(FText::FromString(OutputText));
	}
	RunStatusText = LOCTEXT("ExternalReport", "Content Browser action");

	// Overrides just moved under the panel's feet, so whatever the material tab is showing is stale.
	if (MaterialPanel.IsValid())
	{
		MaterialPanel->Refresh();
	}
}

FReply SMoonToonToolsPanel::OnCopyOutputClicked()
{
	if (!OutputText.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*OutputText);
	}
	return FReply::Handled();
}

FReply SMoonToonToolsPanel::OnClearOutputClicked()
{
	OutputText.Reset();
	RunStatusText = FText::GetEmpty();
	if (OutputBox.IsValid())
	{
		OutputBox->SetText(FText::GetEmpty());
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
