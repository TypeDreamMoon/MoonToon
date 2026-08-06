// Copyright Dream Moon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Shared mesh introspection for the MoonToon tools panel.
 *
 * Everything here exists because static and skeletal meshes expose the same information through two
 * completely different import-data structures, and every tool in the panel needs to ask the same
 * three questions: how many LODs, what sections, and which wedges belong to a section.
 */

/** One material section of a mesh LOD, as the panel lists it. */
struct FMoonToonSectionInfo
{
	/** Material index the faces reference. This is the identity used for filtering, not the row order. */
	int32 MaterialIndex = INDEX_NONE;

	/** Material slot name -- what an artist actually recognises the section by. */
	FName SlotName;

	/** Assigned material asset name, or "None". */
	FString MaterialName;

	int32 NumTriangles = 0;
};

/**
 * Per-wedge inclusion mask built from a section selection.
 *
 * Tools iterate wedges, but sections are a per-face property, so the section filter has to be
 * flattened onto wedges first. bIncludeAll short-circuits the common "no filter" case rather than
 * allocating and filling a full bit array.
 */
struct FMoonToonWedgeMask
{
	bool bIncludeAll = true;
	TBitArray<> Included;

	bool Contains(int32 WedgeIndex) const
	{
		return bIncludeAll || (Included.IsValidIndex(WedgeIndex) && Included[WedgeIndex]);
	}
};

/**
 * Face data for one LOD, loaded once.
 *
 * Reading it is the expensive part of touching a mesh -- on the skeletal side every read decompresses
 * the whole FSkeletalMeshImportData blob -- so tools load this once and derive sections and wedge
 * masks from it, rather than each helper re-reading the mesh behind the caller's back.
 */
struct FMoonToonLODFaces
{
	/**
	 * Wedge indices per face. Not always {F*3, F*3+1, F*3+2}: skeletal import data stores an explicit
	 * FTriangle::WedgeIndex[3] which need not be sequential, while FRawMesh really is. Getting this
	 * wrong silently shuffles per-wedge data across the mesh, so both paths are spelled out once in
	 * GetFaces instead of being re-derived per tool.
	 */
	TArray<FIntVector> Wedges;

	/** Material index per face, parallel to Wedges. */
	TArray<int32> MaterialIndices;

	bool IsValid() const { return Wedges.Num() > 0 && Wedges.Num() == MaterialIndices.Num(); }
};

/**
 * Batches the rebuild that follows writing mesh data.
 *
 * Every write through UMoonToonEditorBPLibrary::MoonSetMeshData ends in PostEditChange, and
 * USkeletalMesh::PostEditChangeProperty rebuilds the whole asset -- chunking, skin weights, morph
 * targets. Writing N LODs therefore pays that N times. Holding one of these across the LOD loop
 * defers it to a single rebuild when the scope closes.
 *
 * Skeletal meshes only: UStaticMesh has no equivalent deferral, so a static mesh still rebuilds per
 * LOD. Constructing one for a static mesh (or null) is a harmless no-op.
 */
class MOONTOONEDITOR_API FMoonToonScopedMeshEdit
{
public:
	explicit FMoonToonScopedMeshEdit(UObject* Mesh);
	~FMoonToonScopedMeshEdit();

	FMoonToonScopedMeshEdit(const FMoonToonScopedMeshEdit&) = delete;
	FMoonToonScopedMeshEdit& operator=(const FMoonToonScopedMeshEdit&) = delete;

private:
	TUniquePtr<class FScopedSkeletalMeshPostEditChange> Inner;
};

namespace MoonToonMesh
{
	/** True for the two asset types every tool in the panel accepts. */
	MOONTOONEDITOR_API bool IsSupportedMesh(const UObject* Asset);

	/**
	 * First level component using this mesh asset.
	 *
	 * Every viewport preview needs it for the same reason: import data is in the mesh's local space,
	 * so without the placed component's transform the lines land at the world origin instead of on
	 * the character. The live preview additionally needs it to know whose materials to override.
	 */
	MOONTOONEDITOR_API class UMeshComponent* FindPlacedMeshComponent(const UObject* MeshAsset);

	/**
	 * Pushes per-point vertex alpha straight into a skeletal mesh's live render data and editor
	 * source model, without triggering a rebuild.
	 *
	 * This is the same sequence the Mesh Paint skeletal adapter uses: release resources, mutate the
	 * colour vertex buffer and the LODModel soft vertices, then re-init and recreate component render
	 * state. Nothing here re-runs chunking, skin weights or morph targets -- which is the entire
	 * point: a full PostEditChange rebuild of a heavy character costs seconds, all of it wasted when
	 * only a colour channel changed. The caller persists the same values into the LOD import data, so
	 * the next natural rebuild (reimport, cook, editor restart) reproduces them exactly.
	 *
	 * PointAlpha is indexed by import point (not wedge); entries < 0 leave that point untouched.
	 * Returns false when the render data or the render-vertex-to-import-point map is unavailable --
	 * the caller must then fall back to a full PostEditChange.
	 */
	MOONTOONEDITOR_API bool PatchVertexAlphaLive(
		class USkeletalMesh* Mesh,
		int32 LODIndex,
		const TArray<float>& PointAlpha);

	MOONTOONEDITOR_API int32 GetNumLODs(UObject* Mesh);

	/** Reads one LOD's face data. False when the LOD has no import data at all (a generated LOD). */
	MOONTOONEDITOR_API bool GetFaces(UObject* Mesh, int32 LODIndex, FMoonToonLODFaces& OutFaces);

	/** Sections of one LOD, ordered by material index, with slot names resolved from the mesh. */
	MOONTOONEDITOR_API bool GetSections(
		UObject* Mesh,
		const FMoonToonLODFaces& Faces,
		TArray<FMoonToonSectionInfo>& OutSections);

	/** Convenience overload that reads the face data itself. Prefer the one above in a loop. */
	MOONTOONEDITOR_API bool GetSections(UObject* Mesh, int32 LODIndex, TArray<FMoonToonSectionInfo>& OutSections);

	/**
	 * Flattens a section selection onto wedges. An empty MaterialIndices array means "no filter",
	 * which yields a mask that includes everything without touching memory.
	 *
	 * NumWedges sizes the bit array; pass 0 to derive it from the face data instead. Callers that
	 * already loaded wedge arrays should pass their length, so a mask is never shorter than the array
	 * it will be tested against.
	 */
	MOONTOONEDITOR_API FMoonToonWedgeMask BuildWedgeMask(
		const FMoonToonLODFaces& Faces,
		const TArray<int32>& MaterialIndices,
		int32 NumWedges = 0);

	/** Convenience overload that reads the face data itself. Prefer the one above in a loop. */
	MOONTOONEDITOR_API FMoonToonWedgeMask BuildWedgeMask(
		UObject* Mesh,
		int32 LODIndex,
		const TArray<int32>& MaterialIndices,
		int32 NumWedges = 0);
}
