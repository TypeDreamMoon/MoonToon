// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonSmoothNormalTool.h"

#include "EditorUtilityLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/ListUtilityFunctions.h"
#include "MoonToonEditorBPLibrary.h"
#include "SkeletalMeshEditorSubsystem.h"
#include "StaticMeshEditorSubsystem.h"
#include "UDynamicMesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogMoonToonSmoothNormal, Log, All);

namespace
{
	/** The build settings the bake requires. Recomputing normals or tangents on import would throw
	 *  away exactly the data this tool writes, and low-precision tangents/UVs would quantise it. */
	constexpr bool bRequiredRecomputeNormals = false;
	constexpr bool bRequiredRecomputeTangents = false;
	constexpr bool bRequiredHighPrecisionTangentBasis = true;
	constexpr bool bRequiredFullPrecisionUVs = true;
	constexpr bool bRequiredGenerateLightmapUVs = false;

	FVector3f SafeAxis(const FVector3f& In, const FVector3f& Fallback, const UObject* Mesh, const TCHAR* AxisName)
	{
		if (In.ContainsNaN() || In.IsNearlyZero())
		{
			// A null Mesh means the caller counts degenerate bases itself and reports them in one line
			// (the normal preview decodes every wedge, so one log error per bad basis is thousands).
			if (Mesh)
			{
				UE_LOG(LogMoonToonSmoothNormal, Error, TEXT("[MoonToon] %s has Invalid %s"),
					*Mesh->GetName(), AxisName);
			}
			return Fallback;
		}
		return In;
	}
}

UMoonToonSmoothNormalTool::UMoonToonSmoothNormalTool()
{
	SupportedClasses.Add(UStaticMesh::StaticClass());
	SupportedClasses.Add(USkeletalMesh::StaticClass());
}

void UMoonToonSmoothNormalTool::ForEachSelectedMeshLOD(TFunctionRef<void(UObject*, int32)> Body)
{
	for (UObject* Asset : UEditorUtilityLibrary::GetSelectedAssets())
	{
		int32 NumLODs = 0;
		if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
		{
			NumLODs = StaticMesh->GetNumLODs();
		}
		else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
		{
			if (USkeletalMeshEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<USkeletalMeshEditorSubsystem>())
			{
				NumLODs = Subsystem->GetLODCount(SkeletalMesh);
			}
		}
		else
		{
			continue;
		}

		for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
		{
			Body(Asset, LODIndex);
		}
	}
}

bool UMoonToonSmoothNormalTool::FixBuildSettingsForLOD(UObject* Mesh, int32 LODIndex)
{
	if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh))
	{
		UStaticMeshEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>();
		if (!Subsystem)
		{
			return false;
		}

		FMeshBuildSettings Settings;
		Subsystem->GetLodBuildSettings(StaticMesh, LODIndex, Settings);
		const bool bWasBad =
			Settings.bRecomputeNormals != bRequiredRecomputeNormals
			|| Settings.bRecomputeTangents != bRequiredRecomputeTangents
			|| Settings.bUseHighPrecisionTangentBasis != bRequiredHighPrecisionTangentBasis
			|| Settings.bUseFullPrecisionUVs != bRequiredFullPrecisionUVs
			|| Settings.bGenerateLightmapUVs != bRequiredGenerateLightmapUVs;

		Settings.bRecomputeNormals = bRequiredRecomputeNormals;
		Settings.bRecomputeTangents = bRequiredRecomputeTangents;
		Settings.bUseHighPrecisionTangentBasis = bRequiredHighPrecisionTangentBasis;
		Settings.bUseFullPrecisionUVs = bRequiredFullPrecisionUVs;
		Settings.bGenerateLightmapUVs = bRequiredGenerateLightmapUVs;
		Subsystem->SetLodBuildSettings(StaticMesh, LODIndex, Settings);
		return bWasBad;
	}

	if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
	{
		// USkeletalMeshEditorSubsystem exposes these as statics, unlike the static-mesh side.
		FSkeletalMeshBuildSettings Settings;
		USkeletalMeshEditorSubsystem::GetLodBuildSettings(SkeletalMesh, LODIndex, Settings);
		// Skeletal meshes have no lightmap UV generation, so only the four shared flags are checked.
		const bool bWasBad =
			Settings.bRecomputeNormals != bRequiredRecomputeNormals
			|| Settings.bRecomputeTangents != bRequiredRecomputeTangents
			|| Settings.bUseHighPrecisionTangentBasis != bRequiredHighPrecisionTangentBasis
			|| Settings.bUseFullPrecisionUVs != bRequiredFullPrecisionUVs;

		Settings.bRecomputeNormals = bRequiredRecomputeNormals;
		Settings.bRecomputeTangents = bRequiredRecomputeTangents;
		Settings.bUseHighPrecisionTangentBasis = bRequiredHighPrecisionTangentBasis;
		Settings.bUseFullPrecisionUVs = bRequiredFullPrecisionUVs;
		USkeletalMeshEditorSubsystem::SetLodBuildSettings(SkeletalMesh, LODIndex, Settings);
		return bWasBad;
	}

	return false;
}

