// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonMeshTargets.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "RawMesh.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkeletalMeshTypes.h"
#include "SkinnedAssetCompiler.h"

FMoonToonScopedMeshEdit::FMoonToonScopedMeshEdit(UObject* Mesh)
{
	if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
	{
		Inner = MakeUnique<FScopedSkeletalMeshPostEditChange>(SkeletalMesh);
	}
}

// Out of line so the header does not have to see FScopedSkeletalMeshPostEditChange.
FMoonToonScopedMeshEdit::~FMoonToonScopedMeshEdit() = default;

namespace MoonToonMesh
{
	bool IsSupportedMesh(const UObject* Asset)
	{
		return Asset && (Asset->IsA<UStaticMesh>() || Asset->IsA<USkeletalMesh>());
	}

	bool PatchVertexAlphaLive(USkeletalMesh* Mesh, int32 LODIndex, const TArray<float>& PointAlpha)
	{
		if (!Mesh)
		{
			return false;
		}

		// An async build in flight owns the buffers this is about to touch.
		if (Mesh->IsCompiling())
		{
			USkinnedAsset* AsSkinned = Mesh;
			FSkinnedAssetCompilingManager::Get().FinishCompilation(MakeArrayView(&AsSkinned, 1));
		}

		FSkeletalMeshModel* Model = Mesh->GetImportedModel();
		FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
		if (!Model || !Model->LODModels.IsValidIndex(LODIndex)
			|| !RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
		{
			return false;
		}

		FSkeletalMeshLODModel& LODModel = Model->LODModels[LODIndex];
		FSkeletalMeshLODRenderData& LODRender = RenderData->LODRenderData[LODIndex];

		// Render vertex -> import point. Without it there is no way to address render vertices from
		// import-space data, and the caller has to pay the full rebuild instead.
		const TArray<int32>& MeshToImport = LODModel.MeshToImportVertexMap;
		const int32 NumRenderVerts = static_cast<int32>(LODRender.GetNumVertices());
		if (NumRenderVerts == 0 || MeshToImport.Num() != NumRenderVerts)
		{
			return false;
		}

		FColorVertexBuffer& ColorBuffer = LODRender.StaticVertexBuffers.ColorVertexBuffer;

		Mesh->SetHasVertexColors(true);
		// Part of the DDC key, so cached render data built from the old colours cannot be reused.
		Mesh->SetVertexColorGuid(FGuid::NewGuid());

		// From here on: the Mesh Paint skeletal adapter's sequence, verbatim in spirit. The RHI must
		// let go of the buffers before the CPU copies change.
		Mesh->ReleaseResources();
		Mesh->ReleaseResourcesFence.Wait();

		if (ColorBuffer.GetNumVertices() == 0)
		{
			// No colour stream yet: materialise the implicit all-white one, then write into it.
			ColorBuffer.InitFromSingleColor(FColor::White, NumRenderVerts);
		}

		for (int32 VertIndex = 0; VertIndex < NumRenderVerts; ++VertIndex)
		{
			const int32 PointIndex = MeshToImport[VertIndex];
			if (!PointAlpha.IsValidIndex(PointIndex) || PointAlpha[PointIndex] < 0.0f)
			{
				continue;
			}

			const uint8 Alpha = static_cast<uint8>(FMath::RoundToInt(
				FMath::Clamp(PointAlpha[PointIndex], 0.0f, 1.0f) * 255.0f));

			// Both copies, or they drift: the render buffer is what the viewport shows, the LODModel
			// soft vertices are what Mesh Paint and any editor tooling read as the current colours.
			ColorBuffer.VertexColor(VertIndex).A = Alpha;

			int32 SectionIndex = 0;
			int32 SectionVertIndex = 0;
			LODModel.GetSectionFromVertexIndex(VertIndex, SectionIndex, SectionVertIndex);
			if (LODModel.Sections.IsValidIndex(SectionIndex)
				&& LODModel.Sections[SectionIndex].SoftVertices.IsValidIndex(SectionVertIndex))
			{
				LODModel.Sections[SectionIndex].SoftVertices[SectionVertIndex].Color.A = Alpha;
			}
		}

		{
			// Detach every component using the mesh, re-init the mesh resources, and on scope exit
			// reattach them against the patched buffers.
			FSkinnedMeshComponentRecreateRenderStateContext RecreateContext(Mesh);
			Mesh->InitResources();
		}

		Mesh->MarkPackageDirty();
		return true;
	}

	int32 GetNumLODs(UObject* Mesh)
	{
		if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh))
		{
			return StaticMesh->GetNumSourceModels();
		}
		if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
		{
			return SkeletalMesh->GetLODNum();
		}
		return 0;
	}

	bool GetFaces(UObject* Mesh, int32 LODIndex, FMoonToonLODFaces& OutFaces)
	{
		TArray<FIntVector>& OutFaceWedges = OutFaces.Wedges;
		TArray<int32>& OutFaceMaterialIndices = OutFaces.MaterialIndices;
		OutFaceWedges.Reset();
		OutFaceMaterialIndices.Reset();

		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
		{
			if (!SkeletalMesh->IsValidLODIndex(LODIndex))
			{
				return false;
			}

			// Deliberately the deprecated import-data path rather than GetMeshDescription: the whole
			// MoonToon bake pipeline (UMoonToonEditorBPLibrary) reads and writes through
			// FSkeletalMeshImportData, and this has to agree with it wedge-for-wedge. Migrating one
			// caller alone would produce a face ordering the bakes do not share.
			FSkeletalMeshImportData ImportData;
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			SkeletalMesh->LoadLODImportedData(LODIndex, ImportData);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
			if (ImportData.Faces.Num() == 0)
			{
				// Generated (non-imported) LODs have no import data at all. Not an error, just nothing to do.
				return false;
			}

			OutFaceWedges.Reserve(ImportData.Faces.Num());
			OutFaceMaterialIndices.Reserve(ImportData.Faces.Num());
			for (const SkeletalMeshImportData::FTriangle& Face : ImportData.Faces)
			{
				OutFaceWedges.Emplace(
					static_cast<int32>(Face.WedgeIndex[0]),
					static_cast<int32>(Face.WedgeIndex[1]),
					static_cast<int32>(Face.WedgeIndex[2]));
				OutFaceMaterialIndices.Add(static_cast<int32>(Face.MatIndex));
			}
			return true;
		}

		if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh))
		{
			if (!StaticMesh->IsSourceModelValid(LODIndex))
			{
				return false;
			}

			FRawMesh RawMesh;
			StaticMesh->GetSourceModel(LODIndex).LoadRawMesh(RawMesh);
			const int32 NumFaces = RawMesh.WedgeIndices.Num() / 3;
			if (NumFaces == 0)
			{
				return false;
			}

			OutFaceWedges.Reserve(NumFaces);
			OutFaceMaterialIndices.Reserve(NumFaces);
			for (int32 FaceIndex = 0; FaceIndex < NumFaces; ++FaceIndex)
			{
				OutFaceWedges.Emplace(FaceIndex * 3, FaceIndex * 3 + 1, FaceIndex * 3 + 2);
				OutFaceMaterialIndices.Add(
					RawMesh.FaceMaterialIndices.IsValidIndex(FaceIndex) ? RawMesh.FaceMaterialIndices[FaceIndex] : 0);
			}
			return true;
		}

		return false;
	}

	bool GetSections(UObject* Mesh, const FMoonToonLODFaces& Faces, TArray<FMoonToonSectionInfo>& OutSections)
	{
		OutSections.Reset();

		if (!Faces.IsValid())
		{
			return false;
		}

		// Triangle counts first, so a section that exists in the material array but has no geometry in
		// this LOD still shows up (with zero triangles) rather than silently vanishing.
		TMap<int32, int32> TriangleCounts;
		for (int32 MaterialIndex : Faces.MaterialIndices)
		{
			TriangleCounts.FindOrAdd(MaterialIndex)++;
		}

		auto AddSection = [&OutSections, &TriangleCounts](int32 MaterialIndex, FName SlotName, const UMaterialInterface* Material)
		{
			FMoonToonSectionInfo& Info = OutSections.AddDefaulted_GetRef();
			Info.MaterialIndex = MaterialIndex;
			Info.SlotName = SlotName;
			Info.MaterialName = Material ? Material->GetName() : TEXT("None");
			Info.NumTriangles = TriangleCounts.FindRef(MaterialIndex);
		};

		if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
		{
			const TArray<FSkeletalMaterial>& Materials = SkeletalMesh->GetMaterials();
			for (int32 Index = 0; Index < Materials.Num(); ++Index)
			{
				AddSection(Index, Materials[Index].MaterialSlotName, Materials[Index].MaterialInterface);
			}
		}
		else if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh))
		{
			const TArray<FStaticMaterial>& Materials = StaticMesh->GetStaticMaterials();
			for (int32 Index = 0; Index < Materials.Num(); ++Index)
			{
				AddSection(Index, Materials[Index].MaterialSlotName, Materials[Index].MaterialInterface);
			}
		}

		// Faces can reference a material index past the end of the material array on damaged assets.
		// Surface those instead of dropping the geometry from the list.
		for (const TPair<int32, int32>& Pair : TriangleCounts)
		{
			const bool bAlreadyListed = OutSections.ContainsByPredicate(
				[&Pair](const FMoonToonSectionInfo& Info) { return Info.MaterialIndex == Pair.Key; });
			if (!bAlreadyListed)
			{
				AddSection(Pair.Key, NAME_None, nullptr);
			}
		}

		OutSections.Sort([](const FMoonToonSectionInfo& A, const FMoonToonSectionInfo& B)
		{
			return A.MaterialIndex < B.MaterialIndex;
		});
		return true;
	}

	bool GetSections(UObject* Mesh, int32 LODIndex, TArray<FMoonToonSectionInfo>& OutSections)
	{
		FMoonToonLODFaces Faces;
		if (!GetFaces(Mesh, LODIndex, Faces))
		{
			OutSections.Reset();
			return false;
		}
		return GetSections(Mesh, Faces, OutSections);
	}

	FMoonToonWedgeMask BuildWedgeMask(
		const FMoonToonLODFaces& Faces,
		const TArray<int32>& MaterialIndices,
		int32 NumWedges)
	{
		FMoonToonWedgeMask Mask;
		// No filter, or no face data to filter by: include everything rather than silently turning
		// the tool into a no-op.
		if (MaterialIndices.Num() == 0 || !Faces.IsValid())
		{
			return Mask;
		}

		// Derive the wedge count when the caller did not supply one. Skeletal wedge indices are not
		// sequential, so this has to be a max over the face data, not a face count times three.
		int32 MaskSize = NumWedges;
		if (MaskSize <= 0)
		{
			for (const FIntVector& Face : Faces.Wedges)
			{
				MaskSize = FMath::Max(MaskSize, FMath::Max3(Face.X, Face.Y, Face.Z) + 1);
			}
		}
		if (MaskSize <= 0)
		{
			return Mask;
		}

		Mask.bIncludeAll = false;
		Mask.Included.Init(false, MaskSize);

		const TSet<int32> Wanted(MaterialIndices);
		for (int32 FaceIndex = 0; FaceIndex < Faces.Wedges.Num(); ++FaceIndex)
		{
			if (!Wanted.Contains(Faces.MaterialIndices[FaceIndex]))
			{
				continue;
			}
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 WedgeIndex = Faces.Wedges[FaceIndex][Corner];
				if (Mask.Included.IsValidIndex(WedgeIndex))
				{
					Mask.Included[WedgeIndex] = true;
				}
			}
		}
		return Mask;
	}

	FMoonToonWedgeMask BuildWedgeMask(
		UObject* Mesh,
		int32 LODIndex,
		const TArray<int32>& MaterialIndices,
		int32 NumWedges)
	{
		if (MaterialIndices.Num() == 0)
		{
			return FMoonToonWedgeMask();
		}

		FMoonToonLODFaces Faces;
		GetFaces(Mesh, LODIndex, Faces);
		return BuildWedgeMask(Faces, MaterialIndices, NumWedges);
	}
}
