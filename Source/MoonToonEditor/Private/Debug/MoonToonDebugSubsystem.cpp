// Copyright Dream Moon. All Rights Reserved.

// UMoonToonDebugSubsystem: lifetime, ticking, views, picking, the synchronous inspector / statistics and the
// name caches. The report builder and JSON are in MoonToonDebugReport.cpp; captures, diffs, tables, style
// layers and validation in MoonToonDebugTools.cpp.

#include "MoonToonDebugSubsystem.h"

#include "MoonToonDebugInput.h"
#include "MoonToonDebugDefinitions.h"	// Engine/Shaders/Shared (the "Shaders" include module)

#include "CanvasItem.h"
#include "Components/PrimitiveComponent.h"
#include "Components/MoonToonActorComponent.h"
#include "Debug/DebugDrawService.h"
#include "Editor.h"
#include "Engine/Canvas.h"
#include "Engine/MoonToonProfile.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "LevelEditorViewport.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/MoonToonDebugSnapshots.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "SEditorViewport.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogMoonToonDebug, Log, All);

UMoonToonDebugSubsystem* UMoonToonDebugSubsystem::Get()
{
	return GEditor ? GEditor->GetEditorSubsystem<UMoonToonDebugSubsystem>() : nullptr;
}

void UMoonToonDebugSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UMoonToonDebugSubsystem::Tick));
	if (FSlateApplication::IsInitialized())
	{
		InputProcessor = MakeShared<FMoonToonDebugInputProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
	}
	DrawHandle = UDebugDrawService::Register(TEXT("Editor"), FDebugDrawDelegate::CreateUObject(this, &UMoonToonDebugSubsystem::DrawMarker));
}

void UMoonToonDebugSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
	UDebugDrawService::Unregister(DrawHandle);
	Super::Deinitialize();
}

bool UMoonToonDebugSubsystem::IsAvailable(FString* OutWhyNot) const
{
	FString Why;
	if (MoonToonDebug::GetApiVersion() != MOONTOON_DEBUG_API_VERSION)
	{
		Why = FString::Printf(TEXT("engine debug API v%d, plugin built for v%d -- rebuild the plugin against this engine"),
			MoonToonDebug::GetApiVersion(), MOONTOON_DEBUG_API_VERSION);
	}
	else if (!MoonToonDebug::IsAvailable())
	{
		Why = TEXT("the renderer's debug toolset is off: needs r.MoonToon.Debug.Shaders=1 ([SystemSettings], restart), r.Substrate=1 with the container GBuffer (not blendable), a PC platform and debug view modes allowed");
	}
	if (OutWhyNot)
	{
		*OutWhyNot = Why;
	}
	return Why.IsEmpty();
}

// -------------------------------------------------------------------------------------------------
// Views

static const TCHAR* GroupName(uint8 Group)
{
	switch (Group)
	{
	case MOONTOON_DEBUG_GROUP_RECORD:	return TEXT("record");
	case MOONTOON_DEBUG_GROUP_VALIDATE:	return TEXT("validate");
	case MOONTOON_DEBUG_GROUP_BUFFER:	return TEXT("buffer");
	default:							return TEXT("shade");
	}
}

static const TCHAR* EncodingName(uint8 Encoding)
{
	switch (Encoding)
	{
	case MOONTOON_DEBUG_ENC_ID:		return TEXT("id");
	case MOONTOON_DEBUG_ENC_ENUM:	return TEXT("enum");
	case MOONTOON_DEBUG_ENC_BITS:	return TEXT("bits");
	case MOONTOON_DEBUG_ENC_UNORM:	return TEXT("unorm");
	case MOONTOON_DEBUG_ENC_SIGNED:	return TEXT("signed");
	case MOONTOON_DEBUG_ENC_COLOR:	return TEXT("color");
	case MOONTOON_DEBUG_ENC_VECTOR:	return TEXT("vector");
	default:						return TEXT("float");
	}
}