void UMoonToonSmoothNormalTool::FixBuildSettings()
{
	// One reimport per mesh, not per LOD: reimporting rebuilds every LOD anyway.
	TSet<UObject*> MeshesNeedingReimport;
	ForEachSelectedMeshLOD([&MeshesNeedingReimport](UObject* Mesh, int32 LODIndex)
	{
		if (FixBuildSettingsForLOD(Mesh, LODIndex))
		{
			MeshesNeedingReimport.Add(Mesh);
		}
	});

	for (UObject* Mesh : MeshesNeedingReimport)
	{
		UE_LOG(LogMoonToonSmoothNormal, Warning,
			TEXT("[MoonToon] %s had bad build settings; corrected and reimporting."), *Mesh->GetName());
		UMoonToonEditorBPLibrary::MoonReimportObjectAsset(Mesh);
	}
}

bool UMoonToonSmoothNormalTool::HasBadTangentOrBinormal(const TArray<FVector3f>& Tangents, const TArray<FVector3f>& Binormals)
{
	if (Tangents.Num() == 0 || Binormals.Num() == 0 || Tangents.Num() != Binormals.Num())
	{
		return true;
	}

	for (int32 Index = 0; Index < Tangents.Num(); ++Index)
	{
		if (Tangents[Index].IsNearlyZero() || Binormals[Index].IsNearlyZero())
		{
			return true;
		}
	}
	return false;
}

void UMoonToonSmoothNormalTool::RecomputeMissingTangentAndBinormal(
	const TArray<FVector3f>& Vertices,
	const TArray<int32>& Indices,
	const TArray<FVector2f>& UVs,
	const TArray<FVector3f>& Normals,
	TArray<FVector3f>& Tangents,
	TArray<FVector3f>& Binormals)
{
	if (!HasBadTangentOrBinormal(Tangents, Binormals))
	{
		return;
	}

	TArray<FVector3f> TangentX;
	TArray<FVector3f> TangentY;
	UMoonToonEditorBPLibrary::MoonRecomputeMikkTSpaceTangentBinormal(
		Vertices, Indices, UVs, Normals, /*bIgnoreDegenerateTriangles=*/true, TangentX, TangentY);

	Tangents = MoveTemp(TangentX);
	Binormals = MoveTemp(TangentY);
}

