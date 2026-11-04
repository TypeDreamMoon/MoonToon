// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "MoonToonDebug.h"
#include "MoonToonDebugTypes.h"
#include "Containers/Ticker.h"
#include "MoonToonDebugSubsystem.generated.h"

class AActor;
class FLevelEditorViewportClient;
class FMoonToonDebugInputProcessor;
class UWorld;

/**
 * The MoonToon debugger's one backend. The "MoonToon 调试器" panel, UMoonToonDebugLibrary (UnrealBridge) and
 * the MCP server all call it, so they read the same data.
 *
 * It wraps the renderer's debug API (Renderer/Public/MoonToonDebug.h): the debug view selection (the
 * r.MoonToon.Debug.* console variables -- the one view state), pixel inspections, region statistics,
 * captures and the actor table, and adds what only the editor knows: names for ids (actors, profiles,
 * ramp slices, constant-table rows), the material under a pixel (hit proxies), the post process volumes'
 * style presets, the T44 validation rules, files.
 *
 * Asynchronous calls answer through OnReportChanged etc. (the panel); the *Sync calls draw the target
 * viewport and wait for the GPU (the function library / MCP). Target viewport: the level viewport the user
 * last worked in.
 */
UCLASS()
class MOONTOONEDITOR_API UMoonToonDebugSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	static UMoonToonDebugSubsystem* Get();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Compiled, Substrate, the debug shaders exist, and the engine speaks the API version this plugin was built for. */
	bool IsAvailable(FString* OutWhyNot = nullptr) const;

	// ---------------------------------------------------------------------------------------------
	// Views

	TArray<FMoonToonDebugViewInfo> GetViews() const;
	MoonToonDebug::FViewSettings GetViewSettings() const;
	void SetViewSettings(const MoonToonDebug::FViewSettings& Settings);
	/** Selects a view and keeps the other settings (channel, bit, isolate reset). Empty = off. */
	void SetView(const FString& Name);
	FString GetCurrentViewName() const;
	/** The legend of the view on screen: the values with names and counts (refreshed by the renderer twice a second). */
	FMoonToonRegionStats GetLegend() const;

	// ---------------------------------------------------------------------------------------------
	// Picking (the panel)

	/** The panel says whether it is open; the pick shortcut only works while it is. */
	void SetPanelOpen(bool bOpen) { bPanelOpen = bOpen; }
	bool IsPanelOpen() const { return bPanelOpen; }
	void SetPickArmed(bool bArmed) { bPickArmed = bArmed; }
	bool IsPickArmed() const { return bPickArmed; }
	void SetFollowMouse(bool bFollow) { bFollowMouse = bFollow; }
	bool IsFollowMouse() const { return bFollowMouse; }
	/** Lock re-inspects the last picked pixel every tick (so a parameter tweak shows up at once). */
	void SetLocked(bool bLock);
	bool IsLocked() const { return bLocked; }
	bool HasPick() const { return bHasPick; }
	FIntPoint GetPickPixel() const { return PickPixel; }
	const FSceneViewStateInterface* GetPickViewKey() const { return PickViewKey; }

	/** Inspects a viewport pixel of a level viewport (asynchronous; OnReportChanged). */
	void PickAt(FLevelEditorViewportClient* Client, FIntPoint ViewportPixel);
	/** The level viewport under the mouse cursor and the pixel, if any. */
	bool FindViewportUnderCursor(FLevelEditorViewportClient*& OutClient, FIntPoint& OutPixel) const;

	const FMoonToonPixelReport& GetReport() const { return Report; }
	const FMoonToonPixelReport* GetPinnedReport() const { return bHasPinned ? &Pinned : nullptr; }
	void PinReport() { Pinned = Report; bHasPinned = Report.bValid; }
	void ClearPin() { bHasPinned = false; }
	/** Field keys (section.name) whose value differs from the previous report -- the panel highlights them. */
	const TSet<FString>& GetChangedFields() const { return ChangedFields; }
	double GetReportTime() const { return ReportTime; }

	DECLARE_MULTICAST_DELEGATE(FOnDebugChanged);
	FOnDebugChanged OnReportChanged;
	FOnDebugChanged OnViewChanged;

	// ---------------------------------------------------------------------------------------------
	// Synchronous calls (function library / MCP). Each draws the target viewport and waits for the GPU.

	/** Space: "viewport" (default), "render", "uv". */
	FMoonToonPixelReport InspectSync(FVector2D Position, const FString& Space, bool bTrace = true, float TimeoutSeconds = 5.0f);
	/** Statistics of any view (it need not be on screen). Rect in render pixels, 0-based; Max <= Min = the whole view. */
	FMoonToonRegionStats RegionStatsSync(const FString& View, int32 Channel, int32 Bit, bool bPerBit, FIntPoint RectMin, FIntPoint RectMax, float TimeoutSeconds = 5.0f);
	/** View: a debug view, or "scene.hdr" / "scene.final". Frames: frames drawn first (TSR history, streaming). */
	FMoonToonCaptureInfo CaptureSync(const FString& View, int32 Channel, int32 Frames, const FString& OutputDirectory, float TimeoutSeconds = 10.0f);
	/** Compares two captures' raw EXRs; with a noise pair (two captures of the same settings) it reports the stable change. */
	FMoonToonDiffResult DiffCaptures(const FString& A, const FString& B, const FString& NoiseA, const FString& NoiseB, float Threshold, const FString& MaskPath);

	// ---------------------------------------------------------------------------------------------
	// Characters, tables, style, validation

	/** Every actor with a toon material or a MoonToonActorComponent in the target world, with the GPU's actor-table rows and on-screen pixel counts. */
	TArray<FMoonToonDebugActorInfo> ListToonActors(bool bWithGpuData = true, float TimeoutSeconds = 5.0f);
	TArray<FMoonToonTableRow> DumpConstantTable();
	TArray<FMoonToonTableRow> DumpRampAtlas();
	TArray<FMoonToonTableRow> DumpProfiles();
	/** The post process volumes' style presets around the target viewport's camera, priority order. */
	TArray<FMoonToonStyleLayer> GetStyleLayers() const;
	/** The T44 rules (level, actors, material instances, ramp atlases) plus the GPU self-check counts. */
	TArray<FMoonToonValidationIssue> Validate(bool bWithGpuChecks = true);

	/** The world the target viewport shows. */
	UWorld* GetTargetWorld() const;
	FLevelEditorViewportClient* GetTargetViewportClient() const;

	/** Names for the values of a view: actor ids, profile ids, ramp slices, constant rows, enums, bits. */
	FString NameForValue(const FString& ViewName, uint32 Value, bool bPerBit) const;

	static FString ToJson(const FMoonToonPixelReport& InReport);
	static FString ToJson(const FMoonToonRegionStats& Stats);
	static FString ToJson(const TArray<FMoonToonDebugActorInfo>& Actors);
	static FString ToJson(const TArray<FMoonToonTableRow>& Rows);
	static FString ToJson(const TArray<FMoonToonValidationIssue>& Issues);
	static FString ToJson(const TArray<FMoonToonStyleLayer>& Layers);
	static FString ToJson(const FMoonToonCaptureInfo& Capture);