static TArray<FString> ChannelNamesForView(const FString& Name)
{
	if (Name == TEXT("ch"))					return { TEXT("A"), TEXT("B"), TEXT("C"), TEXT("D"), TEXT("Vector.r"), TEXT("Vector.g"), TEXT("Vector.b"), TEXT("E"), TEXT("F"), TEXT("G"), TEXT("Vector.a") };
	if (Name == TEXT("rampidx") || Name == TEXT("rampofs"))	return { TEXT("diffuse"), TEXT("specular") };
	if (Name == TEXT("sdfsoft"))			return { TEXT("soft weight"), TEXT("soft U") };
	if (Name == TEXT("buf_mainlightmask"))	return { TEXT("shadow (R)"), TEXT("hair (G)"), TEXT("both") };
	if (Name == TEXT("buf_litmask"))		return { TEXT("lit mask"), TEXT("bleed slope") };
	return {};
}

TArray<FMoonToonDebugViewInfo> UMoonToonDebugSubsystem::GetViews() const
{
	TArray<FMoonToonDebugViewInfo> Out;
	for (const MoonToonDebug::FDebugViewInfo& View : MoonToonDebug::GetViewInfos())
	{
		FMoonToonDebugViewInfo& Info = Out.AddDefaulted_GetRef();
		Info.Id = View.Id;
		Info.Name = View.Name;
		Info.DisplayName = View.DisplayName;
		Info.Group = GroupName(View.Group);
		Info.Encoding = EncodingName(View.Encoding);
		Info.ChannelCount = View.ChannelCount;
		Info.ChannelNames = ChannelNamesForView(View.Name);
		Info.bHasBit = (View.Flags & MOONTOON_DEBUG_VIEWF_BIT) != 0u;
		Info.bHasResolved = (View.Flags & MOONTOON_DEBUG_VIEWF_RESOLVED) != 0u;
	}
	return Out;
}

MoonToonDebug::FViewSettings UMoonToonDebugSubsystem::GetViewSettings() const
{
	return MoonToonDebug::GetViewSettings();
}

void UMoonToonDebugSubsystem::SetViewSettings(const MoonToonDebug::FViewSettings& Settings)
{
	MoonToonDebug::SetViewSettings(Settings);
	LastViewName = GetCurrentViewName();
	OnViewChanged.Broadcast();
}

void UMoonToonDebugSubsystem::SetView(const FString& Name)
{
	MoonToonDebug::FViewSettings Settings = GetViewSettings();
	Settings.View = Name;
	Settings.Channel = 0;
	Settings.Bit = -1;
	Settings.IsolateKey = -1;
	Settings.RangeMin = Settings.RangeMax = 0.0f;
	SetViewSettings(Settings);
}

FString UMoonToonDebugSubsystem::GetCurrentViewName() const
{
	const MoonToonDebug::FDebugViewInfo* View = MoonToonDebug::FindView(MoonToonDebug::GetViewSettings().View);
	return View ? View->Name : FString();
}

FMoonToonRegionStats UMoonToonDebugSubsystem::GetLegend() const
{
	const MoonToonDebug::FRegionStatsResult Stats = MoonToonDebug::GetLastLegendStats();
	return BuildStats(Stats, Stats.ViewName);
}

// -------------------------------------------------------------------------------------------------
// Ticking

bool UMoonToonDebugSubsystem::Tick(float DeltaTime)
{
	MoonToonDebug::PumpResults();
	if (!MoonToonDebug::IsAvailable())
	{
		return true;
	}

	// The console (or another tool) changed the view: tell the panel.
	const FString ViewName = GetCurrentViewName();
	if (ViewName != LastViewName)
	{
		LastViewName = ViewName;
		OnViewChanged.Broadcast();
	}
	UpdateLegendNames();

	// Follow the mouse / keep the locked pixel fresh: one inspection in flight at a time.
	if (!bInspectInFlight)
	{
		FLevelEditorViewportClient* Client = nullptr;
		FIntPoint Pixel;
		if (bFollowMouse && FindViewportUnderCursor(Client, Pixel))
		{
			PickAt(Client, Pixel);
		}
		else if (bLocked && bHasPick)
		{
			for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
			{
				if (Candidate && Candidate->ViewState.GetReference() == PickViewKey)
				{
					PickAt(Candidate, PickPixel);
					break;
				}
			}
		}
	}
	return true;
}

