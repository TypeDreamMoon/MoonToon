// Copyright Dream Moon. All Rights Reserved.

#include "SMoonToonDebugPanel.h"

#include "MoonToonDebug.h"
#include "MoonToonDebugMcp.h"
#include "MoonToonDebugSubsystem.h"
#include "MoonToonToolsUserSettings.h"

#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "LevelEditorViewport.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolMenus.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "MoonToonDebugPanel"

const FName MoonToonDebugPanel::TabName(TEXT("MoonToonDebugger"));

namespace
{
	const TCHAR* const GViewGroups[] = { TEXT("record"), TEXT("validate"), TEXT("buffer"), TEXT("shade") };

	FText GroupLabel(const FString& Group)
	{
		if (Group == TEXT("record"))	{ return LOCTEXT("GroupRecord", "A 记录字段"); }
		if (Group == TEXT("validate"))	{ return LOCTEXT("GroupValidate", "B 自检"); }
		if (Group == TEXT("buffer"))	{ return LOCTEXT("GroupBuffer", "C 中间缓冲"); }
		return LOCTEXT("GroupShade", "D 着色分量");
	}

	FText SectionLabel(const FString& Section)
	{
		static const TPair<const TCHAR*, const TCHAR*> Labels[] =
		{
			{ TEXT("pixel"),		TEXT("像素") },
			{ TEXT("meta"),			TEXT("记录元数据") },
			{ TEXT("record"),		TEXT("记录") },
			{ TEXT("profile"),		TEXT("Profile") },
			{ TEXT("surface"),		TEXT("表面") },
			{ TEXT("feature"),		TEXT("特征通道") },
			{ TEXT("sdf"),			TEXT("SDF 脸") },
			{ TEXT("actor_table"),	TEXT("角色表") },
			{ TEXT("buffers"),		TEXT("中间缓冲") },
			{ TEXT("style"),		TEXT("风格 / 来源链") },
			{ TEXT("validate"),		TEXT("校验") },
		};
		for (const TPair<const TCHAR*, const TCHAR*>& Label : Labels)
		{
			if (Section == Label.Key)
			{
				return FText::FromString(Label.Value);
			}
		}
		return FText::FromString(Section);
	}

	/** The inspector's section order; sections the report adds later go after these. */
	const TCHAR* const GSectionOrder[] =
	{
		TEXT("pixel"), TEXT("meta"), TEXT("record"), TEXT("profile"), TEXT("surface"), TEXT("feature"),
		TEXT("sdf"), TEXT("actor_table"), TEXT("buffers"), TEXT("style"), TEXT("validate"),
	};

	/** One colour per frame stage for the colour-breakdown bars (stage order, MOONTOON_DEBUG_STAGE_LIST). */
	FLinearColor StageColor(int32 Index)
	{
		static const FLinearColor Colors[] =
		{
			FLinearColor(0.45f, 0.45f, 0.50f),	// base pass (emissive / self)
			FLinearColor(0.95f, 0.75f, 0.25f),	// direct lights
			FLinearColor(0.55f, 0.30f, 0.65f),	// shadow bleed
			FLinearColor(0.30f, 0.65f, 0.95f),	// secondary compose
			FLinearColor(0.95f, 0.45f, 0.45f),	// rim
			FLinearColor(0.35f, 0.80f, 0.55f),	// face overlay / inner line
			FLinearColor(0.95f, 0.60f, 0.85f),	// diffusion / bloom
			FLinearColor(0.40f, 0.85f, 0.85f),	// indirect / reflections
			FLinearColor(0.65f, 0.65f, 0.35f),	// fog / sky
			FLinearColor(0.80f, 0.80f, 0.80f),	// translucency
		};
		return Colors[FMath::Abs(Index) % UE_ARRAY_COUNT(Colors)];
	}

	FLinearColor DisplaySwatch(const FLinearColor& Linear)
	{
		return FLinearColor(FMath::Clamp(Linear.R, 0.0f, 1.0f), FMath::Clamp(Linear.G, 0.0f, 1.0f), FMath::Clamp(Linear.B, 0.0f, 1.0f), 1.0f);
	}

	TSharedRef<SWidget> MakeSwatch(const FLinearColor& Color, float Size = 12.0f)
	{
		return SNew(SBox)
			.WidthOverride(Size)
			.HeightOverride(Size)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("WhiteBrush"))
				.ColorAndOpacity(DisplaySwatch(Color))
			];
	}

	/** "profile:/Game/X.X" / "preset:/Game/Y.Y @ PPV_Main" -> the asset path, or empty. */
	FString AssetPathInSource(const FString& Source)
	{
		int32 Colon = INDEX_NONE;
		if (!Source.FindChar(TEXT(':'), Colon))
		{
			return FString();
		}
		FString Path = Source.Mid(Colon + 1).TrimStartAndEnd();
		int32 At = INDEX_NONE;
		if (Path.FindChar(TEXT('@'), At))
		{
			Path = Path.Left(At).TrimStartAndEnd();
		}
		return Path.StartsWith(TEXT("/")) ? Path : FString();
	}

	/** "row 17" / "diffuse 4" / "id 5" -> 17 / 4 / 5. */
	int32 TrailingNumber(const FString& Key)
	{
		int32 Space = INDEX_NONE;
		if (!Key.FindLastChar(TEXT(' '), Space))
		{
			return INDEX_NONE;
		}
		const FString Number = Key.Mid(Space + 1);
		return Number.IsNumeric() ? FCString::Atoi(*Number) : INDEX_NONE;
	}

	TSharedRef<SDockTab> SpawnDebuggerTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SMoonToonDebugPanel)
			];
	}

	void FillGroupMenu(UToolMenu* Menu, FString Group)
	{
		UMoonToonDebugSubsystem* Sub = UMoonToonDebugSubsystem::Get();
		if (!Sub)
		{
			return;
		}
		FToolMenuSection& Section = Menu->AddSection(FName(*Group), GroupLabel(Group));
		for (const FMoonToonDebugViewInfo& View : Sub->GetViews())
		{
			if (View.Group != Group)
			{
				continue;
			}
			const FString Name = View.Name;
			Section.AddMenuEntry(
				FName(*Name),
				FText::FromString(View.DisplayName.IsEmpty() ? Name : FString::Printf(TEXT("%s  (%s)"), *View.DisplayName, *Name)),
				FText::FromString(FString::Printf(TEXT("r.MoonToon.Debug.View %s"), *Name)),
				FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([Name]
					{
						if (UMoonToonDebugSubsystem* S = UMoonToonDebugSubsystem::Get())
						{
							S->SetView(Name);
						}
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([Name]
					{
						const UMoonToonDebugSubsystem* S = UMoonToonDebugSubsystem::Get();
						return S && S->GetCurrentViewName() == Name;
					})),
				EUserInterfaceActionType::RadioButton);
		}
	}

	void FillViewModesMenu(UToolMenu* Menu)
	{
		FToolMenuSection& Top = Menu->AddSection(TEXT("MoonToonDebugTop"));
		Top.AddMenuEntry(
			TEXT("OpenDebugger"),
			LOCTEXT("OpenDebuggerEntry", "打开 MoonToon 调试器…"),
			LOCTEXT("OpenDebuggerEntryTip", "视图、图例、像素检查器、角色 / 表 / 校验"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
			FUIAction(FExecuteAction::CreateLambda([]
			{
				FGlobalTabmanager::Get()->TryInvokeTab(MoonToonDebugPanel::TabName);
			})));
		Top.AddMenuEntry(
			TEXT("Off"),
			LOCTEXT("ViewOffEntry", "关"),
			LOCTEXT("ViewOffEntryTip", "r.MoonToon.Debug.View off"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([]
				{
					if (UMoonToonDebugSubsystem* S = UMoonToonDebugSubsystem::Get())
					{
						S->SetView(FString());
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([]
				{
					const UMoonToonDebugSubsystem* S = UMoonToonDebugSubsystem::Get();
					return !S || S->GetCurrentViewName().IsEmpty();
				})),
			EUserInterfaceActionType::RadioButton);

		FToolMenuSection& Groups = Menu->AddSection(TEXT("MoonToonDebugGroups"));
		for (const TCHAR* Group : GViewGroups)
		{
			const FString GroupName = Group;
			Groups.AddSubMenu(
				FName(*GroupName),
				GroupLabel(GroupName),
				FText::GetEmpty(),
				FNewToolMenuDelegate::CreateLambda([GroupName](UToolMenu* SubMenu)
				{
					FillGroupMenu(SubMenu, GroupName);
				}));
		}
	}
}

void MoonToonDebugPanel::RegisterTab()
{
	FGlobalTabmanager::Get()
		->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&SpawnDebuggerTab))
		.SetDisplayName(LOCTEXT("TabTitle", "MoonToon 调试器"))
		.SetTooltipText(LOCTEXT("TabTooltip", "MoonToon 记录 / 中间缓冲的调试视图、像素检查器、角色、表和校验。"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"));
}

void MoonToonDebugPanel::UnregisterTab()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
	}
}

void MoonToonDebugPanel::ExtendMenus()
{
	if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools")))
	{
		FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("MoonToon"), LOCTEXT("MoonToonSection", "MoonToon"));
		Section.AddMenuEntry(
			TEXT("MoonToonDebugger"),
			LOCTEXT("OpenDebugger", "MoonToon 调试器"),
			LOCTEXT("OpenDebuggerTooltip", "MoonToon 记录 / 中间缓冲的调试视图、像素检查器、角色、表和校验。"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
			FUIAction(FExecuteAction::CreateLambda([]
			{
				FGlobalTabmanager::Get()->TryInvokeTab(TabName);
			})));
	}

	// The level viewport's View Modes menu (the new viewport toolbar; the old one shares the same menu).
	if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.ViewportToolbar.ViewModes")))
	{
		FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("MoonToonDebug"), LOCTEXT("ViewModesSection", "MoonToon"));
		Section.AddSubMenu(
			TEXT("MoonToonDebugViews"),
			LOCTEXT("ViewModesSubMenu", "MoonToon 调试视图"),
			LOCTEXT("ViewModesSubMenuTip", "全屏替换为 MoonToon 记录字段 / 自检 / 中间缓冲 / 着色分量（tonemap 和放大之后）"),
			FNewToolMenuDelegate::CreateStatic(&FillViewModesMenu));
	}
}

// =====================================================================================================
// Construction

void SMoonToonDebugPanel::Construct(const FArguments& InArgs)
{
	Subsystem = UMoonToonDebugSubsystem::Get();
	if (UMoonToonDebugSubsystem* Sub = Subsystem.Get())
	{
		Sub->SetPanelOpen(true);
		ReportChangedHandle = Sub->OnReportChanged.AddSP(this, &SMoonToonDebugPanel::OnReportChanged);
		ViewChangedHandle = Sub->OnViewChanged.AddSP(this, &SMoonToonDebugPanel::OnViewChanged);
	}

	NonToonOptions.Add(MakeShared<FString>(TEXT("压暗")));
	NonToonOptions.Add(MakeShared<FString>(TEXT("黑")));
	NonToonOptions.Add(MakeShared<FString>(TEXT("原样")));

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildToolbar()
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)
			+ SSplitter::Slot()
			.Value(0.27f)
			[
				BuildViewPane()
			]
			+ SSplitter::Slot()
			.Value(0.73f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Vertical)
				+ SSplitter::Slot()
				.Value(0.28f)
				[
					BuildLegend()
				]
				+ SSplitter::Slot()
				.Value(0.72f)
				[
					BuildPages()
				]
			]
		]
	];

	RebuildViewTree();
	OnViewChanged();
	RebuildInspector();
}

