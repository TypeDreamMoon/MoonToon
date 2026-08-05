// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MoonToonTool.h"
#include "MoonToonStrandTangentTool.generated.h"

/** What pressing Run does. Preview is the default: this tool rewrites mesh channels, so seeing the
 *  directions first and keeping a way back are part of its contract, not extras. */
UENUM()
enum class EMoonToonStrandTangentMode : uint8
{
	/** Compute islands and directions, draw them as coloured lines in the viewport, write nothing. */
	Preview,

	/**
	 * Ellipsoid source only: spawn a transient gizmo actor whose transform IS the ellipsoid, swap
	 * the selected sections onto a live band-preview material, and let the ordinary viewport gizmo
	 * drive the highlight in real time. Bake and exit from the actor's details panel. Nothing is
	 * written until its Bake button runs, and exiting (or deleting the actor) restores the
	 * original materials.
	 */
	LivePreview,

	/** Write UV1.xy / UV2.x. The first bake snapshots the original channel values to
	 *  Saved/MoonToon/ChannelBackups/ and later bakes keep that first snapshot, so Restore always
	 *  returns to the pre-bake original no matter how many times the bake was re-run. */
	Bake,

	/** Write the backed-up original channel values back. Whole-channel: the section filter does not
	 *  apply, because the backup captured the channels before the first bake touched them. */
	Restore,
};

/** Where the strand direction comes from. */
UENUM()
enum class EMoonToonStrandTangentSource : uint8
{
	/**
	 * MooaToon's stylized method, in-engine: tangents are sampled from a virtual ellipsoid's
	 * pole-to-pole (meridian) field and transferred to the hair as-is. The highlight becomes ONE
	 * smooth ring shaped by the ellipsoid rather than by the messy card topology -- move, scale or
	 * tilt the ellipsoid and the ring follows. This replaces the Houdini
	 * mooa_highlightTangentTransfer step the original workflow required.
	 */
	Ellipsoid,

	/**
	 * Per-island PCA: each hair card's own dominant axis, projected onto the surface. True
	 * strand-flow anisotropy -- highlights break per card instead of forming one clean ring.
	 */
	IslandPCA,
};

/**
 * Bakes a per-vertex strand-flow tangent into UV1.xy / UV2.x, for the Kajiya hair specular.
 *
 * Kajiya-Kay draws its highlight as a band perpendicular to the strand tangent, so it is only as
 * good as that tangent. UV-derived mesh tangents are useless on atlas-packed hair (this project's
 * hair UVs are not unwrapped root-to-tip -- the same reason the outline width curve defaults to
 * island PCA), which is exactly what smears the toon Kajiya highlight into large flat patches.
 *
 * The bake: split the selected sections into connected islands (one hair card each -- welded by
 * position, connected through faces, exactly as the outline width tool does), take each island's
 * dominant 3D axis as the strand direction, project it onto every vertex's tangent plane so it
 * follows the card around curves, and store it in tangent space so it survives skinning.
 *
 * Storage is the face-forward bake's channel set and encoding (dir * 0.5 + 0.5 across
 * UV1.xy / UV2.x): a face never runs the hair feature and hair never runs the SDF facial shadow,
 * so the two bakes can share the channels as long as they are applied to disjoint sections. The
 * material decodes both the same way -- normalize(float3(UV1.xy, UV2.x) * 2 - 1) -- and the hair
 * material feeds the result to its Tangent output with Anisotropy 1, which GetToonWorldTangent
 * already prefers over the UV tangent.
 *
 * The axis SIGN is arbitrary per island. Kajiya is symmetric in the tangent sign (the lobe uses
 * sin of the angle to the half vector), so no orientation pass is needed or attempted.
 */
UCLASS()
class MOONTOONEDITOR_API UMoonToonStrandTangentTool : public UMoonToonTool
{
	GENERATED_BODY()

public:
	virtual FText GetToolName() const override;
	virtual FText GetToolDescription() const override;
	virtual FText GetRunLabel() const override;
	virtual bool IsDestructive() const override
	{
		return Mode == EMoonToonStrandTangentMode::Bake || Mode == EMoonToonStrandTangentMode::Restore;
	}
	// Reimports when the build settings were wrong, and a reimport can change the material list.
	virtual bool InvalidatesSectionList() const override { return true; }
	virtual FString Run(const FMoonToonToolContext& Context) override;

	/** True while a live-preview actor exists in the editor world. */
	static bool IsLivePreviewActive();

	/** Destroys every live-preview actor (restoring the materials they overrode). Returns how many. */
	static int32 StopLivePreview();

	/**
	 * The live-preview actor's bake entry: same channel write and same first-run backup as the
	 * panel's Bake mode, but with the ellipsoid supplied explicitly in the mesh's local space
	 * (the actor folds its world transform through the placed component's).
	 */
	static FString BakeEllipsoidChannels(
		UObject* Mesh,
		const TArray<int32>& SectionMaterialIndices,
		const FVector& LocalCenter,
		const FVector& LocalRadii,
		const FQuat& LocalRotation,
		bool bAllLODs,
		int32 LODIndex);

	UPROPERTY(EditAnywhere, Category = "Mode")
	EMoonToonStrandTangentMode Mode = EMoonToonStrandTangentMode::Preview;

