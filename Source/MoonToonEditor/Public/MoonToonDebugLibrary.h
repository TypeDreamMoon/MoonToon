// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MoonToonDebugTypes.h"
#include "MoonToonDebugLibrary.generated.h"

/**
 * The MoonToon debugger for scripts: UnrealBridge python calls these as unreal.MoonToonDebugLibrary.<snake_case>(),
 * and the MoonToon MCP server (Plugins/MoonToon/Tools/MoonToonDebugMCP) goes through them. Every *Json call
 * returns the same JSON the debugger panel shows ("copy JSON"). One backend: UMoonToonDebugSubsystem.
 *
 * Synchronous: each call draws the target level viewport (the one last worked in) and waits for the GPU.
 * A minimised editor renders nothing; the call then returns {"valid": false, "error": ...}.
 * Pixel spaces: "viewport" (the viewport's pixels, default), "render" (the internal view rect, 0-based), "uv" (0..1).
 */
UCLASS()
class MOONTOONEDITOR_API UMoonToonDebugLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Is the toolset usable, why not, the API version, the target viewport and world, the current view. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString EditorStatusJson();

	/** Every debug view: name, group, encoding, channels, bit / resolved support. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString ListDebugViewsJson();

	/**
	 * Selects the debug view drawn in the viewports (r.MoonToon.Debug.*). View "" or "off" turns it off.
	 * RangeMin == RangeMax keeps the view's default range. NonToonMode: 0 dim, 1 black, 2 as is. IsolateKey -1 = all.
	 * Returns the settings now in effect.
	 */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString SetDebugView(const FString& View, int32 Channel = 0, int32 Bit = -1, bool bResolved = true,
		float RangeMin = 0.0f, float RangeMax = 0.0f, int32 NonToonMode = 0, int32 IsolateKey = -1, bool bLegend = true);

	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString GetDebugViewJson();

	/** Everything about one pixel: the decoded record with sources, the buffers, the stage ledger, the main-light trace. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString GetPixelJson(float X, float Y, const FString& Space = TEXT("viewport"), bool bTrace = true);

	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FMoonToonPixelReport GetPixel(float X, float Y, const FString& Space = TEXT("viewport"), bool bTrace = true);

	/** Per value of a view: pixel count, bounds and a sample pixel (render and viewport space). Max <= Min = the whole view. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString GetRegionStatsJson(const FString& View, int32 Channel = 0, int32 Bit = -1, bool bPerBit = false,
		int32 MinX = 0, int32 MinY = 0, int32 MaxX = 0, int32 MaxY = 0);

	/** Writes <time>_<view>.exr (raw, linear), _id.png (16-bit key), _color.png (false colour) and .json (meta). View may be scene.hdr / scene.final. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString CaptureJson(const FString& View, int32 Channel = 0, int32 Frames = 1, const FString& OutputDirectory = TEXT(""));

	/** A vs B (raw EXRs); with NoiseA / NoiseB (two captures of the same settings) the change beyond the noise floor. Threshold in raw units (default 0.01). */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString DiffCapturesJson(const FString& A, const FString& B, const FString& NoiseA = TEXT(""), const FString& NoiseB = TEXT(""),
		float Threshold = 0.0f, const FString& MaskPath = TEXT(""));

	/** The captures in a directory (default: the capture directory), newest first. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString ListCapturesJson(const FString& Directory = TEXT(""));

	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString ListToonActorsJson(bool bWithGpuData = true);

	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString DumpConstantTableJson();

	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString DumpRampAtlasJson();

	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString DumpProfilesJson();

	/** The post process volumes' style presets around the camera, and (with a pixel) that pixel's resolved style fields. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString DumpStyleJson();

	/** The T44 rules plus the GPU self-check counts. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString ValidateJson(bool bWithGpuChecks = true);

	/** Opens the "MoonToon 调试器" panel. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static void OpenDebuggerPanel();

	/** Starts the debugger's MCP server (http://127.0.0.1:<Mcp Port>/mcp, bearer token in Saved/MoonToonDebug/mcp_token.txt). Returns its status. */
	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString StartMcpServer();

	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString StopMcpServer();

	UFUNCTION(BlueprintCallable, Category = "MoonToon|Debug")
	static FString McpStatusJson();
};