SMoonToonDebugPanel::~SMoonToonDebugPanel()
{
	if (UMoonToonDebugSubsystem* Sub = Subsystem.Get())
	{
		Sub->OnReportChanged.Remove(ReportChangedHandle);
		Sub->OnViewChanged.Remove(ViewChangedHandle);
		// Picking only works with the panel open; nothing keeps re-inspecting once it is closed.
		Sub->SetPanelOpen(false);
		Sub->SetPickArmed(false);
		Sub->SetFollowMouse(false);
		Sub->SetLocked(false);
	}
}

void SMoonToonDebugPanel::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	const double Now = FPlatformTime::Seconds();
	if (Now - LastLegendRefresh > 0.5)
	{
		LastLegendRefresh = Now;
		RefreshLegend();
	}
	// Following the mouse re-inspects every few frames: rebuild the inspector at most ten times a second.
	if (bInspectorDirty && Now - LastInspectorBuild > 0.1)
	{
		RebuildInspector();
	}
}

// =====================================================================================================
// Toolbar

TSharedRef<SWidget> SMoonToonDebugPanel::BuildToolbar()
{
	const auto Toggle = [](const TAttribute<FText>& Label, const FText& Tip, TFunction<bool()> Get, TFunction<void(bool)> Set, TAttribute<bool> Enabled = true) -> TSharedRef<SWidget>
	{
		return SNew(SCheckBox)
			.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.ToolTipText(Tip)
			.IsEnabled(Enabled)
			.IsChecked_Lambda([Get]() { return Get() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([Set](ECheckBoxState State) { Set(State == ECheckBoxState::Checked); })
			[
				SNew(SBox)
				.Padding(FMargin(6.0f, 2.0f))
				[
					SNew(STextBlock).Text(Label)
				]
			];
	};
	const TWeakObjectPtr<UMoonToonDebugSubsystem> Weak = Subsystem;

	const FText PickTip = FText::Format(
		LOCTEXT("PickTip", "下一次在关卡视口里左键只拾取像素、不改选择（Esc 退出）。\n面板打开时，光标停在视口上按 {0} 也能拾取（插件设置里可改键）。"),
		UMoonToonToolsUserSettings::Get().DebugPickChord.GetInputText());

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
		.Padding(FMargin(4.0f, 3.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(4.0f, 2.0f))
				+ SWrapBox::Slot()
				[
					Toggle(LOCTEXT("Pick", "◎ 拾取"), PickTip,
						[Weak]() { return Weak.IsValid() && Weak->IsPickArmed(); },
						[Weak](bool b) { if (Weak.IsValid()) { Weak->SetPickArmed(b); } })
				]
				+ SWrapBox::Slot()
				[
					Toggle(LOCTEXT("Follow", "↔ 跟随鼠标"), LOCTEXT("FollowTip", "像引擎 Pixel Inspector 一样实时检查光标下的像素（结果晚 1-3 帧）。"),
						[Weak]() { return Weak.IsValid() && Weak->IsFollowMouse(); },
						[Weak](bool b) { if (Weak.IsValid()) { Weak->SetFollowMouse(b); if (b) { Weak->SetLocked(false); } } })
				]
				+ SWrapBox::Slot()
				[
					Toggle(TAttribute<FText>::CreateSP(this, &SMoonToonDebugPanel::GetLockText),
						LOCTEXT("LockTip", "锁住上次拾取的像素，每帧重新检查：在细节面板里调参数时检查器跟着变，变了的字段高亮一秒。"),
						[Weak]() { return Weak.IsValid() && Weak->IsLocked(); },
						[Weak](bool b) { if (Weak.IsValid()) { Weak->SetLocked(b); } },
						TAttribute<bool>::CreateLambda([Weak]() { return Weak.IsValid() && Weak->HasPick(); }))
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text_Lambda([Weak]() { return Weak.IsValid() && Weak->GetPinnedReport() ? LOCTEXT("RePin", "⇄ 重新钉住 A") : LOCTEXT("Pin", "⇄ 钉住 A"); })
					.ToolTipText(LOCTEXT("PinTip", "把当前结果钉成 A：之后的结果和 A 并排显示（可只看有差别的字段），调参前后对比用。"))
					.IsEnabled_Lambda([Weak]() { return Weak.IsValid() && Weak->GetReport().bValid; })
					.OnClicked_Lambda([this, Weak]()
					{
						if (Weak.IsValid())
						{
							Weak->PinReport();
							bInspectorDirty = true;
						}
						return FReply::Handled();
					})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("ClearPin", "清除 A"))
					.Visibility_Lambda([Weak]() { return Weak.IsValid() && Weak->GetPinnedReport() ? EVisibility::Visible : EVisibility::Collapsed; })
					.OnClicked_Lambda([this, Weak]()
					{
						if (Weak.IsValid())
						{
							Weak->ClearPin();
							bInspectorDirty = true;
						}
						return FReply::Handled();
					})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("Capture", "📷 抓当前视图"))
					.ToolTipText(LOCTEXT("CaptureTip", "线性 EXR（原值）+ 16 位 id PNG + 假色 PNG + JSON，写到 Saved/MoonToonDebug/captures/。视图关着时抓 tonemap 后的画面。"))
					.OnClicked(this, &SMoonToonDebugPanel::OnCapture)
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyJson", "⧉ 复制 JSON"))
					.ToolTipText(LOCTEXT("CopyJsonTip", "和 MCP get_pixel 返回的 JSON 一模一样。"))
					.IsEnabled_Lambda([Weak]() { return Weak.IsValid() && Weak->GetReport().bValid; })
					.OnClicked(this, &SMoonToonDebugPanel::OnCopyJson)
				]
				+ SWrapBox::Slot()
				[
					Toggle(TAttribute<FText>::CreateLambda([]()
						{
							return MoonToonDebugMcp::IsRunning()
								? FText::FromString(FString::Printf(TEXT("MCP ● :%d"), MoonToonDebugMcp::GetPort()))
								: LOCTEXT("McpOff", "MCP ○");
						}),
						FText::Format(LOCTEXT("McpTip", "调试器的 MCP 服务（给 Claude Code 用）：{0}\n令牌文件：{1}\n手动启动；端口在编辑器偏好设置 > MoonToon Debugger 里改。"),
							FText::FromString(MoonToonDebugMcp::GetUrl()), FText::FromString(MoonToonDebugMcp::GetTokenPath())),
						[]() { return MoonToonDebugMcp::IsRunning(); },
						[this](bool b)
						{
							if (b)
							{
								FString Error;
								LastCaptureText = MoonToonDebugMcp::Start(&Error)
									? FString::Printf(TEXT("MCP 服务已启动：%s（令牌 %s）"), *MoonToonDebugMcp::GetUrl(), *MoonToonDebugMcp::GetTokenPath())
									: FString::Printf(TEXT("MCP 服务启动失败：%s"), *Error);
								LastCapturePath.Reset();
							}
							else
							{
								MoonToonDebugMcp::Stop();
							}
						})
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f, 3.0f, 2.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SMoonToonDebugPanel::GetStatusText)
				.ColorAndOpacity(this, &SMoonToonDebugPanel::GetStatusColor)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f, 2.0f, 2.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([this]() { return LastCaptureText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(LastCaptureText); })
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("ShowInExplorer", "在资源管理器中显示"))
					.IsEnabled_Lambda([this]() { return !LastCapturePath.IsEmpty(); })
					.OnClicked_Lambda([this]()
					{
						FPlatformProcess::ExploreFolder(*LastCapturePath);
						return FReply::Handled();
					})
				]
			]
		];
}

