// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonNormalPreviewTool.h"

#include "Components/MeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Editor.h"
#include "Engine/World.h"
#include "MoonToonEditorBPLibrary.h"
#include "MoonToonMeshTargets.h"
#include "MoonToonSmoothNormalTool.h"

#define LOCTEXT_NAMESPACE "MoonToonNormalPreviewTool"

namespace
{
	bool IsBakedSource(EMoonToonNormalSource Source)
	{
		return Source == EMoonToonNormalSource::SmoothedNormal || Source == EMoonToonNormalSource::FaceForward;
	}

	const TCHAR* SourceLabel(EMoonToonNormalSource Source)
	{
		switch (Source)
		{
		case EMoonToonNormalSource::VertexNormal:   return TEXT("Vertex Normal (import TangentZ)");
		case EMoonToonNormalSource::FaceNormal:     return TEXT("Face Normal (triangle winding)");
		case EMoonToonNormalSource::SmoothedNormal: return TEXT("Smoothed Normal (baked UV2.y / UV3.xy)");
		case EMoonToonNormalSource::FaceForward:    return TEXT("Face Forward (baked UV1.xy / UV2.x)");
		case EMoonToonNormalSource::Tangent:        return TEXT("Tangent (TangentX, U direction)");
		case EMoonToonNormalSource::Binormal:       return TEXT("Binormal (TangentY, V direction)");
		}
		return TEXT("?");
	}

	/** What Colour = Agreement compares against, spelled out because it changes with the source. */
	const TCHAR* ReferenceLabel(EMoonToonNormalSource Source)
	{
		return Source == EMoonToonNormalSource::VertexNormal ? TEXT("face winding") : TEXT("vertex normal");
	}

	/**
	 * UE's triangle-normal convention, as MeshUtilities' CalculateTriangleTangentInternal builds it:
	 * cross(P1 - P2, P0 - P2). Reversing the operands flips every face normal, which would turn the
	 * flipped-normal check below into a confident report that the whole mesh is inside out.
	 */
	FVector3f GeometricFaceNormal(const FVector3f& P0, const FVector3f& P1, const FVector3f& P2)
	{
		return ((P1 - P2) ^ (P0 - P2)).GetSafeNormal();
	}

