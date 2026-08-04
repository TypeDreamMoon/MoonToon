// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Curves/CurveFloat.h"
#include "MoonToonTool.h"
#include "MoonToonOutlineAlphaTool.generated.h"

/** How the per-vertex 0..1 input to the width curve is derived. */
UENUM()
enum class EMoonToonAlphaSignal : uint8
{
	/**
	 * Per-island principal axis. Splits the selection into connected pieces (one hair card, one
	 * strand), fits the dominant axis of each, and measures how far along it each vertex sits.
	 * Independent of UV layout, which is why it is the default -- it works on atlas-packed meshes
	 * where the UV signal is meaningless.
	 */
	IslandAxis UMETA(DisplayName = "Island Axis (per-island PCA)"),

	/**
	 * A UV coordinate, straight through. Cheap and completely predictable, and the right choice when
	 * hair is already unwrapped root-to-tip along V -- which is common on meshes authored for
	 * gradient hair textures.
	 */
	UVAxis UMETA(DisplayName = "UV Axis"),

	/**
	 * Local curvature. Tips are high-curvature, so this thins them without any notion of direction,
	 * but it also fires on every sharp crease, so it reads as a detail mask more than a taper.
	 */
	Curvature UMETA(DisplayName = "Curvature"),
};

/** How the curve result combines with the alpha already on the mesh. */
UENUM()
enum class EMoonToonAlphaBlend : uint8
{
	/** Overwrite. Hand-painted alpha in the affected sections is lost. */
	Replace,

	/** Multiply into the existing alpha. Procedural taper and a hand-painted mask coexist. */
	Multiply,

	/** Keep whichever is thinner. Good for layering several passes that each only ever remove width. */
	Min,

	/** Keep whichever is thicker. */
	Max,

	/** Blend from existing to curve result by Blend Strength. */
	Lerp,
};

/** Which end of an island's axis counts as the tip. */
UENUM()
enum class EMoonToonIslandOrientation : uint8
{
	/**
	 * Measure how thick the island is at both ends and treat the thinner end as the tip. Correct for
	 * hair, which always tapers, and saves flipping the curve per mesh.
	 */
	Auto UMETA(DisplayName = "Auto (thinner end is the tip)"),

	PositiveEnd UMETA(DisplayName = "Positive axis end is the tip"),
	NegativeEnd UMETA(DisplayName = "Negative axis end is the tip"),
};

/**
 * Drives vertex-colour alpha from a curve, so outline width can be authored as a taper instead of
 * painted by hand.
 *
 * The material reads alpha as an outline width multiplier ("Use Vertex Color A as Outline Width"), so
 * this is the procedural half of that workflow: lay down a clean root-to-tip falloff here, then
 * refine locally with Mesh Paint if needed. Blend modes exist so a re-run does not have to discard
 * that hand work -- Multiply in particular keeps a painted mask intact underneath.
 *
 * Only the alpha channel is written. RGB is left exactly as it was, because the MoonToon base
 * material already reads it for feature ID, rim width and opacity.
 */
UCLASS()
class MOONTOONEDITOR_API UMoonToonOutlineAlphaTool : public UMoonToonTool
{
	GENERATED_BODY()

public:
	UMoonToonOutlineAlphaTool();

	virtual FText GetToolName() const override;
	virtual FText GetToolDescription() const override;
	virtual FText GetRunLabel() const override;
	virtual FString Run(const FMoonToonToolContext& Context) override;

	// --- Signal ---------------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, Category = "Signal")
	EMoonToonAlphaSignal Signal = EMoonToonAlphaSignal::IslandAxis;

	/** Flips the signal before the curve, so the curve itself always reads root-on-the-left. */
	UPROPERTY(EditAnywhere, Category = "Signal")
	bool bInvertSignal = false;

	UPROPERTY(EditAnywhere, Category = "Signal",
		meta = (EditCondition = "Signal == EMoonToonAlphaSignal::IslandAxis"))
	EMoonToonIslandOrientation IslandOrientation = EMoonToonIslandOrientation::Auto;