FText SMoonToonDebugPanel::GetStatusText() const
{
	const UMoonToonDebugSubsystem* Sub = Subsystem.Get();
	if (!Sub)
	{
		return LOCTEXT("NoSubsystem", "调试器子系统没有运行。");
	}
	FString WhyNot;
	if (!Sub->IsAvailable(&WhyNot))
	{
		return FText::Format(LOCTEXT("Unavailable", "不可用：{0}"), FText::FromString(WhyNot));
	}
	FString Text;
	const FMoonToonPixelReport& Report = Sub->GetReport();
	if (Report.bValid)
	{
		Text = FString::Printf(TEXT("帧 %lld · 视口 %d×%d · 渲染 %d×%d (%.0f%%) · "),
			Report.FrameNumber, Report.ViewportSize.X, Report.ViewportSize.Y, Report.RenderSize.X, Report.RenderSize.Y, Report.ScreenPercentage);
	}
	Text += FString::Printf(TEXT("引擎/插件 API v%d ✓"), MoonToonDebug::GetApiVersion());
	const FString View = Sub->GetCurrentViewName();
	Text += View.IsEmpty() ? FString(TEXT(" · 视图 关")) : FString::Printf(TEXT(" · 视图 %s"), *View);
	const int32 Pending = MoonToonDebug::GetPendingRequestCount();
	if (Pending > 0)
	{
		Text += FString::Printf(TEXT(" · 等待 GPU %d"), Pending);
	}
	return FText::FromString(Text);
}

FSlateColor SMoonToonDebugPanel::GetStatusColor() const
{
	const UMoonToonDebugSubsystem* Sub = Subsystem.Get();
	return Sub && Sub->IsAvailable() ? FSlateColor::UseSubduedForeground() : FSlateColor(FLinearColor(1.0f, 0.45f, 0.3f));
}

FText SMoonToonDebugPanel::GetLockText() const
{
	const UMoonToonDebugSubsystem* Sub = Subsystem.Get();
	if (Sub && Sub->HasPick())
	{
		const FIntPoint Pixel = Sub->GetPickPixel();
		return FText::FromString(FString::Printf(TEXT("🔒 锁定 %d,%d"), Pixel.X, Pixel.Y));
	}
	return LOCTEXT("LockNone", "🔒 锁定");
}

FReply SMoonToonDebugPanel::OnCopyJson()
{
	if (const UMoonToonDebugSubsystem* Sub = Subsystem.Get())
	{
		FPlatformApplicationMisc::ClipboardCopy(*Sub->GetReport().Json);
	}
	return FReply::Handled();
}

FReply SMoonToonDebugPanel::OnCapture()
{
	UMoonToonDebugSubsystem* Sub = Subsystem.Get();
	if (!Sub)
	{
		return FReply::Handled();
	}
	const FString View = Sub->GetCurrentViewName();
	const FMoonToonCaptureInfo Info = Sub->CaptureSync(View.IsEmpty() ? FString(TEXT("scene.final")) : View, Sub->GetViewSettings().Channel, 1, FString());
	if (Info.bValid)
	{
		LastCapturePath = Info.RawPath;
		LastCaptureText = FString::Printf(TEXT("已抓 %d×%d：%s"), Info.Width, Info.Height, *Info.RawPath);
	}
	else
	{
		LastCapturePath.Reset();
		LastCaptureText = FString::Printf(TEXT("抓图失败：%s"), *Info.Error);
	}
	return FReply::Handled();
}

// =====================================================================================================
// Views

TSharedRef<SWidget> SMoonToonDebugPanel::BuildViewPane()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("SearchViews", "搜索视图…"))
				.OnTextChanged_Lambda([this](const FText& Text)
				{
					ViewFilter = Text.ToString();
					RebuildViewTree();
				})
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(ViewTree, STreeView<FViewNodePtr>)
				.TreeItemsSource(&Roots)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SMoonToonDebugPanel::OnGenerateViewRow)
				.OnGetChildren(this, &SMoonToonDebugPanel::OnGetViewChildren)
				.OnSelectionChanged(this, &SMoonToonDebugPanel::OnViewSelected)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				BuildViewOptions()
			]
		];
}

TSharedRef<SWidget> SMoonToonDebugPanel::BuildViewOptions()
{
	const auto Row = [](const FText& Label, const TSharedRef<SWidget>& Content) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox)
				.MinDesiredWidth(64.0f)
				[
					SNew(STextBlock).Text(Label).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				Content
			];
	};
	const TWeakObjectPtr<UMoonToonDebugSubsystem> Weak = Subsystem;
	const auto Settings = [Weak]() { return Weak.IsValid() ? Weak->GetViewSettings() : MoonToonDebug::FViewSettings(); };

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	Box->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
	[
		Row(LOCTEXT("Channel", "通道"),
			SAssignNew(ChannelCombo, SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&ChannelOptions)
			.IsEnabled_Lambda([this]() { return ChannelOptions.Num() > 1; })
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
			{
				return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
			})
			.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo)
			{
				const int32 Index = ChannelOptions.IndexOfByKey(Item);
				if (Index != INDEX_NONE && SelectInfo != ESelectInfo::Direct)
				{
					ModifySettings([Index](MoonToonDebug::FViewSettings& S) { S.Channel = Index; S.IsolateKey = -1; });
				}
			})
			[
				SNew(STextBlock)
				.Text_Lambda([this, Settings]()
				{
					const int32 Channel = Settings().Channel;
					return ChannelOptions.IsValidIndex(Channel) ? FText::FromString(*ChannelOptions[Channel]) : LOCTEXT("NoChannel", "—");
				})
			])
	];

	Box->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
	[
		Row(LOCTEXT("Bit", "位"),
			SNew(SSpinBox<int32>)
			.MinValue(-1)
			.MaxValue(31)
			.IsEnabled_Lambda([this]() { const FMoonToonDebugViewInfo* Info = GetCurrentViewInfo(); return Info && Info->bHasBit; })
			.ToolTipText(LOCTEXT("BitTip", "-1 = 所有位（按值着色）；0..31 = 只看这一位"))
			.Value_Lambda([Settings]() { return Settings().Bit; })
			.OnValueChanged_Lambda([this](int32 Value) { ModifySettings([Value](MoonToonDebug::FViewSettings& S) { S.Bit = Value; }); }))
	];

	Box->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
	[
		Row(LOCTEXT("Resolved", "解析后"),
			SNew(SCheckBox)
			.IsEnabled_Lambda([this]() { const FMoonToonDebugViewInfo* Info = GetCurrentViewInfo(); return Info && Info->bHasResolved; })
			.ToolTipText(LOCTEXT("ResolvedTip", "勾上 = 着色时实际用的值（常量表 / profile 解析之后）；不勾 = 记录里的原始位"))
			.IsChecked_Lambda([Settings]() { return Settings().bResolved ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
			{
				ModifySettings([State](MoonToonDebug::FViewSettings& S) { S.bResolved = State == ECheckBoxState::Checked; });
			}))
	];

	Box->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
	[
		Row(LOCTEXT("Range", "范围"),
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<float>)
				.Delta(0.01f)
				.ToolTipText(LOCTEXT("RangeTip", "最小 = 最大 时用视图自己的默认范围"))
				.Value_Lambda([Settings]() { return Settings().RangeMin; })
				.OnValueChanged_Lambda([this](float Value) { ModifySettings([Value](MoonToonDebug::FViewSettings& S) { S.RangeMin = Value; }); })
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SSpinBox<float>)
				.Delta(0.01f)
				.Value_Lambda([Settings]() { return Settings().RangeMax; })
				.OnValueChanged_Lambda([this](float Value) { ModifySettings([Value](MoonToonDebug::FViewSettings& S) { S.RangeMax = Value; }); })
			])
	];

	Box->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
	[
		Row(LOCTEXT("NonToon", "非 toon"),
			SNew(SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&NonToonOptions)
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
			{
				return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
			})
			.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
			{
				const int32 Index = NonToonOptions.IndexOfByKey(Item);
				if (Index != INDEX_NONE)
				{
					ModifySettings([Index](MoonToonDebug::FViewSettings& S) { S.NonToonMode = Index; });
				}
			})
			[
				SNew(STextBlock)
				.Text_Lambda([this, Settings]()
				{
					const int32 Mode = Settings().NonToonMode;
					return NonToonOptions.IsValidIndex(Mode) ? FText::FromString(*NonToonOptions[Mode]) : FText::AsNumber(Mode);
				})
			])
	];

	Box->AddSlot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SCheckBox)
			.ToolTipText(LOCTEXT("LegendTip", "视口左上角画图例"))
			.IsChecked_Lambda([Settings]() { return Settings().bLegend ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { ModifySettings([State](MoonToonDebug::FViewSettings& S) { S.bLegend = State == ECheckBoxState::Checked; }); })
			[
				SNew(STextBlock).Text(LOCTEXT("LegendCheck", "视口图例"))
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(12.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SCheckBox)
			.ToolTipText(LOCTEXT("PrintTip", "在光标处打印该像素的值（ShaderPrint）"))
			.IsChecked_Lambda([Settings]() { return Settings().bPrint ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { ModifySettings([State](MoonToonDebug::FViewSettings& S) { S.bPrint = State == ECheckBoxState::Checked; }); })
			[
				SNew(STextBlock).Text(LOCTEXT("PrintCheck", "光标打印"))
			]
		]
	];

	return Box;
}

