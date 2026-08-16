// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MoonToonTool.h"
#include "MoonToonOutlineSetupTool.generated.h"

class UMaterialInterface;

/** What the tool decided a section is, and therefore whether it gets an outline. */
UENUM()
enum class EMoonToonOutlineSectionKind : uint8
{
	/** Ordinary opaque geometry. */
	Opaque,

	/**
	 * Alpha-cut geometry -- hair cards, lace, gauze. Needs the generated material instance to carry
	 * the same cutout, otherwise the hull fills the holes in solid.
	 */
	Masked,

	/** Blended geometry. No outline: an opaque hull behind it reads as a flat silhouette. */
	Translucent,

	/**
	 * A positional duplicate of another section -- the same triangles at the same coordinates, under
	 * a second material slot. Common on MMD/PMX imports, where the '+' slots duplicate their
	 * partners. One of each pair draws the outline; the other must not, or the outline pass emits
	 * two coincident hulls for the same surface.
	 */
	DuplicateOfAnotherSection,
};

/**
 * Sets up per-section outlines on a mesh.
 *
 * The inverted-hull outline assumes something no imported character actually guarantees: that the
 * mesh is opaque and that each surface exists once. Give it an MMD/PMX character and two things go
 * wrong at once -- the duplicated sections each get their own hull, and every alpha-cut gauze panel
 * gets its holes filled by the opaque hull.
 *
 * So this tool decides per section rather than per mesh, and writes the result to
 * MoonOutlineMaterialSlots -- where a null entry means "no outline on this slot" rather than "fall
 * back to the mesh-wide material".
 *
 * Run it once per character. Re-running is safe: existing generated instances are updated in place,
 * not duplicated.
 */
UCLASS()
class MOONTOONEDITOR_API UMoonToonOutlineSetupTool : public UMoonToonTool
{
	GENERATED_BODY()

public:
	UMoonToonOutlineSetupTool();

	virtual FText GetToolName() const override;
	virtual FText GetToolDescription() const override;
	virtual FText GetRunLabel() const override;
	virtual FName GetToolIconName() const override { return TEXT("Icons.Blueprints"); }
	virtual FString Run(const FMoonToonToolContext& Context) override;

	// --- Source -------------------------------------------------------------------------------

	/**
	 * Parent for every generated per-slot instance. Anything authored on it -- width, colour, the
	 * width ramp -- stays shared, because the generated children only ever override the cutout.
	 */
	UPROPERTY(EditAnywhere, Category = "Outline")
	TObjectPtr<UMaterialInterface> OutlineMaterialTemplate;

	/**
	 * Where the generated instances go. Empty means a "MoonOutline" folder beside the mesh, which
	 * keeps them with the character they belong to instead of in one shared bucket.
	 */
	UPROPERTY(EditAnywhere, Category = "Outline")
	FString OutputFolder;

	// --- Duplicate detection ------------------------------------------------------------------

	/**
	 * Find sections that are positional duplicates of another section, and let only one of each pair
	 * draw an outline.
	 *
	 * This is measured, not guessed. Two earlier hypotheses about MMD/PMX duplicate slots were tried
	 * against SK_星穹铁道—昔涟5 and both are false: the duplicates are NOT wound backwards (0 of 4000
	 * sampled coincident pairs had opposing winding) and they are NOT offset outward into a shell
	 * (their vertex positions match to within 10 microns, which is also why every ray-based test
	 * scores a section and its duplicate identically). What is actually there is 17968 pairs of
	 * exactly coincident triangles -- so coincidence is the thing worth testing for.
	 */
	UPROPERTY(EditAnywhere, Category = "Duplicate Detection")
	bool bExcludeDuplicateSections = true;

	/**
	 * How much of a section has to coincide with one other section before it counts as a duplicate.
	 * High on purpose: two garments touching along a seam share a few faces, and only a wholesale
	 * copy should cost a section its outline.
	 */
	UPROPERTY(EditAnywhere, Category = "Duplicate Detection", meta = (ClampMin = "0.5", ClampMax = "1.0"))
	float DuplicateFaceFraction = 0.9f;