FMatrix UMoonToonSmoothNormalTool::BuildTangentToLocal(
	int32 WedgeIndex,
	const UObject* Mesh,
	const TArray<FVector3f>& Normals,
	const TArray<FVector3f>& Tangents,
	const TArray<FVector3f>& Binormals)
{
	const FVector3f RawTangent = Tangents.IsValidIndex(WedgeIndex) ? Tangents[WedgeIndex] : FVector3f::ZeroVector;
	const FVector3f RawBinormal = Binormals.IsValidIndex(WedgeIndex) ? Binormals[WedgeIndex] : FVector3f::ZeroVector;
	const FVector3f RawNormal = Normals.IsValidIndex(WedgeIndex) ? Normals[WedgeIndex] : FVector3f::ZeroVector;

	const FVector3f Tangent = SafeAxis(RawTangent, FVector3f(1.0f, 0.0f, 0.0f), Mesh, TEXT("Tangent"));
	const FVector3f Binormal = SafeAxis(RawBinormal, FVector3f(0.0f, 1.0f, 0.0f), Mesh, TEXT("Binormal"));
	const FVector3f Normal = SafeAxis(RawNormal, FVector3f(0.0f, 0.0f, 1.0f), Mesh, TEXT("Normal"));

	// Rows are the tangent basis; W row is the identity translation. InverseTransformVector against
	// this matrix takes a local-space vector into tangent space.
	return FMatrix(
		FPlane(Tangent.X, Tangent.Y, Tangent.Z, 0.0),
		FPlane(Binormal.X, Binormal.Y, Binormal.Z, 0.0),
		FPlane(Normal.X, Normal.Y, Normal.Z, 0.0),
		FPlane(0.0, 0.0, 0.0, 1.0));
}

UDynamicMesh* UMoonToonSmoothNormalTool::ToDynamicMesh(UObject* Mesh, int32 LODIndex)
{
	FGeometryScriptCopyMeshFromAssetOptions AssetOptions;
	AssetOptions.bApplyBuildSettings = true;
	AssetOptions.bRequestTangents = true;
	AssetOptions.bIgnoreRemoveDegenerates = true;
	AssetOptions.bUseBuildScale = true;

	FGeometryScriptMeshReadLOD RequestedLOD;
	RequestedLOD.LODType = EGeometryScriptLODType::MaxAvailable;
	RequestedLOD.LODIndex = LODIndex;

	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

	if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh))
	{
		UDynamicMesh* Target = NewObject<UDynamicMesh>();
		// "with Section Materials" is the display name of this overload; materials are irrelevant here.
		return UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
			StaticMesh, Target, AssetOptions, RequestedLOD, Outcome);
	}

	if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
	{
		UDynamicMesh* Target = NewObject<UDynamicMesh>();
		return UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(
			SkeletalMesh, Target, AssetOptions, RequestedLOD, Outcome);
	}

	return nullptr;
}

bool UMoonToonSmoothNormalTool::FixBuildSettingsForMesh(UObject* Mesh)
{
	bool bChanged = false;
	const int32 NumLODs = MoonToonMesh::GetNumLODs(Mesh);
	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		bChanged |= FixBuildSettingsForLOD(Mesh, LODIndex);
	}
	if (bChanged)
	{
		UE_LOG(LogMoonToonSmoothNormal, Warning,
			TEXT("[MoonToon] %s had bad build settings; corrected and reimporting."), *Mesh->GetName());
		UMoonToonEditorBPLibrary::MoonReimportObjectAsset(Mesh);
	}
	return bChanged;
}

void UMoonToonSmoothNormalTool::BakeSmoothedNormalAndCurvature()
{
	FixBuildSettings();

	ForEachSelectedMeshLOD([](UObject* Mesh, int32 LODIndex)
	{
		BakeSmoothedNormalForLOD(Mesh, LODIndex, FMoonToonWedgeMask());
	});
}