private:
	bool Tick(float DeltaTime);
	void OnInspectResult(const MoonToonDebug::FInspectResult& Result, FVector2D RequestPosition, const FString& Space, FIntPoint ViewportPixel,
		FIntPoint ViewportSize, TWeakObjectPtr<UObject> HitMaterial, int32 HitSlot, TWeakObjectPtr<UObject> HitComponent);
	FMoonToonPixelReport BuildReport(const MoonToonDebug::FInspectResult& Result, FVector2D RequestPosition, const FString& Space, FIntPoint ViewportPixel,
		FIntPoint ViewportSize, UObject* HitMaterial, int32 HitSlot, UObject* HitComponent) const;
	FMoonToonRegionStats BuildStats(const MoonToonDebug::FRegionStatsResult& Result, const FString& ViewName) const;
	void RefreshNameCaches(bool bForce) const;
	void UpdateLegendNames();
	/** Draws the target viewport Frames times and resolves the debug work, until Done or the timeout. */
	bool DrawAndWait(TFunctionRef<bool()> Done, int32 MinFrames, float TimeoutSeconds) const;
	void DrawMarker(class UCanvas* Canvas, class APlayerController* PC);

	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle DrawHandle;
	TSharedPtr<FMoonToonDebugInputProcessor> InputProcessor;

	bool bPanelOpen = false;
	bool bPickArmed = false;
	bool bFollowMouse = false;
	bool bLocked = false;
	bool bHasPick = false;
	bool bInspectInFlight = false;
	FIntPoint PickPixel = FIntPoint::ZeroValue;
	FIntPoint LastFollowPixel = FIntPoint(-1, -1);
	const FSceneViewStateInterface* PickViewKey = nullptr;
	TWeakObjectPtr<UObject> PickMaterial;
	int32 PickSlot = -1;
	TWeakObjectPtr<UObject> PickComponent;

	FMoonToonPixelReport Report;
	FMoonToonPixelReport Pinned;
	bool bHasPinned = false;
	TSet<FString> ChangedFields;
	double ReportTime = 0.0;

	FString LastViewName;
	FString LastLegendView;
	double LastNamesRefresh = -1000.0;

	// Name caches (game thread), refreshed every few seconds or on demand.
	mutable TMap<int32, FString> ProfileNames;
	mutable TMap<int32, TWeakObjectPtr<AActor>> ActorsById;
	mutable TMap<int32, FString> RampSliceNames;		// diffuse slices
	mutable TMap<int32, FString> SpecularSliceNames;
	mutable TMap<int32, FString> ConstantRowNames;
	mutable double NameCacheTime = -1000.0;
};