void UMoonToonDebugSubsystem::UpdateLegendNames()
{
	const MoonToonDebug::FRegionStatsResult Stats = MoonToonDebug::GetLastLegendStats();
	const FString Key = FString::Printf(TEXT("%s/%llu"), *Stats.ViewName, Stats.FrameNumber);
	if (Stats.ViewName.IsEmpty() || Key == LastLegendView)
	{
		return;
	}
	LastLegendView = Key;
	RefreshNameCaches(false);
	TMap<uint32, FString> Names;
	for (const MoonToonDebug::FRegionValue& Value : Stats.Values)
	{
		if (!Value.bPerBit)
		{
			const FString Name = NameForValue(Stats.ViewName, Value.Key, false);
			if (!Name.IsEmpty())
			{
				Names.Add(Value.Key, Name);
			}
		}
	}
	MoonToonDebug::SetLegendNames(Names);
}

// -------------------------------------------------------------------------------------------------
// Picking

FLevelEditorViewportClient* UMoonToonDebugSubsystem::GetTargetViewportClient() const
{
	if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
	{
		return GCurrentLevelEditingViewportClient;
	}
	if (GEditor)
	{
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client && Client->Viewport && Client->IsPerspective())
			{
				return Client;
			}
		}
	}
	return nullptr;
}

