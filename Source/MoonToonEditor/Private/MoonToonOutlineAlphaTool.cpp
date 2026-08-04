// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonOutlineAlphaTool.h"

#include "Engine/SkeletalMesh.h"
#include "MoonToonEditorBPLibrary.h"
#include "MoonToonMeshTargets.h"
#include "Rendering/SkeletalMeshLODImporterData.h"

#define LOCTEXT_NAMESPACE "MoonToonOutlineAlphaTool"

namespace
{
	/** Disjoint-set over vertex indices, used to find connected islands. */
	class FUnionFind
	{
	public:
		explicit FUnionFind(int32 Num)
		{
			Parent.SetNum(Num);
			for (int32 Index = 0; Index < Num; ++Index)
			{
				Parent[Index] = Index;
			}
		}

		int32 Find(int32 Index)
		{
			// Path halving: no recursion, and flat enough afterwards that rank tracking is not worth it.
			while (Parent[Index] != Index)
			{
				Parent[Index] = Parent[Parent[Index]];
				Index = Parent[Index];
			}
			return Index;
		}

		void Union(int32 A, int32 B)
		{
			const int32 RootA = Find(A);
			const int32 RootB = Find(B);
			if (RootA != RootB)
			{
				Parent[RootB] = RootA;
			}
		}

	private:
		TArray<int32> Parent;
	};

	/**
	 * Dominant eigenvector of a symmetric 3x3 covariance matrix, by power iteration.
	 *
	 * Enough for this job: hair cards are strongly elongated, so the dominant axis is well separated
	 * and converges in a handful of iterations. A full eigen-decomposition would buy nothing.
	 */
	FVector3f DominantAxis(const FVector3f Covariance[3])
	{
		FVector3f Axis(1.0f, 1.0f, 1.0f);
		Axis.Normalize();

		for (int32 Iteration = 0; Iteration < 32; ++Iteration)
		{
			const FVector3f Next(
				Covariance[0] | Axis,
				Covariance[1] | Axis,
				Covariance[2] | Axis);

			if (Next.IsNearlyZero())
			{
				// Degenerate (a single point, or perfectly isotropic): no meaningful axis exists.
				return FVector3f(0.0f, 0.0f, 1.0f);
			}
			Axis = Next.GetSafeNormal();
		}
		return Axis;
	}

	/** Per-vertex signal in 0..1, plus how many islands contributed. */
	struct FSignalResult
	{
		TArray<float> PerVertex;
		int32 NumIslands = 0;
		int32 NumSkippedIslands = 0;
	};