void SMoonToonDebugPanel::RebuildViewTree()
{
	Roots.Reset();
	ViewInfos.Reset();

	FViewNodePtr Off = MakeShared<FViewNode>();
	Off->bIsOff = true;
	Off->Label = LOCTEXT("ViewOff", "关");
	Off->Tooltip = LOCTEXT("ViewOffTip", "r.MoonToon.Debug.View off");
	Roots.Add(Off);

	if (const UMoonToonDebugSubsystem* Sub = Subsystem.Get())
	{
		ViewInfos = Sub->GetViews();
		for (const TCHAR* Group : GViewGroups)
		{
			FViewNodePtr GroupNode = MakeShared<FViewNode>();
			GroupNode->bIsGroup = true;
			GroupNode->Group = Group;
			GroupNode->Label = GroupLabel(Group);
			for (const FMoonToonDebugViewInfo& View : ViewInfos)
			{
				if (View.Group != Group)
				{
					continue;
				}
				if (!ViewFilter.IsEmpty() && !View.Name.Contains(ViewFilter) && !View.DisplayName.Contains(ViewFilter))
				{
					continue;
				}
				FViewNodePtr Node = MakeShared<FViewNode>();
				Node->Name = View.Name;
				Node->Group = Group;
				Node->Label = FText::FromString(View.DisplayName.IsEmpty() || View.DisplayName == View.Name
					? View.Name : FString::Printf(TEXT("%s  (%s)"), *View.DisplayName, *View.Name));
				FString Tip = FString::Printf(TEXT("r.MoonToon.Debug.View %s\n编码 %s"), *View.Name, *View.Encoding);
				if (View.ChannelNames.Num() > 0)
				{
					Tip += FString::Printf(TEXT("\n通道 %s"), *FString::Join(View.ChannelNames, TEXT(" / ")));
				}
				if (View.bHasBit)
				{
					Tip += TEXT("\n可选单个位");
				}
				if (View.bHasResolved)
				{
					Tip += TEXT("\n可切换 原始 / 解析后");
				}
				Node->Tooltip = FText::FromString(Tip);
				GroupNode->Children.Add(Node);
			}
			if (GroupNode->Children.Num() > 0)
			{
				Roots.Add(GroupNode);
			}
		}
	}

	if (ViewTree.IsValid())
	{
		ViewTree->RequestTreeRefresh();
		for (const FViewNodePtr& Root : Roots)
		{
			ViewTree->SetItemExpansion(Root, true);
		}
		SyncTreeSelection();
	}
}

void SMoonToonDebugPanel::SyncTreeSelection()
{
	if (!ViewTree.IsValid())
	{
		return;
	}
	const UMoonToonDebugSubsystem* Sub = Subsystem.Get();
	const FString Current = Sub ? Sub->GetCurrentViewName() : FString();
	TGuardValue<bool> Guard(bSyncingSelection, true);
	for (const FViewNodePtr& Root : Roots)
	{
		if (Root->bIsOff && Current.IsEmpty())
		{
			ViewTree->SetSelection(Root, ESelectInfo::Direct);
			return;
		}
		for (const FViewNodePtr& Child : Root->Children)
		{
			if (Child->Name == Current)
			{
				ViewTree->SetSelection(Child, ESelectInfo::Direct);
				ViewTree->RequestScrollIntoView(Child);
				return;
			}
		}
	}
	ViewTree->ClearSelection();
}

TSharedRef<ITableRow> SMoonToonDebugPanel::OnGenerateViewRow(FViewNodePtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<FViewNodePtr>, OwnerTable)
		.ToolTipText(Item->Tooltip)
		[
			SNew(STextBlock)
			.Text(Item->Label)
			.Font(Item->bIsGroup ? FAppStyle::GetFontStyle("BoldFont") : FAppStyle::GetFontStyle("NormalFont"))
		];
}

void SMoonToonDebugPanel::OnGetViewChildren(FViewNodePtr Item, TArray<FViewNodePtr>& OutChildren)
{
	if (Item.IsValid())
	{
		OutChildren = Item->Children;
	}
}

void SMoonToonDebugPanel::OnViewSelected(FViewNodePtr Item, ESelectInfo::Type SelectInfo)
{
	if (bSyncingSelection || !Item.IsValid() || Item->bIsGroup)
	{
		return;
	}
	if (UMoonToonDebugSubsystem* Sub = Subsystem.Get())
	{
		Sub->SetView(Item->bIsOff ? FString() : Item->Name);
	}
}

void SMoonToonDebugPanel::OnViewChanged()
{
	ChannelOptions.Reset();
	if (const FMoonToonDebugViewInfo* Info = GetCurrentViewInfo())
	{
		for (const FString& Name : Info->ChannelNames)
		{
			ChannelOptions.Add(MakeShared<FString>(Name));
		}
		for (int32 Channel = ChannelOptions.Num(); Channel < Info->ChannelCount; ++Channel)
		{
			ChannelOptions.Add(MakeShared<FString>(FString::Printf(TEXT("%d"), Channel)));
		}
	}
	if (ChannelCombo.IsValid())
	{
		ChannelCombo->RefreshOptions();
	}
	SyncTreeSelection();
	LastLegendRefresh = -1000.0;
}

const FMoonToonDebugViewInfo* SMoonToonDebugPanel::GetCurrentViewInfo() const
{
	const UMoonToonDebugSubsystem* Sub = Subsystem.Get();
	if (!Sub)
	{
		return nullptr;
	}
	const FString Current = Sub->GetCurrentViewName();
	return Current.IsEmpty() ? nullptr : ViewInfos.FindByPredicate([&Current](const FMoonToonDebugViewInfo& Info) { return Info.Name == Current; });
}

void SMoonToonDebugPanel::ModifySettings(TFunctionRef<void(MoonToonDebug::FViewSettings&)> Change)
{
	if (UMoonToonDebugSubsystem* Sub = Subsystem.Get())
	{
		MoonToonDebug::FViewSettings Settings = Sub->GetViewSettings();
		Change(Settings);
		Sub->SetViewSettings(Settings);
	}
}

// =====================================================================================================
// Legend

TSharedRef<SWidget> SMoonToonDebugPanel::BuildLegend()
{
	const TWeakObjectPtr<UMoonToonDebugSubsystem> Weak = Subsystem;
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 2.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("BoldFont"))
					.Text_Lambda([this]()
					{
						if (LegendViewName.IsEmpty())
						{
							return LOCTEXT("LegendOff", "图例：视图关着（在左边选一个视图）");
						}
						FString Text = FString::Printf(TEXT("图例：%s · %d 个值"), *LegendViewName, LegendItems.Num());
						if (LegendOverflow > 0)
						{
							Text += FString::Printf(TEXT(" · 另有 %d 像素超出统计容量"), LegendOverflow);
						}
						return FText::FromString(Text);
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("ShowAll", "显示全部"))
					.ToolTipText(LOCTEXT("ShowAllTip", "取消\"只亮这个值\"和单个位"))
					.IsEnabled_Lambda([Weak]()
					{
						if (!Weak.IsValid())
						{
							return false;
						}
						const MoonToonDebug::FViewSettings Settings = Weak->GetViewSettings();
						return Settings.IsolateKey >= 0 || Settings.Bit >= 0;
					})
					.OnClicked_Lambda([this]()
					{
						ModifySettings([](MoonToonDebug::FViewSettings& S) { S.IsolateKey = -1; S.Bit = -1; });
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(LegendList, SListView<FLegendPtr>)
				.ListItemsSource(&LegendItems)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SMoonToonDebugPanel::OnGenerateLegendRow)
				.OnMouseButtonClick(this, &SMoonToonDebugPanel::OnLegendClicked)
				.OnMouseButtonDoubleClick(this, &SMoonToonDebugPanel::OnLegendDoubleClicked)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("LegendHint", "单击 = 只亮这个值（再点取消）；双击 = 选中演员 / 打开 profile / 锁到这个值的代表像素"))
			]
		];
}