void UMoonToonSmoothNormalTool::BakeSmoothedNormalForLOD(UObject* Mesh, int32 LODIndex, const FMoonToonWedgeMask& Mask)
{
	{
		TArray<FVector3f> Positions, Normals, Tangents, Binormals;
		TArray<int32> VertexIndices;
		TArray<FColor> Colors;
		TArray<FVector2f> UV0s, UV1s, UV2s, UV3s;
		UMoonToonEditorBPLibrary::MoonGetMeshData(Mesh, LODIndex, Positions, VertexIndices, Normals,
			Tangents, Binormals, Colors, UV0s, UV1s, UV2s, UV3s);

		RecomputeMissingTangentAndBinormal(Positions, VertexIndices, UV0s, Normals, Tangents, Binormals);

		// Welding collapses the UV/normal seams that split a vertex, so ComputeSplitNormals produces
		// the seam-free "smoothed" normal the outline pass needs.
		UDynamicMesh* SmoothedDynamicMesh = ToDynamicMesh(Mesh, LODIndex);
		if (!SmoothedDynamicMesh)
		{
			UE_LOG(LogMoonToonSmoothNormal, Error, TEXT("[MoonToon] %s LOD%d: could not build a dynamic mesh."),
				*Mesh->GetName(), LODIndex);
			return;
		}

		FGeometryScriptWeldEdgesOptions WeldOptions;
		SmoothedDynamicMesh = UGeometryScriptLibrary_MeshRepairFunctions::WeldMeshEdges(SmoothedDynamicMesh, WeldOptions);

		FGeometryScriptSplitNormalsOptions SplitOptions;
		FGeometryScriptCalculateNormalsOptions CalculateOptions;
		SmoothedDynamicMesh = UGeometryScriptLibrary_MeshNormalsFunctions::ComputeSplitNormals(
			SmoothedDynamicMesh, SplitOptions, CalculateOptions);

		FGeometryScriptVectorList SmoothedNormalList;
		bool bIsValidNormalSet = false;
		bool bHasVertexIDGaps = false;
		UGeometryScriptLibrary_MeshNormalsFunctions::GetMeshPerVertexNormals(
			SmoothedDynamicMesh, SmoothedNormalList, bIsValidNormalSet, bHasVertexIDGaps,
			/*bAverageSplitVertexValues=*/true);

		// The welded mesh has its own vertex numbering, so map each original position back onto it.
		TArray<FGeometryScriptTrianglePoint> NearestResult;
		UMoonToonEditorBPLibrary::MoonFindNearestPointsOnDynamicMesh(SmoothedDynamicMesh, Positions, NearestResult);

		TArray<float> Curvatures;
		UMoonToonEditorBPLibrary::MoonGetMeshNormalDiffCurvatures(Mesh, LODIndex, Curvatures);

		// The channels the bake owns have to exist before anything is written, or the loop below
		// runs to completion and stores nothing -- which then reads back as a mesh that was baked.
		// That silence is what let SK_星穹铁道—昔涟5 ship with 5% of its wedges encoding a zero
		// vector; say it out loud instead.
		const int32 NumWedges = VertexIndices.Num();
		if (UV2s.Num() < NumWedges || UV3s.Num() < NumWedges)
		{
			UE_LOG(LogMoonToonSmoothNormal, Error,
				TEXT("[MoonToon] %s LOD%d: the smoothed-normal bake needs UV2 and UV3, and this mesh has "
					 "%d/%d entries in them for %d wedges. Nothing was written. Re-import with at least 4 "
					 "UV channels first."),
				*Mesh->GetName(), LODIndex, UV2s.Num(), UV3s.Num(), NumWedges);
			return;
		}

		// Everything the shader can do with a degenerate encode is guess, so never write one: the
		// honest fallback for "no smoothing information here" is the wedge's own normal, which in
		// tangent space is exactly (0,0,1). Counted so the run can report how much of the mesh
		// ended up on that path rather than leaving it to a later visual surprise.
		int32 NumFallbackWedges = 0;
		const FVector TangentSpaceUp(0.0, 0.0, 1.0);
		// Below this the encoded vector is too short to survive UV quantisation and normalize back
		// to a direction, so it is treated as no information rather than as a very flat normal.
		constexpr float MinEncodedCurvature = 0.05f;

		for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
		{
			if (!Mask.Contains(WedgeIndex))
			{
				continue;
			}

			const int32 VertexIndex = VertexIndices[WedgeIndex];
			if (!NearestResult.IsValidIndex(VertexIndex) || !Positions.IsValidIndex(VertexIndex))
			{
				continue;
			}

			// Snap to whichever corner of the hit triangle is closest to the original position --
			// per-vertex normals only exist at vertices, not at arbitrary surface points.
			bool bIsValidTriangle = false;
			FIntVector TriVerts = UGeometryScriptLibrary_MeshQueryFunctions::GetTriangleIndices(
				SmoothedDynamicMesh, NearestResult[VertexIndex].TriangleID, bIsValidTriangle);

			FVector TangentSpaceNormal = TangentSpaceUp;
			float Curvature = 1.0f;
			bool bHasSmoothedNormal = false;

			if (bIsValidTriangle)
			{
				const FVector QueryPosition(Positions[VertexIndex]);
				bool bIsValidVertex = false;
				const FVector P0 = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexPosition(SmoothedDynamicMesh, TriVerts.X, bIsValidVertex);
				const FVector P1 = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexPosition(SmoothedDynamicMesh, TriVerts.Y, bIsValidVertex);
				const FVector P2 = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexPosition(SmoothedDynamicMesh, TriVerts.Z, bIsValidVertex);

				const double D0 = FVector::Distance(P0, QueryPosition);
				const double D1 = FVector::Distance(P1, QueryPosition);
				const double D2 = FVector::Distance(P2, QueryPosition);

				int32 NearestVertexID = (D0 < D1) ? TriVerts.X : TriVerts.Y;
				NearestVertexID = (FMath::Min(D0, D1) < D2) ? NearestVertexID : TriVerts.Z;

				bool bIsValidIndex = false;
				const FVector SmoothedNormal = UGeometryScriptLibrary_ListUtilityFunctions::GetVectorListItem(
					SmoothedNormalList, NearestVertexID, bIsValidIndex);

				// bIsValidIndex used to be ignored, and a miss returns the zero vector -- which
				// GetSafeNormal() then turns into another zero rather than into a complaint.
				if (bIsValidIndex && !SmoothedNormal.IsNearlyZero())
				{
					const FMatrix TangentToLocal = BuildTangentToLocal(WedgeIndex, Mesh, Normals, Tangents, Binormals);
					const FVector Candidate = TangentToLocal.InverseTransformVector(SmoothedNormal).GetSafeNormal();
					if (!Candidate.IsNearlyZero())
					{
						TangentSpaceNormal = Candidate;
						bHasSmoothedNormal = true;
					}
				}
			}

			// Curvature scales the stored normal, and the whole thing is remapped from [-1,1] to
			// [0,1] so it survives UV channels that may be stored unsigned. A flat patch legitimately
			// has curvature 0, but 0 times a unit normal is the zero vector again, so the floor
			// applies to real data too -- the direction is still correct there, it is just shorter
			// than the encoding can carry.
			if (bHasSmoothedNormal)
			{
				Curvature = Curvatures.IsValidIndex(WedgeIndex) ? Curvatures[WedgeIndex] : 1.0f;
			}
			if (!bHasSmoothedNormal || Curvature < MinEncodedCurvature)
			{
				Curvature = FMath::Max(Curvature, MinEncodedCurvature);
				++NumFallbackWedges;
			}

			const FVector Encoded = TangentSpaceNormal * Curvature * 0.5 + FVector(0.5);

			// UV2.x carries unrelated data and must be preserved; only .y is ours.
			UV2s[WedgeIndex] = FVector2f(UV2s[WedgeIndex].X, static_cast<float>(Encoded.X));
			UV3s[WedgeIndex] = FVector2f(static_cast<float>(Encoded.Y), static_cast<float>(Encoded.Z));
		}

		if (NumFallbackWedges > 0)
		{
			UE_LOG(LogMoonToonSmoothNormal, Warning,
				TEXT("[MoonToon] %s LOD%d: %d of %d wedges (%.1f%%) had no usable smoothed normal (flat, "
					 "or the welded-mesh lookup missed) and were baked as the vertex normal instead."),
				*Mesh->GetName(), LODIndex, NumFallbackWedges, NumWedges,
				NumWedges > 0 ? 100.0f * NumFallbackWedges / NumWedges : 0.0f);
		}

		UMoonToonEditorBPLibrary::MoonSetMeshData(Mesh, LODIndex, Positions, VertexIndices, Normals,
			Tangents, Binormals, Colors, UV0s, UV1s, UV2s, UV3s);
	}
}