UWorld* UMoonToonDebugSubsystem::GetTargetWorld() const
{
	FLevelEditorViewportClient* Client = GetTargetViewportClient();
	if (Client && Client->GetWorld())
	{
		return Client->GetWorld();
	}
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

bool UMoonToonDebugSubsystem::FindViewportUnderCursor(FLevelEditorViewportClient*& OutClient, FIntPoint& OutPixel) const
{
	if (!GEditor || !FSlateApplication::IsInitialized())
	{
		return false;
	}
	const FVector2D Cursor = FSlateApplication::Get().GetCursorPos();
	for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
	{
		if (!Client || !Client->Viewport)
		{
			continue;
		}
		const TSharedPtr<SEditorViewport> Widget = Client->GetEditorViewportWidget();
		if (!Widget.IsValid())
		{
			continue;
		}
		const FGeometry& Geometry = Widget->GetTickSpaceGeometry();
		if (!Geometry.IsUnderLocation(Cursor))
		{
			continue;
		}
		const FVector2D Local = Geometry.AbsoluteToLocal(Cursor);
		const FVector2D LocalSize = Geometry.GetLocalSize();
		const FIntPoint ViewportSize = Client->Viewport->GetSizeXY();
		if (LocalSize.X <= 0.0 || LocalSize.Y <= 0.0 || ViewportSize.X <= 0 || ViewportSize.Y <= 0)
		{
			continue;
		}
		OutClient = Client;
		OutPixel = FIntPoint(
			FMath::Clamp(FMath::FloorToInt(Local.X / LocalSize.X * ViewportSize.X), 0, ViewportSize.X - 1),
			FMath::Clamp(FMath::FloorToInt(Local.Y / LocalSize.Y * ViewportSize.Y), 0, ViewportSize.Y - 1));
		return true;
	}
	return false;
}

void UMoonToonDebugSubsystem::SetLocked(bool bLock)
{
	bLocked = bLock;
	if (bLocked)
	{
		bFollowMouse = false;
	}
}

void UMoonToonDebugSubsystem::PickAt(FLevelEditorViewportClient* Client, FIntPoint ViewportPixel)
{
	if (!Client || !Client->Viewport || !MoonToonDebug::IsAvailable())
	{
		return;
	}
	bHasPick = true;
	PickPixel = ViewportPixel;
	PickViewKey = Client->ViewState.GetReference();

	// The material under the pixel: the editor's hit proxy (the record does not say which material it is).
	// Only on a deliberate pick -- following the mouse would re-render the hit proxies constantly.
	if (!bFollowMouse && !(bLocked && bInspectInFlight))
	{
		PickMaterial.Reset();
		PickComponent.Reset();
		PickSlot = -1;
		if (HHitProxy* Proxy = Client->Viewport->GetHitProxy(ViewportPixel.X, ViewportPixel.Y))
		{
			if (Proxy->IsA(HActor::StaticGetType()))
			{
				const HActor* ActorProxy = static_cast<const HActor*>(Proxy);
				if (const UPrimitiveComponent* Component = ActorProxy->PrimComponent)
				{
					PickComponent = const_cast<UPrimitiveComponent*>(Component);
					PickSlot = ActorProxy->MaterialIndex;
					if (ActorProxy->MaterialIndex >= 0)
					{
						PickMaterial = Component->GetMaterial(ActorProxy->MaterialIndex);
					}
				}
			}
		}
	}

	MoonToonDebug::FInspectRequest Request;
	Request.ViewKey = PickViewKey;
	Request.Position = FVector2f((float)ViewportPixel.X + 0.5f, (float)ViewportPixel.Y + 0.5f);
	Request.Space = MoonToonDebug::ECoordSpace::Viewport;
	Request.bTrace = true;
	Request.bStages = true;
	bInspectInFlight = true;
	const FIntPoint ViewportSize = Client->Viewport->GetSizeXY();
	TWeakObjectPtr<UMoonToonDebugSubsystem> WeakThis(this);
	TWeakObjectPtr<UObject> Material = PickMaterial;
	TWeakObjectPtr<UObject> Component = PickComponent;
	const int32 Slot = PickSlot;
	MoonToonDebug::RequestInspect(Request, [WeakThis, ViewportPixel, ViewportSize, Material, Slot, Component](const MoonToonDebug::FInspectResult& Result)
	{
		if (UMoonToonDebugSubsystem* This = WeakThis.Get())
		{
			This->bInspectInFlight = false;
			This->OnInspectResult(Result, FVector2D(ViewportPixel), TEXT("viewport"), ViewportPixel, ViewportSize, Material, Slot, Component);
		}
	});
}

void UMoonToonDebugSubsystem::OnInspectResult(const MoonToonDebug::FInspectResult& Result, FVector2D RequestPosition, const FString& Space, FIntPoint ViewportPixel,
	FIntPoint ViewportSize, TWeakObjectPtr<UObject> HitMaterial, int32 HitSlot, TWeakObjectPtr<UObject> HitComponent)
{
	FMoonToonPixelReport NewReport = BuildReport(Result, RequestPosition, Space, ViewportPixel, ViewportSize, HitMaterial.Get(), HitSlot, HitComponent.Get());

	// What changed since the last report of the same pixel (the locked-pixel highlight).
	ChangedFields.Reset();
	if (Report.bValid && NewReport.bValid && Report.ViewportPixel == NewReport.ViewportPixel)
	{
		TMap<FString, FString> Previous;
		for (const FMoonToonDebugField& Field : Report.Fields)
		{
			Previous.Add(Field.Section + TEXT(".") + Field.Name, Field.Value);
		}
		for (const FMoonToonDebugField& Field : NewReport.Fields)
		{
			const FString Key = Field.Section + TEXT(".") + Field.Name;
			const FString* Old = Previous.Find(Key);
			if (!Old || *Old != Field.Value)
			{
				ChangedFields.Add(Key);
			}
		}
	}
	Report = MoveTemp(NewReport);
	ReportTime = FPlatformTime::Seconds();
	OnReportChanged.Broadcast();
}

void UMoonToonDebugSubsystem::DrawMarker(UCanvas* Canvas, APlayerController* PC)
{
	if (!Canvas || !bHasPick || !(bLocked || bFollowMouse || (FPlatformTime::Seconds() - ReportTime) < 3.0))
	{
		return;
	}
	if (!Canvas->SceneView || Canvas->SceneView->State != PickViewKey)
	{
		return;
	}
	const FVector2D P(PickPixel.X + 0.5, PickPixel.Y + 0.5);
	const FLinearColor Color = bLocked ? FLinearColor(1.0f, 0.85f, 0.1f) : FLinearColor(0.2f, 1.0f, 0.6f);
	Canvas->K2_DrawLine(P - FVector2D(9.0, 0.0), P - FVector2D(3.0, 0.0), 1.5f, Color);
	Canvas->K2_DrawLine(P + FVector2D(3.0, 0.0), P + FVector2D(9.0, 0.0), 1.5f, Color);
	Canvas->K2_DrawLine(P - FVector2D(0.0, 9.0), P - FVector2D(0.0, 3.0), 1.5f, Color);
	Canvas->K2_DrawLine(P + FVector2D(0.0, 3.0), P + FVector2D(0.0, 9.0), 1.5f, Color);
}

// -------------------------------------------------------------------------------------------------
// Synchronous calls

bool UMoonToonDebugSubsystem::DrawAndWait(TFunctionRef<bool()> Done, int32 MinFrames, float TimeoutSeconds) const
{
	FLevelEditorViewportClient* Client = GetTargetViewportClient();
	if (!Client || !Client->Viewport)
	{
		return false;
	}
	const double Start = FPlatformTime::Seconds();
	int32 Frames = 0;
	for (;;)
	{
		const FIntPoint Size = Client->Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			return false;	// minimised: nothing renders
		}
		Client->Invalidate(false, false);
		Client->Viewport->Draw(false);
		++Frames;
		MoonToonDebug::FlushAndResolve();
		if (Frames >= MinFrames && Done())
		{
			return true;
		}
		if (FPlatformTime::Seconds() - Start > TimeoutSeconds)
		{
			return Done();
		}
	}
}

