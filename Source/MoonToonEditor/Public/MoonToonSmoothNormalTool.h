// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetActionUtility.h"
#include "MoonToonMeshTargets.h"
#include "MoonToonSmoothNormalTool.generated.h"

class UDynamicMesh;

/**
 * Smooth-normal / curvature baking for toon outlines, as a scripted asset action.
 *
 * Right-click one or more Static/Skeletal Meshes in the content browser -> Scripted Asset Actions.
 * This is the C++ port of MooaToon's EUBP_SmoothNormal editor utility Blueprint; the heavy lifting
 * (mesh import-data read/write, MikkTSpace tangents, curvature, nearest-point queries) still lives
 * in UMoonToonEditorBPLibrary, exactly as the Blueprint had it.
 *
 * Why bake into import data rather than render data: the outline pass needs a *smoothed* normal
 * that ignores hard edges, otherwise the extruded outline splits at every UV/normal seam. The
 * smoothed normal is stored in tangent space so it survives skinning, and is packed alongside a
 * curvature term used to modulate outline width.
 */
UCLASS()
class MOONTOONEDITOR_API UMoonToonSmoothNormalTool : public UAssetActionUtility
{
	GENERATED_BODY()

public:
	UMoonToonSmoothNormalTool();

	/**
	 * Bakes the tangent-space smoothed normal (scaled by curvature) into UV2.y / UV3.xy of every
	 * selected mesh, for every LOD. UV2.x is preserved.
	 */
	UFUNCTION(CallInEditor, Category = "MoonToon")
	void BakeSmoothedNormalAndCurvature();

	/**
	 * Bakes a fixed world-space "face forward" direction into tangent space and stores it in
	 * UV2.xy / UV3.x of every selected mesh. Used by the face SDF shadow feature.
	 *
	 * Mutually exclusive with BakeSmoothedNormalAndCurvature: both write UV2.y and UV3.x, so run
	 * one or the other on a given mesh, never both.
	 */
	UFUNCTION(CallInEditor, Category = "MoonToon")
	void BakeFaceForwardDirection(FVector FaceForwardDirWS = FVector(0.0, 1.0, 0.0)); // +Y: matches the original EUBP's FaceForwardDirWS local-variable default

	/**
	 * Forces the LOD build settings the bake depends on (no normal/tangent recompute, high precision
	 * tangents and UVs) and reimports any mesh that had to be corrected.
	 */
	UFUNCTION(CallInEditor, Category = "MoonToon")
	void FixBuildSettings();

	// --- Explicit-target entry points -------------------------------------------------------------
	// The UFUNCTIONs above read the content browser selection, which is what the right-click menu
	// needs. The tools panel has its own target and section selection, so it calls these instead.
	// Both routes end up in the same implementation; there is no second copy of the bake.

	/** Bakes one LOD. Wedges the mask excludes keep whatever they already had. */
	static void BakeSmoothedNormalForLOD(UObject* Mesh, int32 LODIndex, const FMoonToonWedgeMask& Mask);

	/** Bakes one LOD. Wedges the mask excludes keep whatever they already had. */
	static void BakeFaceForwardForLOD(UObject* Mesh, int32 LODIndex, FVector FaceForwardDirWS, const FMoonToonWedgeMask& Mask);

	/** Applies the required build settings to every LOD of one mesh; true if anything had to change. */
	static bool FixBuildSettingsForMesh(UObject* Mesh);

	// Shared with the other bake tools (the strand-tangent bake goes through the same TBN and the
	// same MikkTSpace repair), so these two live here as the single implementation.

	/** Regenerates tangents/binormals via MikkTSpace, but only when HasBadTangentOrBinormal says so. */
	static void RecomputeMissingTangentAndBinormal(
		const TArray<FVector3f>& Vertices,
		const TArray<int32>& Indices,
		const TArray<FVector2f>& UVs,
		const TArray<FVector3f>& Normals,
		TArray<FVector3f>& Tangents,
		TArray<FVector3f>& Binormals);

	/**
	 * Builds the tangent->local matrix for one wedge. Degenerate (NaN / zero) axes fall back to the
	 * identity basis vector and log which mesh was at fault, matching the Blueprint's behaviour.
	 */
	static FMatrix BuildTangentToLocal(
		int32 WedgeIndex,
		const UObject* Mesh,
		const TArray<FVector3f>& Normals,
		const TArray<FVector3f>& Tangents,
		const TArray<FVector3f>& Binormals);

private:
	/** Runs Body once per (selected mesh, LOD) pair. Mirrors the ForEachSelectedMeshesLOD macro. */
	static void ForEachSelectedMeshLOD(TFunctionRef<void(UObject* Mesh, int32 LODIndex)> Body);

	/** Applies the required build settings to one LOD; returns true if anything had to change. */
	static bool FixBuildSettingsForLOD(UObject* Mesh, int32 LODIndex);

	/** True when tangents/binormals are missing, mismatched in count, or contain zero vectors. */
	static bool HasBadTangentOrBinormal(const TArray<FVector3f>& Tangents, const TArray<FVector3f>& Binormals);

	/** Copies a mesh LOD into a UDynamicMesh, applying build settings and requesting tangents. */
	static UDynamicMesh* ToDynamicMesh(UObject* Mesh, int32 LODIndex);
};