	/**
	 * Islands smaller than this are left alone. A handful of vertices has no meaningful principal
	 * axis, and forcing one produces a random taper on stray geometry.
	 */
	UPROPERTY(EditAnywhere, Category = "Signal",
		meta = (EditCondition = "Signal == EMoonToonAlphaSignal::IslandAxis", ClampMin = "3"))
	int32 MinIslandVertices = 8;

	UPROPERTY(EditAnywhere, Category = "Signal",
		meta = (EditCondition = "Signal == EMoonToonAlphaSignal::UVAxis", ClampMin = "0", ClampMax = "3"))
	int32 UVChannel = 0;

	/** Read U instead of V. Hair unwrapped root-to-tip usually runs along V, hence the default. */
	UPROPERTY(EditAnywhere, Category = "Signal",
		meta = (EditCondition = "Signal == EMoonToonAlphaSignal::UVAxis"))
	bool bUseUAxis = false;

	/**
	 * Rescale the UV signal into 0..1 using the actual range found, instead of trusting it to already
	 * be 0..1. Turn off when the UVs are deliberately tiled.
	 */
	UPROPERTY(EditAnywhere, Category = "Signal",
		meta = (EditCondition = "Signal == EMoonToonAlphaSignal::UVAxis"))
	bool bNormalizeUVRange = true;

	// --- Curve ----------------------------------------------------------------------------------

	/**
	 * Width multiplier over the signal. X is 0 at the root and 1 at the tip; Y is what lands in
	 * vertex alpha. The default tapers from full width down to a thin tip.
	 */
	UPROPERTY(EditAnywhere, Category = "Curve")
	FRuntimeFloatCurve WidthCurve;

	/** Added to the signal before the curve is evaluated. Slides the taper along the strand. */
	UPROPERTY(EditAnywhere, Category = "Curve")
	float InputOffset = 0.0f;

	/** Multiplies the signal before the curve is evaluated. Sharpens or stretches the taper. */
	UPROPERTY(EditAnywhere, Category = "Curve")
	float InputScale = 1.0f;

	/**
	 * Lower clamp on the value actually written, applied after blending as well as after the curve.
	 *
	 * Alpha 0 does not remove the outline cleanly -- it collapses the hull exactly onto the surface,
	 * which z-fights. Clamping post-blend matters because Multiply and Min read the existing alpha,
	 * so a mesh that already has zeros there would write zeros straight back out. Set this to 0 to
	 * genuinely allow holes.
	 */
	UPROPERTY(EditAnywhere, Category = "Curve", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OutputFloor = 0.05f;

	UPROPERTY(EditAnywhere, Category = "Curve", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OutputCeiling = 1.0f;

	// --- Write ----------------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, Category = "Write")
	EMoonToonAlphaBlend BlendMode = EMoonToonAlphaBlend::Replace;

	UPROPERTY(EditAnywhere, Category = "Write",
		meta = (EditCondition = "BlendMode == EMoonToonAlphaBlend::Lerp", ClampMin = "0.0", ClampMax = "1.0"))
	float BlendStrength = 1.0f;

	/** Compute and report the result without writing anything. Use it to check a curve first. */
	UPROPERTY(EditAnywhere, Category = "Write")
	bool bPreviewOnly = false;

	/**
	 * Write through a full PostEditChange rebuild instead of the fast path.
	 *
	 * The fast path saves the import data and patches the live render buffers directly (the Mesh
	 * Paint technique), skipping the rebuild of chunking, skin weights and morph targets that a
	 * colour-only change never needed -- seconds down to fractions. The next natural rebuild
	 * regenerates everything from the import data, which holds the same values.
	 *
	 * Turn this on only if the viewport ever disagrees with what a reimport produces.
	 */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "Write")
	bool bFullRebuildOnWrite = false;

	UPROPERTY(EditAnywhere, Category = "Write")
	bool bAllLODs = true;

	UPROPERTY(EditAnywhere, Category = "Write", meta = (EditCondition = "!bAllLODs", ClampMin = "0"))
	int32 LODIndex = 0;
};