static MoonToonDebug::ECoordSpace ParseSpace(const FString& Space)
{
	if (Space.Equals(TEXT("render"), ESearchCase::IgnoreCase))
	{
		return MoonToonDebug::ECoordSpace::Render;
	}
	if (Space.Equals(TEXT("uv"), ESearchCase::IgnoreCase))
	{
		return MoonToonDebug::ECoordSpace::UV;
	}
	return MoonToonDebug::ECoordSpace::Viewport;
}

FMoonToonPixelReport UMoonToonDebugSubsystem::InspectSync(FVector2D Position, const FString& Space, bool bTrace, float TimeoutSeconds)
{
	FMoonToonPixelReport Out;
	FString WhyNot;
	FLevelEditorViewportClient* Client = GetTargetViewportClient();
	if (!IsAvailable(&WhyNot) || !Client)
	{
		Out.Error = !WhyNot.IsEmpty() ? WhyNot : FString(TEXT("no level viewport"));
		Out.Json = ToJson(Out);
		return Out;
	}
	const FIntPoint ViewportSize = Client->Viewport->GetSizeXY();
	const MoonToonDebug::ECoordSpace CoordSpace = ParseSpace(Space);
	FIntPoint ViewportPixel(FMath::FloorToInt(Position.X), FMath::FloorToInt(Position.Y));
	if (CoordSpace == MoonToonDebug::ECoordSpace::UV)
	{
		ViewportPixel = FIntPoint(FMath::FloorToInt(Position.X * ViewportSize.X), FMath::FloorToInt(Position.Y * ViewportSize.Y));
	}

	// The material under the pixel, when the position maps onto the viewport directly.
	UObject* HitMaterial = nullptr;
	UObject* HitComponent = nullptr;
	int32 HitSlot = -1;
	if (CoordSpace != MoonToonDebug::ECoordSpace::Render && ViewportPixel.X >= 0 && ViewportPixel.Y >= 0
		&& ViewportPixel.X < ViewportSize.X && ViewportPixel.Y < ViewportSize.Y)
	{
		if (HHitProxy* Proxy = Client->Viewport->GetHitProxy(ViewportPixel.X, ViewportPixel.Y))
		{
			if (Proxy->IsA(HActor::StaticGetType()))
			{
				const HActor* ActorProxy = static_cast<const HActor*>(Proxy);
				if (const UPrimitiveComponent* Component = ActorProxy->PrimComponent)
				{
					HitComponent = const_cast<UPrimitiveComponent*>(Component);
					HitSlot = ActorProxy->MaterialIndex;
					HitMaterial = HitSlot >= 0 ? Component->GetMaterial(HitSlot) : nullptr;
				}
			}
		}
	}

	// A shared box, not stack references: a result that arrives after a timeout must not write into a
	// finished call.
	struct FInspectBox { bool bDone = false; MoonToonDebug::FInspectResult Result; };
	const TSharedRef<FInspectBox> Box = MakeShared<FInspectBox>();
	MoonToonDebug::FInspectRequest Request;
	Request.ViewKey = Client->ViewState.GetReference();
	Request.Position = FVector2f((float)Position.X, (float)Position.Y);
	if (CoordSpace == MoonToonDebug::ECoordSpace::Viewport)
	{
		Request.Position += FVector2f(0.5f, 0.5f);	// the pixel centre
	}
	Request.Space = CoordSpace;
	Request.bTrace = bTrace;
	MoonToonDebug::RequestInspect(Request, [Box](const MoonToonDebug::FInspectResult& R) { Box->Result = R; Box->bDone = true; });
	if (!DrawAndWait([Box]() { return Box->bDone; }, 1, TimeoutSeconds))
	{
		MoonToonDebug::CancelPendingRequests();
		MoonToonDebug::FlushAndResolve();
		Out.Error = TEXT("timed out: the viewport did not render (minimised? another modal window?)");
		Out.Json = ToJson(Out);
		return Out;
	}
	const MoonToonDebug::FInspectResult& Result = Box->Result;
	Out = BuildReport(Result, Position, Space.IsEmpty() ? FString(TEXT("viewport")) : Space.ToLower(), ViewportPixel, ViewportSize, HitMaterial, HitSlot, HitComponent);
	return Out;
}