void SMoonToonDebugPanel::RefreshLegend()
{
	LegendItems.Reset();
	LegendViewName.Reset();
	LegendOverflow = 0;
	if (const UMoonToonDebugSubsystem* Sub = Subsystem.Get())
	{
		if (!Sub->GetCurrentViewName().IsEmpty())
		{
			const FMoonToonRegionStats Stats = Sub->GetLegend();
			LegendViewName = Stats.bValid ? Stats.ViewName : Sub->GetCurrentViewName();
			LegendOverflow = Stats.OverflowPixels;
			for (const FMoonToonRegionValue& Value : Stats.Values)
			{
				LegendItems.Add(MakeShared<FMoonToonRegionValue>(Value));
			}
			LegendItems.Sort([](const FLegendPtr& A, const FLegendPtr& B) { return A->PixelCount > B->PixelCount; });
		}
	}
	if (LegendList.IsValid())
	{
		LegendList->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SMoonToonDebugPanel::OnGenerateLegendRow(FLegendPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const TWeakObjectPtr<UMoonToonDebugSubsystem> Weak = Subsystem;
	const int64 Key = Item->Key;
	const bool bPerBit = Item->bPerBit;
	const auto IsIsolated = [Weak, Key, bPerBit]()
	{
		if (!Weak.IsValid())
		{
			return false;
		}
		const MoonToonDebug::FViewSettings Settings = Weak->GetViewSettings();
		return bPerBit ? Settings.Bit == (int32)Key : Settings.IsolateKey == (int32)Key;
	};

	return SNew(STableRow<FLegendPtr>, OwnerTable)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(2.0f, 1.0f, 6.0f, 1.0f)
			[
				MakeSwatch(Item->Swatch, 14.0f)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SBox)
				.MinDesiredWidth(56.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(bPerBit ? FString::Printf(TEXT("位 %lld"), Key) : FString::Printf(TEXT("%lld"), Key)))
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Name))
				.ColorAndOpacity_Lambda([IsIsolated]() { return IsIsolated() ? FSlateColor(FLinearColor(1.0f, 0.8f, 0.2f)) : FSlateColor::UseForeground(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 2.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("LegendPixels", "{0} px"), FText::AsNumber(Item->PixelCount)))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

void SMoonToonDebugPanel::OnLegendClicked(FLegendPtr Item)
{
	if (!Item.IsValid())
	{
		return;
	}
	const int32 Key = (int32)Item->Key;
	const bool bPerBit = Item->bPerBit;
	ModifySettings([Key, bPerBit](MoonToonDebug::FViewSettings& S)
	{
		if (bPerBit)
		{
			S.Bit = S.Bit == Key ? -1 : Key;
		}
		else
		{
			S.IsolateKey = S.IsolateKey == Key ? -1 : Key;
		}
	});
}

void SMoonToonDebugPanel::OnLegendDoubleClicked(FLegendPtr Item)
{
	UMoonToonDebugSubsystem* Sub = Subsystem.Get();
	if (!Item.IsValid() || !Sub)
	{
		return;
	}
	if (!Item->bPerBit && LegendViewName == TEXT("actor"))
	{
		for (const FMoonToonDebugActorInfo& Actor : Sub->ListToonActors(false))
		{
			if (Actor.ActorId == (int32)Item->Key && !Actor.ActorPath.IsEmpty())
			{
				SelectActor(Actor.ActorPath, false);
				return;
			}
		}
	}
	if (!Item->bPerBit && LegendViewName == TEXT("profile"))
	{
		const FString Key = FString::Printf(TEXT("id %lld"), Item->Key);
		for (const FMoonToonTableRow& Row : Sub->DumpProfiles())
		{
			if (Row.Key == Key && Row.Columns.Num() > 0)
			{
				OpenAsset(Row.Columns[0]);
				return;
			}
		}
	}
	// Anything else: inspect the value's sample pixel.
	LockOnViewportPixel(Item->SampleViewport);
}

// =====================================================================================================
// Pages

TSharedRef<SWidget> SMoonToonDebugPanel::BuildPages()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SSegmentedControl<int32>)
				.Value_Lambda([this]() { return (int32)Page; })
				.OnValueChanged_Lambda([this](int32 Value)
				{
					Page = (EPage)Value;
					if (Page == EPage::Characters && !bActorsLoaded)
					{
						RefreshActors();
					}
					else if (Page == EPage::Tables && !bTableLoaded)
					{
						RefreshTable();
					}
					else if (Page == EPage::Validation && !bIssuesLoaded)
					{
						RefreshValidation();
					}
				})
				+ SSegmentedControl<int32>::Slot((int32)EPage::Inspector).Text(LOCTEXT("PageInspector", "检查器"))
				+ SSegmentedControl<int32>::Slot((int32)EPage::Characters).Text(LOCTEXT("PageCharacters", "角色"))
				+ SSegmentedControl<int32>::Slot((int32)EPage::Tables).Text(LOCTEXT("PageTables", "表"))
				+ SSegmentedControl<int32>::Slot((int32)EPage::Validation).Text(LOCTEXT("PageValidation", "校验"))
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SWidgetSwitcher)
				.WidgetIndex_Lambda([this]() { return (int32)Page; })
				+ SWidgetSwitcher::Slot()
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(InspectorBox, SBox)
					]
				]
				+ SWidgetSwitcher::Slot()
				[
					BuildCharactersPage()
				]
				+ SWidgetSwitcher::Slot()
				[
					BuildTablesPage()
				]
				+ SWidgetSwitcher::Slot()
				[
					BuildValidationPage()
				]
			]
		];
}

// =====================================================================================================
// Inspector

void SMoonToonDebugPanel::OnReportChanged()
{
	bInspectorDirty = true;
}

void SMoonToonDebugPanel::RebuildInspector()
{
	bInspectorDirty = false;
	LastInspectorBuild = FPlatformTime::Seconds();
	if (!InspectorBox.IsValid())
	{
		return;
	}

	const UMoonToonDebugSubsystem* Sub = Subsystem.Get();
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	if (!Sub || !Sub->GetReport().bValid)
	{
		const FString Error = Sub ? Sub->GetReport().Error : FString();
		const FText Hint = FText::Format(
			LOCTEXT("InspectorEmpty", "还没有拾取像素。点 ◎ 拾取 后在关卡视口里左键，或光标停在视口上按 {0}，或打开 ↔ 跟随鼠标。"),
			UMoonToonToolsUserSettings::Get().DebugPickChord.GetInputText());
		Box->AddSlot().AutoHeight().Padding(4.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(Error.IsEmpty() ? Hint : FText::FromString(Error))
		];
		InspectorBox->SetContent(Box);
		return;
	}

	const FMoonToonPixelReport& Report = Sub->GetReport();
	const FMoonToonPixelReport* Pinned = Sub->GetPinnedReport();

	const auto HeaderLine = [](const FText& Text, const FString& Path, const FText& ButtonText, TFunction<void()> OnClick) -> TSharedRef<SWidget>
	{
		TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Text).AutoWrapText(true)
			];
		if (!Path.IsEmpty())
		{
			Line->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(ButtonText)
					.OnClicked_Lambda([OnClick]() { OnClick(); return FReply::Handled(); })
				];
		}
		return Line;
	};

	// --- header -------------------------------------------------------------------------------------
	Box->AddSlot().AutoHeight().Padding(2.0f, 1.0f)
	[
		SNew(STextBlock)
		.Font(FAppStyle::GetFontStyle("BoldFont"))
		.Text(FText::FromString(FString::Printf(TEXT("像素 视口 %d,%d → 渲染 %d,%d%s"),
			Report.ViewportPixel.X, Report.ViewportPixel.Y, Report.RenderPixel.X, Report.RenderPixel.Y,
			Report.bIsToon ? TEXT("") : TEXT("  （不是 toon 像素）"))))
	];
	if (!Report.MaterialPath.IsEmpty())
	{
		const FString Path = Report.MaterialPath;
		Box->AddSlot().AutoHeight().Padding(2.0f, 1.0f)
		[
			HeaderLine(FText::FromString(FString::Printf(TEXT("材质 %s（槽 %d）"), *FPaths::GetBaseFilename(Path), Report.MaterialSlot)),
				Path, LOCTEXT("Open", "打开"), [Path]() { OpenAsset(Path); })
		];
	}
	if (!Report.ActorPath.IsEmpty() || Report.ActorId != 0)
	{
		const FString Path = Report.ActorPath;
		Box->AddSlot().AutoHeight().Padding(2.0f, 1.0f)
		[
			HeaderLine(FText::FromString(FString::Printf(TEXT("演员 %s（id %d）"), Report.ActorName.IsEmpty() ? TEXT("?") : *Report.ActorName, Report.ActorId)),
				Path, LOCTEXT("Select", "选中"), [Path]() { SelectActor(Path, false); })
		];
	}
	if (!Report.ProfilePath.IsEmpty() || Report.ProfileId != 0)
	{
		const FString Path = Report.ProfilePath;
		Box->AddSlot().AutoHeight().Padding(2.0f, 1.0f)
		[
			HeaderLine(FText::FromString(FString::Printf(TEXT("profile %d %s"), Report.ProfileId, Path.IsEmpty() ? TEXT("") : *FPaths::GetBaseFilename(Path))),
				Path, LOCTEXT("Open", "打开"), [Path]() { OpenAsset(Path); })
		];
	}
	Box->AddSlot().AutoHeight().Padding(2.0f, 1.0f)
	[
		SNew(STextBlock)
		.ColorAndOpacity(Report.ValidateBits != 0 ? FSlateColor(FLinearColor(1.0f, 0.55f, 0.3f)) : FSlateColor::UseSubduedForeground())
		.Text(FText::FromString(Report.ValidateBits != 0
			? FString::Printf(TEXT("校验：%s"), *FString::Join(Report.ValidateNames, TEXT(" · ")))
			: FString(TEXT("校验：无问题"))))
	];
	if (Pinned)
	{
		Box->AddSlot().AutoHeight().Padding(2.0f, 3.0f)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this]() { return bOnlyDifferences ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bOnlyDifferences = State == ECheckBoxState::Checked; bInspectorDirty = true; })
			[
				SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("只看和 A（视口 %d,%d）不同的字段"), Pinned->ViewportPixel.X, Pinned->ViewportPixel.Y)))
			]
		];
	}

	const auto FindPinned = [Pinned](const TArray<FMoonToonDebugField>& PinnedFields, const FMoonToonDebugField& Field) -> const FString*
	{
		if (!Pinned)
		{
			return nullptr;
		}
		const FMoonToonDebugField* Match = PinnedFields.FindByPredicate([&Field](const FMoonToonDebugField& Other)
		{
			return Other.Section == Field.Section && Other.Name == Field.Name;
		});
		return Match ? &Match->Value : nullptr;
	};
	const auto AddSection = [this, &Box](const FString& Key, const FText& Title, const TSharedRef<SWidget>& Body)
	{
		Box->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
		[
			SNew(SExpandableArea)
			.AreaTitle(Title)
			.InitiallyCollapsed(CollapsedSections.Contains(Key))
			.OnAreaExpansionChanged_Lambda([this, Key](bool bExpanded)
			{
				if (bExpanded)
				{
					CollapsedSections.Remove(Key);
				}
				else
				{
					CollapsedSections.Add(Key);
				}
			})
			.BodyContent()
			[
				Body
			]
		];
	};
	const auto BuildFields = [this, Pinned, &FindPinned](const TArray<FMoonToonDebugField>& Fields, const TArray<FMoonToonDebugField>* PinnedFields, const FString& Section) -> TSharedPtr<SVerticalBox>
	{
		TSharedPtr<SVerticalBox> Body;
		for (const FMoonToonDebugField& Field : Fields)
		{
			if (!Section.IsEmpty() && Field.Section != Section)
			{
				continue;
			}
			const FString* PinnedValue = PinnedFields ? FindPinned(*PinnedFields, Field) : nullptr;
			if (Pinned && bOnlyDifferences && PinnedValue && *PinnedValue == Field.Value)
			{
				continue;
			}
			if (!Body.IsValid())
			{
				Body = SNew(SVerticalBox);
			}
			Body->AddSlot().AutoHeight()[ BuildFieldRow(Field, PinnedValue) ];
		}
		return Body;
	};

	// --- the decoded record, surface, features, buffers, style ---------------------------------------
	TArray<FString> Sections;
	for (const TCHAR* Section : GSectionOrder)
	{
		Sections.Add(Section);
	}
	for (const FMoonToonDebugField& Field : Report.Fields)
	{
		Sections.AddUnique(Field.Section);
	}
	for (const FString& Section : Sections)
	{
		const TSharedPtr<SVerticalBox> Body = BuildFields(Report.Fields, Pinned ? &Pinned->Fields : nullptr, Section);
		if (Body.IsValid())
		{
			AddSection(Section, SectionLabel(Section), Body.ToSharedRef());
		}
	}

	// --- colour breakdown (measured stages) -----------------------------------------------------------
	if (Report.Stages.Num() > 0)
	{
		TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
		Body->AddSlot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 4.0f)[ BuildStageBars(Report) ];
		if (const TSharedPtr<SVerticalBox> Rows = BuildFields(Report.Stages, Pinned ? &Pinned->Stages : nullptr, FString()))
		{
			Body->AddSlot().AutoHeight()[ Rows.ToSharedRef() ];
		}
		AddSection(TEXT("stages"), LOCTEXT("SectionStages", "颜色分解（实测，按阶段，未乘曝光）"), Body);
	}

	// --- main light (re-evaluated) -------------------------------------------------------------------
	if (Report.Trace.Num() > 0)
	{
		TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
		const float Agreement = Report.MainLightAgreement;
		const bool bAgrees = Agreement > 0.0f && FMath::Abs(1.0f - Agreement) <= 0.02f;
		Body->AddSlot().AutoHeight().Padding(12.0f, 1.0f, 0.0f, 3.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(Agreement <= 0.0f ? FSlateColor::UseSubduedForeground()
				: (bAgrees ? FSlateColor(FLinearColor(0.4f, 0.9f, 0.5f)) : FSlateColor(FLinearColor(1.0f, 0.8f, 0.3f))))
			.Text(FText::FromString(Agreement <= 0.0f
				? FString::Printf(TEXT("主光 %s · 没有直接光阶段的实测值可对比"), *Report.MainLightName)
				: FString::Printf(TEXT("主光 %s · 复算 / 实测直接光 = %.0f%%%s"), *Report.MainLightName, Agreement * 100.0f,
					bAgrees ? TEXT("（主光能解释全部直接光）") : TEXT("（还有别的灯或效果在加光，见阶段账本）"))))
		];
		if (const TSharedPtr<SVerticalBox> Rows = BuildFields(Report.Trace, Pinned ? &Pinned->Trace : nullptr, FString()))
		{
			Body->AddSlot().AutoHeight()[ Rows.ToSharedRef() ];
		}
		AddSection(TEXT("main_light"), LOCTEXT("SectionMainLight", "主光（复算 + TRACE）"), Body);
	}

	InspectorBox->SetContent(Box);
}

