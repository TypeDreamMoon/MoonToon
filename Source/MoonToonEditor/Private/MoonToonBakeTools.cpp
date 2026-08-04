// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonBakeTools.h"

#include "MoonToonSmoothNormalTool.h"

#define LOCTEXT_NAMESPACE "MoonToonBakeTools"

namespace
{
	/**
	 * Resolves the LOD range a bake tool should touch, honouring bAllLODs.
	 * Returns an empty array when the requested single LOD does not exist, so callers can report it.
	 */
	TArray<int32> ResolveLODRange(UObject* Mesh, bool bAllLODs, int32 LODIndex)
	{
		TArray<int32> LODs;
		const int32 NumLODs = MoonToonMesh::GetNumLODs(Mesh);
		if (bAllLODs)
		{
			for (int32 Index = 0; Index < NumLODs; ++Index)
			{
				LODs.Add(Index);
			}
		}
		else if (LODIndex >= 0 && LODIndex < NumLODs)
		{
			LODs.Add(LODIndex);
		}
		return LODs;
	}

	/** Shared "which sections am I about to touch" line, so both bakes report it identically. */
	FString DescribeSectionFilter(const FMoonToonToolContext& Context)
	{
		if (Context.SectionMaterialIndices.Num() == 0)
		{
			return TEXT("all sections");
		}

		TArray<FString> Parts;
		for (int32 MaterialIndex : Context.SectionMaterialIndices)
		{
			Parts.Add(FString::FromInt(MaterialIndex));
		}
		return FString::Printf(TEXT("sections [%s]"), *FString::Join(Parts, TEXT(", ")));
	}
}

// --- Smoothed normal ------------------------------------------------------------------------------

FText UMoonToonBakeSmoothNormalTool::GetToolName() const
{
	return LOCTEXT("SmoothNormalName", "Bake Smooth Normal");
}

FText UMoonToonBakeSmoothNormalTool::GetToolDescription() const
{
	return LOCTEXT("SmoothNormalDesc",
		"Bakes the tangent-space smoothed normal, scaled by curvature, into UV2.y / UV3.xy.\n\n"
		"This is what the outline hull extrudes along: a normal that ignores hard edges, so the "
		"outline does not split at every UV or normal seam. UV2.x is preserved because the "
		"face-forward bake owns it.");
}

FText UMoonToonBakeSmoothNormalTool::GetRunLabel() const
{
	return LOCTEXT("SmoothNormalRun", "Bake Smooth Normal");
}

FString UMoonToonBakeSmoothNormalTool::Run(const FMoonToonToolContext& Context)
{
	TArray<UObject*> Meshes;
	FString Report;
	if (!ResolveMeshes(Context, Meshes, Report))
	{
		return Report;
	}

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Bake Smooth Normal -- %s"), *DescribeSectionFilter(Context)));

	for (UObject* Mesh : Meshes)
	{
		if (bFixBuildSettingsFirst && UMoonToonSmoothNormalTool::FixBuildSettingsForMesh(Mesh))
		{
			Lines.Add(FString::Printf(TEXT("  %s: build settings were wrong, corrected and reimported."),
				*Mesh->GetName()));
		}

		const TArray<int32> LODs = ResolveLODRange(Mesh, bAllLODs, LODIndex);
		if (LODs.Num() == 0)
		{
			Lines.Add(FString::Printf(TEXT("  %s: LOD %d does not exist -- skipped."), *Mesh->GetName(), LODIndex));
			continue;
		}

		{
			// One rebuild for the whole LOD loop rather than one per LOD; each write otherwise pays a
			// full skeletal mesh rebuild.
			FMoonToonScopedMeshEdit BatchedEdit(Mesh);
			for (int32 LOD : LODs)
			{
				const FMoonToonWedgeMask Mask =
					MoonToonMesh::BuildWedgeMask(Mesh, LOD, Context.SectionMaterialIndices);
				UMoonToonSmoothNormalTool::BakeSmoothedNormalForLOD(Mesh, LOD, Mask);
			}
		}
		Lines.Add(FString::Printf(TEXT("  %s: baked LOD %s."), *Mesh->GetName(),
			bAllLODs ? TEXT("0..N") : *FString::FromInt(LODIndex)));
	}

	Lines.Add(TEXT(""));
	Lines.Add(TEXT("Reimporting the mesh discards this bake. Re-run afterwards."));
	return FString::Join(Lines, TEXT("\n"));
}

// --- Face forward ---------------------------------------------------------------------------------