	FColor DirectionColor(const FVector3f& Direction)
	{
		return FColor(
			static_cast<uint8>(FMath::Clamp((Direction.X * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f)),
			static_cast<uint8>(FMath::Clamp((Direction.Y * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f)),
			static_cast<uint8>(FMath::Clamp((Direction.Z * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f)));
	}

	/** 0 = red, 0.5 = yellow, 1 = green. Through yellow rather than straight red-to-green so the
	 *  perpendicular case is distinguishable instead of washing out into muddy olive. */
	FColor AgreementColor(float Value)
	{
		const float T = FMath::Clamp(Value, 0.0f, 1.0f);
		const FLinearColor Low(1.0f, 0.05f, 0.05f);
		const FLinearColor Mid(1.0f, 0.85f, 0.05f);
		const FLinearColor High(0.1f, 1.0f, 0.1f);
		const FLinearColor Result = (T < 0.5f)
			? FMath::Lerp(Low, Mid, T * 2.0f)
			: FMath::Lerp(Mid, High, (T - 0.5f) * 2.0f);
		return Result.ToFColor(/*bSRGB=*/true);
	}

	/** blue 0 -> white 1 -> magenta sqrt(3). The top of the ramp is the all-zero-channel signature:
	 *  an untouched UV pair decodes to (-1,-1,-1), so "never baked" looks nothing like a weak bake. */
	FColor LengthColor(float Length)
	{
		const FLinearColor Zero(0.05f, 0.15f, 1.0f);
		const FLinearColor One(1.0f, 1.0f, 1.0f);
		const FLinearColor Over(1.0f, 0.0f, 1.0f);
		const FLinearColor Result = (Length <= 1.0f)
			? FMath::Lerp(Zero, One, FMath::Clamp(Length, 0.0f, 1.0f))
			: FMath::Lerp(One, Over, FMath::Clamp((Length - 1.0f) / (UE_SQRT_3 - 1.0f), 0.0f, 1.0f));
		return Result.ToFColor(/*bSRGB=*/true);
	}

	/** min / max / mean of a scalar series. */
	struct FSeries
	{
		float Min = TNumericLimits<float>::Max();
		float Max = TNumericLimits<float>::Lowest();
		double Sum = 0.0;
		int32 Count = 0;

		void Add(float Value)
		{
			Min = FMath::Min(Min, Value);
			Max = FMath::Max(Max, Value);
			Sum += Value;
			++Count;
		}

		float Mean() const { return Count > 0 ? static_cast<float>(Sum / Count) : 0.0f; }

		FString Describe() const
		{
			if (Count == 0)
			{
				return TEXT("(no data)");
			}
			if ((Max - Min) < KINDA_SMALL_NUMBER)
			{
				return FString::Printf(TEXT("constant %.3f"), Min);
			}
			return FString::Printf(TEXT("min %.3f  max %.3f  mean %.3f"), Min, Max, Mean());
		}
	};

	/** One wedge's drawn direction, in the mesh's local space. */
	struct FWedgeDirection
	{
		/** Normalised, or zero when there was nothing to decode. */
		FVector3f Local = FVector3f::ZeroVector;

		/** Length before normalising: 1 for the raw basis vectors, the baked curvature for the
		 *  smoothed normal, and sqrt(3) for an untouched channel. */
		float EncodedLength = 0.0f;

		/** The tangent basis had a zero or NaN axis and fell back to an identity axis. */
		bool bDegenerateBasis = false;

		/** The source channels were all zero -- the bake never ran here. */
		bool bEmptyChannel = false;
	};

	FWedgeDirection ResolveWedgeDirection(
		EMoonToonNormalSource Source,
		int32 WedgeIndex,
		const TArray<FVector3f>& Normals,
		const TArray<FVector3f>& Tangents,
		const TArray<FVector3f>& Binormals,
		const TArray<FVector3f>& WedgeFaceNormals,
		const TArray<FVector2f>& UV1s,
		const TArray<FVector2f>& UV2s,
		const TArray<FVector2f>& UV3s)
	{
		auto FromArray = [WedgeIndex](const TArray<FVector3f>& Array)
		{
			FWedgeDirection Out;
			const FVector3f Raw = Array.IsValidIndex(WedgeIndex) ? Array[WedgeIndex] : FVector3f::ZeroVector;
			Out.EncodedLength = Raw.Size();
			Out.Local = Raw.GetSafeNormal();
			return Out;
		};

		switch (Source)
		{
		case EMoonToonNormalSource::VertexNormal: return FromArray(Normals);
		case EMoonToonNormalSource::FaceNormal:   return FromArray(WedgeFaceNormals);
		case EMoonToonNormalSource::Tangent:      return FromArray(Tangents);
		case EMoonToonNormalSource::Binormal:     return FromArray(Binormals);
		default: break;
		}

		// The two bakes: exactly the inverse of what BakeFaceForwardForLOD / BakeSmoothedNormalForLOD
		// wrote -- undo the *0.5+0.5 remap, then take the tangent-space vector back to local space
		// through the same basis the bake encoded against. Anything else here would be measuring a
		// different quantity than the material reads.
		const FVector2f UV1 = UV1s.IsValidIndex(WedgeIndex) ? UV1s[WedgeIndex] : FVector2f::ZeroVector;
		const FVector2f UV2 = UV2s.IsValidIndex(WedgeIndex) ? UV2s[WedgeIndex] : FVector2f::ZeroVector;
		const FVector2f UV3 = UV3s.IsValidIndex(WedgeIndex) ? UV3s[WedgeIndex] : FVector2f::ZeroVector;

		const FVector3f Encoded = (Source == EMoonToonNormalSource::FaceForward)
			? FVector3f(UV1.X, UV1.Y, UV2.X)
			: FVector3f(UV2.Y, UV3.X, UV3.Y);

		FWedgeDirection Out;
		Out.bEmptyChannel = Encoded.IsNearlyZero();

		const FVector3f Decoded = Encoded * 2.0f - FVector3f(1.0f);
		Out.EncodedLength = Decoded.Size();

		const FVector3f RawNormal = Normals.IsValidIndex(WedgeIndex) ? Normals[WedgeIndex] : FVector3f::ZeroVector;
		const FVector3f RawTangent = Tangents.IsValidIndex(WedgeIndex) ? Tangents[WedgeIndex] : FVector3f::ZeroVector;
		const FVector3f RawBinormal = Binormals.IsValidIndex(WedgeIndex) ? Binormals[WedgeIndex] : FVector3f::ZeroVector;
		Out.bDegenerateBasis =
			RawNormal.IsNearlyZero() || RawNormal.ContainsNaN() ||
			RawTangent.IsNearlyZero() || RawTangent.ContainsNaN() ||
			RawBinormal.IsNearlyZero() || RawBinormal.ContainsNaN();

		// Null mesh: the fallback basis is the same one the bake would have used, but the count of
		// those wedges belongs in the report, not in thousands of log lines.
		const FMatrix TangentToLocal = UMoonToonSmoothNormalTool::BuildTangentToLocal(
			WedgeIndex, /*Mesh=*/nullptr, Normals, Tangents, Binormals);
		const FVector LocalDirection(TangentToLocal.TransformVector(FVector(Decoded.GetSafeNormal())));
		Out.Local = FVector3f(LocalDirection).GetSafeNormal();
		return Out;
	}
}

FText UMoonToonNormalPreviewTool::GetToolName() const
{
	return LOCTEXT("NormalPreviewName", "Normal Preview");
}

FText UMoonToonNormalPreviewTool::GetToolDescription() const
{
	return LOCTEXT("NormalPreviewDesc",
		"Draws the mesh's directions in the viewport: the raw vertex normal, the triangle's own "
		"winding normal, the two MoonToon bakes decoded back through the tangent basis that encoded "
		"them, or the tangent / binormal.\n\n"
		"Read-only. Use it to answer the questions that otherwise need a debug switch in the material: "
		"are any normals flipped, did the smooth-normal bake actually land, does the face-forward "
		"bake still point +Y, is a hard edge a hard edge or a mistake.\n\n"
		"Lines come from IMPORT data, so they sit at the reference pose -- on a posed character they "
		"will not follow the mesh. Select sections in the list above to restrict the draw.");
}

FText UMoonToonNormalPreviewTool::GetRunLabel() const
{
	return Mode == EMoonToonNormalPreviewMode::Clear
		? LOCTEXT("NormalPreviewClear", "Clear")
		: LOCTEXT("NormalPreviewRun", "Preview");
}

FString UMoonToonNormalPreviewTool::Run(const FMoonToonToolContext& Context)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

	if (Mode == EMoonToonNormalPreviewMode::Clear)
	{
		if (!World)
		{
			return TEXT("No editor world -- nothing to clear.");
		}
		FlushPersistentDebugLines(World);
		return TEXT("Cleared. Lines drawn with a Duration expire on their own; this only removes the "
			"persistent ones (Duration = 0).");
	}

	TArray<UObject*> Meshes;
	FString Report;
	if (!ResolveMeshes(Context, Meshes, Report))
	{
		return Report;
	}

	const bool bDraw = !bReportOnly;
	if (bDraw && !World)
	{
		return TEXT("No editor world -- cannot draw the preview.");
	}

	// One flush per run, before any mesh draws: each preview replaces the previous one.
	if (bDraw)
	{
		FlushPersistentDebugLines(World);
	}

	const bool bPersistent = DurationSeconds <= 0.0f;
	const float LifeTime = bPersistent ? -1.0f : DurationSeconds;
	const int32 Stride = FMath::Max(1, VertexStride);

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("%s -- %s  [nothing written]"),
		bDraw ? TEXT("Preview Normals") : TEXT("Normal Report"), SourceLabel(Source)));

