// Copyright Dream Moon. All Rights Reserved.

#include "SMoonToonToolsPanel.h"

#include "ContentBrowserModule.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Selection.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "IDetailsView.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Modules/ModuleManager.h"
#include "MoonToonBakeTools.h"
#include "MoonToonMeshInfoTool.h"
#include "MoonToonOutlineAlphaTool.h"
#include "MoonToonStrandPreviewActor.h"
#include "MoonToonStrandTangentTool.h"
#include "MoonToonTool.h"
#include "PropertyEditorModule.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SMoonToonToolsPanel"

DEFINE_LOG_CATEGORY_STATIC(LogMoonToonTools, Log, All);

namespace
{
	/** Pulls every static/skeletal mesh asset referenced by an actor's components. */
	void CollectMeshesFromActor(const AActor* Actor, TArray<TWeakObjectPtr<UObject>>& OutMeshes)
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
	UObject* GetComponentMesh(const UMeshComponent* Component)
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
}

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

	ChildSlot
	[
		SNew(SVerticalBox)

		// --- Target row -------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("TargetLabel", "Target"))
					.Font(FAppStyle::GetFontStyle("BoldFont"))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SMoonToonToolsPanel::GetTargetText)
					.ToolTipText(LOCTEXT("TargetTooltip",
						"Follows the Content Browser selection, and the meshes used by actors selected "
						"in the level. Lock to keep the current target while selecting something else."))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
					.IsChecked(this, &SMoonToonToolsPanel::IsTargetLocked)
					.OnCheckStateChanged(this, &SMoonToonToolsPanel::OnTargetLockChanged)
					.ToolTipText(LOCTEXT("LockTooltip", "Stop following the selection."))
					.Padding(4.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("Lock", "Lock"))
					]
				]
			]
		]

		// --- Body -------------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(4.0f, 0.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)

			+ SSplitter::Slot()
			.Value(0.25f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				[
					SAssignNew(ToolListView, SListView<TWeakObjectPtr<UMoonToonTool>>)
					.ListItemsSource(&ToolListItems)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SMoonToonToolsPanel::OnGenerateToolRow)
					.OnSelectionChanged(this, &SMoonToonToolsPanel::OnToolSelectionChanged)
				]
			]

			+ SSplitter::Slot()
			.Value(0.75f)
			[
				SNew(SVerticalBox)

				// Sections
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f, 0.0f, 0.0f, 4.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(4.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(this, &SMoonToonToolsPanel::GetSectionHintText)
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								SNew(SCheckBox)
								.IsChecked(this, &SMoonToonToolsPanel::IsIsolateEnabled)
								.OnCheckStateChanged(this, &SMoonToonToolsPanel::OnIsolateChanged)
								.ToolTipText(LOCTEXT("IsolateTooltip",
									"Hide every other section in the viewport instead of just tinting the "
									"selected one. Easier to read on a dense character."))
								[
									SNew(STextBlock).Text(LOCTEXT("Isolate", "Isolate"))
								]
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.Text(LOCTEXT("SelectAll", "All"))
								.OnClicked(this, &SMoonToonToolsPanel::OnSelectAllSections)
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(4.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(SButton)
								.Text(LOCTEXT("ClearSel", "None"))
								.OnClicked(this, &SMoonToonToolsPanel::OnClearSectionSelection)
							]
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.MaxHeight(140.0f)
						.Padding(0.0f, 4.0f, 0.0f, 0.0f)
						[
							SAssignNew(SectionListView, SListView<FSectionPtr>)
							.ListItemsSource(&Sections)
							.SelectionMode(ESelectionMode::Multi)
							.OnGenerateRow(this, &SMoonToonToolsPanel::OnGenerateSectionRow)
							.OnSelectionChanged(this, &SMoonToonToolsPanel::OnSectionSelectionChanged)
						]
					]
				]

				// Tool description + settings
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(4.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(2.0f, 2.0f, 2.0f, 6.0f)
						[
							SNew(STextBlock)
							.Text(this, &SMoonToonToolsPanel::GetToolDescriptionText)
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SNew(SScrollBox)
							+ SScrollBox::Slot()
							[
								DetailsView.ToSharedRef()
							]
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 6.0f, 0.0f, 0.0f)
						[
							SNew(SButton)
							.HAlign(HAlign_Center)
							.ContentPadding(FMargin(0.0f, 6.0f))
							.IsEnabled(this, &SMoonToonToolsPanel::IsRunEnabled)
							.OnClicked(this, &SMoonToonToolsPanel::OnRunClicked)
							[
								SNew(STextBlock)
								.Text(this, &SMoonToonToolsPanel::GetRunButtonText)
								.Font(FAppStyle::GetFontStyle("BoldFont"))
							]
						]
					]
				]
			]
		]

		// --- Output -----------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.FillHeight(0.4f)
		.Padding(4.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(2.0f)
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
	];

	// Seed from whatever is already selected, so the panel is not blank on open.
	OnLevelSelectionChanged(nullptr);

	if (ToolListItems.Num() > 0)
	{
		ToolListView->SetSelection(ToolListItems[0]);
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
}