FMoonToonRegionStats UMoonToonDebugSubsystem::RegionStatsSync(const FString& View, int32 Channel, int32 Bit, bool bPerBit, FIntPoint RectMin, FIntPoint RectMax, float TimeoutSeconds)
{
	FMoonToonRegionStats Out;
	FString WhyNot;
	FLevelEditorViewportClient* Client = GetTargetViewportClient();
	if (!IsAvailable(&WhyNot) || !Client)
	{
		Out.Error = !WhyNot.IsEmpty() ? WhyNot : FString(TEXT("no level viewport"));
		Out.Json = ToJson(Out);
		return Out;
	}
	const MoonToonDebug::FDebugViewInfo* ViewInfo = MoonToonDebug::FindView(View);
	if (!ViewInfo)
	{
		Out.Error = FString::Printf(TEXT("unknown view '%s' (list_debug_views)"), *View);
		Out.Json = ToJson(Out);
		return Out;
	}
	MoonToonDebug::FRegionStatsRequest Request;
	Request.ViewKey = Client->ViewState.GetReference();
	Request.Selection = GetViewSettings();
	Request.Selection.View = ViewInfo->Name;
	Request.Selection.Channel = Channel;
	Request.Selection.Bit = Bit;
	Request.Selection.IsolateKey = -1;
	Request.bPerBit = bPerBit;
	if (RectMax.X > RectMin.X && RectMax.Y > RectMin.Y)
	{
		Request.Rect = FIntRect(RectMin, RectMax);
	}
	struct FStatsBox { bool bDone = false; MoonToonDebug::FRegionStatsResult Result; };
	const TSharedRef<FStatsBox> Box = MakeShared<FStatsBox>();
	MoonToonDebug::RequestRegionStats(Request, [Box](const MoonToonDebug::FRegionStatsResult& R) { Box->Result = R; Box->bDone = true; });
	if (!DrawAndWait([Box]() { return Box->bDone; }, 1, TimeoutSeconds))
	{
		MoonToonDebug::CancelPendingRequests();
		MoonToonDebug::FlushAndResolve();
		Out.Error = TEXT("timed out: the viewport did not render");
		Out.Json = ToJson(Out);
		return Out;
	}
	RefreshNameCaches(true);
	Out = BuildStats(Box->Result, ViewInfo->Name);
	return Out;
}