	for (UObject* Mesh : Meshes)
	{
		// Bounds-check before reading: the mesh-data library indexes the LOD arrays directly.
		const int32 NumLODs = MoonToonMesh::GetNumLODs(Mesh);
		if (LODIndex < 0 || LODIndex >= NumLODs)
		{
			Lines.Add(FString::Printf(TEXT("  %s: LOD %d does not exist (%d LODs) -- skipped."),
				*Mesh->GetName(), LODIndex, NumLODs));
			continue;
		}

		TArray<FVector3f> Positions, Normals, Tangents, Binormals;
		TArray<int32> VertexIndices;
		TArray<FColor> Colors;
		TArray<FVector2f> UV0s, UV1s, UV2s, UV3s;
		UMoonToonEditorBPLibrary::MoonGetMeshData(Mesh, LODIndex, Positions, VertexIndices, Normals,
			Tangents, Binormals, Colors, UV0s, UV1s, UV2s, UV3s);

		const int32 NumWedges = VertexIndices.Num();
		if (NumWedges == 0)
		{
			Lines.Add(FString::Printf(TEXT("  %s LOD%d: no import data -- skipped."), *Mesh->GetName(), LODIndex));
			continue;
		}

		// Same repair the bakes run, so a mesh whose import tangents are missing decodes against the
		// basis the bake would have used rather than against zeros.
		UMoonToonSmoothNormalTool::RecomputeMissingTangentAndBinormal(
			Positions, VertexIndices, UV0s, Normals, Tangents, Binormals);

		FMoonToonLODFaces Faces;
		MoonToonMesh::GetFaces(Mesh, LODIndex, Faces);
		const FMoonToonWedgeMask Mask =
			MoonToonMesh::BuildWedgeMask(Faces, Context.SectionMaterialIndices, NumWedges);

		// Face normals, fanned out to the wedges: the Face Normal source draws them, and every other
		// source uses them for the flipped-normal check.
		TArray<FVector3f> WedgeFaceNormals;
		WedgeFaceNormals.Init(FVector3f::ZeroVector, NumWedges);
		for (const FIntVector& FaceWedges : Faces.Wedges)
		{
			if (!VertexIndices.IsValidIndex(FaceWedges.X) || !VertexIndices.IsValidIndex(FaceWedges.Y)
				|| !VertexIndices.IsValidIndex(FaceWedges.Z))
			{
				continue;
			}
			const FVector3f FaceNormal = GeometricFaceNormal(
				Positions[VertexIndices[FaceWedges.X]],
				Positions[VertexIndices[FaceWedges.Y]],
				Positions[VertexIndices[FaceWedges.Z]]);
			WedgeFaceNormals[FaceWedges.X] = FaceNormal;
			WedgeFaceNormals[FaceWedges.Y] = FaceNormal;
			WedgeFaceNormals[FaceWedges.Z] = FaceNormal;
		}

		// --- Statistics: every masked wedge, no stride, no cap. Deliberately independent of the draw
		// settings, so turning the stride up to keep the viewport responsive does not quietly change
		// the numbers the report is judged on.
		FSeries LengthStats;
		FSeries AngleStats;
		int32 NumConsidered = 0;
		int32 NumOpposed = 0;
		int32 NumZeroDirection = 0;
		int32 NumDegenerateBasis = 0;
		int32 NumEmptyChannel = 0;
		int32 NumFlippedVsWinding = 0;
		int32 NumWindingComparable = 0;
		FVector3f DirectionSum = FVector3f::ZeroVector;

		for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
		{
			if (!Mask.Contains(WedgeIndex))
			{
				continue;
			}
			++NumConsidered;

			const FWedgeDirection Direction = ResolveWedgeDirection(Source, WedgeIndex, Normals, Tangents,
				Binormals, WedgeFaceNormals, UV1s, UV2s, UV3s);
			NumDegenerateBasis += Direction.bDegenerateBasis ? 1 : 0;
			NumEmptyChannel += Direction.bEmptyChannel ? 1 : 0;

			// The flipped check runs whatever is being drawn: it is the question the tool exists for,
			// and it costs one dot product.
			const FVector3f RawNormal = Normals.IsValidIndex(WedgeIndex)
				? Normals[WedgeIndex].GetSafeNormal() : FVector3f::ZeroVector;
			if (!RawNormal.IsNearlyZero() && !WedgeFaceNormals[WedgeIndex].IsNearlyZero())
			{
				++NumWindingComparable;
				NumFlippedVsWinding += ((RawNormal | WedgeFaceNormals[WedgeIndex]) < 0.0f) ? 1 : 0;
			}

			if (Direction.Local.IsNearlyZero())
			{
				++NumZeroDirection;
				continue;
			}

			LengthStats.Add(Direction.EncodedLength);
			DirectionSum += Direction.Local;

			const FVector3f Reference = (Source == EMoonToonNormalSource::VertexNormal)
				? WedgeFaceNormals[WedgeIndex] : RawNormal;
			if (!Reference.IsNearlyZero())
			{
				const float Dot = FMath::Clamp(Direction.Local | Reference, -1.0f, 1.0f);
				AngleStats.Add(FMath::RadiansToDegrees(FMath::Acos(Dot)));
				NumOpposed += (Dot < 0.0f) ? 1 : 0;
			}
		}

		if (NumConsidered == 0)
		{
			Lines.Add(FString::Printf(TEXT("  %s LOD%d: the section filter excludes every wedge."),
				*Mesh->GetName(), LODIndex));
			continue;
		}

		// --- Draw. -----------------------------------------------------------------------------
		int32 NumDrawn = 0;
		int32 NumSplitVertices = 0;
		bool bCapped = false;
		bool bPlaced = true;

		if (bDraw)
		{
			const UMeshComponent* Placed = MoonToonMesh::FindPlacedMeshComponent(Mesh);
			bPlaced = Placed != nullptr;
			const FTransform MeshToWorld = bPlaced ? Placed->GetComponentTransform() : FTransform::Identity;

			// TransformVectorNoScale, matching the strand preview: a normal under non-uniform scale
			// would want the inverse transpose, and characters are not placed with one.
			auto DrawOne = [&](const FVector3f& LocalPosition, const FVector3f& LocalOffsetDir,
				const FVector3f& LocalDirection, const FColor& Color)
			{
				// Lift the line off the surface so it does not z-fight the mesh.
				const FVector WorldStart =
					MeshToWorld.TransformPosition(FVector(LocalPosition + LocalOffsetDir * 0.05f));
				const FVector WorldDirection =
					MeshToWorld.TransformVectorNoScale(FVector(LocalDirection)).GetSafeNormal();
				const FVector WorldEnd = WorldStart + WorldDirection * LineLength;

				if (bArrowHeads)
				{
					DrawDebugDirectionalArrow(World, WorldStart, WorldEnd, LineLength * 0.35f, Color,
						bPersistent, LifeTime, /*DepthPriority=*/0, LineThickness);
				}
				else
				{
					DrawDebugLine(World, WorldStart, WorldEnd, Color,
						bPersistent, LifeTime, /*DepthPriority=*/0, LineThickness);
				}
				++NumDrawn;
			};

			auto ColorFor = [this](const FWedgeDirection& Direction, const FVector3f& Reference)
			{
				switch (ColorMode)
				{
				case EMoonToonNormalColor::Length:
					return LengthColor(Direction.EncodedLength);

				case EMoonToonNormalColor::Agreement:
				{
					if (Reference.IsNearlyZero())
					{
						return FColor(90, 90, 90);
					}
					const float Dot = FMath::Clamp(Direction.Local | Reference, -1.0f, 1.0f);
					// Tangent and binormal are supposed to be perpendicular to the normal, so for them
					// agreement means orthogonality -- red is a basis that collapsed onto the normal.
					const bool bOrthogonalityCheck = Source == EMoonToonNormalSource::Tangent
						|| Source == EMoonToonNormalSource::Binormal;
					return AgreementColor(bOrthogonalityCheck
						? 1.0f - FMath::Abs(Dot)
						: (Dot + 1.0f) * 0.5f);
				}

				default:
					return DirectionColor(Direction.Local);
				}
			};

			if (Source == EMoonToonNormalSource::FaceNormal)
			{
				// One line per face, from the face centre: a geometric normal belongs to the triangle,
				// not to its corners, and drawing it three times only thickens the same arrow.
				for (int32 FaceIndex = 0; FaceIndex < Faces.Wedges.Num(); FaceIndex += Stride)
				{
					const FIntVector& FaceWedges = Faces.Wedges[FaceIndex];
					if (!VertexIndices.IsValidIndex(FaceWedges.X) || !VertexIndices.IsValidIndex(FaceWedges.Y)
						|| !VertexIndices.IsValidIndex(FaceWedges.Z) || !Mask.Contains(FaceWedges.X))
					{
						continue;
					}
					const FVector3f FaceNormal = WedgeFaceNormals[FaceWedges.X];
					if (FaceNormal.IsNearlyZero())
					{
						continue;
					}
					if (NumDrawn >= MaxLines)
					{
						bCapped = true;
						break;
					}

					const FVector3f Centre = (Positions[VertexIndices[FaceWedges.X]]
						+ Positions[VertexIndices[FaceWedges.Y]]
						+ Positions[VertexIndices[FaceWedges.Z]]) / 3.0f;

					FWedgeDirection Direction;
					Direction.Local = FaceNormal;
					Direction.EncodedLength = 1.0f;
					const FVector3f Reference = Normals.IsValidIndex(FaceWedges.X)
						? Normals[FaceWedges.X].GetSafeNormal() : FVector3f::ZeroVector;

					DrawOne(Centre, FaceNormal, FaceNormal, ColorFor(Direction, Reference));
					if (bAlsoDrawVertexNormal && !Reference.IsNearlyZero())
					{
						DrawOne(Centre, FaceNormal, Reference, FColor(110, 110, 110));
					}
				}
			}
			else
			{
				// One line per UNIQUE direction at a vertex. Drawing every wedge triples the count and
				// stacks identical arrows; collapsing to one per vertex would hide split normals, which
				// are the thing worth seeing. Keeping the distinct ones does both.
				TArray<TArray<FVector3f>> DrawnAtVertex;
				DrawnAtVertex.SetNum(Positions.Num());

				for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
				{
					const int32 VertexIndex = VertexIndices[WedgeIndex];
					if (!Mask.Contains(WedgeIndex) || !Positions.IsValidIndex(VertexIndex)
						|| (VertexIndex % Stride) != 0)
					{
						continue;
					}

					const FWedgeDirection Direction = ResolveWedgeDirection(Source, WedgeIndex, Normals,
						Tangents, Binormals, WedgeFaceNormals, UV1s, UV2s, UV3s);
					if (Direction.Local.IsNearlyZero())
					{
						continue;
					}

					TArray<FVector3f>& Drawn = DrawnAtVertex[VertexIndex];
					const bool bAlreadyDrawn = Drawn.ContainsByPredicate(
						[&Direction](const FVector3f& Existing) { return (Existing | Direction.Local) > 0.9998f; });
					if (bAlreadyDrawn)
					{
						continue;
					}
					if (NumDrawn >= MaxLines)
					{
						bCapped = true;
						break;
					}
					NumSplitVertices += (Drawn.Num() == 1) ? 1 : 0; // second distinct direction here
					Drawn.Add(Direction.Local);

					const FVector3f RawNormal = Normals.IsValidIndex(WedgeIndex)
						? Normals[WedgeIndex].GetSafeNormal() : FVector3f::ZeroVector;
					const FVector3f Reference = (Source == EMoonToonNormalSource::VertexNormal)
						? WedgeFaceNormals[WedgeIndex] : RawNormal;
					// Offset along the surface normal even when drawing something else, so the whole
					// set lifts off the mesh by the same amount.
					const FVector3f OffsetDir = RawNormal.IsNearlyZero() ? Direction.Local : RawNormal;

					DrawOne(Positions[VertexIndex], OffsetDir, Direction.Local, ColorFor(Direction, Reference));
					if (bAlsoDrawVertexNormal && !RawNormal.IsNearlyZero())
					{
						DrawOne(Positions[VertexIndex], OffsetDir, RawNormal, FColor(110, 110, 110));
					}
				}
			}
		}

		// --- Report. ---------------------------------------------------------------------------
		Lines.Add(FString::Printf(TEXT("  %s LOD%d  (%d wedges in the selection, %d points)"),
			*Mesh->GetName(), LODIndex, NumConsidered, Positions.Num()));

		if (bDraw)
		{
			Lines.Add(FString::Printf(TEXT("    %d lines drawn, stride %d%s%s%s"),
				NumDrawn, Stride,
				(Source != EMoonToonNormalSource::FaceNormal)
					? *FString::Printf(TEXT(", %d split-normal vertices"), NumSplitVertices) : TEXT(""),
				bCapped ? *FString::Printf(TEXT("  -- CAPPED at Max Lines (%d): the draw is truncated, "
					"raise the stride to sample the whole mesh"), MaxLines) : TEXT(""),
				bPlaced ? TEXT("") : TEXT("  -- AT WORLD ORIGIN (mesh is not placed in the level)")));
		}

		Lines.Add(FString::Printf(TEXT("    Decoded length: %s"), *LengthStats.Describe()));
		Lines.Add(FString::Printf(TEXT("    Angle to %s: %s deg, %d opposed (>90 deg)"),
			ReferenceLabel(Source), *AngleStats.Describe(), NumOpposed));

		const FVector3f MeanDirection = DirectionSum.GetSafeNormal();
		if (!MeanDirection.IsNearlyZero() && LengthStats.Count > 0)
		{
			// |sum| / count is the directional coherence: 1 means every direction agreed exactly. On a
			// face-forward bake it should be ~1 (one constant direction); on a normal it never is.
			const float Coherence = DirectionSum.Size() / LengthStats.Count;
			Lines.Add(FString::Printf(TEXT("    Mean direction: (%.3f, %.3f, %.3f) local, coherence %.3f"),
				MeanDirection.X, MeanDirection.Y, MeanDirection.Z, Coherence));
		}

		Lines.Add(FString::Printf(
			TEXT("    Flipped check: %d / %d wedges (%.2f%%) have a vertex normal opposed to their triangle winding"),
			NumFlippedVsWinding, NumWindingComparable,
			NumWindingComparable > 0 ? 100.0f * NumFlippedVsWinding / NumWindingComparable : 0.0f));

		if (NumZeroDirection > 0)
		{
			Lines.Add(FString::Printf(TEXT("    %d wedges had no direction to draw (zero-length source)."),
				NumZeroDirection));
		}
		if (NumDegenerateBasis > 0)
		{
			Lines.Add(FString::Printf(
				TEXT("    %d wedges have a degenerate tangent basis -- decoded against an identity axis, "
					"same fallback the bake used."), NumDegenerateBasis));
		}
		if (IsBakedSource(Source) && NumEmptyChannel > 0)
		{
			const float EmptyPercent = 100.0f * NumEmptyChannel / NumConsidered;
			Lines.Add(FString::Printf(
				TEXT("    %d wedges (%.1f%%) have all-zero source channels -- this bake has not been run "
					"there. Decoded length ~1.732 and magenta lines in Colour = Length say the same thing."),
				NumEmptyChannel, EmptyPercent));
		}
	}

	Lines.Add(TEXT(""));
	Lines.Add(bDraw
		? TEXT("Preview only -- nothing was written. Lines are at the import reference pose; Mode = Clear "
			"removes them when Duration is 0.")
		: TEXT("Report only -- nothing was drawn and nothing was written."));
	return FString::Join(Lines, TEXT("\n"));
}

#undef LOCTEXT_NAMESPACE
