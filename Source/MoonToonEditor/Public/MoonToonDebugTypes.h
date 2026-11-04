// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MoonToonDebugTypes.generated.h"

/*
 * The MoonToon debugger's data, as the panel, the UnrealBridge function library and the MCP tools see it.
 * All of it comes from UMoonToonDebugSubsystem, which turns the renderer's raw results
 * (Renderer/Public/MoonToonDebug.h) into these.
 */

/** One decoded value, with where it came from. */
USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonDebugField
{
	GENERATED_BODY()

	/** record / surface / feature / meta / validate / buffers / style / stages / main_light / tables */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug")
	FString Section;

	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug")
	FString Name;

	/** The value as text (names for enums and bits, "r g b" for colours). */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug")
	FString Value;

	/** The first component as a number. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug")
	double Number = 0.0;

	/** Where the value resolved from: material, profile:<asset>, preset:<asset> @ <volume>, default, constant row N, derived ... Empty when it is plain data. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug")
	FString Source;

	/** A colour for the panel's swatch (linear), alpha 0 = none. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug")
	FLinearColor Swatch = FLinearColor::Transparent;
};

/** Everything the inspector knows about one pixel. */
USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonPixelReport
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") bool bValid = false;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Error;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int64 FrameNumber = 0;
	/** The position as requested, and in which space (viewport / render / uv). */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FVector2D RequestPosition = FVector2D::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString RequestSpace;
	/** Viewport pixel (what a screenshot of the viewport shows). */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FIntPoint ViewportPixel = FIntPoint::ZeroValue;
	/** Render pixel, 0-based in the view rect (the internal resolution). */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FIntPoint RenderPixel = FIntPoint::ZeroValue;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FIntPoint RenderSize = FIntPoint::ZeroValue;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FIntPoint ViewportSize = FIntPoint::ZeroValue;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") float ScreenPercentage = 100.0f;

	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") bool bIsToon = false;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 ActorId = 0;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString ActorName;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString ActorPath;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 ProfileId = 0;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString ProfilePath;
	/** From the editor's hit proxy at the pixel (editor viewports only). */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString MaterialPath;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 MaterialSlot = -1;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString ComponentPath;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 ValidateBits = 0;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") TArray<FString> ValidateNames;

	/** The decoded record, the surface, the feature channels, the buffers, the style chain. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") TArray<FMoonToonDebugField> Fields;
	/** The measured stage ledger: scene colour after each stage, and what each stage added (un-exposed). */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") TArray<FMoonToonDebugField> Stages;
	/** The main light, re-evaluated (MOONTOON_TRACE_*). */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") TArray<FMoonToonDebugField> Trace;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString MainLightName;
	/** Modelled main light / measured direct-light stage, in luminance; 0 = not available. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") float MainLightAgreement = 0.0f;

	/** The whole report as JSON (what get_pixel returns). */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Json;
};

USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonDebugViewInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 Id = 0;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString DisplayName;
	/** record / validate / buffer / shade */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Group;
	/** id / enum / bits / unorm / signed / color / vector / float */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Encoding;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 ChannelCount = 0;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") TArray<FString> ChannelNames;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") bool bHasBit = false;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") bool bHasResolved = false;
};

USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonRegionValue
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int64 Key = 0;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") bool bPerBit = false;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 PixelCount = 0;
	/** Render pixels, 0-based in the view rect. Max is exclusive. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FIntPoint BoundsMin = FIntPoint::ZeroValue;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FIntPoint BoundsMax = FIntPoint::ZeroValue;
	/** The pixel nearest the bounds' centre, render space; and the same pixel in viewport space. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FIntPoint Sample = FIntPoint::ZeroValue;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FIntPoint SampleViewport = FIntPoint::ZeroValue;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FLinearColor Swatch = FLinearColor::Transparent;
};

USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonRegionStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") bool bValid = false;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Error;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString ViewName;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") TArray<FMoonToonRegionValue> Values;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 OverflowPixels = 0;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Json;
};

USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonDebugActorInfo
{
	GENERATED_BODY()

	/** 0 = no MoonToonActorComponent (the actor still has toon materials). */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 ActorId = 0;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString ActorLabel;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString ActorPath;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") bool bHasComponent = false;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 PixelCount = 0;
	/** "<material path> -> profile <id> <asset>" per toon material. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") TArray<FString> Materials;
	/** The 13 actor-table rows as the GPU holds them this frame, named. Empty without a component / table readback. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") TArray<FMoonToonDebugField> ActorRows;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") TArray<FString> Warnings;
};

USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonValidationIssue
{
	GENERATED_BODY()

	/** ERROR / WARN / INFO */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Severity;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Where;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Message;
	/** An object to select / open, when there is one. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString ObjectPath;
	/** A render pixel showing the problem (GPU self-check rows), -1 when none. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FIntPoint SamplePixel = FIntPoint(-1, -1);
};

USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonTableRow
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Key;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") TArray<FString> Columns;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") TArray<FString> ObjectPaths;
};

USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonCaptureInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") bool bValid = false;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Error;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString ViewName;
	/** Linear EXR (32-bit float) of the raw values. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString RawPath;
	/** 16-bit PNG of the key (ids exact), debug views only. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString IdPath;
	/** 8-bit PNG of what the viewport shows, debug views only. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString FalseColorPath;
	/** JSON: coordinates, view, cvars, camera, frame. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString MetaPath;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 Width = 0;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 Height = 0;
};

USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonDiffResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") bool bValid = false;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Error;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 Width = 0;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 Height = 0;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") float Threshold = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") float MaxDiff = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") float MeanDiff = 0.0f;
	/** Pixels over the threshold between A and B. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 ChangedPixels = 0;
	/** Pixels over the threshold between the two noise captures (same settings); -1 without a noise pair. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 NoisePixels = -1;
	/** Changed in A/B and not in the noise pair. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") int32 StablePixels = 0;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FIntPoint BoundsMin = FIntPoint::ZeroValue;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FIntPoint BoundsMax = FIntPoint::ZeroValue;
	/** SAME or DIFF (the regression harness rule: stable pixels <= 1.5 x noise + 50). */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Verdict;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString MaskPath;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Json;
};

/** A post process volume's style presets as seen from the camera (the "preset" layer of the source chain). */
USTRUCT(BlueprintType)
struct MOONTOONEDITOR_API FMoonToonStyleLayer
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString Volume;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString VolumePath;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") float Priority = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") float Weight = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") FString PresetPath;
	/** The ticked (bOverride_) fields of the preset and its parents, by property name. */
	UPROPERTY(BlueprintReadOnly, Category = "MoonToon Debug") TArray<FString> OverriddenFields;
};