void UMoonToonSmoothNormalTool::BakeFaceForwardDirection(FVector FaceForwardDirWS)
{
	FixBuildSettings();

	ForEachSelectedMeshLOD([FaceForwardDirWS](UObject* Mesh, int32 LODIndex)
	{
		BakeFaceForwardForLOD(Mesh, LODIndex, FaceForwardDirWS, FMoonToonWedgeMask());
	});
}

void UMoonToonSmoothNormalTool::BakeFaceForwardForLOD(
	UObject* Mesh, int32 LODIndex, FVector FaceForwardDirWS, const FMoonToonWedgeMask& Mask)
{
	{
		TArray<FVector3f> Positions, Normals, Tangents, Binormals;
		TArray<int32> VertexIndices;
		TArray<FColor> Colors;
		TArray<FVector2f> UV0s, UV1s, UV2s, UV3s;
		UMoonToonEditorBPLibrary::MoonGetMeshData(Mesh, LODIndex, Positions, VertexIndices, Normals,
			Tangents, Binormals, Colors, UV0s, UV1s, UV2s, UV3s);

		RecomputeMissingTangentAndBinormal(Positions, VertexIndices, UV0s, Normals, Tangents, Binormals);

		for (int32 WedgeIndex = 0; WedgeIndex < VertexIndices.Num(); ++WedgeIndex)
		{
			if (!Mask.Contains(WedgeIndex))
			{
				continue;
			}

			const FMatrix TangentToLocal = BuildTangentToLocal(WedgeIndex, Mesh, Normals, Tangents, Binormals);
			const FVector TangentSpaceDir = TangentToLocal.InverseTransformVector(FaceForwardDirWS).GetSafeNormal();
			const FVector Encoded = TangentSpaceDir * 0.5 + FVector(0.5);

			// DELIBERATE DEVIATION from the original Blueprint, which wired these Set Array Elem nodes
			// to UV2/UV3 even though its own node comments say "UV1"/"UV2". The material reads the
			// direction back as normalize(float3(UV1.rg, UV2.r)) -- with the Blueprint's channels UV1
			// stays empty, so the direction collapses to a constant (0,0,+-1) and the facial shadow
			// stops tracking head orientation. Writing UV1/UV2 also makes the two bakes stop fighting:
			// face-forward owns UV1.xy + UV2.x, smoothed-normal owns UV2.y + UV3.xy, which is exactly
			// why the smoothed-normal bake goes out of its way to preserve UV2.x.
			if (UV1s.IsValidIndex(WedgeIndex))
			{
				UV1s[WedgeIndex] = FVector2f(static_cast<float>(Encoded.X), static_cast<float>(Encoded.Y));
			}
			if (UV2s.IsValidIndex(WedgeIndex))
			{
				UV2s[WedgeIndex] = FVector2f(static_cast<float>(Encoded.Z), UV2s[WedgeIndex].Y);
			}
		}

		UMoonToonEditorBPLibrary::MoonSetMeshData(Mesh, LODIndex, Positions, VertexIndices, Normals,
			Tangents, Binormals, Colors, UV0s, UV1s, UV2s, UV3s);
	}
}