	/**
	 * Splits the masked geometry into connected islands and measures, per vertex, how far along each
	 * island's principal axis it sits -- 0 at the root end, 1 at the tip end.
	 */
	FSignalResult ComputeIslandAxisSignal(
		const TArray<FVector3f>& Positions,
		const TArray<int32>& VertexIndices,
		const FMoonToonLODFaces& Faces,
		const FMoonToonWedgeMask& Mask,
		int32 MinIslandVertices,
		EMoonToonIslandOrientation Orientation)
	{
		FSignalResult Result;
		Result.PerVertex.Init(0.0f, Positions.Num());

		// Weld by exact position first. Import data routinely stores several points at one location
		// (one per UV or normal seam); without this every card would shatter into many islands.
		FUnionFind Islands(Positions.Num());
		TMap<FVector3f, int32> PositionToFirstVertex;
		PositionToFirstVertex.Reserve(Positions.Num());
		for (int32 VertexIndex = 0; VertexIndex < Positions.Num(); ++VertexIndex)
		{
			if (const int32* Existing = PositionToFirstVertex.Find(Positions[VertexIndex]))
			{
				Islands.Union(*Existing, VertexIndex);
			}
			else
			{
				PositionToFirstVertex.Add(Positions[VertexIndex], VertexIndex);
			}
		}

		// Then connect through faces, but only faces the section filter includes -- otherwise a hair
		// card touching the scalp would merge into the head and lose its own axis.
		for (const FIntVector& Face : Faces.Wedges)
		{
			const bool bIncluded =
				Mask.Contains(Face.X) && Mask.Contains(Face.Y) && Mask.Contains(Face.Z);
			if (!bIncluded)
			{
				continue;
			}
			if (!VertexIndices.IsValidIndex(Face.X) || !VertexIndices.IsValidIndex(Face.Y)
				|| !VertexIndices.IsValidIndex(Face.Z))
			{
				continue;
			}
			Islands.Union(VertexIndices[Face.X], VertexIndices[Face.Y]);
			Islands.Union(VertexIndices[Face.X], VertexIndices[Face.Z]);
		}

		// Gather members, restricted to vertices that an included wedge actually references.
		TSet<int32> UsedVertices;
		for (int32 WedgeIndex = 0; WedgeIndex < VertexIndices.Num(); ++WedgeIndex)
		{
			if (Mask.Contains(WedgeIndex))
			{
				UsedVertices.Add(VertexIndices[WedgeIndex]);
			}
		}

		TMap<int32, TArray<int32>> IslandMembers;
		for (int32 VertexIndex : UsedVertices)
		{
			IslandMembers.FindOrAdd(Islands.Find(VertexIndex)).Add(VertexIndex);
		}

		for (const TPair<int32, TArray<int32>>& Island : IslandMembers)
		{
			const TArray<int32>& Members = Island.Value;
			if (Members.Num() < MinIslandVertices)
			{
				++Result.NumSkippedIslands;
				// Left at 0, i.e. treated as root: a stray fragment keeps full width rather than
				// being randomly tapered.
				continue;
			}
			++Result.NumIslands;

			FVector3f Centroid = FVector3f::ZeroVector;
			for (int32 VertexIndex : Members)
			{
				Centroid += Positions[VertexIndex];
			}
			Centroid /= static_cast<float>(Members.Num());

			FVector3f Covariance[3] = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector };
			for (int32 VertexIndex : Members)
			{
				const FVector3f D = Positions[VertexIndex] - Centroid;
				Covariance[0] += FVector3f(D.X * D.X, D.X * D.Y, D.X * D.Z);
				Covariance[1] += FVector3f(D.Y * D.X, D.Y * D.Y, D.Y * D.Z);
				Covariance[2] += FVector3f(D.Z * D.X, D.Z * D.Y, D.Z * D.Z);
			}

			const FVector3f Axis = DominantAxis(Covariance);

			float MinProjection = TNumericLimits<float>::Max();
			float MaxProjection = TNumericLimits<float>::Lowest();
			for (int32 VertexIndex : Members)
			{
				const float Projection = (Positions[VertexIndex] - Centroid) | Axis;
				MinProjection = FMath::Min(MinProjection, Projection);
				MaxProjection = FMath::Max(MaxProjection, Projection);
			}

			const float Span = MaxProjection - MinProjection;
			if (Span < UE_KINDA_SMALL_NUMBER)
			{
				continue;
			}

			// Which end is the tip: compare how far the geometry spreads off-axis near each end.
			// A hair card is wide at the root and narrow at the tip, so the thinner end is the tip.
			bool bPositiveEndIsTip = true;
			if (Orientation == EMoonToonIslandOrientation::Auto)
			{
				double NearRadius = 0.0, FarRadius = 0.0;
				int32 NearCount = 0, FarCount = 0;
				for (int32 VertexIndex : Members)
				{
					const FVector3f D = Positions[VertexIndex] - Centroid;
					const float Projection = D | Axis;
					const float Normalized = (Projection - MinProjection) / Span;
					const float Radius = (D - Axis * Projection).Size();

					if (Normalized < 0.2f) { NearRadius += Radius; ++NearCount; }
					else if (Normalized > 0.8f) { FarRadius += Radius; ++FarCount; }
				}
				if (NearCount > 0 && FarCount > 0)
				{
					const double NearMean = NearRadius / NearCount;
					const double FarMean = FarRadius / FarCount;
					bPositiveEndIsTip = FarMean <= NearMean;
				}
			}
			else
			{
				bPositiveEndIsTip = (Orientation == EMoonToonIslandOrientation::PositiveEnd);
			}

			for (int32 VertexIndex : Members)
			{
				const float Projection = (Positions[VertexIndex] - Centroid) | Axis;
				const float Normalized = (Projection - MinProjection) / Span;
				Result.PerVertex[VertexIndex] = bPositiveEndIsTip ? Normalized : (1.0f - Normalized);
			}
		}