TSharedRef<SWidget> SMoonToonDebugPanel::BuildFieldRow(const FMoonToonDebugField& Field, const FString* PinnedValue)
{
	const FString Key = Field.Section + TEXT(".") + Field.Name;
	const TWeakObjectPtr<UMoonToonDebugSubsystem> Weak = Subsystem;
	const auto ValueColor = [Weak, Key]() -> FSlateColor
	{
		const UMoonToonDebugSubsystem* Sub = Weak.Get();
		if (Sub && Sub->GetChangedFields().Contains(Key) && FPlatformTime::Seconds() - Sub->GetReportTime() < 1.0)
		{
			return FSlateColor(FLinearColor(1.0f, 0.8f, 0.2f));
		}
		return FSlateColor::UseForeground();
	};

	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Top)
		.Padding(12.0f, 1.0f, 6.0f, 1.0f)
		[
			SNew(SBox)
			.WidthOverride(200.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Field.Name))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
	if (Field.Swatch.A > 0.0f)
	{
		Row->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				MakeSwatch(Field.Swatch)
			];
	}
	Row->AddSlot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Top)
		.Padding(0.0f, 1.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Field.Value))
			.ColorAndOpacity_Lambda(ValueColor)
			.AutoWrapText(true)
		];
	if (!Field.Source.IsEmpty())
	{
		Row->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(6.0f, 1.0f, 0.0f, 1.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("[%s]"), *Field.Source)))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
		const FString AssetPath = AssetPathInSource(Field.Source);
		if (!AssetPath.IsEmpty())
		{
			Row->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(FText::FromString(AssetPath))
					.OnClicked_Lambda([AssetPath]() { OpenAsset(AssetPath); return FReply::Handled(); })
					[
						SNew(STextBlock).Text(LOCTEXT("OpenSmall", "打开"))
					]
				];
		}
	}
	if (PinnedValue && *PinnedValue != Field.Value)
	{
		Row->AddSlot()
			.FillWidth(0.6f)
			.VAlign(VAlign_Top)
			.Padding(8.0f, 1.0f, 0.0f, 1.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("A: %s"), **PinnedValue)))
				.ColorAndOpacity(FLinearColor(1.0f, 0.6f, 0.25f))
				.AutoWrapText(true)
			];
	}
	return Row;
}

TSharedRef<SWidget> SMoonToonDebugPanel::BuildStageBars(const FMoonToonPixelReport& Report)
{
	// Each stage's measured increment, from the cumulative scene colour after it (linear, un-exposed).
	// The tonemap stages are not increments: PreTonemap repeats the sum, PostTonemap is the display value.
	struct FSegment
	{
		FString Name;
		FLinearColor Delta;
		int32 Order = 0;
	};
	TArray<FSegment> Segments;
	FLinearColor Previous(0.0f, 0.0f, 0.0f, 0.0f);
	bool bHavePrevious = false;
	for (const FMoonToonDebugField& Stage : Report.Stages)
	{
		if (Stage.Name.Contains(TEXT("Tonemap")))
		{
			continue;
		}
		const FLinearColor Value(Stage.Swatch.R, Stage.Swatch.G, Stage.Swatch.B, 0.0f);
		FSegment& Segment = Segments.AddDefaulted_GetRef();
		Segment.Name = Stage.Name;
		Segment.Delta = bHavePrevious ? Value - Previous : Value;
		Segment.Order = Segments.Num() - 1;
		Previous = Value;
		bHavePrevious = true;
	}

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	const TCHAR* const ChannelNames[] = { TEXT("R"), TEXT("G"), TEXT("B") };
	FString Negatives;
	for (int32 Channel = 0; Channel < 3; ++Channel)
	{
		const auto Component = [Channel](const FLinearColor& C) { return Channel == 0 ? C.R : (Channel == 1 ? C.G : C.B); };
		float Total = 0.0f;
		for (const FSegment& Segment : Segments)
		{
			Total += FMath::Max(Component(Segment.Delta), 0.0f);
		}
		TSharedRef<SHorizontalBox> Fill = SNew(SHorizontalBox);
		for (const FSegment& Segment : Segments)
		{
			const float Value = Component(Segment.Delta);
			if (Value > 0.0f && Total > 0.0f)
			{
				Fill->AddSlot()
					.FillWidth(Value / Total)
					[
						SNew(SBox)
						.HeightOverride(12.0f)
						.ToolTipText(FText::FromString(FString::Printf(TEXT("%s %s +%.4f (%.0f%%)"), *Segment.Name, ChannelNames[Channel], Value, Value / Total * 100.0f)))
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("WhiteBrush"))
							.ColorAndOpacity(StageColor(Segment.Order))
						]
					];
			}
			else if (Value < -1e-4f)
			{
				Negatives += FString::Printf(TEXT("%s %s %.4f  "), *Segment.Name, ChannelNames[Channel], Value);
			}
		}
		Box->AddSlot().AutoHeight().Padding(12.0f, 1.0f, 0.0f, 1.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(14.0f)[ SNew(STextBlock).Text(FText::FromString(ChannelNames[Channel])) ]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				Fill
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("%.4f"), Total)))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
	}

	TSharedRef<SWrapBox> Key = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(10.0f, 2.0f));
	for (const FSegment& Segment : Segments)
	{
		if (Segment.Delta.R <= 0.0f && Segment.Delta.G <= 0.0f && Segment.Delta.B <= 0.0f)
		{
			continue;
		}
		Key->AddSlot()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 3.0f, 0.0f)[ MakeSwatch(StageColor(Segment.Order), 10.0f) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ SNew(STextBlock).Text(FText::FromString(Segment.Name)).ColorAndOpacity(FSlateColor::UseSubduedForeground()) ]
		];
	}
	Box->AddSlot().AutoHeight().Padding(32.0f, 2.0f, 0.0f, 0.0f)[ Key ];
	if (!Negatives.IsEmpty())
	{
		Box->AddSlot().AutoHeight().Padding(32.0f, 2.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FLinearColor(1.0f, 0.45f, 0.4f))
			.Text(FText::FromString(FString::Printf(TEXT("压暗：%s"), *Negatives)))
		];
	}
	return Box;
}

// =====================================================================================================
// Characters

