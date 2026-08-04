// Copyright Dream Moon. All Rights Reserved.

#include "MoonToonMeshInfoTool.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "MoonToonEditorBPLibrary.h"
#include "MoonToonMeshTargets.h"

#define LOCTEXT_NAMESPACE "MoonToonMeshInfoTool"

namespace
{
	/** min / max / mean of a scalar series, plus whether it never changed. */
	struct FChannelStats
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

		bool IsConstant() const { return Count > 0 && (Max - Min) < KINDA_SMALL_NUMBER; }
		float Mean() const { return Count > 0 ? static_cast<float>(Sum / Count) : 0.0f; }

		FString Describe() const
		{
			if (Count == 0)
			{
				return TEXT("(no data)");
			}
			if (IsConstant())
			{
				return FString::Printf(TEXT("constant %.3f"), Min);
			}
			return FString::Printf(TEXT("min %.3f  max %.3f  mean %.3f"), Min, Max, Mean());
		}
	};

	/**
	 * Classifies one of the two bake channel sets by the length of the decoded vector.
	 *
	 * The encodings make this cleanly separable:
	 *   - face-forward stores a unit tangent-space direction, so |decoded| == 1
	 *   - smooth-normal stores the normal scaled by curvature in (0,1], so |decoded| <= 1 and varies
	 *   - an untouched channel is all zeros, and 0*2-1 == -1 per component, so |decoded| == sqrt(3)
	 *
	 * That last one is the useful part: "never baked" has a distinctive signature rather than looking
	 * like a degenerate bake.
	 */
	FString ClassifyBake(const FChannelStats& LengthStats, int32 NumZeroSourceWedges, int32 NumWedges)
	{
		if (LengthStats.Count == 0)
		{
			return TEXT("no data");
		}

		const float ZeroFraction = NumWedges > 0 ? static_cast<float>(NumZeroSourceWedges) / NumWedges : 0.0f;
		if (ZeroFraction > 0.95f)
		{
			return TEXT("EMPTY (channels are all zero -- never baked)");
		}

		const float Mean = LengthStats.Mean();
		if (FMath::Abs(Mean - 1.0f) < 0.05f && (LengthStats.Max - LengthStats.Min) < 0.15f)
		{
			return TEXT("FACE-FORWARD (unit length -- looks like a face-forward bake)");
		}
		if (LengthStats.Max <= 1.05f && Mean > 0.02f)
		{
			return TEXT("SMOOTH-NORMAL (length varies within [0,1] -- that variation is the curvature)");
		}
		if (FMath::Abs(Mean - UE_SQRT_3) < 0.1f)
		{
			return TEXT("EMPTY (decoded length ~= sqrt(3) -- channels are zero)");
		}
		return TEXT("UNKNOWN (does not match either bake signature)");
	}

	void AppendLODReport(
		UObject* Mesh,
		int32 LODIndex,
		bool bAnalyzeVertexColors,
		bool bDetectBakes,
		const FMoonToonLODFaces& Faces,
		const FMoonToonWedgeMask& Mask,
		TArray<FString>& Lines)
	{
		TArray<FVector3f> Positions, Normals, Tangents, Binormals;
		TArray<int32> VertexIndices;
		TArray<FColor> Colors;
		TArray<FVector2f> UV0s, UV1s, UV2s, UV3s;
		UMoonToonEditorBPLibrary::MoonGetMeshData(Mesh, LODIndex, Positions, VertexIndices, Normals,
			Tangents, Binormals, Colors, UV0s, UV1s, UV2s, UV3s);

		const int32 NumWedges = VertexIndices.Num();
		if (NumWedges == 0)
		{
			Lines.Add(FString::Printf(TEXT("  LOD %d: no import data (generated LOD?)."), LODIndex));
			return;
		}

		Lines.Add(FString::Printf(TEXT("  LOD %d: %d points, %d wedges, %d triangles"),
			LODIndex, Positions.Num(), NumWedges, NumWedges / 3));

		TArray<FMoonToonSectionInfo> Sections;
		if (MoonToonMesh::GetSections(Mesh, Faces, Sections))
		{
			Lines.Add(TEXT("    Sections:"));
			for (const FMoonToonSectionInfo& Section : Sections)
			{
				Lines.Add(FString::Printf(TEXT("      [%d] %-28s %-40s %6d tris"),
					Section.MaterialIndex,
					*Section.SlotName.ToString(),
					*Section.MaterialName,
					Section.NumTriangles));
			}
		}

		// Count what the filter actually covers, so every statistic below has a denominator the
		// reader can see.
		int32 NumConsidered = 0;
		for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
		{
			if (Mask.Contains(WedgeIndex))
			{
				++NumConsidered;
			}
		}
		if (!Mask.bIncludeAll)
		{
			Lines.Add(FString::Printf(TEXT("    Statistics below cover %d of %d wedges (section filter)."),
				NumConsidered, NumWedges));
		}

		if (bAnalyzeVertexColors)
		{
			FChannelStats R, G, B, A;
			for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
			{
				if (!Mask.Contains(WedgeIndex) || !Colors.IsValidIndex(WedgeIndex))
				{
					continue;
				}
				const FColor& Color = Colors[WedgeIndex];
				R.Add(Color.R / 255.0f);
				G.Add(Color.G / 255.0f);
				B.Add(Color.B / 255.0f);
				A.Add(Color.A / 255.0f);
			}

			Lines.Add(TEXT("    Vertex colour:"));
			Lines.Add(FString::Printf(TEXT("      R  %s"), *R.Describe()));
			Lines.Add(FString::Printf(TEXT("      G  %s"), *G.Describe()));
			Lines.Add(FString::Printf(TEXT("      B  %s"), *B.Describe()));
			Lines.Add(FString::Printf(TEXT("      A  %s"), *A.Describe()));

			// The failure mode worth calling out by name: an all-white stream means nothing has been
			// painted, and an all-black alpha silently deletes the outline once the width switch is on.
			if (R.IsConstant() && G.IsConstant() && B.IsConstant() && A.IsConstant()
				&& R.Min > 0.99f && G.Min > 0.99f && B.Min > 0.99f && A.Min > 0.99f)
			{
				Lines.Add(TEXT("      -> Uniform white: nothing painted (or no colour stream at all)."));
			}
			else if (A.IsConstant() && A.Min < 0.01f)
			{
				Lines.Add(TEXT("      -> Alpha is zero everywhere. 'Use Vertex Color A as Outline Width' "
					"would collapse the outline hull onto the surface."));
			}
		}

		if (bDetectBakes)
		{
			// Face-forward owns UV1.xy + UV2.x; smooth-normal owns UV2.y + UV3.xy. Both are stored
			// remapped from [-1,1] to [0,1], so decoding is v*2-1 per component.
			FChannelStats FaceForwardLen, SmoothNormalLen;
			int32 FaceForwardZeros = 0;
			int32 SmoothNormalZeros = 0;

			for (int32 WedgeIndex = 0; WedgeIndex < NumWedges; ++WedgeIndex)
			{
				if (!Mask.Contains(WedgeIndex))
				{
					continue;
				}
				if (UV1s.IsValidIndex(WedgeIndex) && UV2s.IsValidIndex(WedgeIndex))
				{
					const FVector3f Raw(UV1s[WedgeIndex].X, UV1s[WedgeIndex].Y, UV2s[WedgeIndex].X);
					FaceForwardLen.Add((Raw * 2.0f - FVector3f(1.0f)).Size());
					FaceForwardZeros += Raw.IsNearlyZero() ? 1 : 0;
				}
				if (UV2s.IsValidIndex(WedgeIndex) && UV3s.IsValidIndex(WedgeIndex))
				{
					const FVector3f Raw(UV2s[WedgeIndex].Y, UV3s[WedgeIndex].X, UV3s[WedgeIndex].Y);
					SmoothNormalLen.Add((Raw * 2.0f - FVector3f(1.0f)).Size());
					SmoothNormalZeros += Raw.IsNearlyZero() ? 1 : 0;
				}
			}

			Lines.Add(TEXT("    Bake channels (decoded as v*2-1):"));
			Lines.Add(FString::Printf(TEXT("      UV1.xy + UV2.x  |v| %s"), *FaceForwardLen.Describe()));
			Lines.Add(FString::Printf(TEXT("        -> %s"),
				*ClassifyBake(FaceForwardLen, FaceForwardZeros, NumConsidered)));
			Lines.Add(FString::Printf(TEXT("      UV2.y + UV3.xy  |v| %s"), *SmoothNormalLen.Describe()));
			Lines.Add(FString::Printf(TEXT("        -> %s"),
				*ClassifyBake(SmoothNormalLen, SmoothNormalZeros, NumConsidered)));
		}
	}
}