	UPROPERTY(EditAnywhere, Category = "Mode",
		meta = (EditCondition = "Mode != EMoonToonStrandTangentMode::Restore"))
	EMoonToonStrandTangentSource TangentSource = EMoonToonStrandTangentSource::Ellipsoid;

	// --- Ellipsoid (MooaToon-style ring) ---------------------------------------------------------

	/** Fit the ellipsoid to the selected sections' bounds, then apply the offset/scale/rotation
	 *  below. Turn off to type an absolute centre and radii instead. The report prints the fitted
	 *  values either way, so switching to manual starts from the fitted baseline. */
	UPROPERTY(EditAnywhere, Category = "Ellipsoid",
		meta = (EditCondition = "TangentSource == EMoonToonStrandTangentSource::Ellipsoid && Mode != EMoonToonStrandTangentMode::Restore"))
	bool bFitEllipsoidToSelection = true;

	/** Added to the (fitted or manual) centre, in the mesh's local space. Slides the ring. */
	UPROPERTY(EditAnywhere, Category = "Ellipsoid",
		meta = (EditCondition = "TangentSource == EMoonToonStrandTangentSource::Ellipsoid && Mode != EMoonToonStrandTangentMode::Restore"))
	FVector EllipsoidCenterOffset = FVector::ZeroVector;

	/** Multiplies the fitted radii. Squash Z below 1 to flatten the ring's curve, stretch to arch it. */
	UPROPERTY(EditAnywhere, Category = "Ellipsoid",
		meta = (EditCondition = "bFitEllipsoidToSelection && TangentSource == EMoonToonStrandTangentSource::Ellipsoid && Mode != EMoonToonStrandTangentMode::Restore"))
	FVector EllipsoidRadiiScale = FVector::OneVector;

	/** Absolute centre in mesh local space, used when the fit is off. */
	UPROPERTY(EditAnywhere, Category = "Ellipsoid",
		meta = (EditCondition = "!bFitEllipsoidToSelection && TangentSource == EMoonToonStrandTangentSource::Ellipsoid && Mode != EMoonToonStrandTangentMode::Restore"))
	FVector EllipsoidCenter = FVector::ZeroVector;

	/** Absolute radii in mesh local space, used when the fit is off. */
	UPROPERTY(EditAnywhere, Category = "Ellipsoid",
		meta = (EditCondition = "!bFitEllipsoidToSelection && TangentSource == EMoonToonStrandTangentSource::Ellipsoid && Mode != EMoonToonStrandTangentMode::Restore"))
	FVector EllipsoidRadii = FVector(12.0, 12.0, 14.0);

	/** Tilts the pole axis (the meridians run pole to pole, so this tips the whole ring). */
	UPROPERTY(EditAnywhere, Category = "Ellipsoid",
		meta = (EditCondition = "TangentSource == EMoonToonStrandTangentSource::Ellipsoid && Mode != EMoonToonStrandTangentMode::Restore"))
	FRotator EllipsoidRotation = FRotator::ZeroRotator;

	/** Draw the ellipsoid's wireframe with the preview lines, so the fit can be judged directly. */
	UPROPERTY(EditAnywhere, Category = "Ellipsoid",
		meta = (EditCondition = "TangentSource == EMoonToonStrandTangentSource::Ellipsoid && Mode == EMoonToonStrandTangentMode::Preview"))
	bool bPreviewEllipsoidWireframe = true;

	// --- Island PCA ------------------------------------------------------------------------------

	/**
	 * Islands smaller than this keep their existing UV data. A handful of vertices has no
	 * meaningful principal axis, and a random strand direction reads as a glitched highlight.
	 */
	UPROPERTY(EditAnywhere, Category = "Bake",
		meta = (ClampMin = "3", EditCondition = "TangentSource == EMoonToonStrandTangentSource::IslandPCA && Mode != EMoonToonStrandTangentMode::Restore"))
	int32 MinIslandVertices = 8;

	/**
	 * Recomputing tangents on import would replace the basis this bake encodes against, and
	 * low-precision UVs would quantise the stored direction. Same requirement as the other bakes.
	 */
	UPROPERTY(EditAnywhere, Category = "Bake",
		meta = (EditCondition = "Mode == EMoonToonStrandTangentMode::Bake"))
	bool bFixBuildSettingsFirst = true;

	UPROPERTY(EditAnywhere, Category = "Bake")
	bool bAllLODs = true;

	UPROPERTY(EditAnywhere, Category = "Bake", meta = (EditCondition = "!bAllLODs", ClampMin = "0"))
	int32 LODIndex = 0;

	/** World-space length of each preview line, in centimetres. */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (ClampMin = "0.1", EditCondition = "Mode == EMoonToonStrandTangentMode::Preview"))
	float PreviewLineLength = 1.5f;

	/**
	 * Seconds the preview lines stay up. 0 keeps them until the next preview (or any debug-line
	 * flush) replaces them.
	 */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (ClampMin = "0", EditCondition = "Mode == EMoonToonStrandTangentMode::Preview"))
	float PreviewDurationSeconds = 30.0f;

	/** Draw every Nth vertex. 1 = every vertex; raise it if a dense mesh makes the viewport crawl. */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (ClampMin = "1", EditCondition = "Mode == EMoonToonStrandTangentMode::Preview"))
	int32 PreviewVertexStride = 2;
};
