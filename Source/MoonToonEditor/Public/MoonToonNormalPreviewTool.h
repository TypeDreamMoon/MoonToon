// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MoonToonTool.h"
#include "MoonToonNormalPreviewTool.generated.h"

/** Which direction the preview draws. */
UENUM()
enum class EMoonToonNormalSource : uint8
{
	/** The import data's per-wedge TangentZ -- the normal the renderer shades and extrudes with. */
	VertexNormal,

	/** The triangle's own geometric normal, from its winding. Drawn at the face centre, one per face.
	 *  Comparing it against the vertex normal is what tells a flipped normal apart from a hard edge. */
	FaceNormal,

	/** The smoothed normal baked into UV2.y / UV3.xy, decoded back through the wedge's tangent basis
	 *  -- i.e. the direction the outline pass actually extrudes along. Its length is the curvature the
	 *  bake folded in, so Colour = Length reads out the width modulation. */
	SmoothedNormal,

	/** The face-forward direction baked into UV1.xy / UV2.x, decoded the same way. On a correct bake
	 *  every arrow points the same way in local space (+Y by default), which Colour = Direction shows
	 *  as one flat colour across the face. */
	FaceForward,

	/** TangentX, the U direction. What the Kajiya highlight uses when the strand-tangent bake is off. */
	Tangent,

	/** TangentY, the V direction. */
	Binormal,
};

/** What the line colour encodes. Each mode answers a different question about the same lines. */
UENUM()
enum class EMoonToonNormalColor : uint8
{
	/** Local-space XYZ as RGB (dir * 0.5 + 0.5). Deliberately the mesh's own space, not the world's,
	 *  so the colours describe the asset rather than how the actor happens to be rotated. */
	Direction,

	/** Agreement with a reference direction: green agrees, yellow is perpendicular, red is opposed.
	 *  The reference depends on the source -- face winding for Vertex Normal (so red = flipped), the
	 *  raw vertex normal for the two bakes (so red = the bake drifted far from the surface), and for
	 *  Tangent / Binormal it is orthogonality to the normal (red = a collapsed basis). */
	Agreement,

	/** Length of the decoded vector before normalising: blue 0, white 1, magenta ~1.73. That last one
	 *  is the tell for an unbaked channel -- all-zero UVs decode to (-1,-1,-1). */
	Length,
};

UENUM()
enum class EMoonToonNormalPreviewMode : uint8
{
	/** Replace the current lines with a fresh set. */
	Draw,

	/** Remove persistent preview lines and draw nothing. Only needed when Duration is 0; lines drawn
	 *  with a duration expire on their own. */
	Clear,
};

/**
 * Draws a mesh's directions in the viewport, so "which way does this point" stops being a question
 * you answer by putting a debug switch in the material.
 *
 * Read-only, and deliberately fed from IMPORT data rather than render data: that is where every
 * MoonToon bake lives, so the same tool shows the raw vertex normal, the smoothed normal the outline
 * extrudes along, and the face-forward direction the facial SDF shadow reads -- decoded through the
 * very tangent basis the bake encoded against, which is the only way to confirm a bake landed the way
 * the material will read it.
 *
 * Two consequences of using import data are worth knowing before reading the lines: they sit at the
 * REFERENCE POSE (a posed or animated character will have its lines standing away from it), and one
 * line is drawn per unique direction at a vertex rather than per wedge -- so a split normal shows up
 * as two lines from one point, which is exactly how a hard edge should look.
 */
UCLASS()
class MOONTOONEDITOR_API UMoonToonNormalPreviewTool : public UMoonToonTool
{
	GENERATED_BODY()

public:
	virtual FText GetToolName() const override;
	virtual FText GetToolDescription() const override;
	virtual FText GetRunLabel() const override;
	virtual FName GetToolIconName() const override { return TEXT("Icons.Visible"); }
	virtual bool IsDestructive() const override { return false; }
	virtual FString Run(const FMoonToonToolContext& Context) override;

	UPROPERTY(EditAnywhere, Category = "Preview")
	EMoonToonNormalPreviewMode Mode = EMoonToonNormalPreviewMode::Draw;

	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (EditCondition = "Mode == EMoonToonNormalPreviewMode::Draw"))
	EMoonToonNormalSource Source = EMoonToonNormalSource::VertexNormal;

	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (EditCondition = "Mode == EMoonToonNormalPreviewMode::Draw"))
	EMoonToonNormalColor ColorMode = EMoonToonNormalColor::Direction;

	/** Also draw the raw vertex normal, dimmed, next to every line. The point of the tool when a bake
	 *  is being checked: the pair shows how far the baked direction has been pulled off the surface. */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (EditCondition = "Mode == EMoonToonNormalPreviewMode::Draw && Source != EMoonToonNormalSource::VertexNormal"))
	bool bAlsoDrawVertexNormal = false;

	/** Arrow heads read the sign at a glance, at three times the line count. Off for dense sweeps. */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (EditCondition = "Mode == EMoonToonNormalPreviewMode::Draw"))
	bool bArrowHeads = true;

	/** Which LOD's import data to read. One LOD only: every LOD occupies the same space, so drawing
	 *  two at once is unreadable. */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (ClampMin = "0", EditCondition = "Mode == EMoonToonNormalPreviewMode::Draw"))
	int32 LODIndex = 0;

	/** World-space length of each line, in centimetres. */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (ClampMin = "0.1", EditCondition = "Mode == EMoonToonNormalPreviewMode::Draw"))
	float LineLength = 2.0f;

	/** 0 draws hairlines, which is what you want on a dense mesh. */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (ClampMin = "0", EditCondition = "Mode == EMoonToonNormalPreviewMode::Draw"))
	float LineThickness = 0.0f;

	/** Draw every Nth vertex (every Nth face for the Face Normal source). Split normals at a drawn
	 *  vertex all survive the stride -- it thins the sample, it does not hide seams. */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (ClampMin = "1", EditCondition = "Mode == EMoonToonNormalPreviewMode::Draw"))
	int32 VertexStride = 2;

	/** Seconds the lines stay up. 0 keeps them until the next run, or until Mode = Clear. */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (ClampMin = "0", EditCondition = "Mode == EMoonToonNormalPreviewMode::Draw"))
	float DurationSeconds = 30.0f;

	/** Hard ceiling on lines per mesh. A full-density character is six figures of wedges, which drops
	 *  the viewport to single-digit FPS; the report always says when the cap truncated the draw. */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (ClampMin = "100", EditCondition = "Mode == EMoonToonNormalPreviewMode::Draw"))
	int32 MaxLines = 20000;

	/** Report the statistics (deviation, flipped-normal count, decoded lengths) without drawing. */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (EditCondition = "Mode == EMoonToonNormalPreviewMode::Draw"))
	bool bReportOnly = false;
};