FText UMoonToonMeshInfoTool::GetToolName() const
{
	return LOCTEXT("MeshInfoName", "Mesh Info");
}

FText UMoonToonMeshInfoTool::GetToolDescription() const
{
	return LOCTEXT("MeshInfoDesc",
		"Reports what the mesh actually carries: sections, vertex colour usage per channel, and "
		"whether the two MoonToon UV bakes are present.\n\n"
		"Read-only. Run this before a bake or before painting outline width, to see which channels "
		"are already spoken for.");
}

FText UMoonToonMeshInfoTool::GetRunLabel() const
{
	return LOCTEXT("MeshInfoRun", "Inspect");
}

FString UMoonToonMeshInfoTool::Run(const FMoonToonToolContext& Context)
{
	TArray<UObject*> Meshes;
	FString Report;
	if (!ResolveMeshes(Context, Meshes, Report))
	{
		return Report;
	}

	TArray<FString> Lines;
	for (UObject* Mesh : Meshes)
	{
		Lines.Add(FString::Printf(TEXT("%s  (%s)"), *Mesh->GetName(), *Mesh->GetClass()->GetName()));
		Lines.Add(FString::Printf(TEXT("  %s"), *Mesh->GetPathName()));

		const int32 NumLODs = MoonToonMesh::GetNumLODs(Mesh);
		Lines.Add(FString::Printf(TEXT("  LODs: %d"), NumLODs));

		if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh))
		{
			Lines.Add(FString::Printf(TEXT("  Bones: %d    Morph targets: %d"),
				SkeletalMesh->GetRefSkeleton().GetNum(), SkeletalMesh->GetMorphTargets().Num()));
		}

		const int32 LastLOD = bAllLODs ? NumLODs - 1 : 0;
		for (int32 LODIndex = 0; LODIndex <= LastLOD; ++LODIndex)
		{
			// Read the face data once: the sections, the wedge mask and the report below all derive
			// from it, and on the skeletal side every read decompresses the whole import-data blob.
			FMoonToonLODFaces Faces;
			MoonToonMesh::GetFaces(Mesh, LODIndex, Faces);

			const FMoonToonWedgeMask Mask = bRespectSectionFilter
				? MoonToonMesh::BuildWedgeMask(Faces, Context.SectionMaterialIndices)
				: FMoonToonWedgeMask();
			AppendLODReport(Mesh, LODIndex, bAnalyzeVertexColors, bDetectBakes, Faces, Mask, Lines);
		}
		Lines.Add(TEXT(""));
	}

	return FString::Join(Lines, TEXT("\n"));
}

#undef LOCTEXT_NAMESPACE