		return Result;
	}

	float BlendAlpha(EMoonToonAlphaBlend Mode, float Existing, float Incoming, float Strength)
	{
		switch (Mode)
		{
		case EMoonToonAlphaBlend::Replace:  return Incoming;
		case EMoonToonAlphaBlend::Multiply: return Existing * Incoming;
		case EMoonToonAlphaBlend::Min:      return FMath::Min(Existing, Incoming);
		case EMoonToonAlphaBlend::Max:      return FMath::Max(Existing, Incoming);
		case EMoonToonAlphaBlend::Lerp:     return FMath::Lerp(Existing, Incoming, Strength);
		default:                            return Incoming;
		}
	}
}

UMoonToonOutlineAlphaTool::UMoonToonOutlineAlphaTool()
{
	// A sensible starting taper: full width for the first third, then falling away to a thin tip.
	// Authored here rather than left empty so the tool does something reasonable before it is touched.
	if (FRichCurve* Curve = WidthCurve.GetRichCurve())
	{
		Curve->Reset();
		const FKeyHandle Handles[] = {
			Curve->AddKey(0.0f, 1.0f),
			Curve->AddKey(0.35f, 0.9f),
			Curve->AddKey(1.0f, 0.15f),
		};
		for (const FKeyHandle& Handle : Handles)
		{
			Curve->SetKeyInterpMode(Handle, RCIM_Cubic);
			Curve->SetKeyTangentMode(Handle, RCTM_Auto);
		}
	}
}

FText UMoonToonOutlineAlphaTool::GetToolName() const
{
	return LOCTEXT("AlphaName", "Outline Width Curve");
}

FText UMoonToonOutlineAlphaTool::GetToolDescription() const
{
	return LOCTEXT("AlphaDesc",
		"Writes vertex colour alpha from a curve, so outline width can be authored as a root-to-tip "
		"taper instead of painted by hand.\n\n"
		"The material reads alpha through 'Use Vertex Color A as Outline Width'. Only alpha is "
		"written -- RGB is preserved, because the base material reads it for feature ID, rim width "
		"and opacity.\n\n"
		"Use Preview Only to check a curve before committing, and Multiply to re-run without "
		"discarding hand-painted work.");
}

FText UMoonToonOutlineAlphaTool::GetRunLabel() const
{
	return bPreviewOnly
		? LOCTEXT("AlphaPreview", "Preview")
		: LOCTEXT("AlphaRun", "Write Vertex Alpha");
}