void SMoonToonToolsPanel::BuildToolList()
{
	// Registration order is display order. Adding a tool is this one line plus its own class.
	auto AddTool = [this](UClass* ToolClass)
	{
		UMoonToonTool* Tool = NewObject<UMoonToonTool>(GetTransientPackage(), ToolClass);
		Tools.Emplace(Tool);
		ToolListItems.Add(Tool);
	};

	AddTool(UMoonToonMeshInfoTool::StaticClass());
	AddTool(UMoonToonOutlineAlphaTool::StaticClass());
	AddTool(UMoonToonBakeSmoothNormalTool::StaticClass());
	AddTool(UMoonToonBakeFaceForwardTool::StaticClass());
	AddTool(UMoonToonStrandTangentTool::StaticClass());
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
			CollectMeshesFromActor(Actor, NewTargets);
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
	Sections.Reset();
	if (TargetMeshes.Num() > 0)
	{
		if (UObject* Mesh = TargetMeshes[0].Get())
		{
			TArray<FMoonToonSectionInfo> SectionInfos;
			MoonToonMesh::GetSections(Mesh, 0, SectionInfos);
			for (const FMoonToonSectionInfo& Info : SectionInfos)
			{
				Sections.Add(MakeShared<FMoonToonSectionInfo>(Info));
			}
		}
	}

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
}

FText SMoonToonToolsPanel::GetTargetText() const
{
	if (TargetMeshes.Num() == 0)
	{
		return LOCTEXT("NoTarget", "<nothing selected>");
	}
	if (TargetMeshes.Num() == 1)
	{
		const UObject* Mesh = TargetMeshes[0].Get();
		return Mesh ? FText::FromString(Mesh->GetName()) : LOCTEXT("StaleTarget", "<target was unloaded>");
	}

	const UObject* First = TargetMeshes[0].Get();
	return FText::Format(LOCTEXT("MultiTarget", "{0}  (+{1} more)"),
		First ? FText::FromString(First->GetName()) : LOCTEXT("Unknown", "?"),
		FText::AsNumber(TargetMeshes.Num() - 1));
}

ECheckBoxState SMoonToonToolsPanel::IsTargetLocked() const
{
	return bLockTarget ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SMoonToonToolsPanel::OnTargetLockChanged(ECheckBoxState NewState)
{
	bLockTarget = (NewState == ECheckBoxState::Checked);
}

// --- Sections -----------------------------------------------------------------------------------

TSharedRef<ITableRow> SMoonToonToolsPanel::OnGenerateSectionRow(FSectionPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<FSectionPtr>, OwnerTable)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.0f, 2.0f)
		[
			SNew(STextBlock)
			.Text(FText::AsNumber(Item->MaterialIndex))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.MinDesiredWidth(20.0f)
		]
		+ SHorizontalBox::Slot()
		.FillWidth(0.45f)
		.Padding(4.0f, 2.0f)
		[
			SNew(STextBlock).Text(FText::FromName(Item->SlotName))
		]
		+ SHorizontalBox::Slot()
		.FillWidth(0.4f)
		.Padding(4.0f, 2.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Item->MaterialName))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.0f, 2.0f)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("TriCount", "{0} tris"), FText::AsNumber(Item->NumTriangles)))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
	];
}

FText SMoonToonToolsPanel::GetSectionHintText() const
{
	const int32 NumSelected = SectionListView.IsValid() ? SectionListView->GetNumItemsSelected() : 0;
	if (NumSelected == 0)
	{
		return LOCTEXT("SectionsAll", "Sections  --  none selected, so all of them");
	}
	return FText::Format(LOCTEXT("SectionsSome", "Sections  --  {0} selected"), FText::AsNumber(NumSelected));
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
			UObject* ComponentMesh = GetComponentMesh(Component);
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
	[
		SNew(STextBlock)
		.Text(Tool ? Tool->GetToolName() : FText::GetEmpty())
		.ToolTipText(Tool ? Tool->GetToolDescription() : FText::GetEmpty())
		.Margin(FMargin(6.0f, 4.0f))
	];
}

void SMoonToonToolsPanel::OnToolSelectionChanged(TWeakObjectPtr<UMoonToonTool> Item, ESelectInfo::Type SelectInfo)
{
	SelectedTool = Item;
	if (DetailsView.IsValid())
	{
		DetailsView->SetObject(Item.Get());
	}
	LastSeenToolEditSerial = Item.IsValid() ? Item->ExternalEditSerial : 0;
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

bool SMoonToonToolsPanel::IsRunEnabled() const
{
	return SelectedTool.IsValid() && TargetMeshes.Num() > 0;
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

	if (OutputBox.IsValid())
	{
		OutputBox->SetText(FText::FromString(Result));
	}

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

#undef LOCTEXT_NAMESPACE