	/**
	 * Grid the vertex positions are snapped to before comparison, in cm. The duplicates measured so
	 * far are bit-identical, so this only needs to absorb float noise; raising it far enough to
	 * merge genuinely distinct surfaces would start reporting neighbours as duplicates.
	 */
	UPROPERTY(EditAnywhere, Category = "Duplicate Detection", meta = (ClampMin = "0.00001"))
	float DuplicateWeldTolerance = 0.001f;

	// --- Classification -----------------------------------------------------------------------

	/** Leave out sections whose material is blended. An opaque hull behind glass is a black card. */
	UPROPERTY(EditAnywhere, Category = "Classification")
	bool bExcludeTranslucentSections = true;

	/**
	 * Copy the section's own cutout -- base colour map, opacity map and channel, clip value -- onto
	 * its generated outline instance, and make that instance masked.
	 *
	 * Without this the hull is opaque everywhere, which is what fills alpha-cut lace and gauze in
	 * solid. Only the parameters that feed the opacity mask are copied; shading parameters are
	 * deliberately left alone, since a hull is never lit like the surface it wraps.
	 */
	UPROPERTY(EditAnywhere, Category = "Classification")
	bool bCopySectionOpacity = true;

	/**
	 * How much lower the hull's opacity-mask threshold sits than the section's, as a multiplier.
	 *
	 * Copying the section's clip value verbatim looks right and is not: the hull then cuts at exactly
	 * the same texels as the surface, so the two silhouettes coincide and no outline can appear
	 * between them. It matters most on hair, whose visible shape comes from the alpha cutout rather
	 * than from the card's geometric edge -- the outline could only show along the card border, which
	 * is fully transparent and clipped away too, so hair ends up with almost no outline at all.
	 *
	 * A lower threshold dilates the hull's cutout into the alpha falloff just outside the surface,
	 * which is exactly where the outline belongs. 1.0 restores the coincident-cut behaviour; 0
	 * ignores the cutout entirely and gives a fully opaque hull.
	 */
	UPROPERTY(EditAnywhere, Category = "Classification",
		meta = (EditCondition = "bCopySectionOpacity", ClampMin = "0.0", ClampMax = "1.0"))
	float HullMaskDilation = 0.1f;

	// --- Write --------------------------------------------------------------------------------

	/**
	 * Write to the mesh asset, so every actor placed from it inherits the setup. Off writes to the
	 * placed component instead, which is the right choice only for a one-off override in one level.
	 */
	UPROPERTY(EditAnywhere, Category = "Write")
	bool bWriteToMeshAsset = true;

	/** Classify and report without creating assets or writing anything. */
	UPROPERTY(EditAnywhere, Category = "Write")
	bool bPreviewOnly = false;

	/** LOD whose geometry decides the classification. Sections are shared across LODs. */
	UPROPERTY(EditAnywhere, Category = "Write", meta = (ClampMin = "0"))
	int32 ClassifyFromLOD = 0;

private:
	/** One row of the classification, before anything is generated. */
	struct FSlotPlan
	{
		int32 MaterialIndex = INDEX_NONE;
		FName SlotName;
		UMaterialInterface* SourceMaterial = nullptr;
		EMoonToonOutlineSectionKind Kind = EMoonToonOutlineSectionKind::Opaque;
		int32 NumFaces = 0;
		/** Section this one duplicates, and how much of it coincides. */
		int32 DuplicateOfMaterialIndex = INDEX_NONE;
		float DuplicateFraction = 0.0f;
		/** Filled in during generation; null for every excluded slot. */
		UMaterialInterface* OutlineMaterial = nullptr;
	};

	bool BuildPlan(UObject* Mesh, TArray<FSlotPlan>& OutPlan, TArray<FString>& Lines) const;
	UMaterialInterface* GenerateSlotOutlineMaterial(UObject* Mesh, const FSlotPlan& Plan, TArray<FString>& Lines) const;
	void ApplyPlan(UObject* Mesh, const TArray<FSlotPlan>& Plan, TArray<FString>& Lines) const;
};