FText UMoonToonBakeFaceForwardTool::GetToolName() const
{
	return LOCTEXT("FaceForwardName", "Bake Face Forward");
}

FText UMoonToonBakeFaceForwardTool::GetToolDescription() const
{
	return LOCTEXT("FaceForwardDesc",
		"Bakes a fixed world-space direction into tangent space and stores it in UV1.xy / UV2.x, for "
		"the distance-field facial shadow feature.\n\n"
		"Run this on the face section only. It writes a different channel set than the smooth-normal "
		"bake, so the two can coexist on the same mesh.");
}

FText UMoonToonBakeFaceForwardTool::GetRunLabel() const
{
	return LOCTEXT("FaceForwardRun", "Bake Face Forward");
}

FString UMoonToonBakeFaceForwardTool::Run(const FMoonToonToolContext& Context)
{
	TArray<UObject*> Meshes;
	FString Report;
	if (!ResolveMeshes(Context, Meshes, Report))
	{
		return Report;
	}

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Bake Face Forward %s -- %s"),
		*FaceForwardDirWS.ToString(), *DescribeSectionFilter(Context)));

	if (Context.SectionMaterialIndices.Num() == 0)
	{
		Lines.Add(TEXT("  NOTE: no section filter. This writes UV1.xy / UV2.x across the whole mesh, "
			"not just the face."));
	}

	for (UObject* Mesh : Meshes)
	{
		if (bFixBuildSettingsFirst && UMoonToonSmoothNormalTool::FixBuildSettingsForMesh(Mesh))
		{
			Lines.Add(FString::Printf(TEXT("  %s: build settings were wrong, corrected and reimported."),
				*Mesh->GetName()));
		}

		const TArray<int32> LODs = ResolveLODRange(Mesh, bAllLODs, LODIndex);
		if (LODs.Num() == 0)
		{
			Lines.Add(FString::Printf(TEXT("  %s: LOD %d does not exist -- skipped."), *Mesh->GetName(), LODIndex));
			continue;
		}

		{
			FMoonToonScopedMeshEdit BatchedEdit(Mesh);
			for (int32 LOD : LODs)
			{
				const FMoonToonWedgeMask Mask =
					MoonToonMesh::BuildWedgeMask(Mesh, LOD, Context.SectionMaterialIndices);
				UMoonToonSmoothNormalTool::BakeFaceForwardForLOD(Mesh, LOD, FaceForwardDirWS, Mask);
			}
		}
		Lines.Add(FString::Printf(TEXT("  %s: baked LOD %s."), *Mesh->GetName(),
			bAllLODs ? TEXT("0..N") : *FString::FromInt(LODIndex)));
	}

	return FString::Join(Lines, TEXT("\n"));
}

// --- Build settings -------------------------------------------------------------------------------

FText UMoonToonFixBuildSettingsTool::GetToolName() const
{
	return LOCTEXT("FixBuildName", "Fix Build Settings");
}

FText UMoonToonFixBuildSettingsTool::GetToolDescription() const
{
	return LOCTEXT("FixBuildDesc",
		"Forces the LOD build settings both bakes depend on -- no normal or tangent recompute, high "
		"precision tangents and UVs, no lightmap UV generation -- and reimports any mesh that had to "
		"be corrected.\n\n"
		"Both bake tools do this for you. Run it standalone to check a mesh without baking.");
}

FText UMoonToonFixBuildSettingsTool::GetRunLabel() const
{
	return LOCTEXT("FixBuildRun", "Fix Build Settings");
}

FString UMoonToonFixBuildSettingsTool::Run(const FMoonToonToolContext& Context)
{
	TArray<UObject*> Meshes;
	FString Report;
	if (!ResolveMeshes(Context, Meshes, Report))
	{
		return Report;
	}

	TArray<FString> Lines;
	int32 NumCorrected = 0;
	for (UObject* Mesh : Meshes)
	{
		if (UMoonToonSmoothNormalTool::FixBuildSettingsForMesh(Mesh))
		{
			++NumCorrected;
			Lines.Add(FString::Printf(TEXT("  %s: corrected and reimported."), *Mesh->GetName()));
		}
		else
		{
			Lines.Add(FString::Printf(TEXT("  %s: already correct."), *Mesh->GetName()));
		}
	}

	Lines.Insert(FString::Printf(TEXT("Fix Build Settings -- %d mesh(es), %d corrected."),
		Meshes.Num(), NumCorrected), 0);
	return FString::Join(Lines, TEXT("\n"));
}

#undef LOCTEXT_NAMESPACE