// -------------------------------------------------------------------------------------------------
// Names

void UMoonToonDebugSubsystem::RefreshNameCaches(bool bForce) const
{
	const double Now = FPlatformTime::Seconds();
	if (!bForce && Now - NameCacheTime < 3.0)
	{
		return;
	}
	NameCacheTime = Now;

	// Actors: every MoonToonActorComponent in the target world.
	ActorsById.Reset();
	if (UWorld* World = GetTargetWorld())
	{
		for (TObjectIterator<UMoonToonActorComponent> It; It; ++It)
		{
			if (It->GetWorld() == World && It->GetOwner() && It->GetToonActorId() > 0)
			{
				ActorsById.Add(It->GetToonActorId(), It->GetOwner());
			}
		}
	}

	// Profiles: the registry is render-thread; it answers id -> asset for the pointers handed in.
	TArray<const UMoonToonProfile*> Profiles;
	TArray<FString> ProfilePaths;
	for (TObjectIterator<UMoonToonProfile> It; It; ++It)
	{
		if (!It->IsTemplate())
		{
			Profiles.Add(*It);
			ProfilePaths.Add(It->GetPathName());
		}
	}
	TArray<int32> ProfileIds;
	ProfileIds.SetNumZeroed(Profiles.Num());
	MoonToonDebugSnapshots::FRampAtlas RampAtlas;
	MoonToonDebugSnapshots::FConstantTable ConstantTable;
	ENQUEUE_RENDER_COMMAND(MoonToonDebugNames)([&Profiles, &ProfileIds, &RampAtlas, &ConstantTable](FRHICommandListImmediate&)
	{
		for (int32 It = 0; It < Profiles.Num(); ++It)
		{
			ProfileIds[It] = (int32)MoonToonProfile::GetMoonToonProfileId(Profiles[It]);
		}
		RampAtlas = MoonToonDebugSnapshots::GetRampAtlas();
		ConstantTable = MoonToonDebugSnapshots::GetConstantTable();
	});
	FlushRenderingCommands();

	ProfileNames.Reset();
	for (int32 It = 0; It < Profiles.Num(); ++It)
	{
		if (ProfileIds[It] > 0)
		{
			ProfileNames.Add(ProfileIds[It], ProfilePaths[It]);
		}
	}
	RampSliceNames.Reset();
	SpecularSliceNames.Reset();
	for (const MoonToonDebugSnapshots::FRampSlice& Slice : RampAtlas.Slices)
	{
		(Slice.bSpecular ? SpecularSliceNames : RampSliceNames).Add((int32)Slice.Slice,
			Slice.SourceRow >= 0 ? FString::Printf(TEXT("%s row %d"), *Slice.Source, Slice.SourceRow) : Slice.Source);
	}
	ConstantRowNames.Reset();
	for (const MoonToonDebugSnapshots::FConstantRow& Row : ConstantTable.Rows)
	{
		TArray<FString> Names;
		for (int32 It = 0; It < Row.Materials.Num() && It < 4; ++It)
		{
			Names.Add(FPaths::GetBaseFilename(Row.Materials[It]));
		}
		FString Text = FString::Join(Names, TEXT(", "));
		if (Row.Materials.Num() > Names.Num())
		{
			Text += FString::Printf(TEXT(" (+%d more, dump_constant_table)"), Row.Materials.Num() - Names.Num());
		}
		ConstantRowNames.Add((int32)Row.Row, Text);
	}
}