TSharedRef<SWidget> SMoonToonDebugPanel::BuildCharactersPage()
{
	const auto Header = [](const FText& Text, float Width) -> TSharedRef<SWidget>
	{
		return SNew(SBox)
			.WidthOverride(Width > 0.0f ? FOptionalSize(Width) : FOptionalSize())
			[
				SNew(STextBlock).Text(Text).Font(FAppStyle::GetFontStyle("BoldFont"))
			];
	};

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]() { return bOnlyOnScreen ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bOnlyOnScreen = State == ECheckBoxState::Checked; RefreshActors(); })
				[
					SNew(STextBlock).Text(LOCTEXT("OnlyOnScreen", "只看屏幕上的"))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(8.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Refresh", "刷新"))
				.ToolTipText(LOCTEXT("RefreshActorsTip", "重新列出目标世界里的 toon 演员，并回读 GPU 的角色表和屏幕像素数（会画一帧目标视口）。"))
				.OnClicked_Lambda([this]() { RefreshActors(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNullWidget::NullWidget
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("SelectInOutliner", "在大纲里选中"))
				.IsEnabled_Lambda([this]() { return SelectedActor.IsValid(); })
				.OnClicked_Lambda([this]() { if (SelectedActor.IsValid()) { SelectActor(SelectedActor->ActorPath, false); } return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("FocusViewport", "聚焦视口"))
				.IsEnabled_Lambda([this]() { return SelectedActor.IsValid(); })
				.OnClicked_Lambda([this]() { if (SelectedActor.IsValid()) { SelectActor(SelectedActor->ActorPath, true); } return FReply::Handled(); })
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()[ Header(LOCTEXT("ColId", "id"), 40.0f) ]
			+ SHorizontalBox::Slot().AutoWidth()[ Header(LOCTEXT("ColActor", "演员"), 220.0f) ]
			+ SHorizontalBox::Slot().AutoWidth()[ Header(LOCTEXT("ColComponent", "组件"), 44.0f) ]
			+ SHorizontalBox::Slot().AutoWidth()[ Header(LOCTEXT("ColPixels", "屏幕像素"), 90.0f) ]
			+ SHorizontalBox::Slot().FillWidth(1.0f)[ Header(LOCTEXT("ColWarnings", "警告"), 0.0f) ]
		]
		+ SVerticalBox::Slot()
		.FillHeight(0.45f)
		[
			SAssignNew(ActorList, SListView<FActorPtr>)
			.ListItemsSource(&Actors)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SMoonToonDebugPanel::OnGenerateActorRow)
			.OnSelectionChanged(this, &SMoonToonDebugPanel::OnActorSelected)
			.OnMouseButtonDoubleClick_Lambda([](FActorPtr Item) { if (Item.IsValid()) { SelectActor(Item->ActorPath, true); } })
		]
		+ SVerticalBox::Slot()
		.FillHeight(0.55f)
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SAssignNew(ActorDetailsBox, SBox)
			]
		];
}

void SMoonToonDebugPanel::RefreshActors()
{
	bActorsLoaded = true;
	const FString SelectedPath = SelectedActor.IsValid() ? SelectedActor->ActorPath : FString();
	Actors.Reset();
	SelectedActor.Reset();
	if (UMoonToonDebugSubsystem* Sub = Subsystem.Get())
	{
		for (const FMoonToonDebugActorInfo& Actor : Sub->ListToonActors(true))
		{
			if (!bOnlyOnScreen || Actor.PixelCount > 0)
			{
				Actors.Add(MakeShared<FMoonToonDebugActorInfo>(Actor));
			}
		}
		Actors.Sort([](const FActorPtr& A, const FActorPtr& B)
		{
			return A->PixelCount != B->PixelCount ? A->PixelCount > B->PixelCount : A->ActorId < B->ActorId;
		});
	}
	if (ActorList.IsValid())
	{
		ActorList->RequestListRefresh();
		for (const FActorPtr& Actor : Actors)
		{
			if (Actor->ActorPath == SelectedPath)
			{
				ActorList->SetSelection(Actor);
				break;
			}
		}
	}
	if (!SelectedActor.IsValid())
	{
		OnActorSelected(nullptr, ESelectInfo::Direct);
	}
}

TSharedRef<ITableRow> SMoonToonDebugPanel::OnGenerateActorRow(FActorPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const auto Cell = [](const FString& Text, float Width, FSlateColor Color = FSlateColor::UseForeground()) -> TSharedRef<SWidget>
	{
		return SNew(SBox)
			.WidthOverride(Width)
			[
				SNew(STextBlock).Text(FText::FromString(Text)).ColorAndOpacity(Color)
			];
	};
	const FString Warnings = FString::Join(Item->Warnings, TEXT(" · "));
	return SNew(STableRow<FActorPtr>, OwnerTable)
		.Padding(FMargin(4.0f, 1.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()[ Cell(Item->ActorId != 0 ? FString::FromInt(Item->ActorId) : FString(TEXT("—")), 40.0f) ]
			+ SHorizontalBox::Slot().AutoWidth()[ Cell(Item->ActorLabel, 220.0f) ]
			+ SHorizontalBox::Slot().AutoWidth()[ Cell(Item->bHasComponent ? TEXT("✓") : TEXT("✗"), 44.0f) ]
			+ SHorizontalBox::Slot().AutoWidth()[ Cell(FText::AsNumber(Item->PixelCount).ToString(), 90.0f) ]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Warnings.IsEmpty() ? FString(TEXT("—")) : TEXT("⚠ ") + Warnings))
				.ColorAndOpacity(Warnings.IsEmpty() ? FSlateColor::UseSubduedForeground() : FSlateColor(FLinearColor(1.0f, 0.75f, 0.3f)))
				.ToolTipText(FText::FromString(Warnings))
			]
		];
}

void SMoonToonDebugPanel::OnActorSelected(FActorPtr Item, ESelectInfo::Type SelectInfo)
{
	SelectedActor = Item;
	if (!ActorDetailsBox.IsValid())
	{
		return;
	}
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	if (!Item.IsValid())
	{
		Box->AddSlot().AutoHeight().Padding(4.0f)
		[
			SNew(STextBlock)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(bActorsLoaded && Actors.Num() == 0
				? LOCTEXT("NoActors", "目标世界里没有（屏幕上的）toon 演员。")
				: LOCTEXT("PickActor", "选一个演员看它的角色表、材质和风格链。双击 = 聚焦视口。"))
		];
		ActorDetailsBox->SetContent(Box);
		return;
	}

	Box->AddSlot().AutoHeight().Padding(2.0f, 2.0f)
	[
		SNew(STextBlock)
		.Font(FAppStyle::GetFontStyle("BoldFont"))
		.Text(FText::FromString(FString::Printf(TEXT("%s  ·  %s"), *Item->ActorLabel, *Item->ActorPath)))
	];
	if (Item->ActorRows.Num() > 0)
	{
		Box->AddSlot().AutoHeight().Padding(2.0f, 4.0f, 2.0f, 1.0f)
		[
			SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("角色表 %d 行（GPU 本帧）"), Item->ActorRows.Num())))
		];
		for (const FMoonToonDebugField& Row : Item->ActorRows)
		{
			Box->AddSlot().AutoHeight()[ BuildFieldRow(Row, nullptr) ];
		}
	}
	else if (Item->bHasComponent)
	{
		Box->AddSlot().AutoHeight().Padding(2.0f, 4.0f)
		[
			SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("NoRows", "没有回读到角色表（视口没在画？点刷新再试）。"))
		];
	}
	if (Item->Materials.Num() > 0)
	{
		Box->AddSlot().AutoHeight().Padding(2.0f, 6.0f, 2.0f, 1.0f)
		[
			SNew(STextBlock).Text(LOCTEXT("Materials", "材质"))
		];
		for (const FString& Material : Item->Materials)
		{
			// "<material path> -> profile <id> <asset>"
			FString Path = Material;
			int32 Arrow = Material.Find(TEXT(" -> "));
			if (Arrow != INDEX_NONE)
			{
				Path = Material.Left(Arrow);
			}
			Box->AddSlot().AutoHeight().Padding(12.0f, 1.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(FText::FromString(Material)).AutoWrapText(true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.OnClicked_Lambda([Path]() { OpenAsset(Path); return FReply::Handled(); })
					[
						SNew(STextBlock).Text(LOCTEXT("OpenSmall", "打开"))
					]
				]
			];
		}
	}
	for (const FString& Warning : Item->Warnings)
	{
		Box->AddSlot().AutoHeight().Padding(2.0f, 2.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FLinearColor(1.0f, 0.75f, 0.3f))
			.Text(FText::FromString(TEXT("⚠ ") + Warning))
		];
	}
	ActorDetailsBox->SetContent(Box);
}

// =====================================================================================================
// Tables

TSharedRef<SWidget> SMoonToonDebugPanel::BuildTablesPage()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SSegmentedControl<int32>)
				.Value_Lambda([this]() { return (int32)Table; })
				.OnValueChanged_Lambda([this](int32 Value) { Table = (ETable)Value; RefreshTable(); })
				+ SSegmentedControl<int32>::Slot((int32)ETable::ConstantTable).Text(LOCTEXT("TableConstant", "常量表"))
				+ SSegmentedControl<int32>::Slot((int32)ETable::RampAtlas).Text(LOCTEXT("TableRamp", "ramp 图集"))
				+ SSegmentedControl<int32>::Slot((int32)ETable::Profiles).Text(LOCTEXT("TableProfiles", "profile"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(8.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Refresh", "刷新"))
				.OnClicked_Lambda([this]() { RefreshTable(); return FReply::Handled(); })
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(TableList, SListView<FRowPtr>)
			.ListItemsSource(&TableRows)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SMoonToonDebugPanel::OnGenerateTableRow)
			.OnMouseButtonClick(this, &SMoonToonDebugPanel::OnTableRowClicked)
			.OnMouseButtonDoubleClick(this, &SMoonToonDebugPanel::OnTableRowDoubleClicked)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("TableHint", "单击一行 = 视图切到对应字段并只亮这个值；双击 = 打开资产"))
		];
}