FString UMoonToonOutlineAlphaTool::Run(const FMoonToonToolContext& Context)
{
	TArray<UObject*> Meshes;
	FString Report;
	if (!ResolveMeshes(Context, Meshes, Report))
	{
		return Report;
	}

	const FRichCurve* Curve = WidthCurve.GetRichCurveConst();
	if (!Curve || Curve->GetNumKeys() == 0)
	{
		return TEXT("Width Curve has no keys. Add at least one key before running.");
	}

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Outline Width Curve%s"),
		bPreviewOnly ? TEXT("  [PREVIEW -- nothing written]") : TEXT("")));
	Lines.Add(FString::Printf(TEXT("  Sections: %s"),
		Context.SectionMaterialIndices.Num() == 0 ? TEXT("all") : TEXT("filtered")));

	for (UObject* Mesh : Meshes)
	{
		const int32 NumLODs = MoonToonMesh::GetNumLODs(Mesh);
		const int32 FirstLOD = bAllLODs ? 0 : LODIndex;
		const int32 LastLOD = bAllLODs ? NumLODs - 1 : LODIndex;

		if (FirstLOD < 0 || FirstLOD >= NumLODs)
		{
			Lines.Add(FString::Printf(TEXT("  %s: LOD %d does not exist -- skipped."), *Mesh->GetName(), LODIndex));
			continue;
		}

		// Skeletal meshes take the fast path: import data persisted directly, render buffers patched
		// live, no rebuild. Static meshes keep the full path -- their write is a fraction of the cost
		// and there is no equivalent of the render-vertex-to-import-point map to patch through.
		USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh);
		const bool bTryFastPath = SkeletalMesh != nullptr && !bFullRebuildOnWrite && !bPreviewOnly;

		// One rebuild for the whole LOD loop instead of one per LOD. Only the full path wants this:
		// the scope's destructor runs PostEditChange, which is exactly what the fast path avoids.
		TUniquePtr<FMoonToonScopedMeshEdit> BatchedEdit;
		if (!bPreviewOnly && !bTryFastPath && LastLOD > FirstLOD)
		{
			BatchedEdit = MakeUnique<FMoonToonScopedMeshEdit>(Mesh);
		}

		for (int32 LOD = FirstLOD; LOD <= LastLOD; ++LOD)
		{
			TArray<FVector3f> Positions, Normals, Tangents, Binormals;
			TArray<int32> VertexIndices;
			TArray<FColor> Colors;
			TArray<FVector2f> UV0s, UV1s, UV2s, UV3s;
			UMoonToonEditorBPLibrary::MoonGetMeshData(Mesh, LOD, Positions, VertexIndices, Normals,
				Tangents, Binormals, Colors, UV0s, UV1s, UV2s, UV3s);

			const int32 NumWedges = VertexIndices.Num();
			if (NumWedges == 0)
			{
				Lines.Add(FString::Printf(TEXT("  %s LOD%d: no import data -- skipped."), *Mesh->GetName(), LOD));
				continue;
			}

			// One face read per LOD: the mask and the island connectivity both come off it, and on
			// the skeletal side every read decompresses the whole import-data blob.
			FMoonToonLODFaces Faces;
			MoonToonMesh::GetFaces(Mesh, LOD, Faces);

			const FMoonToonWedgeMask Mask =
				MoonToonMesh::BuildWedgeMask(Faces, Context.SectionMaterialIndices, NumWedges);

			// --- Signal ---------------------------------------------------------------------------
			TArray<float> WedgeSignal;
			WedgeSignal.Init(0.0f, NumWedges);
			FString SignalNote;

			if (Signal == EMoonToonAlphaSignal::IslandAxis)
			{
				const FSignalResult IslandSignal = ComputeIslandAxisSignal(
					Positions, VertexIndices, Faces, Mask, MinIslandVertices, IslandOrientation);

				for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
				{
					const int32 VertexIndex = VertexIndices[WedgeIndex];
					if (IslandSignal.PerVertex.IsValidIndex(VertexIndex))
					{
						WedgeSignal[WedgeIndex] = IslandSignal.PerVertex[VertexIndex];
					}
				}
				SignalNote = FString::Printf(TEXT("%d island(s), %d skipped as too small"),
					IslandSignal.NumIslands, IslandSignal.NumSkippedIslands);
			}
			else if (Signal == EMoonToonAlphaSignal::UVAxis)
			{
				const TArray<FVector2f>* Channels[4] = { &UV0s, &UV1s, &UV2s, &UV3s };
				const TArray<FVector2f>& UVs = *Channels[FMath::Clamp(UVChannel, 0, 3)];

				float MinValue = TNumericLimits<float>::Max();
				float MaxValue = TNumericLimits<float>::Lowest();
				for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
				{
					if (!Mask.Contains(WedgeIndex) || !UVs.IsValidIndex(WedgeIndex))
					{
						continue;
					}
					const float Value = bUseUAxis ? UVs[WedgeIndex].X : UVs[WedgeIndex].Y;
					WedgeSignal[WedgeIndex] = Value;
					MinValue = FMath::Min(MinValue, Value);
					MaxValue = FMath::Max(MaxValue, Value);
				}

				if (bNormalizeUVRange && MaxValue > MinValue)
				{
					const float Span = MaxValue - MinValue;
					for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
					{
						WedgeSignal[WedgeIndex] = (WedgeSignal[WedgeIndex] - MinValue) / Span;
					}
				}
				SignalNote = FString::Printf(TEXT("UV%d.%s range [%.3f, %.3f]%s"),
					UVChannel, bUseUAxis ? TEXT("u") : TEXT("v"), MinValue, MaxValue,
					bNormalizeUVRange ? TEXT(", normalized") : TEXT(""));
			}
			else // Curvature
			{
				TArray<float> Curvatures;
				UMoonToonEditorBPLibrary::MoonGetMeshNormalDiffCurvatures(Mesh, LOD, Curvatures);
				for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
				{
					if (Curvatures.IsValidIndex(WedgeIndex))
					{
						WedgeSignal[WedgeIndex] = Curvatures[WedgeIndex];
					}
				}
				SignalNote = TEXT("normal-difference curvature");
			}

			// --- Curve + blend --------------------------------------------------------------------
			int32 NumWritten = 0;
			float MinAlpha = TNumericLimits<float>::Max();
			float MaxAlpha = TNumericLimits<float>::Lowest();
			double SumAlpha = 0.0;

			for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
			{
				if (!Mask.Contains(WedgeIndex) || !Colors.IsValidIndex(WedgeIndex))
				{
					continue;
				}

				float SignalValue = WedgeSignal[WedgeIndex];
				if (bInvertSignal)
				{
					SignalValue = 1.0f - SignalValue;
				}

				const float CurveResult = Curve->Eval(SignalValue * InputScale + InputOffset);
				const float Clamped = FMath::Clamp(CurveResult, OutputFloor, OutputCeiling);

				// Clamp again after blending, not only after the curve. Multiply and Min both read the
				// existing alpha, so a mesh that already has zeros there would otherwise write zeros
				// straight back out and reintroduce the collapsed-hull z-fighting the floor exists to
				// prevent. Set the floor to 0 to genuinely allow holes.
				const float Existing = Colors[WedgeIndex].A / 255.0f;
				const float Blended = BlendAlpha(BlendMode, Existing, Clamped, BlendStrength);
				const float Final = FMath::Clamp(Blended, OutputFloor, OutputCeiling);

				// Alpha only. RGB stays untouched -- the base material reads it for other features.
				if (!bPreviewOnly)
				{
					Colors[WedgeIndex].A = static_cast<uint8>(FMath::RoundToInt(Final * 255.0f));
				}

				++NumWritten;
				MinAlpha = FMath::Min(MinAlpha, Final);
				MaxAlpha = FMath::Max(MaxAlpha, Final);
				SumAlpha += Final;
			}

			if (NumWritten == 0)
			{
				Lines.Add(FString::Printf(TEXT("  %s LOD%d: section filter matched no wedges."),
					*Mesh->GetName(), LOD));
				continue;
			}

			Lines.Add(FString::Printf(TEXT("  %s LOD%d: %d/%d wedges  |  %s"),
				*Mesh->GetName(), LOD, NumWritten, NumWedges, *SignalNote));
			Lines.Add(FString::Printf(TEXT("      alpha min %.3f  max %.3f  mean %.3f"),
				MinAlpha, MaxAlpha, SumAlpha / NumWritten));

			if (!bPreviewOnly)
			{
				bool bFastPathDone = false;
				if (bTryFastPath)
				{
					const double SaveStart = FPlatformTime::Seconds();

					// Persist alpha into the LOD import data -- the source of truth every future
					// rebuild reads. Loaded fresh here and only alpha touched, so everything else in
					// the blob round-trips untouched.
					PRAGMA_DISABLE_DEPRECATION_WARNINGS
					FSkeletalMeshImportData ImportData;
					SkeletalMesh->LoadLODImportedData(LOD, ImportData);
					bool bImportDataFits = ImportData.Wedges.Num() == NumWedges;
					if (bImportDataFits)
					{
						for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
						{
							if (Mask.Contains(WedgeIndex) && Colors.IsValidIndex(WedgeIndex))
							{
								ImportData.Wedges[WedgeIndex].Color.A = Colors[WedgeIndex].A;
							}
						}
						ImportData.bHasVertexColors = true;
						SkeletalMesh->SaveLODImportedData(LOD, ImportData);
					}
					PRAGMA_ENABLE_DEPRECATION_WARNINGS

					const double PatchStart = FPlatformTime::Seconds();
					if (bImportDataFits)
					{
						// Per-point alpha for the live patch: the render map addresses points, not
						// wedges. Wedges of one point rarely disagree here (every signal is
						// effectively per-point), and if they do, the average is displayed while the
						// import data keeps the exact per-wedge values.
						TArray<float> PointAlpha;
						PointAlpha.Init(-1.0f, Positions.Num());
						TArray<uint16> PointCounts;
						PointCounts.Init(0, Positions.Num());
						TArray<float> PointSums;
						PointSums.Init(0.0f, Positions.Num());
						for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
						{
							if (!Mask.Contains(WedgeIndex) || !Colors.IsValidIndex(WedgeIndex))
							{
								continue;
							}
							const int32 PointIndex = VertexIndices[WedgeIndex];
							if (PointSums.IsValidIndex(PointIndex))
							{
								PointSums[PointIndex] += Colors[WedgeIndex].A / 255.0f;
								PointCounts[PointIndex]++;
							}
						}
						for (int32 PointIndex = 0; PointIndex < PointAlpha.Num(); ++PointIndex)
						{
							if (PointCounts[PointIndex] > 0)
							{
								PointAlpha[PointIndex] = PointSums[PointIndex] / PointCounts[PointIndex];
							}
						}

						bFastPathDone = MoonToonMesh::PatchVertexAlphaLive(SkeletalMesh, LOD, PointAlpha);
					}

					const double PatchEnd = FPlatformTime::Seconds();
					if (bFastPathDone)
					{
						Lines.Add(FString::Printf(
							TEXT("      fast path: import data %.2fs, live patch %.2fs, no rebuild"),
							PatchStart - SaveStart, PatchEnd - PatchStart));
					}
					else if (bImportDataFits)
					{
						// Import data is already saved; a full PostEditChange turns it into render
						// data the slow way.
						Lines.Add(TEXT("      live patch unavailable (no render map) -- full rebuild instead"));
						SkeletalMesh->PostEditChange();
						SkeletalMesh->MarkPackageDirty();
						bFastPathDone = true;
					}
					// else: wedge counts disagreed; fall through to the library path below.
				}

				if (!bFastPathDone)
				{
					UMoonToonEditorBPLibrary::MoonSetMeshData(Mesh, LOD, Positions, VertexIndices, Normals,
						Tangents, Binormals, Colors, UV0s, UV1s, UV2s, UV3s);
				}
			}
		}
	}

	if (bPreviewOnly)
	{
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("Preview only -- the mesh was not modified. Turn off Preview Only to write."));
	}
	else
	{
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("Enable 'Use Vertex Color A as Outline Width' on the outline material to see this."));
	}
	return FString::Join(Lines, TEXT("\n"));
}

#undef LOCTEXT_NAMESPACE
