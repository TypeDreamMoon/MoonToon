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

	/** Per-wedge signal in 0..1, plus enough about the segmentation to explain a disappointing run. */
	struct FSignalResult
	{
		TArray<float> PerWedge;
		int32 NumIslands = 0;
		int32 NumSkippedIslands = 0;

		/** Islands Auto could not call, oriented by Tip Direction instead. */
		int32 NumDirectionOriented = 0;

		/** Welded-vertex counts of the islands that were kept, for the report. */
		int32 MedianIslandVertices = 0;
		int32 LargestIslandVertices = 0;
	};

	/**
	 * Splits the masked geometry into UV shells and measures, per wedge, how far along each shell's
	 * principal axis it sits -- 0 at the root end, 1 at the tip end.
	 *
	 * Shells, not connected geometry. The connectivity used to be built in point space after welding
	 * every pair of points sharing a position. That weld is necessary on its own terms -- import data
	 * stores one point per UV or normal seam, so without it a single hair card shatters into pieces --
	 * but it cannot tell "two halves of one card" from "two different cards that happen to touch".
	 * Hair cards touch at the scalp, so on SK_ww_yy the entire bangs section welded into ONE island
	 * and the per-island PCA degenerated into a single global axis: the taper then ran across the
	 * whole hairdo instead of root-to-tip along each strand.
	 *
	 * Cards are separate in UV even where they touch in 3D, which makes the shell the right unit.
	 * Walking wedges gets it for free: two wedges only merge when the UV is continuous between them,
	 * which keeps a card whole and still splits it from its neighbour.
	 */
	FSignalResult ComputeIslandAxisSignal(
		const TArray<FVector3f>& Positions,
		const TArray<int32>& VertexIndices,
		const TArray<FVector2f>& UV0s,
		const FMoonToonLODFaces& Faces,
		const FMoonToonWedgeMask& Mask,
		int32 MinIslandVertices,
		EMoonToonIslandOrientation Orientation,
		const FVector3f& TipDirection,
		float AutoTipContrast)
	{
		const int32 NumWedges = VertexIndices.Num();

		FSignalResult Result;
		Result.PerWedge.Init(0.0f, NumWedges);

		FUnionFind Islands(NumWedges);

		// Connect through faces, but only faces the section filter includes -- otherwise a hair card
		// touching the scalp would merge into the head and lose its own axis.
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
			Islands.Union(Face.X, Face.Y);
			Islands.Union(Face.X, Face.Z);
		}

		// Then merge the duplicate wedges that are NOT a seam: same place, same UV. This is the part
		// the old position weld was really there for -- a normal seam duplicates the vertex but leaves
		// the UV alone, so those halves rejoin, while a UV seam keeps the two cards apart.
		//
		// Keyed on the position, not on the import point index. The index is only ever a proxy for
		// "same place", and it is a proxy that some importers do not honour: SK_ww_yy's LOD0 has
		// 268299 wedges over 268299 points, one point per face corner and never shared. No two wedges
		// could collide on it, so nothing merged across a face boundary, every triangle became its own
		// island, all of them fell under Min Island Vertices, and the signal stayed 0 everywhere --
		// which the curve turns into full width on every vertex. The tool ran, reported, saved, and
		// changed nothing.
		//
		// Quantum is relative to the selection's own size so this behaves the same on a 2 m character
		// and on a 2 cm prop. It only has to absorb float round-trip noise -- a seam stores duplicated,
		// normally bit-identical positions -- so it stays far below any real modelling slack.
		FBox3f Bounds(ForceInit);
		for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
		{
			if (Mask.Contains(WedgeIndex) && VertexIndices.IsValidIndex(WedgeIndex)
				&& Positions.IsValidIndex(VertexIndices[WedgeIndex]))
			{
				Bounds += Positions[VertexIndices[WedgeIndex]];
			}
		}
		const FVector3f WeldOrigin = Bounds.IsValid ? Bounds.Min : FVector3f::ZeroVector;
		const float PositionQuantum =
			FMath::Max((Bounds.IsValid ? Bounds.GetSize().GetMax() : 1.0f) * 1e-5f, UE_SMALL_NUMBER);

		constexpr float UVQuantum = 1.0f / 65536.0f;

		// Kept for the island sizing below, which counts welded vertices rather than wedges.
		TArray<FIntVector> WeldedPosition;
		WeldedPosition.Init(FIntVector::ZeroValue, NumWedges);

		TMap<TPair<FIntVector, FIntPoint>, int32> SeamlessKeyToWedge;
		SeamlessKeyToWedge.Reserve(NumWedges);
		for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
		{
			if (!Mask.Contains(WedgeIndex) || !UV0s.IsValidIndex(WedgeIndex)
				|| !VertexIndices.IsValidIndex(WedgeIndex)
				|| !Positions.IsValidIndex(VertexIndices[WedgeIndex]))
			{
				continue;
			}

			const FVector3f Local = (Positions[VertexIndices[WedgeIndex]] - WeldOrigin) / PositionQuantum;
			const FIntVector QuantizedPosition(
				FMath::RoundToInt(Local.X), FMath::RoundToInt(Local.Y), FMath::RoundToInt(Local.Z));
			WeldedPosition[WedgeIndex] = QuantizedPosition;

			const FIntPoint QuantizedUV(
				FMath::RoundToInt(UV0s[WedgeIndex].X / UVQuantum),
				FMath::RoundToInt(UV0s[WedgeIndex].Y / UVQuantum));

			const TPair<FIntVector, FIntPoint> Key(QuantizedPosition, QuantizedUV);
			if (const int32* Existing = SeamlessKeyToWedge.Find(Key))
			{
				Islands.Union(*Existing, WedgeIndex);
			}
			else
			{
				SeamlessKeyToWedge.Add(Key, WedgeIndex);
			}
		}

		TMap<int32, TArray<int32>> IslandMembers;
		for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
		{
			if (Mask.Contains(WedgeIndex))
			{
				IslandMembers.FindOrAdd(Islands.Find(WedgeIndex)).Add(WedgeIndex);
			}
		}

		TArray<int32> KeptIslandSizes;
		KeptIslandSizes.Reserve(IslandMembers.Num());

		for (const TPair<int32, TArray<int32>>& Island : IslandMembers)
		{
			const TArray<int32>& Members = Island.Value;

			// Sized in welded vertices rather than in wedges, so Min Island Vertices means the same
			// thing on every mesh: a wedge count runs roughly three to six times larger and would
			// quietly stop skipping the fragments the setting exists to skip. Counting import point
			// indices instead would inherit whatever the importer decided about sharing them, which
			// is the very thing that made this segmentation collapse in the first place.
			TSet<FIntVector> DistinctPositions;
			DistinctPositions.Reserve(Members.Num());
			for (int32 WedgeIndex : Members)
			{
				DistinctPositions.Add(WeldedPosition[WedgeIndex]);
			}
			if (DistinctPositions.Num() < MinIslandVertices)
			{
				++Result.NumSkippedIslands;
				// Left at 0, i.e. treated as root: a stray fragment keeps full width rather than
				// being randomly tapered.
				continue;
			}
			++Result.NumIslands;
			KeptIslandSizes.Add(DistinctPositions.Num());

			FVector3f Centroid = FVector3f::ZeroVector;
			for (int32 WedgeIndex : Members)
			{
				Centroid += Positions[VertexIndices[WedgeIndex]];
			}
			Centroid /= static_cast<float>(Members.Num());

			FVector3f Covariance[3] = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector };
			for (int32 WedgeIndex : Members)
			{
				const FVector3f D = Positions[VertexIndices[WedgeIndex]] - Centroid;
				Covariance[0] += FVector3f(D.X * D.X, D.X * D.Y, D.X * D.Z);
				Covariance[1] += FVector3f(D.Y * D.X, D.Y * D.Y, D.Y * D.Z);
				Covariance[2] += FVector3f(D.Z * D.X, D.Z * D.Y, D.Z * D.Z);
			}

			FVector3f Axis = DominantAxis(Covariance);

			// Power iteration converges to +v or -v depending on where it started and on the data, so
			// the sign carries no meaning of its own. Pin it -- +Axis always points up -- so "upper
			// end" names the same physical end on every island. Without this the two explicit
			// orientation modes are per-island coin flips wearing the label of a choice.
			if (Axis.Z < 0.0f
				|| (Axis.Z == 0.0f && (Axis.X < 0.0f || (Axis.X == 0.0f && Axis.Y < 0.0f))))
			{
				Axis = -Axis;
			}

			float MinProjection = TNumericLimits<float>::Max();
			float MaxProjection = TNumericLimits<float>::Lowest();
			for (int32 WedgeIndex : Members)
			{
				const float Projection = (Positions[VertexIndices[WedgeIndex]] - Centroid) | Axis;
				MinProjection = FMath::Min(MinProjection, Projection);
				MaxProjection = FMath::Max(MaxProjection, Projection);
			}

			const float Span = MaxProjection - MinProjection;
			if (Span < UE_KINDA_SMALL_NUMBER)
			{
				continue;
			}

			// Which end is the tip. Tip Direction is the fallback everywhere, because it is the one
			// rule that answers the same way for every island in the selection.
			const bool bDirectionPicksPositive = (Axis | TipDirection) > 0.0f;

			bool bPositiveEndIsTip = bDirectionPicksPositive;
			switch (Orientation)
			{
			case EMoonToonIslandOrientation::PositiveEnd:
				bPositiveEndIsTip = true;
				break;
			case EMoonToonIslandOrientation::NegativeEnd:
				bPositiveEndIsTip = false;
				break;
			case EMoonToonIslandOrientation::Direction:
				bPositiveEndIsTip = bDirectionPicksPositive;
				break;
			default:
				{
					// Compare how far the geometry spreads off-axis near each end. A hair card is wide
					// at the root and narrow at the tip, so the thinner end is the tip.
					double NearRadius = 0.0, FarRadius = 0.0;
					int32 NearCount = 0, FarCount = 0;
					for (int32 WedgeIndex : Members)
					{
						const FVector3f D = Positions[VertexIndices[WedgeIndex]] - Centroid;
						const float Projection = D | Axis;
						const float Normalized = (Projection - MinProjection) / Span;
						const float Radius = (D - Axis * Projection).Size();

						if (Normalized < 0.2f) { NearRadius += Radius; ++NearCount; }
						else if (Normalized > 0.8f) { FarRadius += Radius; ++FarCount; }
					}

					// Only trust the measurement when the ends actually differ. A card of even width
					// -- and hair has plenty of those -- puts the two means within noise of each
					// other, and taking the winner anyway tapers every second card backwards. That
					// reads as noise along the silhouette and no single setting can undo it, whereas
					// one consistent direction is at worst one flick of Invert Signal from right.
					bool bResolved = false;
					if (NearCount > 0 && FarCount > 0)
					{
						const double NearMean = NearRadius / NearCount;
						const double FarMean = FarRadius / FarCount;
						const double Contrast = FMath::Abs(NearMean - FarMean)
							/ FMath::Max(FMath::Max(NearMean, FarMean), UE_DOUBLE_SMALL_NUMBER);
						if (Contrast >= AutoTipContrast)
						{
							bPositiveEndIsTip = FarMean <= NearMean;
							bResolved = true;
						}
					}
					if (!bResolved)
					{
						bPositiveEndIsTip = bDirectionPicksPositive;
						++Result.NumDirectionOriented;
					}
				}
				break;
			}

			for (int32 WedgeIndex : Members)
			{
				const float Projection = (Positions[VertexIndices[WedgeIndex]] - Centroid) | Axis;
				const float Normalized = (Projection - MinProjection) / Span;
				Result.PerWedge[WedgeIndex] = bPositiveEndIsTip ? Normalized : (1.0f - Normalized);
			}
		}

		if (KeptIslandSizes.Num() > 0)
		{
			KeptIslandSizes.Sort();
			Result.MedianIslandVertices = KeptIslandSizes[KeptIslandSizes.Num() / 2];
			Result.LargestIslandVertices = KeptIslandSizes.Last();
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
		"discarding hand-painted work.\n\n"
		"Island Axis splits the selection into UV shells -- one hair card each -- and tapers along "
		"every shell's own axis. Read the report: it says how many shells were found and how many "
		"had no clear thin end, and those are oriented by Tip Direction rather than guessed.");
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

	// Zero would silently orient every ambiguous island the same way while claiming to be a direction,
	// so it falls back to the documented default instead.
	FVector3f NormalizedTipDirection(
		static_cast<float>(TipDirection.X),
		static_cast<float>(TipDirection.Y),
		static_cast<float>(TipDirection.Z));
	if (!NormalizedTipDirection.Normalize())
	{
		NormalizedTipDirection = FVector3f(0.0f, 0.0f, -1.0f);
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
			TArray<FString> ExtraNotes;

			if (Signal == EMoonToonAlphaSignal::IslandAxis)
			{
				const FSignalResult IslandSignal = ComputeIslandAxisSignal(
					Positions, VertexIndices, UV0s, Faces, Mask, MinIslandVertices, IslandOrientation,
					NormalizedTipDirection, AutoTipContrast);

				for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
				{
					if (IslandSignal.PerWedge.IsValidIndex(WedgeIndex))
					{
						WedgeSignal[WedgeIndex] = IslandSignal.PerWedge[WedgeIndex];
					}
				}
				SignalNote = FString::Printf(
					TEXT("%d UV shell(s), median %d verts, largest %d; %d skipped as under %d verts"),
					IslandSignal.NumIslands, IslandSignal.MedianIslandVertices,
					IslandSignal.LargestIslandVertices, IslandSignal.NumSkippedIslands,
					MinIslandVertices);

				if (IslandSignal.NumIslands == 0)
				{
					// Worth spelling out: with no island the signal is 0 everywhere, so the curve
					// writes its value at 0 to every vertex and the run looks successful while
					// changing nothing.
					ExtraNotes.Add(FString::Printf(TEXT(
						"      -> no shell survived Min Island Vertices, so the signal is 0 everywhere. "
						"Lower Min Island Vertices, or widen the section filter.")));
				}
				else if (IslandSignal.NumDirectionOriented > 0)
				{
					ExtraNotes.Add(FString::Printf(TEXT(
						"      %d of %d shell(s) had no clear thin end -- oriented by Tip Direction (%s)."),
						IslandSignal.NumDirectionOriented, IslandSignal.NumIslands,
						*NormalizedTipDirection.ToString()));
				}
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
			Lines.Append(ExtraNotes);
			Lines.Add(FString::Printf(TEXT("      alpha min %.3f  max %.3f  mean %.3f"),
				MinAlpha, MaxAlpha, SumAlpha / NumWritten));

			// A flat result is the shape every silent failure takes -- a collapsed segmentation, a
			// signal the curve flattens, a floor or ceiling clamping everything together. It is easy
			// to read past in a wall of statistics, so say it outright rather than leave the reader to
			// notice that min and max agree.
			if (MaxAlpha - MinAlpha < 1.0f / 255.0f)
			{
				Lines.Add(FString::Printf(TEXT(
					"      -> every vertex got the same width (%.3f). Nothing tapered; check the notes above, "
					"the curve, and Output Floor/Ceiling."), MinAlpha));
			}

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
					// Vertex colour only. Writing the UV channels back from here would undo the two
					// tangent bakes exactly the way they used to undo this taper.
					MoonToonMesh::WriteMeshChannels(Mesh, LOD, nullptr, nullptr, nullptr, nullptr, &Colors);
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