void SMoonToonDebugPanel::RefreshTable()
{
	bTableLoaded = true;
	TableRows.Reset();
	if (UMoonToonDebugSubsystem* Sub = Subsystem.Get())
	{
		const TArray<FMoonToonTableRow> Rows = Table == ETable::ConstantTable ? Sub->DumpConstantTable()
			: (Table == ETable::RampAtlas ? Sub->DumpRampAtlas() : Sub->DumpProfiles());
		for (const FMoonToonTableRow& Row : Rows)
		{
			TableRows.Add(MakeShared<FMoonToonTableRow>(Row));
		}
	}
	if (TableList.IsValid())
	{
		TableList->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SMoonToonDebugPanel::OnGenerateTableRow(FRowPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<FRowPtr>, OwnerTable)
		.Padding(FMargin(4.0f, 1.0f))
		.ToolTipText(FText::FromString(FString::Join(Item->ObjectPaths, TEXT("\n"))))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			[
				SNew(SBox)
				.WidthOverride(110.0f)
				[
					SNew(STextBlock).Text(FText::FromString(Item->Key)).Font(FAppStyle::GetFontStyle("BoldFont"))
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Top)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Join(Item->Columns, TEXT("  │  "))))
				.AutoWrapText(true)
			]
		];
}

void SMoonToonDebugPanel::OnTableRowClicked(FRowPtr Item)
{
	if (!Item.IsValid())
	{
		return;
	}
	const int32 Number = TrailingNumber(Item->Key);
	if (Number == INDEX_NONE)
	{
		return;	// the table's header row
	}
	FString View;
	if (Table == ETable::ConstantTable)
	{
		View = TEXT("ctrow");
	}
	else if (Table == ETable::Profiles)
	{
		View = TEXT("profile");
	}
	else if (Item->Key.StartsWith(TEXT("diffuse")))
	{
		View = TEXT("rampidx");
	}
	if (View.IsEmpty() || !MoonToonDebug::FindView(View))
	{
		return;
	}
	ModifySettings([&View, Number](MoonToonDebug::FViewSettings& S)
	{
		S.View = View;
		S.Channel = 0;
		S.Bit = -1;
		S.RangeMin = S.RangeMax = 0.0f;
		S.IsolateKey = Number;
	});
}

void SMoonToonDebugPanel::OnTableRowDoubleClicked(FRowPtr Item)
{
	if (!Item.IsValid())
	{
		return;
	}
	if (Item->ObjectPaths.Num() > 0)
	{
		OpenAsset(Item->ObjectPaths[0]);
	}
	else if (Table == ETable::Profiles && Item->Columns.Num() > 0)
	{
		OpenAsset(Item->Columns[0]);
	}
	else if (Table == ETable::RampAtlas && Item->Columns.Num() > 0)
	{
		OpenAsset(Item->Columns[0]);
	}
}

// =====================================================================================================
// Validation

TSharedRef<SWidget> SMoonToonDebugPanel::BuildValidationPage()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.ToolTipText(LOCTEXT("GpuChecksTip", "加上 GPU 自检（记录截断、保留位、坏格式、坏常量表行…）每一位的像素数；会画一帧目标视口。"))
				.IsChecked_Lambda([this]() { return bGpuChecks ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bGpuChecks = State == ECheckBoxState::Checked; })
				[
					SNew(STextBlock).Text(LOCTEXT("GpuChecks", "含 GPU 自检"))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(8.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("RunValidation", "运行校验"))
				.OnClicked_Lambda([this]() { RefreshValidation(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([this]()
				{
					int32 Errors = 0, Warnings = 0;
					for (const FIssuePtr& Issue : Issues)
					{
						Errors += Issue->Severity == TEXT("ERROR") ? 1 : 0;
						Warnings += Issue->Severity == TEXT("WARN") ? 1 : 0;
					}
					return bIssuesLoaded ? FText::FromString(FString::Printf(TEXT("%d 错误 · %d 警告 · 共 %d 条"), Errors, Warnings, Issues.Num())) : FText::GetEmpty();
				})
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(IssueList, SListView<FIssuePtr>)
			.ListItemsSource(&Issues)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SMoonToonDebugPanel::OnGenerateIssueRow)
			.OnMouseButtonClick(this, &SMoonToonDebugPanel::OnIssueClicked)
			.OnMouseButtonDoubleClick_Lambda([](FIssuePtr Item)
			{
				if (Item.IsValid() && !Item->ObjectPath.IsEmpty() && !Cast<AActor>(ResolveObject(Item->ObjectPath)))
				{
					OpenAsset(Item->ObjectPath);
				}
			})
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("IssueHint", "单击 = 选中演员 / 把检查器锁到这个问题的代表像素；双击 = 打开资产"))
		];
}

void SMoonToonDebugPanel::RefreshValidation()
{
	bIssuesLoaded = true;
	Issues.Reset();
	if (UMoonToonDebugSubsystem* Sub = Subsystem.Get())
	{
		for (const FMoonToonValidationIssue& Issue : Sub->Validate(bGpuChecks))
		{
			Issues.Add(MakeShared<FMoonToonValidationIssue>(Issue));
		}
	}
	if (IssueList.IsValid())
	{
		IssueList->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SMoonToonDebugPanel::OnGenerateIssueRow(FIssuePtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FLinearColor Color = Item->Severity == TEXT("ERROR") ? FLinearColor(1.0f, 0.4f, 0.35f)
		: (Item->Severity == TEXT("WARN") ? FLinearColor(1.0f, 0.75f, 0.3f) : FLinearColor(0.6f, 0.6f, 0.6f));
	return SNew(STableRow<FIssuePtr>, OwnerTable)
		.Padding(FMargin(4.0f, 2.0f))
		.ToolTipText(FText::FromString(Item->ObjectPath))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			[
				SNew(SBox)
				.WidthOverride(56.0f)
				[
					SNew(STextBlock).Text(FText::FromString(Item->Severity)).ColorAndOpacity(Color)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			[
				SNew(SBox)
				.WidthOverride(220.0f)
				[
					SNew(STextBlock).Text(FText::FromString(Item->Where)).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Top)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->SamplePixel.X >= 0
					? FString::Printf(TEXT("%s  （代表像素 %d,%d）"), *Item->Message, Item->SamplePixel.X, Item->SamplePixel.Y)
					: Item->Message))
				.AutoWrapText(true)
			]
		];
}

void SMoonToonDebugPanel::OnIssueClicked(FIssuePtr Item)
{
	if (!Item.IsValid())
	{
		return;
	}
	if (!Item->ObjectPath.IsEmpty() && Cast<AActor>(ResolveObject(Item->ObjectPath)))
	{
		SelectActor(Item->ObjectPath, false);
	}
	if (Item->SamplePixel.X >= 0 && Item->SamplePixel.Y >= 0)
	{
		LockOnRenderPixel(Item->SamplePixel);
	}
}

// =====================================================================================================
// Helpers

UObject* SMoonToonDebugPanel::ResolveObject(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	const FSoftObjectPath SoftPath(Path);
	if (UObject* Object = SoftPath.ResolveObject())
	{
		return Object;
	}
	return SoftPath.TryLoad();
}

void SMoonToonDebugPanel::OpenAsset(const FString& Path)
{
	UObject* Object = ResolveObject(Path);
	if (Object && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditors->OpenEditorForAsset(Object);
		}
	}
}

void SMoonToonDebugPanel::SelectActor(const FString& Path, bool bFocus)
{
	AActor* Actor = Cast<AActor>(ResolveObject(Path));
	if (!Actor || !GEditor)
	{
		return;
	}
	GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
	GEditor->SelectActor(Actor, /*bInSelected*/ true, /*bNotify*/ true);
	if (bFocus)
	{
		GEditor->MoveViewportCamerasToActor(*Actor, /*bActiveViewportOnly*/ false);
	}
}

void SMoonToonDebugPanel::LockOnViewportPixel(FIntPoint ViewportPixel)
{
	UMoonToonDebugSubsystem* Sub = Subsystem.Get();
	FLevelEditorViewportClient* Client = Sub ? Sub->GetTargetViewportClient() : nullptr;
	if (!Client)
	{
		return;
	}
	Sub->SetFollowMouse(false);
	Sub->PickAt(Client, ViewportPixel);
	Sub->SetLocked(true);
	Page = EPage::Inspector;
}

void SMoonToonDebugPanel::LockOnRenderPixel(FIntPoint RenderPixel)
{
	const UMoonToonDebugSubsystem* Sub = Subsystem.Get();
	if (!Sub)
	{
		return;
	}
	// Render -> viewport pixel with the last report's sizes (the screen percentage); 1:1 without one.
	const FMoonToonPixelReport& Report = Sub->GetReport();
	FIntPoint Pixel = RenderPixel;
	if (Report.bValid && Report.RenderSize.X > 0 && Report.RenderSize.Y > 0)
	{
		Pixel.X = FMath::FloorToInt((RenderPixel.X + 0.5f) * Report.ViewportSize.X / (float)Report.RenderSize.X);
		Pixel.Y = FMath::FloorToInt((RenderPixel.Y + 0.5f) * Report.ViewportSize.Y / (float)Report.RenderSize.Y);
	}
	LockOnViewportPixel(Pixel);
}

#undef LOCTEXT_NAMESPACE
